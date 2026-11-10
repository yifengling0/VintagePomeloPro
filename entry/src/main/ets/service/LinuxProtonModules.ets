import { common } from '@kit.AbilityKit';
import { fileIo as fs, statfs } from '@kit.CoreFileKit';
import { zlib } from '@kit.BasicServicesKit';
import { ConfigurationFiles } from './ConfigurationFiles';

interface FileError { code: number; }
interface ModuleFile extends ModuleArchive { path: string; original: string; source: string; builtin?: boolean; }
interface ModuleBackend extends ModuleArchive { path: string; }
interface DirectoryReceipt { files: number; bytes: number; membersSha256: string; }
interface ModuleClosure { schema: number; directories: Record<string, DirectoryReceipt>; assemblies: DirectoryReceipt; }
interface ModuleArchive { bytes: number; sha256: string; }
interface ModuleManifest {
  schemaVersion: number; architecture: string; id: string; backend: ModuleBackend;
  files: ModuleFile[]; installedBytes: number; archiveInstalledBytes: number; builtinInstalledBytes: number;
  archive: ModuleArchive; closure: ModuleClosure;
}
const ROOT = '/data/storage/el2/base/files/steamohos-native/proton-probe';
const WINDOWS = `${ROOT}/prefix/drive_c/windows`;
const SCRATCH = `${ROOT}/module-install`;
const RAW = 'linux-proton/modules-support';
const COMMON_CONTROLS = 'microsoft.windows.common-controls_6595b64144ccf1df_6.0.2600.2982_none_deadbeef';
function assemblyPath(path: string): boolean {
  for (const arch of ['arm64', 'x86']) {
    const name = `${arch}_${COMMON_CONTROLS}`;
    if (path === `winsxs/${name}/comctl32.dll` || path === `winsxs/manifests/${name}.manifest`) return true;
  }
  return false;
}
const pause = (): Promise<void> => new Promise<void>((resolve: () => void): void => { setTimeout(resolve, 0); });

