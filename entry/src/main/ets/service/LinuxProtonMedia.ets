import { common } from '@kit.AbilityKit';
import { fileIo as fs, statfs } from '@kit.CoreFileKit';
import { ConfigurationFiles } from './ConfigurationFiles';
import { readSmokeText } from './ManagedSmokePayloadService';

interface MediaFile { path: string; bytes: number; sha256: string; }
interface MediaBackend { path: string; bytes: number; sha256: string; }
interface MediaManifest { schemaVersion: number; architecture: string; id: string; installedBytes: number; files: MediaFile[]; backend: MediaBackend; }
interface MediaStamp { path: string; bytes: number; mtime: number; }
interface MediaReceipt { id: string; stamps: MediaStamp[]; }
const ROOT = '/data/storage/el2/base/files/steamohos-native/proton-probe';
const RAW = 'linux-proton/media';
const pause = (): Promise<void> => new Promise<void>((resolve: () => void): void => { setTimeout(resolve, 0); });

export function validateLinuxProtonMedia(manifest: MediaManifest): void {
  if (manifest.schemaVersion !== 1 || manifest.architecture !== 'aarch64-linux-gnu' ||
      !/^[a-f0-9]{64}$/.test(manifest.id) || !Array.isArray(manifest.files) || manifest.files.length < 6 ||
      manifest.files.length > 128 || manifest.backend.path !== 'files/lib/wine/aarch64-unix/winedmo.so' ||
      !/^[a-f0-9]{64}$/.test(manifest.backend.sha256)) throw new Error('Proton 媒体支持清单无效');
  const seen = new Set<string>(); let bytes = 0;
  for (const file of manifest.files) {
    if (!/^(files\/lib\/(aarch64-linux-gnu|wine\/aarch64-windows)\/[a-zA-Z0-9_.+-]+|licenses\/[a-zA-Z0-9_.+-]+|prefix-modules\.list|runtime-modules\.list)$/.test(file.path) ||
        seen.has(file.path) || !Number.isSafeInteger(file.bytes) || file.bytes < 1 || file.bytes > 67108864 ||
        !/^[a-f0-9]{64}$/.test(file.sha256)) throw new Error('Proton 媒体支持文件无效');
    seen.add(file.path); bytes += file.bytes;
  }
  if (bytes !== manifest.installedBytes || bytes > 134217728 || !seen.has('prefix-modules.list') ||
      !seen.has('runtime-modules.list')) throw new Error('Proton 媒体支持不完整');
}

// This layer owns media/ and its manifest-named GNU codec libraries in ROOT. It never replaces a selected upstream package,
// Wine registries, the Steam library, or user saves. A failed partial copy is
// repaired before Wine starts; the receipt is committed after all files pass.
export async function ensureLinuxProtonMedia(context: common.Context): Promise<void> {
  const manifest = JSON.parse(ConfigurationFiles.text(await context.resourceManager.getRawFileContent(`${RAW}/manifest.json`))) as MediaManifest;
  validateLinuxProtonMedia(manifest);
  const backend = `${ROOT}/package/${manifest.backend.path}`;
  const backendStat = fs.lstatSync(backend);
  if (!backendStat.isFile() || backendStat.isSymbolicLink() || backendStat.size !== manifest.backend.bytes ||
      await ConfigurationFiles.fileHash(backend) !== manifest.backend.sha256) throw new Error('当前 Proton 与媒体适配层不匹配，请更新媒体支持');
  ConfigurationFiles.ensure(`${ROOT}/media`);
  const receiptPath = `${ROOT}/media-installed.json`;
  let receipt: MediaReceipt | undefined = undefined;
  try { receipt = JSON.parse(readSmokeText(receiptPath)) as MediaReceipt; } catch (_) {}
  const previous = new Map<string, MediaStamp>(); const stamps: MediaStamp[] = [];
  if (receipt && receipt.id === manifest.id) receipt.stamps.forEach((stamp: MediaStamp): void => { previous.set(stamp.path, stamp); });
  for (const file of manifest.files) {
    // Child Wine loaders already search ROOT; install new codec SONAMEs there
    // once instead of copying them again or rebuilding the immutable shim ZIP.
    const native = file.path.startsWith('files/lib/aarch64-linux-gnu/');
    const path = native ? `${ROOT}/${file.path.split('/').pop()}` : `${ROOT}/media/${file.path}`;
    const parts = native ? [] as string[] : file.path.split('/'); parts.pop(); let parent = `${ROOT}/media`;
    for (const part of parts) { parent += '/' + part; ConfigurationFiles.ensure(parent); }
    let matches = false;
    if (fs.accessSync(path)) {
      const stat = fs.lstatSync(path); const stamp = previous.get(file.path);
      if (!stat.isFile() || stat.isSymbolicLink()) throw new Error('Proton 媒体支持路径不安全');
      matches = stat.size === file.bytes && ((stamp !== undefined && stamp.bytes === stat.size && stamp.mtime === stat.mtime) ||
        await ConfigurationFiles.fileHash(path) === file.sha256);
    }
    if (!matches) {
      if (await statfs.getFreeSize(ROOT) < file.bytes + 33554432) throw new Error('空间不足，无法安装 Proton 媒体支持');
      const pending = path + '.pending';
      if (fs.accessSync(pending) && fs.lstatSync(pending).isSymbolicLink()) throw new Error('Proton 媒体支持临时路径不安全');
      const input = context.resourceManager.getRawFdSync(`${RAW}/${file.path}`);
      try {
        if (input.length !== file.bytes) throw new Error('Proton 媒体支持大小错误');
        const output = fs.openSync(pending, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC | fs.OpenMode.NOFOLLOW);
        try {
          const buffer = new ArrayBuffer(1048576); let copied = 0;
          while (copied < input.length) {
            const n = fs.readSync(input.fd, buffer, { offset: input.offset + copied, length: Math.min(buffer.byteLength, input.length - copied) });
            if (n <= 0) throw new Error('无法读取 Proton 媒体支持');
            let sent = 0;
            while (sent < n) { const count = fs.writeSync(output.fd, buffer.slice(sent, n)); if (count <= 0) throw new Error('无法写入 Proton 媒体支持'); sent += count; }
            copied += n; await pause();
          }
          fs.fsyncSync(output.fd);
        } finally { fs.closeSync(output); }
        if (await ConfigurationFiles.fileHash(pending) !== file.sha256) throw new Error('Proton 媒体支持校验失败');
        fs.renameSync(pending, path);
      } finally {
        context.resourceManager.closeRawFdSync(`${RAW}/${file.path}`);
        if (fs.accessSync(pending)) fs.unlinkSync(pending);
      }
    }
    const stat = fs.lstatSync(path); stamps.push({ path: file.path, bytes: stat.size, mtime: stat.mtime });
    await pause();
  }
  ConfigurationFiles.json(receiptPath, JSON.stringify({ id: manifest.id, stamps }));
}
