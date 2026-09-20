import { fileIo as fs } from '@kit.CoreFileKit';
import { image } from '@kit.ImageKit';
import { hilog } from './LogService';

const DOMAIN = 0x0000;
const TAG = 'IconExtractor';
const CACHE_DIR = '/data/storage/el2/base/cache/icons';
const RT_ICON = 3;
const RT_GROUP_ICON = 14;

interface IconCandidate {
  width: number;
  height: number;
  bitCount: number;
  nId: number;
}

interface ResDataRef {
  dataRva: number;
  size: number;
}

interface RgbaImage {
  pixels: ArrayBuffer;
  width: number;
  height: number;
}

/**
 * exe 图标提取: 自实现 PE .rsrc 解析。
 * 流程: PE 头 → 资源段 → RT_GROUP_ICON 选候选 → RT_ICON 取数据 →
 * 内嵌 PNG 直接落盘 / DIB 转 RGBA → PixelMap → PNG 缓存。
 */
export class IconExtractor {
  private static instance: IconExtractor | null = null;
  private why: string = '';

  static getInstance(): IconExtractor {
    if (!IconExtractor.instance) {
      IconExtractor.instance = new IconExtractor();
    }
    return IconExtractor.instance;
  }

  async extract(exePath: string): Promise<string> {
    const outPath = `${CACHE_DIR}/${this.cacheName(exePath)}`;
    try {
      if (fs.accessSync(outPath)) {
        try {
          const exeSt = fs.statSync(exePath);
          const iconSt = fs.statSync(outPath);
          if (Number(iconSt.mtime) >= Number(exeSt.mtime)) {
            return outPath;
          }
        } catch (_) {
          return outPath;
        }
      }
      const fd = fs.openSync(exePath, fs.OpenMode.READ_ONLY).fd;
      try {
        this.why = '';
        const png = this.readIconBytes(fd);
        if (!png) {
          hilog.warn(DOMAIN, TAG, 'no icon %{public}s: %{public}s', exePath, this.why || 'unknown');
          return '';
        }
        this.ensureCacheDir();
        if (this.isPng(png)) {
          const out = fs.openSync(outPath, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
          fs.writeSync(out.fd, png);
          fs.closeSync(out);
          return outPath;
        }
        const rgba = this.dibToRgba(png);
        if (!rgba) {
          hilog.warn(DOMAIN, TAG, 'no icon %{public}s: %{public}s', exePath, this.why || 'dib');
          return '';
        }
        return await this.writePng(rgba.pixels, rgba.width, rgba.height, outPath);
      } finally {
        fs.closeSync(fd);
      }
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'extract fail %{public}s: %{public}s', exePath, JSON.stringify(e));
      return '';
    }
  }

  private cacheName(exePath: string): string {
    let h = 5381;
    for (let i = 0; i < exePath.length; i++) {
      h = (((h << 5) + h) + exePath.charCodeAt(i)) | 0;
    }
    const base = exePath.substring(exePath.lastIndexOf('/') + 1)
      .replace(/[^A-Za-z0-9._-]/g, '_')
      .substring(0, 40);
    return `${(h >>> 0).toString(16)}_${base}.png`;
  }

  private ensureCacheDir(): void {
    if (!fs.accessSync(CACHE_DIR)) {
      fs.mkdirSync(CACHE_DIR, true);
    }
  }

  private readAt(fd: number, offset: number, length: number): ArrayBuffer | null {
    if (length <= 0) {
      return null;
    }
    const buf = new ArrayBuffer(length);
    const n = fs.readSync(fd, buf, { offset: offset, length: length });
    return n >= length ? buf : null;
  }