export function validateLinuxProtonModules(manifest: ModuleManifest): void {
  if (manifest.schemaVersion !== 1 || manifest.architecture !== 'aarch64-linux-gnu' ||
      !/^[a-f0-9]{64}$/.test(manifest.id) || !Array.isArray(manifest.files) ||
      manifest.files.length < 1 || manifest.files.length > 2048 || manifest.closure.schema !== 1 ||
      manifest.backend.path !== 'files/lib/wine/aarch64-unix/winevulkan.so' ||
      !/^[a-f0-9]{64}$/.test(manifest.backend.sha256) || !Number.isSafeInteger(manifest.backend.bytes) ||
      manifest.backend.bytes < 1 || manifest.backend.bytes > 134217728 ||
      !Number.isSafeInteger(manifest.archive.bytes) || manifest.archive.bytes < 1 || manifest.archive.bytes > 536870912 ||
      !/^[a-f0-9]{64}$/.test(manifest.archive.sha256)) throw new Error('Proton 模块清单无效');
  const seen = new Set<string>(); let total = 0; let added = 0; let builtins = 0;
  const counts: Record<string, number> = {}; const sizes: Record<string, number> = {};
  let assemblyCount = 0; let assemblyBytes = 0;
  for (const file of manifest.files) {
    const assembly = assemblyPath(file.path);
    if ((!/^(system32|syswow64)\/(drivers\/)?[a-z0-9_.-]+$/.test(file.path) && !assembly) || seen.has(file.path) ||
        !Number.isSafeInteger(file.bytes) || file.bytes < 1 || file.bytes > 134217728 ||
        !/^[a-f0-9]{64}$/.test(file.sha256)) throw new Error('Proton 模块文件无效');
    if (assembly) {
      if (file.original !== `files/share/default_pfx_arm64/drive_c/windows/${file.path}` ||
          file.source !== 'archive' || file.builtin !== undefined) throw new Error('Proton 程序集来源无效');
      seen.add(file.path); total += file.bytes; added += file.bytes;
      assemblyCount++; assemblyBytes += file.bytes;
      continue;
    }
    const name = file.path.split('/').pop() as string;
    const arch = file.path.startsWith('system32/') ? 'aarch64-windows' : 'i386-windows';
    const regular = `files/lib/wine/${arch}/${name}`;
    const icu = `files/lib/wine/icu/${arch}/${name}`;
    const isIcu = file.original === icu;
    const driver = name.endsWith('.sys') && name !== 'win32k.sys';
    if ((file.original !== regular && !isIcu) || file.path.includes('/drivers/') !== driver ||
        (isIcu && (driver || file.source !== 'archive')) ||
        !['archive', `package/${regular}`, `media/${regular}`].includes(file.source)) throw new Error('Proton 模块来源无效');
    if (file.builtin !== undefined && (file.builtin !== true || isIcu || arch !== 'aarch64-windows' || file.source.startsWith('package/')))
      throw new Error('Proton Unix 配套模块无效');
    seen.add(file.path); total += file.bytes;
    if (file.source === 'archive') added += file.bytes;
    if (file.builtin) builtins += file.bytes;
    const directory = `files/lib/wine/${isIcu ? 'icu/' : ''}${arch}`;
    counts[directory] = (counts[directory] ?? 0) + 1;
    sizes[directory] = (sizes[directory] ?? 0) + file.bytes;
  }
  if (!manifest.closure.assemblies || assemblyCount !== 4 ||
      assemblyCount !== manifest.closure.assemblies.files || assemblyBytes !== manifest.closure.assemblies.bytes ||
      total !== manifest.installedBytes || added !== manifest.archiveInstalledBytes || builtins !== manifest.builtinInstalledBytes || total > 1610612736)
    throw new Error('Proton 模块目录不完整');
  for (const directory of ['files/lib/wine/aarch64-windows', 'files/lib/wine/i386-windows',
    'files/lib/wine/icu/aarch64-windows', 'files/lib/wine/icu/i386-windows']) {
    const receipt = manifest.closure.directories[directory];
    if (!receipt || counts[directory] !== receipt.files || sizes[directory] !== receipt.bytes)
      throw new Error('Proton 模块目录不完整');
  }
}
function exists(path: string): boolean {
  try { fs.lstatSync(path); return true; } catch (error) {
    if ((error as FileError).code === 13900002) return false;
    throw new Error(`无法检查 Proton 模块：${(error as Error).message}`);
  }
}
function parents(base: string, relative: string): void {
  const parts = relative.split('/'); parts.pop(); let path = base;
  ConfigurationFiles.ensure(path);
  for (const part of parts) { path += '/' + part; ConfigurationFiles.ensure(path); }
}
function removeScratch(path: string): void {
  if (path !== SCRATCH && !path.startsWith(SCRATCH + '/')) throw new Error('Proton 模块清理路径无效');
  if (!exists(path)) return;
  const info = fs.lstatSync(path);
  if (info.isSymbolicLink()) throw new Error('Proton 模块临时路径不安全');
  if (info.isDirectory()) {
    for (const name of fs.listFileSync(path)) {
      if (!name || name === '.' || name === '..' || name.includes('/') || name.includes('\\')) throw new Error('Proton 模块临时路径无效');
      removeScratch(`${path}/${name}`);
    }
    fs.rmdirSync(path);
  } else { fs.unlinkSync(path); }
}
async function copyFd(fd: number, offset: number, bytes: number, destination: string): Promise<void> {
  const output = fs.openSync(destination, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC | fs.OpenMode.NOFOLLOW);
  try {
    const buffer = new ArrayBuffer(1048576); let copied = 0;
    while (copied < bytes) {
      const n = fs.readSync(fd, buffer, { offset: offset + copied, length: Math.min(buffer.byteLength, bytes - copied) });
      if (n <= 0) throw new Error('无法读取 Proton 模块');
      let sent = 0;
      while (sent < n) { const count = fs.writeSync(output.fd, buffer.slice(sent, n)); if (count <= 0) throw new Error('无法写入 Proton 模块'); sent += count; }
      copied += n; await pause();
    }
    fs.fsyncSync(output.fd);
  } finally { fs.closeSync(output); }
}
async function matches(path: string, file: ModuleArchive): Promise<boolean> {
  const stat = fs.lstatSync(path);
  return stat.isFile() && !stat.isSymbolicLink() && stat.size === file.bytes && await ConfigurationFiles.fileHash(path) === file.sha256;
}
async function verifiedCopy(source: string, pending: string, file: ModuleArchive): Promise<void> {
  const input = fs.openSync(source, fs.OpenMode.READ_ONLY | fs.OpenMode.NOFOLLOW);
  try {
    const info = fs.statSync(input.fd);
    if (!info.isFile() || info.size !== file.bytes) throw new Error('Proton 原始模块无效');
    await copyFd(input.fd, 0, file.bytes, pending);
  } finally { fs.closeSync(input); }
  if (!await matches(pending, file)) throw new Error('Proton 原始模块校验失败');
}
async function inspectArchive(directory: string, wanted: Map<string, ModuleFile>, seen: Set<string>, relative: string = ''): Promise<void> {
  for (const name of fs.listFileSync(directory)) {
    if (!name || name === '.' || name === '..' || name.includes('/') || name.includes('\\')) throw new Error('Proton 模块压缩包路径无效');
    const child = relative ? relative + '/' + name : name;
    const path = `${directory}/${name}`; const info = fs.lstatSync(path);
    if (info.isSymbolicLink()) throw new Error('Proton 模块压缩包不允许链接');
    if (info.isDirectory()) {
      let expectedDirectory = false;
      for (const file of wanted.values()) { if (file.path.startsWith(child + '/')) expectedDirectory = true; }
      if (!expectedDirectory) throw new Error('Proton 模块压缩包目录无效');
      await inspectArchive(path, wanted, seen, child);
    } else {
      const file = wanted.get(child);
      if (!file || !await matches(path, file)) throw new Error('Proton 模块校验失败');
      seen.add(child);
    }
    await pause();
  }
}

