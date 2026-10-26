import { common } from '@kit.AbilityKit';
import { fileIo as fs, statfs } from '@kit.CoreFileKit';
import { http } from '@kit.NetworkKit';
import { zlib } from '@kit.BasicServicesKit';
import { LinuxSteamDownloadArchive } from '../model/LinuxSteamDownloadModels';
import { ConfigurationFiles } from './ConfigurationFiles';
import { LinuxSteamComponentDownload } from './LinuxSteamComponentDownload';
import { ProtonRuntimeCatalog, ProtonRuntimeRelease, ProtonRuntimeSource, ProtonRuntimeSnapshot, ProtonSteamRuntime,
  PROTON_CATALOG_URL, validateProtonRuntime, validateProtonRuntimeCatalog } from '../model/LinuxProtonRuntimeModels';

const BASE = '/data/storage/el2/base/files/steamohos-native';
const ROOT = `${BASE}/proton-probe`;
const STORE = `${BASE}/proton-runtimes`;
const SCRATCH = `${STORE}/staging`;
const STATE = `${STORE}/selection.json`;
const JOURNAL = `${STORE}/activation.json`;
interface Selection { activeId: string; previousId: string; }
interface RuntimeStamp { path: string; bytes: number; mtime: number; }
interface RuntimeRecord { release: ProtonRuntimeRelease; source: ProtonRuntimeSource; stamps: RuntimeStamp[]; }
interface Activation { from: string; to: string; previous: Selection; committed: boolean; hadActive: boolean; }
export type ProtonRuntimeListener = (value: ProtonRuntimeSnapshot) => void;
const pause = (): Promise<void> => new Promise<void>((resolve: () => void): void => { setTimeout(resolve, 0); });
function idPath(id: string): string {
  if (!/^[a-f0-9]{64}$/.test(id)) throw new Error('Proton 版本标识无效');
  return `${STORE}/versions/${id}`;
}
function directory(path: string): void {
  if (!path.startsWith(BASE + '/')) throw new Error('Proton 目录超出应用范围');
  let current = BASE;
  ConfigurationFiles.ensure(BASE);
  if (!fs.lstatSync(BASE).isDirectory() || fs.lstatSync(BASE).isSymbolicLink()) throw new Error('Proton 基础目录无效');
  for (const part of path.substring(BASE.length + 1).split('/')) {
    if (!part || part === '.' || part === '..') throw new Error('Proton 目录无效');
    current += '/' + part;
    if (fs.accessSync(current)) {
      const stat = fs.lstatSync(current);
      if (!stat.isDirectory() || stat.isSymbolicLink()) throw new Error('Proton 目录包含无效链接');
    } else ConfigurationFiles.ensure(current);
  }
}
function parents(path: string): void { directory(path.substring(0, path.lastIndexOf('/'))); }
function readJson(path: string): string {
  if (!fs.accessSync(path)) return '';
  const stat = fs.lstatSync(path);
  if (!stat.isFile() || stat.isSymbolicLink() || stat.size > 2097152) throw new Error('Proton 管理记录无效');
  return ConfigurationFiles.text(ConfigurationFiles.read(path, 2097152));
}
function save(path: string, value: Object): void {
  parents(path); ConfigurationFiles.json(path + '.tmp', JSON.stringify(value)); fs.renameSync(path + '.tmp', path);
}
function selection(): Selection {
  const text = readJson(STATE);
  if (!text) return { activeId: '', previousId: '' };
  const value = JSON.parse(text) as Selection;
  for (const id of [value.activeId, value.previousId]) if (id !== '' && !/^[a-f0-9]{64}$/.test(id)) throw new Error('Proton 选择记录无效');
  return value;
}
function record(id: string): RuntimeRecord {
  const value = JSON.parse(readJson(`${idPath(id)}/runtime.json`)) as RuntimeRecord;
  validateProtonRuntime(value.release);
  if (value.release.id !== id || !['embedded', 'download', 'local', 'steam'].includes(value.source) || !Array.isArray(value.stamps)) {
    throw new Error('Proton 安装记录无效');
  }
  return value;
}
function removeScratch(path: string): void {
  if (!path.startsWith(SCRATCH + '/')) throw new Error('Proton 临时清理范围无效');
  if (!fs.accessSync(path)) return;
  const stat = fs.lstatSync(path);
  if (stat.isSymbolicLink()) throw new Error('Proton 临时目录包含符号链接');
  if (stat.isDirectory()) {
    for (const name of fs.listFileSync(path)) {
      if (!name || name.includes('/') || name.includes('\\') || name === '.' || name === '..') throw new Error('Proton 临时文件名无效');
      removeScratch(`${path}/${name}`);
    }
    fs.rmdirSync(path);
  } else fs.unlinkSync(path);
}
function ownedPackage(path: string): void {
  if (fs.accessSync(path) && (!fs.lstatSync(path).isDirectory() || fs.lstatSync(path).isSymbolicLink())) throw new Error('Proton 运行目录无效');
}
function invalidateProvisioning(): void {
  for (const name of ['hmosdeck-locale.state', 'hmosdeck-media.state']) {
    const path = `${ROOT}/prefix/${name}`;
    if (fs.accessSync(path)) {
      const stat = fs.lstatSync(path);
      if (!stat.isFile() || stat.isSymbolicLink()) throw new Error('Proton 初始化记录无效');
      fs.unlinkSync(path);
    }
  }
}
// All packages keep the existing execution path, so no GNU loader/shim ABI or
// native graphics/audio endpoints change when a version is selected.
function recoverActivation(): void {
  const text = readJson(JOURNAL); if (!text) return;
  const transaction = JSON.parse(text) as Activation;
  idPath(transaction.to);
  if (transaction.from) idPath(transaction.from);
  const previous = transaction.previous;
  if (!previous || previous.activeId !== transaction.from || typeof transaction.committed !== 'boolean' ||
      typeof transaction.hadActive !== 'boolean' || typeof previous.previousId !== 'string' ||
      (previous.previousId && !/^[a-f0-9]{64}$/.test(previous.previousId))) throw new Error('Proton 切换记录无效');
  const incoming = `${idPath(transaction.to)}/package`;
  const old = transaction.from === transaction.to ? `${SCRATCH}/replaced-package` :
    transaction.from ? `${idPath(transaction.from)}/package` : '';
  for (const path of [incoming, `${ROOT}/package`, ...(old ? [old] : [])]) ownedPackage(path);
  if (!transaction.committed) {
    if ((old && fs.accessSync(old)) || !transaction.hadActive) {
      if (!fs.accessSync(incoming) && fs.accessSync(`${ROOT}/package`)) fs.renameSync(`${ROOT}/package`, incoming);
      if (old && fs.accessSync(old)) fs.renameSync(old, `${ROOT}/package`);
    }
    save(STATE, previous);
  } else {
    invalidateProvisioning();
    if (transaction.from === transaction.to) removeScratch(`${SCRATCH}/replaced-package`);
  }
  fs.unlinkSync(JOURNAL);
}