  private readIconBytes(fd: number): ArrayBuffer | null {
    const dos = this.readAt(fd, 0, 64);
    if (!dos) {
      this.why = 'read-dos';
      return null;
    }
    const dv = new DataView(dos);
    if (dv.getUint16(0, true) !== 0x5A4D) {
      this.why = 'not-mz';
      return null;
    }
    const peOff = dv.getUint32(0x3C, true);
    const pe = this.readAt(fd, peOff, 24);
    if (!pe) {
      this.why = 'read-pe';
      return null;
    }
    const pv = new DataView(pe);
    if (pv.getUint32(0, true) !== 0x00004550) {
      this.why = 'not-pe';
      return null;
    }
    const numSections = pv.getUint16(6, true);
    const optSize = pv.getUint16(20, true);
    const opt = this.readAt(fd, peOff + 24, optSize);
    if (!opt || optSize < 2) {
      this.why = 'read-opt';
      return null;
    }
    const ov = new DataView(opt);
    const magic = ov.getUint16(0, true);
    const dirBase = magic === 0x20B ? 112 : (magic === 0x10B ? 96 : -1);
    if (dirBase < 0 || dirBase + 24 > optSize) {
      this.why = 'bad-magic';
      return null;
    }
    const resRva = ov.getUint32(dirBase + 16, true);
    if (resRva === 0) {
      this.why = 'no-rsrc';
      return null;
    }
    const secOff = peOff + 24 + optSize;
    const secs = this.readAt(fd, secOff, numSections * 40);
    if (!secs) {
      this.why = 'read-sections';
      return null;
    }
    const sv = new DataView(secs);
    const rvaToOff = (rva: number, size: number): number => {
      for (let i = 0; i < numSections; i++) {
        const b = i * 40;
        const vsize = sv.getUint32(b + 8, true);
        const vaddr = sv.getUint32(b + 12, true);
        const rawSize = sv.getUint32(b + 16, true);
        const rawPtr = sv.getUint32(b + 20, true);
        if (rva >= vaddr && rva + size <= vaddr + Math.max(vsize, rawSize)) {
          return rawPtr + (rva - vaddr);
        }
      }
      return -1;
    };
    return this.readGroupIcon(fd, rvaToOff, resRva);
  }

  private findTypeDir(fd: number, resOff: number, typeId: number): number {
    const hdr = this.readAt(fd, resOff + 12, 4);
    if (!hdr) {
      return -1;
    }
    const hv = new DataView(hdr);
    const total = hv.getUint16(0, true) + hv.getUint16(2, true);
    const entries = this.readAt(fd, resOff + 16, total * 8);
    if (!entries) {
      return -1;
    }
    const ev = new DataView(entries);
    for (let i = 0; i < total; i++) {
      const id = ev.getUint32(i * 8, true);
      const off = ev.getUint32(i * 8 + 4, true);
      if (!(id & 0x80000000) && (id & 0xFFFF) === typeId && (off & 0x80000000)) {
        return off & 0x7FFFFFFF;
      }
    }
    return -1;
  }

  private firstDataRef(fd: number, resOff: number, dirOff: number): ResDataRef | null {
    return this.dataRefByIndex(fd, resOff, dirOff, -1);
  }

  private dataRefByIndex(fd: number, resOff: number, dirOff: number, wantId: number): ResDataRef | null {
    const hdr = this.readAt(fd, resOff + dirOff + 12, 4);
    if (!hdr) {
      return null;
    }
    const hv = new DataView(hdr);
    const total = hv.getUint16(0, true) + hv.getUint16(2, true);
    const entries = this.readAt(fd, resOff + dirOff + 16, total * 8);
    if (!entries) {
      return null;
    }
    const ev = new DataView(entries);
    for (let i = 0; i < total; i++) {
      const id = ev.getUint32(i * 8, true);
      const off = ev.getUint32(i * 8 + 4, true);
      if (!(off & 0x80000000)) {
        continue;
      }
      // 资源既能用数字 ID 也能用字符串名标识, 部分链接器把整组图标挂在一个名字下。
      // 取"任意一个"时不能跳过名字型条目, 否则整组图标都读不到 (实测 4399启动器 命中);
      // 只有按数字 ID 精确匹配时才必须排除名字型条目。
      if (wantId >= 0 && (id & 0x80000000) !== 0) {
        continue;
      }
      if (wantId >= 0 && (id & 0xFFFF) !== wantId) {
        continue;
      }
      const langDir = off & 0x7FFFFFFF;
      const lhdr = this.readAt(fd, resOff + langDir + 12, 4);
      if (!lhdr) {
        return null;
      }
      const lv = new DataView(lhdr);
      const ltotal = lv.getUint16(0, true) + lv.getUint16(2, true);
      if (ltotal <= 0) {
        return null;
      }
      const dentry = this.readAt(fd, resOff + langDir + 16, 8);
      if (!dentry) {
        return null;
      }
      const dvv = new DataView(dentry);
      const dataOff = dvv.getUint32(4, true);
      if (dataOff & 0x80000000) {
        return null;
      }
      const dataEntry = this.readAt(fd, resOff + dataOff, 8);
      if (!dataEntry) {
        return null;
      }
      const dev = new DataView(dataEntry);
      return { dataRva: dev.getUint32(0, true), size: dev.getUint32(4, true) };
    }
    return null;
  }

