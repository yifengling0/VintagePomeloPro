import { common } from '@kit.AbilityKit';
import { fileIo as fs, picker } from '@kit.CoreFileKit';
import { preferences } from '@kit.ArkData';
import testNapi, { SteamZipProgress } from 'libentry.so';
import { AppDescriptor, AppSource, LaunchKind, NativeProcessInfo } from '../model/AppModels';
import { AppSettingsStore } from '../service/AppSettingsStore';
import { wineDriveCForContainer } from '../service/GameContainer';
import { chooseCoverFileName, chooseBackgroundFileName } from '../model/AppCatalogRules';
import { SteamVdfNode, parseSteamVdf, serializeSteamVdf, safeSteamRelativePath,
  resolveSteamLibraryPath, steamWindowsPath, STEAM_CLIENT_ID, steamGameCardId, isSteamAppId,
  SteamCatalogScope, steamCatalogPathKind } from './SteamLocalRules';
import { SteamMatch } from './SteamMatch';
import { SteamImportFiles, SteamImportJournal, commitSteamImport, recoverSteamImport } from './SteamImportTransaction';

export interface SteamImportProgress { busy: boolean; message: string; percent: number; }
export type SteamImportListener = (progress: SteamImportProgress) => void;

function exists(path: string): boolean {
  try { return fs.accessSync(path); } catch (_) { return false; }
}
function directory(path: string): boolean {
  try { return fs.statSync(path).isDirectory(); } catch (_) { return false; }
}
function textFile(path: string): string {
  if (fs.statSync(path).size > 4 * 1024 * 1024) throw new Error('Steam 清单过大');
  return fs.readTextSync(path, { encoding: 'utf-8' });
}
function names(path: string): string[] {
  try { return fs.listFileSync(path, { listNum: 10000 }); } catch (_) { return []; }
}
async function removeOwned(path: string): Promise<void> {
  if (!exists(path)) return;
  if (fs.lstatSync(path).isDirectory()) await fs.rmdir(path);
  else await fs.unlink(path);
}
async function atomicText(path: string, value: string): Promise<void> {
  const temporary = path + '.new';
  const out = fs.openSync(temporary, fs.OpenMode.CREATE | fs.OpenMode.TRUNC | fs.OpenMode.WRITE_ONLY);
  try { fs.writeSync(out.fd, value); fs.fsyncSync(out.fd); } finally { fs.closeSync(out); }
  await fs.rename(temporary, path);
}

export class SteamLocalService {
  private static instance: SteamLocalService | null = null;
  static getInstance(): SteamLocalService {
    if (!SteamLocalService.instance) SteamLocalService.instance = new SteamLocalService();
    return SteamLocalService.instance;
  }
  private context: common.UIAbilityContext | null = null;
  private root: string = '';
  private store: preferences.Preferences | null = null;
  private compatibility: boolean = false;
  private importing: boolean = false;
  private committing: boolean = false;
  private listeners: SteamImportListener[] = [];
  private cards: AppDescriptor[] = [];
  private fingerprint: string = '';
  private coverAttempts: Map<string, number> = new Map<string, number>();
  private coverDownloads: number = 0;
  private scanning: Promise<AppDescriptor[]> | null = null;
  private progress: SteamImportProgress = { busy: false, message: '', percent: 0 };

  async initialize(context: common.UIAbilityContext, root: string): Promise<void> {
    this.context = context;
    this.root = root;
    this.store = await preferences.getPreferences(context, 'steam_local_client');
    this.compatibility = this.store.getSync('compatibility', false) as boolean;
    await this.recover();
  }
  getClientDirectory(): string { return this.root ? this.root + '/Steam' : ''; }
  isInstalled(): boolean { return this.root.length > 0 && exists(this.getClientDirectory() + '/steam.exe'); }
  isCompatibility(): boolean { return this.compatibility; }
  isImporting(): boolean { return this.importing; }
  canLaunch(): boolean { return !this.committing; }
  hasForeignClient(): boolean {
    const c = wineDriveCForContainer('default');
    const expected = steamWindowsPath(this.getClientDirectory() + '/steam.exe', this.root, c).toLowerCase();
    return (testNapi.getProcessList() as NativeProcessInfo[]).some((process: NativeProcessInfo): boolean => {
      const path = steamWindowsPath(process.path, this.root, c).toLowerCase();
      return process.state === 'running' && path.endsWith('/steam.exe') && path !== expected;
    });
  }
  async ensureGpuDependencies(): Promise<void> {
    if (!this.context) return;
    const bin = this.getClientDirectory() + '/bin';
    const target = bin + '/SDL2.dll';
    if (!directory(bin) || exists(target)) return;
    const temporary = target + '.importing';
    // getRawFdSync only accepts uncompressed resources. HAP packaging may
    // compress this small DLL, so read its bounded decoded resource content.
    const bytes = await this.context.resourceManager.getRawFileContent('sdl2-x86.dll');
    if (bytes.byteLength === 0 || bytes.byteLength > 16 * 1024 * 1024) throw new Error('Steam GPU 依赖无效');
    try {
      const output = fs.openSync(temporary, fs.OpenMode.CREATE | fs.OpenMode.TRUNC | fs.OpenMode.WRITE_ONLY);
      try {
        const content = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
        if (fs.writeSync(output.fd, content) !== bytes.byteLength) throw new Error('Steam GPU 依赖写入不完整');
        fs.fsyncSync(output.fd);
      } finally { fs.closeSync(output); }
      await fs.rename(temporary, target);
    } finally { if (exists(temporary)) await fs.unlink(temporary); }
  }

