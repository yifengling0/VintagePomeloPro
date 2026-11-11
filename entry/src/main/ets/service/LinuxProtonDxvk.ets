import { withLinuxProtonMaintenance } from './LinuxProtonLease';
import { common } from '@kit.AbilityKit';
import { fileIo as fs, statfs } from '@kit.CoreFileKit';
import { ConfigurationFiles } from './ConfigurationFiles';
import { readSmokeText } from './ManagedSmokePayloadService';

interface SupportFile { path: string; bytes: number; sha256: string; }
interface SupportBackend { path: string; bytes: number; sha256: string; }
interface SupportNetwork { wineRevision: string; unixCallSlots: number; backend: SupportBackend; ntdll: SupportBackend; }
interface SupportManifest { network: SupportNetwork; schemaVersion: number; architecture: string; id: string; installedBytes: number; files: SupportFile[]; backend: SupportBackend; }
interface PrefixChange { path: string; before: string; after: string; }
interface PrefixJournal { committed: boolean; previousReceipt: string; changes: PrefixChange[]; }
interface PrefixReceipt { files: SupportFile[]; }
interface FileError { code: number; }
const ROOT = '/data/storage/el2/base/files/steamohos-native/proton-probe';
const SUPPORT = `${ROOT}/dxvk-support`;
const STATE = `${ROOT}/dxvk-prefix-state`;
const RAW = 'linux-proton/dxvk-support';
const TARGETS = ['system32/d3d11.dll', 'system32/dxgi.dll', 'syswow64/d3d11.dll', 'syswow64/dxgi.dll'];
const pause = (): Promise<void> => new Promise<void>((resolve: () => void): void => { setTimeout(resolve, 0); });