  private readGroupIcon(fd: number, rvaToOff: (rva: number, size: number) => number,
    resRva: number): ArrayBuffer | null {
    const resOff = rvaToOff(resRva, 16);
    if (resOff < 0) {
      this.why = 'rsrc-unmapped';
      return null;
    }
    const groupDirOff = this.findTypeDir(fd, resOff, RT_GROUP_ICON);
    if (groupDirOff < 0) {
      this.why = 'no-group-icon';
      return null;
    }
    const groupRef = this.firstDataRef(fd, resOff, groupDirOff);
    if (!groupRef) {
      this.why = 'group-ref';
      return null;
    }
    const groupFileOff = rvaToOff(groupRef.dataRva, groupRef.size);
    if (groupFileOff < 0) {
      this.why = 'group-unmapped';
      return null;
    }
    const grp = this.readAt(fd, groupFileOff, groupRef.size);
    if (!grp || groupRef.size < 6) {
      this.why = 'group-read';
      return null;
    }
    const gv = new DataView(grp);
    const count = gv.getUint16(4, true);
    const cands: IconCandidate[] = [];
    for (let i = 0; i < count; i++) {
      const b = 6 + i * 14;
      if (b + 14 > groupRef.size) {
        break;
      }
      const w = gv.getUint8(b) || 256;
      const h = gv.getUint8(b + 1) || 256;
      cands.push({
        width: w,
        height: h,
        bitCount: gv.getUint16(b + 6, true),
        nId: gv.getUint16(b + 12, true)
      });
    }
    cands.sort((a: IconCandidate, b: IconCandidate) =>
      (b.width * b.height - a.width * a.height) || (b.bitCount - a.bitCount));
    const iconDirOff = this.findTypeDir(fd, resOff, RT_ICON);
    if (iconDirOff < 0) {
      this.why = 'no-icon-dir';
      return null;
    }
    for (const cand of cands) {
      const ref = this.dataRefByIndex(fd, resOff, iconDirOff, cand.nId);
      if (!ref || ref.size <= 0 || ref.size > 4 * 1024 * 1024) {
        this.why = `cand-ref-nid${cand.nId}`;
        continue;
      }
      const off = rvaToOff(ref.dataRva, ref.size);
      if (off < 0) {
        this.why = `cand-unmapped-nid${cand.nId}`;
        continue;
      }
      const data = this.readAt(fd, off, ref.size);
      if (data) {
        return data;
      }
      this.why = `cand-read-nid${cand.nId}-sz${ref.size}`;
    }
    return null;
  }

  private isPng(data: ArrayBuffer): boolean {
    if (data.byteLength < 8) {
      return false;
    }
    const v = new Uint8Array(data);
    return v[0] === 0x89 && v[1] === 0x50 && v[2] === 0x4E && v[3] === 0x47;
  }