  async setCompatibility(value: boolean): Promise<void> {
    this.compatibility = value;
    if (this.store) { await this.store.put('compatibility', value); await this.store.flush(); }
  }
  subscribe(listener: SteamImportListener): void { this.listeners.push(listener); listener(this.progress); }
  unsubscribe(listener: SteamImportListener): void { this.listeners = this.listeners.filter((item: SteamImportListener) => item !== listener); }
  private notify(message: string, percent: number = 0): void {
    this.progress = { busy: this.importing, message, percent };
    this.listeners.forEach((listener: SteamImportListener): void => listener(this.progress));
  }
  private journalPath(): string { return this.root + '/.steam-import.json'; }
  private files(): SteamImportFiles {
    const adapter: SteamImportFiles = {
      exists: (path: string): boolean => exists(path),
      rename: async (from: string, to: string): Promise<void> => { await fs.rename(from, to); },
      remove: async (path: string): Promise<void> => { await removeOwned(path); },
      journal: async (value: SteamImportJournal | null): Promise<void> => {
        if (value) await atomicText(this.journalPath(), JSON.stringify(value));
        else if (exists(this.journalPath())) await fs.unlink(this.journalPath());
      }
    };
    return adapter;
  }
  private readJournal(): SteamImportJournal | null {
    if (!exists(this.journalPath())) return null;
    const value = JSON.parse(textFile(this.journalPath())) as SteamImportJournal;
    if (!['extract', 'replace', 'committed'].includes(value.phase) || !Array.isArray(value.preserved) ||
      !value.preserved.every((name: string): boolean => safeSteamRelativePath(name) && !/[\\/]/.test(name))) {
      throw new Error('Steam 导入记录无效，原目录已保留');
    }
    return value;
  }
  private async recover(force: boolean = false): Promise<void> {
    if (!this.root || (this.importing && !force)) return;
    const journal = this.readJournal();
    if (!journal) return;
    await recoverSteamImport(this.files(), this.getClientDirectory(), this.root + '/.steam-import-stage',
      this.root + '/.steam-import-backup', journal);
  }

  async importZip(): Promise<boolean> {
    if (!this.context || !this.root) throw new Error('请先建立游戏目录');
    if (this.importing) throw new Error('Steam 导入正在进行');
    const options = new picker.DocumentSelectOptions();
    options.maxSelectNumber = 1;
    options.fileSuffixFilters = ['Steam ZIP|.zip'];
    const selected = await new picker.DocumentViewPicker(this.context).select(options);
    if (selected.length === 0) return false;
    return this.importUri(selected[0]);
  }