export class LinuxProtonRuntimeService {
  private static instance: LinuxProtonRuntimeService | null = null;
  static getInstance(): LinuxProtonRuntimeService {
    if (!LinuxProtonRuntimeService.instance) LinuxProtonRuntimeService.instance = new LinuxProtonRuntimeService();
    return LinuxProtonRuntimeService.instance;
  }
  private catalog: ProtonRuntimeCatalog | null = null;
  private embedded: ProtonRuntimeRelease | null = null;
  private sessionActive: boolean = false;
  private adapterActive: boolean = false;
  private preparingSession: boolean = false;
  private games: number = 0;
  private transfer: LinuxSteamComponentDownload | null = null;
  private listeners: Set<ProtonRuntimeListener> = new Set<ProtonRuntimeListener>();
  private value: ProtonRuntimeSnapshot = { busy: false, message: '使用随应用提供的 Proton，亦可独立安装运行包', error: '',
    receivedBytes: 0, totalBytes: 0, activeId: '', previousId: '', versions: [], available: [] };
  snapshot(): ProtonRuntimeSnapshot { return { busy: this.value.busy, message: this.value.message, error: this.value.error,
    receivedBytes: this.value.receivedBytes, totalBytes: this.value.totalBytes, activeId: this.value.activeId,
    previousId: this.value.previousId, versions: this.value.versions.slice(), available: this.value.available.slice() }; }
  subscribe(listener: ProtonRuntimeListener): void { this.listeners.add(listener); listener(this.snapshot()); }
  unsubscribe(listener: ProtonRuntimeListener): void { this.listeners.delete(listener); }
  setSessionActive(value: boolean): void { this.sessionActive = value; }
  holdGame(): () => void {
    if (this.value.busy || this.adapterActive) throw new Error('Proton 组件正在维护，请稍后启动');
    this.games++; let released = false;
    return (): void => { if (!released) { released = true; this.games--; } };
  }
  beginSessionPreparation(): void {
    if (this.value.busy || this.adapterActive || this.preparingSession) throw new Error('Proton 组件正在维护，请稍后启动 Steam');
    this.preparingSession = true;
  }
  endSessionPreparation(): void { this.preparingSession = false; }
  inUse(): boolean { return this.sessionActive || this.games > 0; }
  beginAdapter(): void { if (this.value.busy || this.inUse() || this.adapterActive) throw new Error('Proton 运行包正在维护，请稍后启动'); this.adapterActive = true; }
  endAdapter(): void { this.adapterActive = false; }
  pauseDownload(): void { this.transfer?.pause(); }
  private publish(message: string = ''): void {
    if (message) this.value.message = message;
    this.listeners.forEach((listener: ProtonRuntimeListener): void => { listener(this.snapshot()); });
  }
  private reload(): void {
    const state = selection(); this.value.activeId = state.activeId; this.value.previousId = state.previousId;
    this.value.versions = [];
    if (fs.accessSync(`${STORE}/versions`)) {
      for (const id of fs.listFileSync(`${STORE}/versions`).slice(0, 32)) {
        if (!/^[a-f0-9]{64}$/.test(id)) continue;
        if (!fs.accessSync(`${idPath(id)}/runtime.json`)) continue;
        try {
          const installed = record(id);
          if (id === state.activeId || fs.accessSync(`${idPath(id)}/package`))
            this.value.versions.push({ id, name: installed.release.name, source: installed.source, active: id === state.activeId });
        } catch (_) { /* An incomplete inactive install must not hide healthy versions. */ }
      }
    }
    this.value.available = this.catalog ? this.catalog.releases : [];
    this.publish();
  }
  async initialize(context: common.Context): Promise<void> {
    if (!this.catalog) {
      const data = await context.resourceManager.getRawFileContent('linux-proton/runtime-catalog.json');
      const embedded = JSON.parse(ConfigurationFiles.text(data)) as ProtonRuntimeCatalog;
      validateProtonRuntimeCatalog(embedded); this.embedded = embedded.releases[0]; this.catalog = embedded;
      try {
      const cache = readJson(`${STORE}/catalog.json`);
      if (cache) {
        const saved = JSON.parse(cache) as ProtonRuntimeCatalog; validateProtonRuntimeCatalog(saved);
        if (saved.revision >= embedded.revision) this.catalog = saved;
      }
      } catch (_) { this.publish('缓存版本列表不可用，已使用内置列表'); }
    }
    this.reload();
  }
  private async operation(context: common.Context, action: () => Promise<void>): Promise<void> {
    if (this.sessionActive || this.preparingSession || this.games > 0 || this.adapterActive || this.value.busy) throw new Error('请先退出 Steam 和游戏，并等待组件准备完成');
    this.value.busy = true; this.value.error = ''; this.value.totalBytes = 0; this.value.receivedBytes = 0; this.publish();
    try { directory(STORE); directory(SCRATCH); directory(ROOT); recoverActivation(); await this.initialize(context); await action(); }
    catch (error) { this.value.error = (error as Error).message; this.publish('Proton 操作未完成'); throw error as Error; }
    finally {
      this.transfer = null;
      try {
        if (!fs.accessSync(JOURNAL)) { removeScratch(`${SCRATCH}/discarded-package`); removeScratch(`${SCRATCH}/removed-version`); }
      } finally { this.value.busy = false; this.reload(); }
    }
  }
  async checkUpdates(context: common.Context): Promise<void> {
    await this.operation(context, async (): Promise<void> => {
      this.publish('正在检查 Proton 版本');
      const request = http.createHttp();
      try {
        const response = await request.request(PROTON_CATALOG_URL, { method: http.RequestMethod.GET,
          expectDataType: http.HttpDataType.STRING, connectTimeout: 15000, readTimeout: 20000, maxLimit: 2097152, usingCache: false });
        if (response.responseCode !== 200 || typeof response.result !== 'string') throw new Error('无法获取 Proton 版本列表，请稍后重试');
        const catalog = JSON.parse(response.result) as ProtonRuntimeCatalog; validateProtonRuntimeCatalog(catalog);
        if (this.catalog && catalog.revision < this.catalog.revision) throw new Error('收到旧的 Proton 列表，已保留当前版本');
        // A published identity cannot silently acquire different executable bytes.
        for (const release of catalog.releases) {
          const existing = this.catalog?.releases.find((item: ProtonRuntimeRelease): boolean => item.id === release.id);
          if (existing && JSON.stringify(existing.files) !== JSON.stringify(release.files)) throw new Error('Proton 版本身份冲突');
        }
        save(`${STORE}/catalog.json`, catalog); this.catalog = catalog; this.publish('Proton 版本列表已更新');
      } finally { request.destroy(); }
    });
  }
  private find(id: string): ProtonRuntimeRelease {
    const release = this.catalog?.releases.find((item: ProtonRuntimeRelease): boolean => item.id === id);
    if (!release) throw new Error('此 Proton 版本不在受支持列表中，请先检查更新');
    return release;
  }
  private async verify(base: string, release: ProtonRuntimeRelease, stamps: RuntimeStamp[] = []): Promise<RuntimeStamp[] | null> {
    const previous = new Map<string, RuntimeStamp>(); stamps.forEach((item: RuntimeStamp): void => { previous.set(item.path, item); });
    const result: RuntimeStamp[] = [];
    for (let i = 0; i < release.files.length; i++) {
      const file = release.files[i]; const path = `${base}/${file.path}`;
      try {
        // Check every parent without following symlinks, including Steam input.
        let parent = base;
        if (!fs.lstatSync(parent).isDirectory() || fs.lstatSync(parent).isSymbolicLink()) return null;
        for (const part of file.path.split('/').slice(0, -1)) {
          parent += '/' + part; const stat = fs.lstatSync(parent);
          if (!stat.isDirectory() || stat.isSymbolicLink()) return null;
        }
        const stat = fs.lstatSync(path); const old = previous.get(file.path);
        if (!stat.isFile() || stat.isSymbolicLink() || stat.size !== file.bytes) return null;
        if (!old || old.bytes !== stat.size || old.mtime !== stat.mtime) {
          if (await ConfigurationFiles.fileHash(path) !== file.sha256) return null;
        }
        result.push({ path: file.path, bytes: stat.size, mtime: stat.mtime });
      } catch (_) { return null; }
      if (i % 24 === 0) { this.publish(`正在校验 Proton（${i + 1}/${release.files.length}）`); await pause(); }
    }
    return result;
  }
  // Called only under the adapter preparation gate; no network is used here.
  async prepareSelected(context: common.Context): Promise<boolean> {
    directory(STORE); directory(SCRATCH); directory(ROOT); recoverActivation(); await this.initialize(context);
    const state = selection(); if (!state.activeId) return false;
    const installed = record(state.activeId);
    const stamps = await this.verify(ROOT, installed.release, installed.stamps);
    if (!stamps) {
      if (installed.release.id === this.embedded?.id && installed.source === 'embedded') return false;
      throw new Error('所选 Proton 运行包缺失或损坏，请在组件页重新安装；现有游戏和存档已保留');
    }
    const updated: RuntimeRecord = { release: installed.release, source: installed.source, stamps };
    save(`${idPath(state.activeId)}/runtime.json`, updated); this.reload(); this.publish('Proton 运行包已就绪'); return true;
  }
  async adoptEmbedded(context: common.Context, refresh: boolean = false): Promise<void> {
    await this.initialize(context);
    const state = selection(); if (state.activeId && (!refresh || state.activeId !== this.embedded?.id)) return;
    const release = this.embedded;
    if (!release) throw new Error('内置 Proton 版本记录缺失');
    const stamps = await this.verify(ROOT, release);
    if (!stamps) throw new Error('内置 Proton 运行包校验失败');
    const adopted: RuntimeRecord = { release, source: 'embedded', stamps };
    save(`${idPath(release.id)}/runtime.json`, adopted);
    const selected: Selection = { activeId: release.id, previousId: state.previousId }; save(STATE, selected); this.reload(); this.publish(`${release.name} 已就绪`);
  }
  private async activate(id: string): Promise<void> {
    const state = selection(); const installed = record(id); const incoming = `${idPath(id)}/package`;
    const stamps = await this.verify(idPath(id), installed.release);
    if (!stamps) throw new Error('待启用的 Proton 文件不完整');
    ownedPackage(`${ROOT}/package`); ownedPackage(incoming);
    const old = state.activeId === id ? `${SCRATCH}/replaced-package` : state.activeId ? `${idPath(state.activeId)}/package` : '';
    if (old && fs.accessSync(old)) throw new Error('旧 Proton 版本目录冲突，未切换运行包');
    if (!state.activeId && fs.accessSync(`${ROOT}/package`)) throw new Error('请先启动一次 Steam 完成现有 Proton 登记');
    const transaction: Activation = { from: state.activeId, to: id, previous: state, committed: false, hadActive: fs.accessSync(`${ROOT}/package`) };
    save(JOURNAL, transaction);
    try {
      if (old && transaction.hadActive) { parents(old); fs.renameSync(`${ROOT}/package`, old); }
      fs.renameSync(incoming, `${ROOT}/package`);
      const updated: RuntimeRecord = { release: installed.release, source: installed.source, stamps };
      save(`${idPath(id)}/runtime.json`, updated);
      const selected: Selection = { activeId: id, previousId: state.activeId !== id ? state.activeId : state.previousId };
      save(STATE, selected);
      transaction.committed = true; save(JOURNAL, transaction); recoverActivation();
    } catch (error) { recoverActivation(); throw error as Error; }
    this.publish(`${installed.release.name} 已启用`);
  }
  async select(context: common.Context, id: string): Promise<void> {
    await this.operation(context, async (): Promise<void> => {
      if (selection().activeId === id) { this.publish('已在使用此 Proton 版本'); return; }
      await this.activate(id);
    });
  }
  async remove(context: common.Context, id: string): Promise<void> {
    await this.operation(context, async (): Promise<void> => {
      const state = selection();
      if (id === state.activeId || id === state.previousId) throw new Error('当前版本及上一版本需保留用于回退');
      const path = idPath(id); ownedPackage(path);
      removeScratch(`${SCRATCH}/removed-version`);
      if (fs.accessSync(path)) fs.renameSync(path, `${SCRATCH}/removed-version`);
      removeScratch(`${SCRATCH}/removed-version`); this.publish('未使用的 Proton 版本已移除');
    });
  }
  private async copy(inputPath: string, outputPath: string, size: number): Promise<void> {
    parents(outputPath);
    const input = fs.openSync(inputPath, fs.OpenMode.READ_ONLY | fs.OpenMode.NOFOLLOW);
    let output: fs.File | null = null;
    try {
      output = fs.openSync(outputPath, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC | fs.OpenMode.NOFOLLOW);
      if (fs.statSync(input.fd).size !== size) throw new Error('Proton 输入大小已变化');
      const buffer = new ArrayBuffer(1048576); let copied = 0;
      while (copied < size) {
        const count = fs.readSync(input.fd, buffer, { length: Math.min(buffer.byteLength, size - copied) });
        if (count <= 0) throw new Error('Proton 输入未读取完整');
        let written = 0;
        while (written < count) { const n = fs.writeSync(output.fd, buffer.slice(written, count)); if (n <= 0) throw new Error('Proton 文件写入失败'); written += n; }
        copied += count; await pause();
      }
      fs.fsyncSync(output.fd);
    } finally { if (output) fs.closeSync(output); fs.closeSync(input); }
  }
  private async accept(release: ProtonRuntimeRelease, source: ProtonRuntimeSource): Promise<void> {
    const stamps = await this.verify(`${SCRATCH}/unpacked`, release);
    if (!stamps) throw new Error('Proton 运行包文件校验失败，原版本未改变');
    const target = idPath(release.id); directory(target);
    removeScratch(`${SCRATCH}/discarded-package`);
    if (fs.accessSync(`${target}/package`)) {
      ownedPackage(`${target}/package`);
      fs.renameSync(`${target}/package`, `${SCRATCH}/discarded-package`);
    }
    const installed: RuntimeRecord = { release, source, stamps }; save(`${target}/runtime.json`, installed);
    fs.renameSync(`${SCRATCH}/unpacked/package`, `${target}/package`);
    await this.activate(release.id); removeScratch(`${SCRATCH}/discarded-package`);
  }
  private async reuseActive(release: ProtonRuntimeRelease): Promise<boolean> {
    if (selection().activeId !== release.id) return false;
    const stamps = await this.verify(ROOT, release); if (!stamps) return false;
    let source: ProtonRuntimeSource = release.id === this.embedded?.id ? 'embedded' : 'local';
    try { source = record(release.id).source; } catch (_) { /* Repair only the known package descriptor. */ }
    const installed: RuntimeRecord = { release, source, stamps };
    save(`${idPath(release.id)}/runtime.json`, installed); this.publish('此 Proton 已校验，无需重复安装'); return true;
  }
  async install(context: common.Context, id: string, uri: string = ''): Promise<void> {
    await this.operation(context, async (): Promise<void> => {
      if (!selection().activeId && fs.accessSync(`${ROOT}/package`)) await this.adoptEmbedded(context);
      const release = this.find(id);
      if (await this.reuseActive(release)) return;
      if (fs.accessSync(`${idPath(id)}/package`) && await this.verify(idPath(id), release)) { await this.activate(id); return; }
      if (!this.value.versions.some((item): boolean => item.id === id) && this.value.versions.length >= 8)
        throw new Error('已保留 8 个 Proton 版本，请先移除不再使用的版本');
      if (await statfs.getFreeSize(BASE) < release.archive.bytes + release.archive.unpackedBytes + 134217728) throw new Error('可用空间不足，原 Proton 版本未改变');
      const archive = `${SCRATCH}/runtime.zip`; removeScratch(`${SCRATCH}/unpacked`);
      if (uri) { removeScratch(archive + '.part'); removeScratch(archive + '.part.append-v1'); }
      try {
        this.value.totalBytes = release.archive.bytes; this.value.receivedBytes = 0;
        if (uri) { this.publish('正在读取本地 Proton 包'); await this.copy(uri, archive, release.archive.bytes); }
        else {
          this.transfer = new LinuxSteamComponentDownload(); this.publish(`正在下载 ${release.name}`);
          const download: LinuxSteamDownloadArchive = { id: release.id, name: release.name, url: release.archive.url,
            bytes: release.archive.bytes, sha256: release.archive.sha256, unpackedBytes: release.archive.unpackedBytes };
          await this.transfer.fetch(download, archive,
            (bytes: number): void => { this.value.receivedBytes = bytes; this.publish(); });
        }
        this.transfer = null; this.value.totalBytes = 0;
        if (await ConfigurationFiles.fileHash(archive) !== release.archive.sha256) throw new Error('Proton 压缩包校验失败');
        this.publish('正在解压 Proton 运行包'); directory(`${SCRATCH}/unpacked`);
        await zlib.decompressFile(archive, `${SCRATCH}/unpacked`, {});
        await this.accept(release, uri ? 'local' : 'download');
      } finally { removeScratch(`${SCRATCH}/unpacked`); removeScratch(archive); }
    });
  }
  async discoverSteam(context: common.Context): Promise<ProtonSteamRuntime[]> {
    await this.initialize(context);
    const path = `${BASE}/external-root.path`;
    if (!fs.accessSync(path)) throw new Error('尚未安装 Linux Steam');
    const steam = ConfigurationFiles.text(ConfigurationFiles.read(path, 4096)).trim();
    if (!steam.startsWith('/') || !steam.endsWith('/SteamLinuxARM64') || steam.split('/').includes('..') || /[\\\x00-\x1f]/.test(steam)) throw new Error('尚未安装 Linux Steam');
    const result: ProtonSteamRuntime[] = [];
    for (const commonPath of [`${steam}/library/steamapps/common`, `${steam}/client/.steam/steam/steamapps/common`]) {
      if (!fs.accessSync(commonPath)) continue;
      const stat = fs.lstatSync(commonPath); if (!stat.isDirectory() || stat.isSymbolicLink()) continue;
      for (const name of fs.listFileSync(commonPath).slice(0, 256)) {
        if (!/proton/i.test(name) || name.includes('/') || name.includes('\\') || name === '..') continue;
        const input = `${commonPath}/${name}`;
        if (!fs.lstatSync(input).isDirectory() || fs.lstatSync(input).isSymbolicLink()) continue;
        let matched = '';
        for (const release of this.catalog?.releases ?? []) {
          const wine = release.files.find((file): boolean => file.path === 'package/files/lib/wine/aarch64-unix/wine');
          try { if (wine && await ConfigurationFiles.fileHash(`${input}/files/lib/wine/aarch64-unix/wine`) === wine.sha256) { matched = release.id; break; } } catch (_) {}
        }
        result.push({ path: input, name, releaseId: matched, supported: matched.length > 0 });
      }
    }
    return result;
  }
  async importSteam(context: common.Context, input: ProtonSteamRuntime): Promise<void> {
    await this.operation(context, async (): Promise<void> => {
      if (!selection().activeId && fs.accessSync(`${ROOT}/package`)) await this.adoptEmbedded(context);
      const candidates = await this.discoverSteam(context);
      const current = candidates.find((item: ProtonSteamRuntime): boolean => item.path === input.path && item.supported && item.releaseId === input.releaseId);
      if (!current) throw new Error('此 Steam Proton 架构或版本不受支持，未修改原安装');
      const release = this.find(current.releaseId);
      if (await this.reuseActive(release)) return;
      if (fs.accessSync(`${idPath(release.id)}/package`) && await this.verify(idPath(release.id), release)) { await this.activate(release.id); return; }
      if (!this.value.versions.some((item): boolean => item.id === release.id) && this.value.versions.length >= 8)
        throw new Error('已保留 8 个 Proton 版本，请先移除不再使用的版本');
      if (await statfs.getFreeSize(BASE) < release.installedBytes + 134217728) throw new Error('可用空间不足');
      removeScratch(`${SCRATCH}/unpacked`); directory(`${SCRATCH}/unpacked`);
      try {
        // Only curated regular upstream files are copied. Never move, patch or
        // replace Steam's originals, or import its prefix/user data.
        for (let index = 0; index < release.files.length; index++) {
          const file = release.files[index];
          const relative = (file.sourcePath ?? file.path).substring(8); let parent = current.path;
          for (const part of relative.split('/').slice(0, -1)) {
            parent += '/' + part; const stat = fs.lstatSync(parent);
            if (!stat.isDirectory() || stat.isSymbolicLink()) throw new Error('Steam Proton 输入包含不受支持的链接');
          }
          const from = `${current.path}/${relative}`;
          if (await ConfigurationFiles.fileHash(from) !== file.sha256) throw new Error('Steam Proton 文件与受支持版本不匹配');
          this.publish(`正在导入 ${release.name}（${index + 1}/${release.files.length}）`);
          await this.copy(from, `${SCRATCH}/unpacked/${file.path}`, file.bytes);
        }
        await this.accept(release, 'steam');
      } finally { removeScratch(`${SCRATCH}/unpacked`); }
    });
  }
}