  private dibToRgba(data: ArrayBuffer): RgbaImage | null {
    if (data.byteLength < 40) {
      return null;
    }
    const dv = new DataView(data);
    const hdrSize = dv.getUint32(0, true);
    const width = dv.getInt32(4, true);
    const rawHeight = dv.getInt32(8, true);
    const bpp = dv.getUint16(14, true);
    const compression = dv.getUint32(16, true);
    const clrUsed = dv.getUint32(32, true);
    if (width <= 0 || rawHeight <= 0 || rawHeight % 2 !== 0 || compression !== 0) {
      return null;
    }
    if (bpp !== 32 && bpp !== 24 && bpp !== 8 && bpp !== 4) {
      return null;
    }
    const height = rawHeight / 2;
    let palette: Uint8Array | null = null;
    let paletteCount = 0;
    if (bpp <= 8) {
      paletteCount = clrUsed > 0 ? clrUsed : (1 << bpp);
      if (hdrSize + paletteCount * 4 > data.byteLength) {
        return null;
      }
      palette = new Uint8Array(data, hdrSize, paletteCount * 4);
    }
    const xorOff = hdrSize + paletteCount * 4;
    const xorRow = Math.floor((width * bpp + 31) / 32) * 4;
    const andRow = Math.floor((width + 31) / 32) * 4;
    if (xorOff + xorRow * height > data.byteLength) {
      return null;
    }
    const hasAnd = xorOff + xorRow * height + andRow * height <= data.byteLength;
    const andOff = xorOff + xorRow * height;
    const src = new Uint8Array(data);
    const out = new Uint8Array(width * height * 4);
    let opaqueAlphaHint = true;
    for (let y = 0; y < height; y++) {
      const srcRow = xorOff + (height - 1 - y) * xorRow;
      const andRowOff = andOff + (height - 1 - y) * andRow;
      for (let x = 0; x < width; x++) {
        const o = (y * width + x) * 4;
        let r = 0;
        let g = 0;
        let b = 0;
        let a = 255;
        if (bpp === 32) {
          const p = srcRow + x * 4;
          b = src[p];
          g = src[p + 1];
          r = src[p + 2];
          a = src[p + 3];
          if (a !== 0) {
            opaqueAlphaHint = false;
          }
        } else if (bpp === 24) {
          const p = srcRow + x * 3;
          b = src[p];
          g = src[p + 1];
          r = src[p + 2];
        } else if (bpp === 8) {
          const idx = src[srcRow + x];
          if (palette && idx < paletteCount) {
            b = palette[idx * 4];
            g = palette[idx * 4 + 1];
            r = palette[idx * 4 + 2];
          }
        } else {
          const byte = src[srcRow + Math.floor(x / 2)];
          const idx = (x % 2 === 0) ? (byte >> 4) : (byte & 0x0F);
          if (palette && idx < paletteCount) {
            b = palette[idx * 4];
            g = palette[idx * 4 + 1];
            r = palette[idx * 4 + 2];
          }
        }
        if (hasAnd) {
          const andByte = src[andRowOff + Math.floor(x / 8)];
          if ((andByte >> (7 - (x % 8))) & 1) {
            a = 0;
          }
        }
        out[o] = r;
        out[o + 1] = g;
        out[o + 2] = b;
        out[o + 3] = a;
      }
    }
    if (bpp === 32 && opaqueAlphaHint && !hasAnd) {
      for (let i = 3; i < out.length; i += 4) {
        out[i] = 255;
      }
    }
    return { pixels: out.buffer as ArrayBuffer, width: width, height: height };
  }

  private async writePng(rgba: ArrayBuffer, width: number, height: number, outPath: string): Promise<string> {
    const opts: image.InitializationOptions = {
      size: { width: width, height: height },
      pixelFormat: image.PixelMapFormat.RGBA_8888,
      editable: false
    };
    const pixelMap = await image.createPixelMap(rgba, opts);
    this.ensureCacheDir();
    const out = fs.openSync(outPath, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
    try {
      const packer = image.createImagePacker();
      const packOpts: image.PackingOption = { format: 'image/png', quality: 100 };
      await packer.packToFile(pixelMap, out.fd, packOpts);
    } finally {
      fs.closeSync(out);
    }
    return outPath;
  }
}