export function validateLinuxProtonDxvk(manifest: SupportManifest): void {
  const required = new Set<string>(['libproton_probe.so', 'libproton_vulkan.so', 'libvulkan_virtio.so', 'nsiproxy.so', 'LICENSE.dxvk',
    'dlls/arm64x/d3d11.dll', 'dlls/arm64x/dxgi.dll', 'dlls/i386/d3d11.dll', 'dlls/i386/dxgi.dll',
    'pe/vulkan-1.dll', 'pe/winevulkan.dll', 'pe/d3dcompiler_47.dll']);
  if (manifest.schemaVersion !== 1 || manifest.architecture !== 'aarch64-linux-gnu' ||
      !/^[a-f0-9]{64}$/.test(manifest.id) || !Array.isArray(manifest.files) || manifest.files.length !== required.size ||
      manifest.backend.path !== 'files/lib/wine/aarch64-unix/winevulkan.so' ||
      !/^[a-f0-9]{64}$/.test(manifest.backend.sha256)) throw new Error('GNU DXVK 支持清单无效');
  if (!manifest.network || !/^[a-f0-9]{40}$/.test(manifest.network.wineRevision) || manifest.network.unixCallSlots !== 7 ||
      manifest.network.backend?.path !== 'files/lib/wine/aarch64-unix/nsiproxy.so' ||
      manifest.network.ntdll?.path !== 'files/lib/wine/aarch64-unix/ntdll.so') throw new Error('GNU NSI 接口清单无效');
  for (const backend of [manifest.backend, manifest.network.backend, manifest.network.ntdll]) {
    if (!Number.isSafeInteger(backend.bytes) || backend.bytes < 1 || backend.bytes > 33554432 ||
        !/^[a-f0-9]{64}$/.test(backend.sha256)) throw new Error('GNU NSI 接口版本无效');
  }
  let bytes = 0;
  for (const file of manifest.files) {
    if (!required.delete(file.path) || !Number.isSafeInteger(file.bytes) || file.bytes < 1 || file.bytes > 33554432 ||
        !/^[a-f0-9]{64}$/.test(file.sha256)) throw new Error('GNU DXVK 支持文件无效');
    bytes += file.bytes;
  }
  if (bytes !== manifest.installedBytes || bytes > 67108864) throw new Error('GNU DXVK 支持大小无效');
}
function regular(path: string): boolean {
  // access() follows links and treats a dangling link as absent. Only ENOENT
  // from lstat means absent; never replace an existing link or hide I/O errors.
  let stat: fs.Stat;
  try { stat = fs.lstatSync(path); } catch (error) {
    if ((error as FileError).code === 13900002) return false;
    throw new Error(`GNU DXVK 无法检查文件：${(error as Error).message}`);
  }
  if (!stat.isFile() || stat.isSymbolicLink()) throw new Error('GNU DXVK 文件路径不安全');
  return true;
}
function syncDirectory(path: string): void {
  const dir = fs.openSync(path, fs.OpenMode.READ_ONLY | fs.OpenMode.DIR | fs.OpenMode.NOFOLLOW);
  try { fs.fsyncSync(dir.fd); } finally { fs.closeSync(dir); }
}
function parentOf(path: string): string { return path.slice(0, path.lastIndexOf('/')); }
function durableJson(path: string, text: string): void {
  regular(path); ConfigurationFiles.json(path, text); syncDirectory(parentOf(path));
}
function parents(root: string, relative: string): void {
  ConfigurationFiles.ensure(root);
  const parts = relative.split('/'); parts.pop(); let path = root;
  for (const part of parts) { path += '/' + part; ConfigurationFiles.ensure(path); }
}
async function hashIfPresent(path: string): Promise<string> {
  return regular(path) ? ConfigurationFiles.fileHash(path) : '';
}
function durableCopy(source: string, target: string): void {
  regular(source); regular(target); regular(target + '.pending');
  const input = fs.openSync(source, fs.OpenMode.READ_ONLY | fs.OpenMode.NOFOLLOW);
  try {
    const output = fs.openSync(target + '.pending', fs.OpenMode.WRITE_ONLY | fs.OpenMode.CREATE | fs.OpenMode.TRUNC | fs.OpenMode.NOFOLLOW);
    try {
      const buffer = new ArrayBuffer(1048576); let n = 0;
      while ((n = fs.readSync(input.fd, buffer)) > 0) {
        let sent = 0;
        while (sent < n) { const count = fs.writeSync(output.fd, buffer.slice(sent, n)); if (count <= 0) throw new Error('GNU DXVK 写入失败'); sent += count; }
      }
      fs.fsyncSync(output.fd);
    } finally { fs.closeSync(output); }
    fs.renameSync(target + '.pending', target); syncDirectory(parentOf(target));
  } finally {
    fs.closeSync(input);
    if (regular(target + '.pending')) fs.unlinkSync(target + '.pending');
  }
}

// Installed before any Wine process, under the existing component/session gate.
// All bytes live outside the selected GE archive and user game libraries.
export async function ensureLinuxProtonDxvk(context: common.Context): Promise<void> {
  const manifest = JSON.parse(ConfigurationFiles.text(await context.resourceManager.getRawFileContent(`${RAW}/manifest.json`))) as SupportManifest;
  validateLinuxProtonDxvk(manifest);
  for (const paired of [manifest.backend, manifest.network.backend, manifest.network.ntdll]) {
    const backend = `${ROOT}/package/${paired.path}`;
    if (!regular(backend) || fs.lstatSync(backend).size !== paired.bytes ||
        await ConfigurationFiles.fileHash(backend) !== paired.sha256) throw new Error('GNU Proton 支持模块接口版本不匹配');
  }
  ConfigurationFiles.ensure(SUPPORT);
  for (const file of manifest.files) {
    const target = `${SUPPORT}/${file.path}`; parents(SUPPORT, file.path);
    if (await hashIfPresent(target) === file.sha256) continue;
    if (await statfs.getFreeSize(ROOT) < file.bytes + 33554432) throw new Error('空间不足，无法安装 GNU DXVK 支持');
    const data = await context.resourceManager.getRawFileContent(`${RAW}/${file.path}`);
    if (data.byteLength !== file.bytes || await ConfigurationFiles.hash(data) !== file.sha256) throw new Error('GNU DXVK 支持校验失败');
    regular(target + '.pending'); ConfigurationFiles.write(target + '.pending', data);
    fs.renameSync(target + '.pending', target); syncDirectory(parentOf(target)); await pause();
  }
  durableJson(`${SUPPORT}/manifest.json`, JSON.stringify(manifest));
}

