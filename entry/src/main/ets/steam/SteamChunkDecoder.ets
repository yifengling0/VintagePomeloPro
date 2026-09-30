import native from 'libsteamdecompress.so';
import { hilog } from '../service/LogService';

const DOMAIN = 0x0000;
const TAG = 'SteamChunkDecoder';

/**
 * Depot chunk 解码 (W6): 识别魔数 → 剥壳 → 调 libsteamdecompress.so。
 * 魔数 (SteamKit DepotChunk): 'VSZa'=zstd, 'VZa'=VZip(LZMA), 裸 LZMA (VZip 旧变体无壳) 兜底。
 */
export class SteamChunkDecoder {
  /** 解压一个 chunk; 输出尺寸来自 manifest 的 uncompressedSize。 */
  static decode(payload: Uint8Array, uncompressedSize: number): Uint8Array {
    if (!Number.isSafeInteger(uncompressedSize) || uncompressedSize <= 0 || uncompressedSize > 128 * 1024 * 1024) {
      throw new Error('Steam chunk has an invalid original size');
    }
    if (payload.length >= 4 && payload[0] === 0x56 && payload[1] === 0x53
      && payload[2] === 0x5A && payload[3] === 0x61) {
      if (payload.length <= 23 || payload[payload.length - 3] !== 0x7A ||
        payload[payload.length - 2] !== 0x73 || payload[payload.length - 1] !== 0x76) {
        throw new Error('Steam zstd chunk is truncated');
      }
      // VSZa + CRC32 (8 bytes), zstd frame, 15-byte footer.
      const body = payload.subarray(8, payload.length - 15);
      return SteamChunkDecoder.runNative(body, uncompressedSize, 'zstd');
    }
    if (payload.length >= 3 && payload[0] === 0x56 && payload[1] === 0x5A
      && payload[2] === 0x61) {
      if (payload.length <= 22 || payload[payload.length - 2] !== 0x7A ||
        payload[payload.length - 1] !== 0x76) throw new Error('Steam VZip chunk is truncated');
      // VZa + 4-byte CRC/timestamp, five LZMA properties, stream, 10-byte footer.
      const body = payload.subarray(7, payload.length - 10);
      return SteamChunkDecoder.runNative(body, uncompressedSize, 'lzma');
    }
    if (payload.length >= 4 && payload[0] === 0x50 && payload[1] === 0x4B &&
      payload[2] === 0x03 && payload[3] === 0x04) {
      const bytes = new Uint8Array(native.unzipManifest(payload) as ArrayBuffer);
      if (bytes.length !== uncompressedSize) throw new Error('Steam ZIP chunk size mismatch');
      return bytes;
    }
    // 无壳: 直接是 5B props + LZMA 流
    return SteamChunkDecoder.runNative(payload, uncompressedSize, 'lzma');
  }

  private static runNative(body: Uint8Array, expectedSize: number, format: string): Uint8Array {
    const copy = new Uint8Array(body.length);
    copy.set(body);
    const result = native.decompressChunk(copy, expectedSize, format) as ArrayBuffer;
    const out = new Uint8Array(result);
    if (out.length !== expectedSize) {
      hilog.warn(DOMAIN, TAG, 'chunk size mismatch: %{public}d != %{public}d', out.length, expectedSize);
      throw new Error(`解压尺寸不符 (${out.length} != ${expectedSize})`);
    }
    return out;
  }
}
