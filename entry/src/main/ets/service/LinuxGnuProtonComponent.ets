import { LinuxProtonRuntimeService } from './LinuxProtonRuntimeService';
import { common } from '@kit.AbilityKit';
import { fileIo as fs, statfs } from '@kit.CoreFileKit';
import { zlib } from '@kit.BasicServicesKit';
import { ConfigurationFiles } from './ConfigurationFiles';
import { ensureSmokeDirectory, readSmokeText } from './ManagedSmokePayloadService';

const BASE = '/data/storage/el2/base/files/steamohos-native';
const ROOT = `${BASE}/proton-probe`;
const SCRATCH = `${BASE}/proton-component-install`;
const RECEIPT = `${ROOT}/component-installed.json`;
const JOURNAL = `${SCRATCH}/transaction.json`;
interface ComponentFile { path: string; bytes: number; sha256: string; mode: number; }
interface ComponentManifest { schemaVersion: number; id: string; bytes: number; sha256: string; installedBytes: number; files: ComponentFile[]; }
interface FileStamp { path: string; bytes: number; mtime: number; }
interface ComponentReceipt { id: string; stamps: FileStamp[]; }
interface Change { path: string; hadOld: boolean; }
interface Transaction { committed: boolean; previousReceipt: string; changes: Change[]; }
type Progress = (message: string) => void;
const pause = (): Promise<void> => new Promise<void>((resolve: () => void): void => { setTimeout(resolve, 0); });

// These are exclusively application-owned code/data. No other prefix file,
// home directory, registry, drive mapping, Steam library or account is eligible.
export function protonComponentGroup(path: string): string {
  if (!path || path.startsWith('/') || path.includes('\\') || /[\x00-\x1f]/.test(path) ||
      path.split('/').some((part: string): boolean => !part || part === '.' || part === '..')) throw new Error('Proton 组件路径无效');
  const first = path.split('/')[0];
  if (['package', 'graphics', 'locale', 'licenses'].includes(first) && path.includes('/')) return first;
  if (/^fonts\/(marlett|tahoma|tahomabd)\.ttf$/.test(path) ||
      path === 'prefix/drive_c/proton-x86-locale.exe' || /^prefix\/drive_c\/dxvk26\/(d3d11|dxgi)\.dll$/.test(path)) return path;
  if (['libproton_probe.so', 'hmosdeck', 'proton-steam-ready', 'libSDL2-2.0.so.0', 'libwineohos-gnu.so',
    'runtime-modules.list', 'prefix-modules.list', 'audio-direct', 'manual-launch.version'].includes(path)) return path;
  throw new Error('Proton 组件包含非托管文件');
}
export function validateProtonComponent(manifest: ComponentManifest): void {
  if (manifest.schemaVersion !== 1 || !/^[a-f0-9]{64}$/.test(manifest.id) || !/^[a-f0-9]{64}$/.test(manifest.sha256) ||
      manifest.bytes < 1 || manifest.bytes > 536870912 || manifest.files.length < 20 || manifest.files.length > 4096) throw new Error('Proton 组件清单无效');
  const seen = new Set<string>(); let bytes = 0;
  for (const file of manifest.files) {
    protonComponentGroup(file.path);
    if (seen.has(file.path) || !/^[a-f0-9]{64}$/.test(file.sha256) || !Number.isSafeInteger(file.bytes) ||
        file.bytes < 1 || file.bytes > 67108864 || ![384, 448].includes(file.mode)) throw new Error('Proton 组件文件清单无效');
    seen.add(file.path); bytes += file.bytes;
  }
  if (bytes !== manifest.installedBytes || bytes > 1073741824 ||
      !seen.has('package/files/lib/wine/aarch64-unix/wine') || !seen.has('hmosdeck') ||
      !seen.has('prefix/drive_c/proton-x86-locale.exe') || !seen.has('graphics/libproton_egl.so') ||
      !seen.has('graphics/libproton_vulkan.so')) throw new Error('Proton 组件不完整');
}
function safeParents(base: string, relative: string): void {
  ConfigurationFiles.ensure(base);
  const parts = relative.split('/'); parts.pop(); let directory = base;
  for (const part of parts) { directory += '/' + part; ConfigurationFiles.ensure(directory); }
}
function removeOwned(path: string): void {
  if (!path.startsWith(SCRATCH + '/') && !path.startsWith(ROOT + '/')) throw new Error('Proton 清理路径无效');
  if (!fs.accessSync(path)) return;
  const stat = fs.lstatSync(path);
  if (stat.isSymbolicLink()) throw new Error('Proton 组件不允许符号链接');
  if (!stat.isDirectory()) { fs.unlinkSync(path); return; }
  for (const name of fs.listFileSync(path)) {
    if (!name || name.includes('/') || name.includes('\\') || name === '..' || name === '.') throw new Error('Proton 清理路径无效');
    removeOwned(`${path}/${name}`);
  }
  fs.rmdirSync(path);
}
function recoverTransaction(): void {
  if (!fs.accessSync(JOURNAL)) return;
  const journal = JSON.parse(readSmokeText(JOURNAL)) as Transaction;
  if (!Array.isArray(journal.changes) || journal.changes.length > 64) throw new Error('Proton 更新记录无效');
  if (!journal.committed) {
    for (const change of journal.changes.slice().reverse()) {
      const group = protonComponentGroup(change.path.includes('/') ? change.path :
        ['package', 'graphics', 'locale', 'licenses'].includes(change.path) ? change.path + '/entry' : change.path);
      if (group !== change.path) throw new Error('Proton 更新记录路径无效');
      const backup = `${SCRATCH}/old/${group}`; const active = `${ROOT}/${group}`;
      if (fs.accessSync(backup)) { removeOwned(active); safeParents(ROOT, group); fs.renameSync(backup, active); }
      else if (!change.hadOld) removeOwned(active);
    }
    if (journal.previousReceipt) ConfigurationFiles.json(RECEIPT, journal.previousReceipt);
    else if (fs.accessSync(RECEIPT)) fs.unlinkSync(RECEIPT);
  }
  removeOwned(`${SCRATCH}/old`); fs.unlinkSync(JOURNAL);
}