async function recoverPrefix(): Promise<void> {
  const journalPath = `${STATE}/journal.json`;
  if (!regular(journalPath)) return;
  const journal = JSON.parse(readSmokeText(journalPath)) as PrefixJournal;
  if (typeof journal.committed !== 'boolean' || typeof journal.previousReceipt !== 'string' || journal.previousReceipt.length > 8192 || !Array.isArray(journal.changes) || journal.changes.length > 7) throw new Error('GNU DXVK 更新记录无效');
  const seen = new Set<string>();
  for (const change of journal.changes) {
    if (!/^(system32\/(d3d11|dxgi|vulkan-1|winevulkan|d3dcompiler_47)|syswow64\/(d3d11|dxgi))\.dll$/.test(change.path) ||
        seen.has(change.path) || !/^[a-f0-9]{64}$/.test(change.after) || (change.before !== '' && !/^[a-f0-9]{64}$/.test(change.before))) throw new Error('GNU DXVK 更新路径无效');
    seen.add(change.path);
    const target = `${ROOT}/prefix/drive_c/windows/${change.path}`;
    const current = await hashIfPresent(target);
    if (journal.committed && current !== change.after) throw new Error('GNU DXVK 已安装文件已变化，保留现状');
    if (!journal.committed && current !== change.before && current !== change.after) throw new Error('GNU DXVK 更新后文件已被修改，保留现状');
    if (!journal.committed && change.before && await hashIfPresent(`${STATE}/old/${change.path}`) !== change.before) throw new Error('GNU DXVK 回退文件不完整');
  }
  if (!journal.committed) {
    for (const change of journal.changes.slice().reverse()) {
      const target = `${ROOT}/prefix/drive_c/windows/${change.path}`;
      if (change.before) durableCopy(`${STATE}/old/${change.path}`, target);
      else if (regular(target)) { fs.unlinkSync(target); syncDirectory(parentOf(target)); }
    }
  }
  if (!journal.committed) durableJson(`${STATE}/installed.json`, journal.previousReceipt);
  fs.unlinkSync(journalPath); syncDirectory(STATE);
  for (const change of journal.changes) {
    const old = `${STATE}/old/${change.path}`; if (regular(old)) fs.unlinkSync(old);
    const pending = `${ROOT}/prefix/drive_c/windows/${change.path}.pending`; if (regular(pending)) fs.unlinkSync(pending);
  }
}
function wineBuiltin(path: string): boolean {
  // Wine's generated builtin placeholder signature at sizeof(IMAGE_DOS_HEADER).
  // Back it up as well; an arbitrary native user DLL is never replaced.
  const fd = fs.openSync(path, fs.OpenMode.READ_ONLY | fs.OpenMode.NOFOLLOW);
  const bytes = new ArrayBuffer(96); let count = 0;
  try { count = fs.readSync(fd.fd, bytes); } finally { fs.closeSync(fd); }
  const b = new Uint8Array(bytes); const signature = 'Wine builtin DLL';
  if (count < 96 || b[0] !== 77 || b[1] !== 90) return false;
  for (let i = 0; i < signature.length; i++) if (b[64 + i] !== signature.charCodeAt(i)) return false;
  return b[64 + signature.length] === 0;
}