  async importUri(uri: string): Promise<boolean> {
    if (!this.root || this.importing) throw new Error('Steam 导入尚未就绪');
    this.importing = true;
    this.notify('正在检查 Steam ZIP');
    const stage = this.root + '/.steam-import-stage';
    const backup = this.root + '/.steam-import-backup';
    let timer = -1;
    let ownsStage = false;
    try {
      await this.recover(true);
      const old = this.readJournal();
      if (old?.phase === 'committed') {
        if (exists(backup)) await removeOwned(backup);
        await this.files().journal(null);
      }
      if (exists(stage) || exists(backup)) throw new Error('Steam 暂存目录已有文件，已保留以便恢复');
      await this.files().journal({ phase: 'extract', hadClient: this.isInstalled(), preserved: [] });
      ownsStage = true;
      const input = fs.openSync(uri, fs.OpenMode.READ_ONLY);
      try {
        timer = setInterval((): void => {
          const progress = testNapi.getSteamZipProgress() as SteamZipProgress;
          if (progress.total > 0) this.notify('正在解包 Steam', Math.min(95, Math.floor(progress.done * 95 / progress.total)));
        }, 300);
        await testNapi.extractSteamZipAsync(input.fd, stage);
      } finally { fs.closeSync(input); if (timer >= 0) clearInterval(timer); timer = -1; }
      if (this.hasRunningProcesses() || this.hasForeignClient()) throw new Error('请先退出 Steam 和 Steam 游戏，再重新导入');
      this.committing = true;
      this.notify('正在保留游戏与登录数据', 96);
      const protectedNames = names(this.getClientDirectory()).filter((name: string): boolean =>
        ['steamapps', 'userdata', 'config', 'depotcache'].includes(name.toLowerCase()) || name.toLowerCase().startsWith('ssfn'));
      await commitSteamImport(this.files(), this.getClientDirectory(), stage, backup, protectedNames,
        async (path: string): Promise<void> => { await this.normalizeLibraryFolders(path); });
      this.committing = false;
      this.fingerprint = '';
      await this.scan();
      this.notify('Steam 导入完成', 100);
      return true;
    } catch (error) {
      const journal = ownsStage ? this.readJournal() : null;
      if (journal) await recoverSteamImport(this.files(), this.getClientDirectory(), stage, backup, journal);
      this.notify((error as Error).message || 'Steam 导入失败');
      throw (error as Error);
    } finally {
      if (timer >= 0) clearInterval(timer);
      this.committing = false;
      this.importing = false;
      this.notify(this.progress.message, this.progress.percent);
    }
  }

  private legacyClientDirectories(): string[] {
    if (!this.root) return [];
    const result: string[] = [];
    const games = this.root + '/games';
    for (const parent of [games, games + '/games']) {
      for (const name of names(parent)) {
        const candidate = parent + '/' + name;
        for (const path of [candidate, candidate + '/Steam']) {
          // Detect clients by their contents, never by helper directory names.
          // A client with a missing entry still must not become helper cards.
          if (exists(path + '/steam.exe') ||
            (directory(path + '/steamapps') && directory(path + '/clientui'))) result.push(path);
        }
      }
    }
    return result;
  }