// The existing explicit developer smoke suite can exercise an empty prefix.
// Keep the original prefix by rename; a process death is recovered on the
// next normal component preparation, before any game is allowed to start.
function restorePrefixProbe(): void {
  const marker = `${ROOT}/prefix-install-test.json`;
  if (readSmokeText(marker) !== 'HMOSDECK_PREFIX_INSTALL_TEST_V1') return;
  const backup = `${ROOT}/prefix-before-install-test`;
  if (fs.accessSync(backup)) {
    ConfigurationFiles.ensure(SCRATCH);
    if (fs.accessSync(`${ROOT}/prefix`)) {
      removeOwned(`${SCRATCH}/generated-prefix`);
      fs.renameSync(`${ROOT}/prefix`, `${SCRATCH}/generated-prefix`);
    }
    fs.renameSync(backup, `${ROOT}/prefix`);
    removeOwned(`${SCRATCH}/generated-prefix`);
  }
  fs.unlinkSync(marker);
}

export class LinuxGnuProtonComponent {
  private static pending: Promise<void> | null = null;
  private static verified = false;
  private static sessionActive = false;
  private static prefixProbeActive = false;
  private static listeners: Set<Progress> = new Set<Progress>();
  static ready(): boolean { return LinuxGnuProtonComponent.verified; }
  static setSessionActive(value: boolean): void {
    LinuxGnuProtonComponent.sessionActive = value; LinuxProtonRuntimeService.getInstance().setSessionActive(value);
  }
  static subscribe(listener: Progress): void { LinuxGnuProtonComponent.listeners.add(listener); }
  static unsubscribe(listener: Progress): void { LinuxGnuProtonComponent.listeners.delete(listener); }
  private static progress(message: string): void {
    ConfigurationFiles.json(`${BASE}/proton-component-status.json`, JSON.stringify({ message, at: Date.now() }));
    LinuxGnuProtonComponent.listeners.forEach((listener: Progress): void => { listener(message); });
  }
  static async ensureReady(context: common.Context): Promise<void> {
    if (LinuxGnuProtonComponent.pending) return LinuxGnuProtonComponent.pending;
    const runtimes = LinuxProtonRuntimeService.getInstance();
    if (runtimes.inUse() && LinuxGnuProtonComponent.verified) return;
    runtimes.beginAdapter();
    const operation = LinuxGnuProtonComponent.install(context); LinuxGnuProtonComponent.pending = operation;
    try { await operation; } finally { if (LinuxGnuProtonComponent.pending === operation) LinuxGnuProtonComponent.pending = null; runtimes.endAdapter(); }
  }
  static async verifyFreshPrefix(context: common.Context, probe: () => Promise<string>): Promise<string> {
    if (LinuxProtonRuntimeService.getInstance().inUse() || LinuxGnuProtonComponent.pending) throw new Error('请先退出 Steam 和游戏');
    restorePrefixProbe();
    if (!fs.accessSync(`${ROOT}/prefix`) || fs.accessSync(`${ROOT}/prefix-before-install-test`)) throw new Error('前缀诊断环境未就绪');
    ConfigurationFiles.json(`${ROOT}/prefix-install-test.json`, 'HMOSDECK_PREFIX_INSTALL_TEST_V1');
    fs.renameSync(`${ROOT}/prefix`, `${ROOT}/prefix-before-install-test`);
    LinuxGnuProtonComponent.prefixProbeActive = true;
    try {
      await LinuxGnuProtonComponent.ensureReady(context);
      return await probe();
    } finally {
      LinuxGnuProtonComponent.prefixProbeActive = false;
      restorePrefixProbe();
      await LinuxGnuProtonComponent.ensureReady(context);
    }
  }
  private static async matches(base: string, manifest: ComponentManifest, receipt?: ComponentReceipt): Promise<FileStamp[] | null> {
    const stamps: FileStamp[] = []; const previous = new Map<string, FileStamp>();
    if (receipt && receipt.id === manifest.id) receipt.stamps.forEach((stamp: FileStamp): void => { previous.set(stamp.path, stamp); });
    for (let i = 0; i < manifest.files.length; i++) {
      const file = manifest.files[i]; const path = `${base}/${file.path}`;
      try {
        safeParents(base, file.path);
        const stat = fs.lstatSync(path); const before = previous.get(file.path);
        if (!stat.isFile() || stat.isSymbolicLink() || stat.size !== file.bytes) return null;
        if (!before || before.bytes !== stat.size || before.mtime !== stat.mtime) {
          if (await ConfigurationFiles.fileHash(path) !== file.sha256) return null;
        }
        stamps.push({ path: file.path, bytes: stat.size, mtime: stat.mtime });
      } catch (_) { return null; }
      if (i % 24 === 0) { LinuxGnuProtonComponent.progress(`正在校验 GNU Proton（${i + 1}/${manifest.files.length}）`); await pause(); }
    }
    return stamps;
  }
  private static async embedded(context: common.Context, manifest: ComponentManifest): Promise<string> {
    const name = 'linux-proton/gnu-proton.zip'; const path = `${SCRATCH}/component.zip`;
    const input = context.resourceManager.getRawFdSync(name);
    const output = fs.openSync(path, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC | fs.OpenMode.NOFOLLOW);
    try {
      if (input.length !== manifest.bytes) throw new Error('内置 Proton 大小不正确，请重新安装应用');
      const buffer = new ArrayBuffer(1048576); let copied = 0;
      while (copied < input.length) {
        const n = fs.readSync(input.fd, buffer, { offset: input.offset + copied, length: Math.min(buffer.byteLength, input.length - copied) });
        if (n <= 0) throw new Error('无法读取 Proton 组件');
        let sent = 0;
        while (sent < n) { const count = fs.writeSync(output.fd, buffer.slice(sent, n)); if (count <= 0) throw new Error('无法写入 Proton 组件'); sent += count; }
        copied += n; LinuxGnuProtonComponent.progress(`正在准备 GNU Proton（${Math.floor(copied * 100 / input.length)}%）`); await pause();
      }
      fs.fsyncSync(output.fd);
    } finally { fs.closeSync(output); context.resourceManager.closeRawFdSync(name); }
    if (await ConfigurationFiles.fileHash(path) !== manifest.sha256) throw new Error('Proton 压缩包校验失败，请重新安装应用');
    return path;
  }
  private static async install(context: common.Context): Promise<void> {
    ConfigurationFiles.ensure(BASE); ConfigurationFiles.ensure(ROOT); ConfigurationFiles.ensure(SCRATCH);
    try {
      const data = await context.resourceManager.getRawFileContent('linux-proton/manifest.json');
      let manifest = JSON.parse(ConfigurationFiles.text(data)) as ComponentManifest; validateProtonComponent(manifest);
      const extractionBytes = manifest.installedBytes;
      // Never replace code or recover a partial activation under a live process.
      if (LinuxGnuProtonComponent.sessionActive) { if (LinuxGnuProtonComponent.verified) return; throw new Error('请退出 Steam 和游戏后安装 GNU Proton'); }
      if (!LinuxGnuProtonComponent.prefixProbeActive) restorePrefixProbe();
      recoverTransaction(); LinuxGnuProtonComponent.verified = false;
      const runtimes = LinuxProtonRuntimeService.getInstance();
      const selected = await runtimes.prepareSelected(context);
      if (selected) {
        // App upgrades own adapter files only. A user-selected upstream version
        // must never be replaced by the bundled fallback during preparation.
        const files = manifest.files.filter((file: ComponentFile): boolean => !file.path.startsWith('package/'));
        manifest.files = files; manifest.installedBytes = files.reduce((total: number, file: ComponentFile): number => total + file.bytes, 0);
      }
      let receipt: ComponentReceipt | undefined = undefined;
      try { receipt = JSON.parse(readSmokeText(RECEIPT)) as ComponentReceipt; } catch (_) {}
      let stamps = await LinuxGnuProtonComponent.matches(ROOT, manifest, receipt);
      if (stamps) {
        ConfigurationFiles.json(RECEIPT, JSON.stringify({ id: manifest.id, stamps }));
        await runtimes.adoptEmbedded(context, !selected);
        LinuxGnuProtonComponent.verified = true; LinuxGnuProtonComponent.progress('GNU Proton 已就绪'); return;
      }
      if (await statfs.getFreeSize(BASE) < extractionBytes + manifest.bytes + 134217728) throw new Error('空间不足：GNU Proton 安装需要约 700 MB 可用空间');
      removeOwned(`${SCRATCH}/new`); removeOwned(`${SCRATCH}/old`);
      const archive = await LinuxGnuProtonComponent.embedded(context, manifest);
      LinuxGnuProtonComponent.progress('正在解压 GNU Proton'); ConfigurationFiles.ensure(`${SCRATCH}/new`);
      await zlib.decompressFile(archive, `${SCRATCH}/new`, {});
      stamps = await LinuxGnuProtonComponent.matches(`${SCRATCH}/new`, manifest);
      if (!stamps) throw new Error('Proton 文件校验失败，请重试安装');
      // GNU ELF files run through the existing verified native loader; OHOS
      // does not expose chmod/exec permissions to the application installer.
      const groups = Array.from(new Set<string>(manifest.files.map((file: ComponentFile): string => protonComponentGroup(file.path))));
      const transaction: Transaction = { committed: false, previousReceipt: readSmokeText(RECEIPT), changes: [] };
      LinuxGnuProtonComponent.progress('正在启用 GNU Proton');
      for (const group of groups) {
        const active = `${ROOT}/${group}`; safeParents(ROOT, group); safeParents(`${SCRATCH}/old`, group);
        const exists = fs.accessSync(active);
        transaction.changes.push({ path: group, hadOld: exists }); ConfigurationFiles.json(JOURNAL, JSON.stringify(transaction));
        if (exists) { if (fs.lstatSync(active).isSymbolicLink()) throw new Error('Proton 目标路径不安全'); fs.renameSync(active, `${SCRATCH}/old/${group}`); }
        fs.renameSync(`${SCRATCH}/new/${group}`, active);
      }
      ConfigurationFiles.json(RECEIPT, JSON.stringify({ id: manifest.id, stamps }));
      transaction.committed = true; ConfigurationFiles.json(JOURNAL, JSON.stringify(transaction));
      recoverTransaction(); await runtimes.adoptEmbedded(context, !selected);
      LinuxGnuProtonComponent.verified = true; LinuxGnuProtonComponent.progress('GNU Proton 已就绪');
    } catch (error) {
      LinuxGnuProtonComponent.verified = false;
      try { recoverTransaction(); } catch (_) { throw new Error('Proton 更新未完成，请退出并重新打开应用恢复'); }
      LinuxGnuProtonComponent.progress(`GNU Proton 安装失败：${(error as Error).message.substring(0, 180)}`);
      throw new Error((error as Error).message);
    } finally {
      if (!fs.accessSync(JOURNAL)) { removeOwned(`${SCRATCH}/new`); removeOwned(`${SCRATCH}/component.zip`); }
    }
  }
}