// The broker serializes GNU sessions. Prepare both PE widths before the first
// Wine/helper process; children then share a consistent group. No WINEPATH
// guessing based on the parent's bitness, and no changes to the upstream GE.
export async function prepareLinuxProtonDxvkPrefix(maintenanceToken: number = 0): Promise<void> {
  if (maintenanceToken > 0) await preparePrefixUnderLease();
  else await withLinuxProtonMaintenance(preparePrefixUnderLease);
}
async function preparePrefixUnderLease(): Promise<void> {
  // Verify intermediate prefix directories too, before trusting a recovered
  // path. A user link in drive_c must not redirect system DLL installation.
  parents(ROOT, 'prefix/drive_c/windows/placeholder');
  ConfigurationFiles.ensure(STATE); await recoverPrefix();
  regular(`${STATE}/installed.json`);
  const manifest = JSON.parse(readSmokeText(`${SUPPORT}/manifest.json`)) as SupportManifest;
  validateLinuxProtonDxvk(manifest);
  const previous = new Map<string, string>();
  try {
    const receipt = JSON.parse(readSmokeText(`${STATE}/installed.json`)) as PrefixReceipt;
    for (const file of receipt.files) previous.set(file.path, file.sha256);
  } catch (_) {}
  const planned: SupportFile[] = [];
  for (const file of manifest.files) {
    let target = '';
    if (file.path.startsWith('dlls/')) target = (file.path.includes('/arm64x/') ? 'system32/' : 'syswow64/') + file.path.split('/').pop();
    else if (file.path.startsWith('pe/')) target = 'system32/' + file.path.split('/').pop();
    if (target) planned.push({ path: target, bytes: file.bytes, sha256: file.sha256 });
  }
  const journal: PrefixJournal = { committed: false, previousReceipt: readSmokeText(`${STATE}/installed.json`), changes: [] };
  let requiredBytes = 33554432;
  // Validate the entire group and all backup bytes BEFORE modifying any DLL.
  for (const file of planned) {
    parents(`${ROOT}/prefix/drive_c/windows`, file.path);
    const target = `${ROOT}/prefix/drive_c/windows/${file.path}`;
    const current = await hashIfPresent(target);
    if (current === file.sha256) continue;
    if (current && current !== previous.get(file.path) && !wineBuiltin(target)) throw new Error(`GNU DXVK 保留用户已有 DLL：${file.path}`);
    requiredBytes += file.bytes + (current ? fs.lstatSync(target).size : 0);
    journal.changes.push({ path: file.path, before: current, after: file.sha256 });
  }
  if (journal.changes.length === 0) return;
  if (await statfs.getFreeSize(ROOT) < requiredBytes) throw new Error('空间不足，无法更新 GNU DXVK 前缀');
  for (const change of journal.changes) {
    parents(`${STATE}/old`, change.path);
    if (change.before) {
      durableCopy(`${ROOT}/prefix/drive_c/windows/${change.path}`, `${STATE}/old/${change.path}`);
      if (await hashIfPresent(`${STATE}/old/${change.path}`) !== change.before) throw new Error('GNU DXVK 回退文件校验失败');
    }
  }
  durableJson(`${STATE}/journal.json`, JSON.stringify(journal));
  try {
    for (const change of journal.changes) {
      const sourceName = change.path.startsWith('syswow64/') ? 'dlls/i386/' + change.path.split('/').pop() :
        TARGETS.includes(change.path) ? 'dlls/arm64x/' + change.path.split('/').pop() : 'pe/' + change.path.split('/').pop();
      if (await hashIfPresent(`${SUPPORT}/${sourceName}`) !== change.after) throw new Error('GNU DXVK 安装源已变化');
      durableCopy(`${SUPPORT}/${sourceName}`, `${ROOT}/prefix/drive_c/windows/${change.path}`);
      if (await hashIfPresent(`${ROOT}/prefix/drive_c/windows/${change.path}`) !== change.after) throw new Error('GNU DXVK 安装校验失败');
    }
    durableJson(`${STATE}/installed.json`, JSON.stringify({ files: planned }));
    journal.committed = true; durableJson(`${STATE}/journal.json`, JSON.stringify(journal));
  } catch (error) { await recoverPrefix(); throw new Error((error as Error).message); }
  await recoverPrefix();
}