  private existingLocalLibraries(): string[] {
    const result: string[] = [];
    for (const path of this.legacyClientDirectories()) {
      if (directory(path + '/steamapps') &&
        names(path + '/steamapps').some((file: string): boolean => /^appmanifest_\d+\.acf$/i.test(file))) result.push(path);
    }
    const oldC = wineDriveCForContainer('default') + '/Program Files (x86)/Steam';
    if (exists(oldC + '/steam.exe') && directory(oldC + '/steamapps')) result.push(oldC);
    return result;
  }
  private async normalizeLibraryFolders(client: string): Promise<void> {
    const steamapps = client + '/steamapps';
    if (!directory(steamapps)) fs.mkdirSync(steamapps);
    const file = steamapps + '/libraryfolders.vdf';
    let root = new SteamVdfNode();
    if (exists(file)) {
      try { root = parseSteamVdf(textFile(file)); }
      catch (_) { throw new Error('Steam 库清单损坏，更新已撤回；请在 Steam 中修复库路径'); }
    }
    let libraries = root.child('libraryfolders');
    if (!libraries) { libraries = new SteamVdfNode(); root.children.set('libraryfolders', libraries); }
    let primary = libraries.child('0');
    if (!primary || primary.children.size === 0) { primary = new SteamVdfNode(); libraries.children.set('0', primary); }
    primary.set('path', 'Z:\\Steam');
    // Existing manually imported clients may own installed games. Register
    // these libraries in place; moving their games would break ordinary cards.
    const registered: string[] = [];
    libraries.children.forEach((node: SteamVdfNode): void => {
      registered.push(resolveSteamLibraryPath(node.text('path') || node.value, this.root,
        wineDriveCForContainer('default')).toLowerCase());
    });
    for (const path of this.existingLocalLibraries()) {
      if (registered.includes(path.toLowerCase())) continue;
      let index = 1;
      while (libraries.child(index.toString())) index++;
      const node = new SteamVdfNode();
      node.set('path', steamWindowsPath(path, this.root, wineDriveCForContainer('default')).replace(/\//g, '\\'));
      libraries.children.set(index.toString(), node);
      registered.push(path.toLowerCase());
    }
    await atomicText(file, serializeSteamVdf(root));
  }

  private libraries(): string[] {
    const client = this.getClientDirectory();
    const result: string[] = [client];
    try {
      const tree = parseSteamVdf(textFile(client + '/steamapps/libraryfolders.vdf')).child('libraryfolders');
      tree?.children.forEach((node: SteamVdfNode, key: string): void => {
        if (!/^\d+$/.test(key)) return;
        const path = resolveSteamLibraryPath(node.text('path') || node.value, this.root, wineDriveCForContainer('default'));
        if (path && directory(path) && !result.some((item: string): boolean => item.toLowerCase() === path.toLowerCase())) result.push(path);
      });
    } catch (_) {}
    // Also discover old clients added after ZIP import without moving games
    // or requiring the user to import again just to register their library.
    for (const path of this.existingLocalLibraries()) {
      if (!result.some((item: string): boolean => item.toLowerCase() === path.toLowerCase())) result.push(path);
    }
    return result;
  }
  async libraryChanged(): Promise<boolean> {
    if (this.importing) return false;
    let value = this.isInstalled() ? 'installed' : '';
    for (const library of this.libraries()) {
      value += library;
      for (const name of names(library + '/steamapps').filter((name: string): boolean => /^appmanifest_\d+\.acf$/i.test(name)).sort()) {
        try { const stat = await fs.stat(library + '/steamapps/' + name); value += name + ':' + stat.mtime + ':' + stat.size; } catch (_) {}
      }
    }
    const changed = value !== this.fingerprint;
    this.fingerprint = value;
    return changed;
  }
  async downloadMissingCover(app: AppDescriptor): Promise<boolean> {
    if (!app.steam?.appId || app.coverPath || !app.available || this.importing || this.coverDownloads >= 2) return false;
    const previous = this.coverAttempts.get(app.id) ?? 0;
    if (Date.now() - previous < 5 * 60 * 1000) return false;
    this.coverAttempts.set(app.id, Date.now());
    this.coverDownloads++;
    try {
      return await SteamMatch.downloadCoverTo(app.steam.appId, '', app.directory, (): boolean =>
        !this.importing && exists(app.directory) && !AppSettingsStore.getInstance().getAppSettings(app.id).customCover);
    } finally { this.coverDownloads--; }
  }
  getCards(): AppDescriptor[] { return this.cards.slice(); }
  getCatalogScope(): SteamCatalogScope {
    if (!this.root) return { excludedRoots: [], libraryRoots: [], legacyGameRoots: [] };
    const libraries = this.libraries();
    const excluded = [this.getClientDirectory(), this.root + '/.steam-import-stage',
      this.root + '/.steam-import-backup', this.root + '/games/steam-download-staging',
      this.root + '/games/games/steam-download-staging'];
    this.legacyClientDirectories().forEach((path: string): void => { excluded.push(path); });
    libraries.forEach((path: string): void => {
      excluded.push(path + '/steamapps');
      if (exists(path + '/steam.exe')) excluded.push(path);
    });
    const legacyGames: string[] = [];
    for (const parent of [this.root + '/games', this.root + '/games/games']) {
      for (const name of names(parent).filter((name: string): boolean => name.toLowerCase() === 'steam')) {
        const container = parent + '/' + name;
        for (const appId of names(container).filter((name: string): boolean => isSteamAppId(name))) {
          const path = container + '/' + appId;
          if (directory(path)) legacyGames.push(path);
        }
      }
    }
    return { excludedRoots: excluded, libraryRoots: libraries, legacyGameRoots: legacyGames };
  }
  isLibraryContainer(path: string): boolean {
    return steamCatalogPathKind(path, this.getCatalogScope()) !== 'game';
  }
  getGame(appId: string): AppDescriptor | null { return this.cards.find((card: AppDescriptor) => card.steam?.appId === appId) ?? null; }
  clientCard(): AppDescriptor {
    return { id: STEAM_CLIENT_ID, title: 'Steam', source: AppSource.DOWNLOAD,
      directory: this.getClientDirectory(), coverPath: '', available: this.isInstalled(),
      launchTarget: { kind: LaunchKind.STEAM_CLIENT, executable: this.getClientDirectory() + '/steam.exe', arguments: [], workingDirectory: this.getClientDirectory() },
      displayModeOverride: null, steam: { clientId: STEAM_CLIENT_ID, appId: '', state: 'client' } };
  }
  installCard(appId: string): AppDescriptor {
    if (!isSteamAppId(appId)) throw new Error('Steam AppID 无效');
    const card = this.clientCard();
    card.id = 'steam-install-' + appId;
    card.steam = { clientId: STEAM_CLIENT_ID, appId, state: 'install-request' };
    return card;
  }
  scan(): Promise<AppDescriptor[]> {
    if (this.committing) return Promise.resolve(this.cards.slice());
    if (this.scanning) return this.scanning;
    const task = this.scanLibrary();
    this.scanning = task;
    return task.finally((): void => { this.scanning = null; });
  }
  private async scanLibrary(): Promise<AppDescriptor[]> {
    if (!this.isInstalled()) { this.cards = []; return []; }
    const result: AppDescriptor[] = [this.clientCard()];
    for (const library of this.libraries()) {
      const manifestNames = names(library + '/steamapps').filter((name: string): boolean => /^appmanifest_\d+\.acf$/i.test(name));
      for (const name of manifestNames) {
        const appId = name.substring(12, name.length - 4);
        if (!isSteamAppId(appId) || appId === '228980') continue; // Steamworks redistributables are not a game.
        try {
          const state = parseSteamVdf(textFile(library + '/steamapps/' + name)).child('appstate');
          if (!state || state.text('appid') !== appId || !safeSteamRelativePath(state.text('installdir'))) continue;
          const dir = library + '/steamapps/common/' + state.text('installdir').replace(/\\/g, '/');
          const flags = Number(state.text('stateflags'));
          const installed = (flags & 4) !== 0 && (flags & (1 | 32 | 128 | 256 | 512 | 1024 | 2048)) === 0 && directory(dir);
          const id = steamGameCardId(appId);
          const settings = AppSettingsStore.getInstance().getAppSettings(id);
          const files = names(dir);
          const localCover = chooseCoverFileName(files);
          let cover = settings.customCover || (localCover ? dir + '/' + localCover : '');
          if (!cover) {
            for (const suffix of ['_library_600x900.jpg', '_library_600x900_2x.jpg', '_header.jpg']) {
              const path = this.getClientDirectory() + '/appcache/librarycache/' + appId + suffix;
              if (exists(path)) { cover = path; break; }
            }
          }
          const background = chooseBackgroundFileName(files);
          const card: AppDescriptor = { id, title: settings.customTitle || state.text('name') || 'Steam ' + appId,
            source: AppSource.DOWNLOAD, directory: dir, coverPath: cover,
            backgroundPath: background ? dir + '/' + background : '', available: installed,
            launchTarget: { kind: LaunchKind.STEAM_GAME, executable: this.getClientDirectory() + '/steam.exe', arguments: settings.arguments, workingDirectory: this.getClientDirectory() },
            displayModeOverride: settings.displayModeOverride,
            steam: { clientId: STEAM_CLIENT_ID, appId, state: installed ? 'installed' : 'installing' } };
          const duplicate = result.findIndex((item: AppDescriptor): boolean => item.id === id);
          if (duplicate < 0) result.push(card);
          else if (installed && !result[duplicate].available) result[duplicate] = card;
        } catch (_) {
          // Steam writes manifests during install. A partial write must not
          // erase a previously known card; deletion is handled by enumeration.
          const previous = this.cards.find((card: AppDescriptor) => card.steam?.appId === appId);
          if (previous && !result.some((card: AppDescriptor) => card.id === previous.id)) result.push(previous);
        }
      }
    }
    this.cards = result;
    return result.slice();
  }
  processesFor(app: AppDescriptor, processes: NativeProcessInfo[]): NativeProcessInfo[] {
    const c = wineDriveCForContainer('default');
    const dir = steamWindowsPath(app.directory, this.root, c).toLowerCase().replace(/\/+$/, '') + '/';
    const exe = steamWindowsPath(this.getClientDirectory() + '/steam.exe', this.root, c).toLowerCase();
    return processes.filter((process: NativeProcessInfo): boolean => {
      if (process.state !== 'running') return false;
      const path = steamWindowsPath(process.path, this.root, c).toLowerCase();
      return app.steam?.appId ? path.startsWith(dir) && !/(?:^|\/)(?:_commonredist|redist|redistributables|installer)(?:\/|$)/.test(path) &&
        !/(?:^|\/)(?:crashreporter|crashpad_handler|unitycrashhandler(?:32|64)?|unins\d*|uninstall|vcredist[^/]*|dxsetup)\.exe$/.test(path) : path === exe;
    });
  }
  hasRunningProcesses(): boolean {
    const processes = testNapi.getProcessList() as NativeProcessInfo[];
    const c = wineDriveCForContainer('default');
    const client = steamWindowsPath(this.getClientDirectory(), this.root, c).toLowerCase() + '/';
    return processes.some((process: NativeProcessInfo): boolean => process.state === 'running' &&
      steamWindowsPath(process.path, this.root, c).toLowerCase().startsWith(client)) ||
      this.cards.some((card: AppDescriptor): boolean => this.processesFor(card, processes).length > 0);
  }
}