// One shared GNU prefix: add missing ORIGINAL modules only. Immutable upstream
// ZIPs and user DLLs are never replaced. Newly extracted modules move directly
// into the prefix, so the 64-bit closure has no duplicate installed copy.
// A failed install leaves only complete, hash-verified files. Retrying fills
// remaining holes before any Wine process is allowed to start.
export async function ensureLinuxProtonModules(context: common.Context, progress: (message: string) => void): Promise<void> {
  const manifest = JSON.parse(ConfigurationFiles.text(await context.resourceManager.getRawFileContent(`${RAW}/manifest.json`))) as ModuleManifest;
  validateLinuxProtonModules(manifest);
  const backend = `${ROOT}/package/${manifest.backend.path}`;
  if (!await matches(backend, manifest.backend)) throw new Error('当前 Proton 与完整模块适配层不匹配，请更新 Proton');
  // Check every ancestor through the same no-symlink directory guard used by
  // the component installer. Unknown user leaf links, including dangling ones,
  // are preserved instead of being treated as missing files.
  let directory = ROOT;
  for (const part of ['prefix', 'drive_c', 'windows']) { directory += '/' + part; ConfigurationFiles.ensure(directory); }
  const missing: ModuleFile[] = [];
  const prepared = new Set<string>(); let inspected = 0;
  for (const file of manifest.files) {
    const parent = file.path.slice(0, file.path.lastIndexOf('/'));
    if (!prepared.has(parent)) { parents(WINDOWS, file.path); prepared.add(parent); }
    let builtinMissing = false;
    if (file.builtin) {
      parents(`${ROOT}/package`, file.original);
      const original = `${ROOT}/package/${file.original}`;
      builtinMissing = !exists(original);
      if (!builtinMissing && !await matches(original, file)) throw new Error('Proton Unix 配套原始模块已改变，请修复组件');
    }
    if (builtinMissing || !exists(`${WINDOWS}/${file.path}`)) missing.push(file);
    if (++inspected % 64 === 0) await pause();
  }
  if (!missing.length) { removeScratch(SCRATCH); return; }
  const archiveNeeded = missing.some((file: ModuleFile): boolean => file.source === 'archive');
  const copiedBytes = missing.filter((file: ModuleFile): boolean => file.source !== 'archive').reduce((n: number, file: ModuleFile): number => n + file.bytes, 0);
  removeScratch(SCRATCH); ConfigurationFiles.ensure(SCRATCH);
  try {
    const builtinBytes = missing.filter((file: ModuleFile): boolean => file.builtin === true).reduce((n: number, file: ModuleFile): number => n + file.bytes, 0);
    const required = copiedBytes + builtinBytes + (archiveNeeded ? manifest.archive.bytes + manifest.archiveInstalledBytes : 0) + 134217728;
    if (await statfs.getFreeSize(ROOT) < required) throw new Error('空间不足，无法补齐 Proton 模块');
    if (archiveNeeded) {
      progress('正在补齐 Proton 32／64 位模块');
      const fd = context.resourceManager.getRawFdSync(`${RAW}/modules.zip`);
      try {
        if (fd.length !== manifest.archive.bytes) throw new Error('Proton 模块包大小错误');
        await copyFd(fd.fd, fd.offset, fd.length, `${SCRATCH}/modules.zip`);
      } finally { context.resourceManager.closeRawFdSync(`${RAW}/modules.zip`); }
      if (!await matches(`${SCRATCH}/modules.zip`, manifest.archive)) throw new Error('Proton 模块包校验失败');
      ConfigurationFiles.ensure(`${SCRATCH}/new`);
      await zlib.decompressFile(`${SCRATCH}/modules.zip`, `${SCRATCH}/new`, {});
      const wanted = new Map<string, ModuleFile>(); const seen = new Set<string>();
      for (const file of manifest.files) if (file.source === 'archive') wanted.set(file.path, file);
      await inspectArchive(`${SCRATCH}/new`, wanted, seen);
      if (seen.size !== wanted.size) throw new Error('Proton 模块包不完整');
    }
    let installed = 0;
    for (const file of missing) {
      const destination = `${WINDOWS}/${file.path}`;
      parents(WINDOWS, file.path);
      let pending = `${SCRATCH}/new/${file.path}`;
      if (file.source !== 'archive') {
        const source = `${ROOT}/${file.source}`;
        // Pin a nofollow FD before copying; hash that copied data before
        // publication. Failed output opens still close the input descriptor.
        pending = `${SCRATCH}/module.pending`;
        await verifiedCopy(source, pending, file);
      }
      if (file.builtin) {
        const original = `${ROOT}/package/${file.original}`;
        if (!exists(original)) {
          // Never derive a builtin from an existing user-overridden prefix DLL.
          const copy = `${SCRATCH}/builtin.pending`;
          await verifiedCopy(pending, copy, file);
          if (!exists(original)) fs.renameSync(copy, original);
        }
      }
      // Preparation is serialized by LinuxGnuProtonComponent; no Wine session
      // is live here. Recheck immediately before publishing, preserving edits.
      if (!exists(destination)) { fs.renameSync(pending, destination); installed++; }
      if (installed % 64 === 0) progress(`正在补齐 Proton 模块 ${installed}／${missing.length}`);
      await pause();
    }
    ConfigurationFiles.json(`${ROOT}/modules-installed.json`, JSON.stringify({ id: manifest.id, added: installed, preserved: manifest.files.length - installed }));
  } finally { removeScratch(SCRATCH); }
}
