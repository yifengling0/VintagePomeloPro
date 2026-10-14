import { common } from '@kit.AbilityKit';
import { fileIo as fs, fileUri } from '@kit.CoreFileKit';
import preferences from '@ohos.data.preferences';
import bundleManager from '@ohos.bundle.bundleManager';
import util from '@ohos.util';
import { CONFIGURATION_REGISTRY, ConfigurationRegistration, ConfigurationModule, ConfigurationEntry, ConfigurationAsset,
  UserConfiguration, ConfigurationValue, validateUserConfiguration } from '../model/UserConfiguration';
import { ConfigurationZipMember, encodeConfigurationZip, decodeConfigurationZip } from '../model/ConfigurationZip';
import { ConfigurationFiles as files } from './ConfigurationFiles';
import { BUILT_IN_FONT_HASHES } from '../model/BuiltInFontBaseline';

interface ConfigurationState { schemaVersion: number; migrated: boolean; userModified: boolean; restored: boolean; revision: number; localBackupStatus?: string; localBackupSHA256?: string; }
interface RestoreFont { name: string; existed: boolean; }
interface RestoreChange { store: string; key: string; removed: boolean; value: ConfigurationValue; }
interface RestoreJournal { phase: string; before: string; previousState: ConfigurationState; fonts: RestoreFont[]; changes?: RestoreChange[]; }
interface ImportedFont { name: string; path: string; }
/** Complete allowlisted user configuration. Existing services remain the runtime-facing storage API. */
export class UserConfigurationRepository {
  private static instance: UserConfigurationRepository | null = null;
  private context: common.UIAbilityContext | null = null;
  private stores: Map<string, preferences.Preferences> = new Map<string, preferences.Preferences>();
  private state: ConfigurationState = { schemaVersion: 1, migrated: false, userModified: false, restored: false, revision: 0 };
  private listeners: Set<() => void> = new Set<() => void>();
  private restoreListeners: Set<() => Promise<void>> = new Set<() => Promise<void>>();
  private bootstrap: Promise<void> | null = null;
  private started: boolean = false;
  private restoring: boolean = false;
  private blocked: boolean = false;
  private localStatus: string = '';
  private timer: number = -1;
  private writing: boolean = false;
  private pending: boolean = false;
  private root: string = '';
  private libraryRoot: string = '';
  private fontsRoot: string = '';
  private observed: Map<string, string> = new Map<string, string>();
  private appVersion: string = '';
  private knownAssets: ConfigurationAsset[] = [];
  private restoreJournal: RestoreJournal | null = null;
  private restoreExpected: Map<string, string> = new Map<string, string>();
  private flushers: Set<() => Promise<void>> = new Set<() => Promise<void>>();
  beforeSnapshot(flusher: () => Promise<void>): void { this.flushers.add(flusher); }
  static getInstance(): UserConfigurationRepository {
    if (!UserConfigurationRepository.instance) UserConfigurationRepository.instance = new UserConfigurationRepository(); return UserConfigurationRepository.instance;
  }
  subscribe(listener: () => void): void { this.listeners.add(listener); }
  onRestored(listener: () => Promise<void>): void { this.restoreListeners.add(listener); }
  revision(): number { return this.state.revision; }
  hasLocalUserData(): boolean { return this.state.userModified || this.state.migrated || this.state.restored; }
  canWrite(): boolean { return !this.blocked; }
  localBackupStatus(): string { return this.localStatus || this.state.localBackupStatus || ''; }
  private rememberLocalBackup(status: string, digest: string = ''): void {
    this.localStatus = status;
    if (this.state.localBackupStatus === status && (!digest || this.state.localBackupSHA256 === digest)) return;
    this.state.localBackupStatus = status; if (digest) this.state.localBackupSHA256 = digest; this.persistState();
  }
  private emit(): void { this.listeners.forEach((listener: () => void): void => listener()); }
  initialize(context: common.UIAbilityContext): Promise<void> {
    if (!this.bootstrap) this.bootstrap = this.prepare(context).catch((error: Error): void => {
      this.blocked = true; this.localStatus = '本地配置暂不可用，原备份已保留'; throw error;
    }); return this.bootstrap;
  }
  private async prepare(context: common.UIAbilityContext): Promise<void> {
    this.context = context; this.root = context.filesDir + '/user_config'; files.ensure(this.root); files.ensure(this.root + '/assets');
    this.fontsRoot = context.getApplicationContext().filesDir + '/.wine/drive_c/windows/fonts';
    const bundle = bundleManager.getBundleInfoForSelfSync(bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_APPLICATION);
    this.appVersion = bundle.versionName;
    this.libraryRoot = new fileUri.FileUri('file://docs/storage/Users/currentUser/Download/' + bundle.name).path;
    for (const registration of CONFIGURATION_REGISTRY) {
      if (!this.stores.has(registration.store)) this.stores.set(registration.store, await preferences.getPreferences(context, registration.store));
    }
    const marker = this.root + '/state.json';
    if (fs.accessSync(marker)) {
      try { this.state = JSON.parse(files.text(files.read(marker))) as ConfigurationState;
        if (this.state.schemaVersion !== 1) throw new Error('CONFIG_REQUIRES_UPDATE'); }
      catch (_) { this.blocked = true; this.localStatus = '配置需要更新应用后恢复'; return; }
    } else {
      // Existing persisted configuration, rather than resemblance to defaults, establishes migration ownership.
      this.state.migrated = CONFIGURATION_REGISTRY.some((r: ConfigurationRegistration): boolean =>
        r.keys.some((key: string): boolean => this.stores.get(r.store)?.hasSync(key) ?? false));
    }
    const journal = this.root + '/restore-journal.json';
    if (fs.accessSync(journal)) {
      try {
        const pending = JSON.parse(files.text(files.read(journal))) as RestoreJournal;
        if (pending.phase === 'applying') {
          const members = decodeConfigurationZip(files.read(this.root + '/restore-before.zip', 75497472));
          const before = await this.validateMembers(members); await this.applyEntries(before);
          await this.applyConcurrent(pending.changes ?? []);
          this.rollbackFonts(pending.fonts);
          this.state = pending.previousState;
          if (pending.changes?.length) { this.state.userModified = true; this.state.revision++; }
          this.persistState();
        } else if (pending.phase !== 'committed') throw new Error('CONFIG_INVALID');
        fs.unlinkSync(journal);
      } catch (_) { this.blocked = true; this.localStatus = '配置恢复未完成，原备份已保留'; return; }
    }
    if (!this.stores.get('pomelo_config_resources')?.hasSync('fonts_v1')) await this.migrateFonts();
    const path = this.libraryRoot + '/.pomelo/app-settings-backup.zip';
    if (!this.hasLocalUserData() && fs.accessSync(path)) {
      try { await this.restore(decodeConfigurationZip(files.read(path, 75497472)), this.state.revision); this.localStatus = '已从本地恢复应用配置'; }
      catch (error) { this.blocked = true; this.localStatus = (error as Error).message === 'CONFIG_REQUIRES_UPDATE' ? '本地配置需要更新应用后恢复' : '本地备份无法读取，已保留原文件'; }
    }
    this.persistState();
  }
  private persistState(): void { files.json(this.root + '/state.json', JSON.stringify(this.state)); }
  private async migrateFonts(): Promise<void> {
    if (!this.context) return;
    const result: ImportedFont[] = [];
    for (const directory of [this.fontsRoot, this.fontsRoot.slice(0, -5) + 'Fonts']) {
      if (!fs.accessSync(directory)) continue;
      for (const name of fs.listFileSync(directory)) {
        if (!/^[^/\\:]{1,128}\.(ttf|otf|ttc|fon|fnt)$/i.test(name)) continue;
        const path = directory + '/' + name, stat = fs.lstatSync(path);
        if (!stat.isFile() || stat.isSymbolicLink()) continue;
        if (BUILT_IN_FONT_HASHES[name.toLowerCase()] === await files.fileHash(path)) continue;
        if (result.length >= 256) throw new Error('CONFIG_QUOTA');
        result.push({ name: name, path: path });
      }
    }
    if (result.length) {
      const store = this.stores.get('pomelo_config_resources');
      if (store) { await store.put('fonts_v1', JSON.stringify(result)); await store.flush(); this.state.migrated = true; }
    }
  }
  private rollbackFonts(fonts: RestoreFont[]): void {
    for (const font of fonts) {
      if (!/^[^/\\:]{1,128}$/.test(font.name)) throw new Error('CONFIG_UNSAFE_PATH');
      const path = this.fontsRoot + '/' + font.name;
      if (font.existed) files.write(path, files.read(this.root + '/restore-fonts/' + font.name, 16777216));
      else if (fs.accessSync(path)) fs.unlinkSync(path);
    }
  }
  /** Begin observing only after initial service hydration/default provisioning has finished. */
  async start(): Promise<void> {
    if (this.started || !this.context || this.blocked) return;
    this.started = true;
    for (const registration of CONFIGURATION_REGISTRY) {
      const store = this.stores.get(registration.store); if (!store) continue;
      for (const key of registration.keys) this.observed.set(registration.store + '/' + key, store.hasSync(key) ? JSON.stringify(store.getSync(key, '')) : 'absent');
      store.on('change', (key: string): void => {
        if (!registration.keys.includes(key)) return;
        const id = registration.store + '/' + key, value = store.hasSync(key) ? JSON.stringify(store.getSync(key, '')) : 'absent';
        if (this.observed.get(id) === value) return; this.observed.set(id, value);
        if (this.restoring) {
          if (this.restoreJournal?.phase === 'applying' && this.restoreExpected.get(id) !== value) {
            const changes = this.restoreJournal.changes ?? [];
            this.restoreJournal.changes = changes.filter((c: RestoreChange): boolean => c.store !== registration.store || c.key !== key);
            this.restoreJournal.changes.push({ store: registration.store, key: key, removed: !store.hasSync(key), value: store.getSync(key, '') as ConfigurationValue });
            files.json(this.root + '/restore-journal.json', JSON.stringify(this.restoreJournal));
          }
          return;
        }
        this.state.userModified = true; this.state.revision++; this.persistState(); this.emit(); this.scheduleLocal();
      });
    }
    // A fresh install must first check cloud. Do not publish defaults as an existing user's new head.
    if (this.hasLocalUserData()) this.scheduleLocal();
  }
  private scheduleLocal(): void {
    if (this.timer >= 0) clearTimeout(this.timer);
    this.timer = setTimeout((): void => { this.timer = -1; void this.backupLocal().catch((): void => {}); }, 700);
  }
  async importWallpaper(uri: string): Promise<string> {
    if (!this.context || this.blocked) throw new Error('CONFIG_REQUIRES_UPDATE');
    const file = fs.openSync(uri, fs.OpenMode.READ_ONLY);
    const temporary = this.root + '/wallpaper-import';
    try { await fs.copyFile(file.fd, temporary); } finally { fs.closeSync(file); }
    try {
      const bytes = files.read(temporary, 12582912); if (bytes.length < 16) throw new Error('CONFIG_INVALID');
      const hash = await files.hash(bytes), path = this.root + '/assets/' + hash + '.img'; files.write(path, bytes); return path;
    } finally { try { fs.unlinkSync(temporary); } catch (_) {} }
  }
  async recordFonts(names: string[], directory: string): Promise<void> {
    const store = this.stores.get('pomelo_config_resources'); if (!store) return;
    const result: ImportedFont[] = [];
    names.forEach((name: string): void => { if (/^[^/\\:]{1,128}$/.test(name)) result.push({ name: name, path: directory + '/' + name }); });
    await store.put('fonts_v1', JSON.stringify(result)); await store.flush();
  }
  private entries(): ConfigurationModule[] {
    const modules: ConfigurationModule[] = [];
    for (const registration of CONFIGURATION_REGISTRY) {
      let module = modules.find((m: ConfigurationModule): boolean => m.name === registration.name);
      if (!module) { module = { name: registration.name, schemaVersion: registration.schemaVersion, entries: [] }; modules.push(module); }
      const store = this.stores.get(registration.store); if (!store) throw new Error('CONFIG_STORAGE_UNAVAILABLE');
      for (const key of registration.keys) {
        if (store.hasSync(key)) module.entries.push({ store: registration.store, key: key, value: store.getSync(key, '') as string | number | boolean });
      }
    }
    return modules;
  }
  async snapshot(): Promise<ConfigurationZipMember[]> {
    if (this.blocked || !this.context) throw new Error('CONFIG_REQUIRES_UPDATE');
    for (const flusher of this.flushers) await flusher();
    const modules = this.entries(), assets: ConfigurationAsset[] = [], members: ConfigurationZipMember[] = [];
    const global = modules[0].entries.find((e: ConfigurationEntry): boolean => e.key === 'global_settings_v1');
    if (global) {
      const settings = JSON.parse(global.value as string) as Record<string, string | boolean | number>;
      const path = settings['pageBackgroundUri'] as string;
      if (path) {
        let imported = path;
        if (!path.startsWith(this.root + '/assets/')) imported = await this.importWallpaper(path);
        const asset = await this.asset(imported, 'wallpaper'); assets.push(asset); settings['pageBackgroundUri'] = '@config/' + asset.path;
        global.value = JSON.stringify(settings);
      }
    }
    const fonts = this.stores.get('pomelo_config_resources');
    const imported = JSON.parse(fonts?.getSync('fonts_v1', '[]') as string || '[]') as ImportedFont[];
    for (const font of imported) assets.push(await this.asset(font.path, 'font:' + font.name));
    for (const asset of assets) if (!members.some((m: ConfigurationZipMember): boolean => m.name === asset.path)) {
      members.push({ name: asset.path, bytes: files.read(this.root + '/' + asset.path, 16777216) });
    }
    const value: UserConfiguration = { formatVersion: 1, minReaderVersion: 1, minWriterVersion: 1, writtenByAppVersion: this.appVersion,
      requiredFeatures: ['portable-games-v1', 'input-v6', 'assets-v1'], modules: modules, assets: assets };
    validateUserConfiguration(value); const text = JSON.stringify(value); if (files.bytes(text).length > 8388608) throw new Error('CONFIG_QUOTA');
    members.unshift({ name: 'manifest.json', bytes: files.bytes(text) }); this.knownAssets = assets;
    return members;
  }
  private async asset(source: string, target: string): Promise<ConfigurationAsset> {
    const bytes = files.read(source, 16777216), hash = await files.hash(bytes), extension = target === 'wallpaper' ? 'img' : 'font';
    const path = 'assets/' + hash + '.' + extension;
    if (!fs.accessSync(this.root + '/' + path)) files.write(this.root + '/' + path, bytes);
    return { path: path, sha256: hash, size: bytes.length, target: target };
  }
  async cachedAsset(name: string, size: number, hash: string): Promise<Uint8Array | null> {
    if (!/^assets\/[a-f0-9]{64}\.(img|font)$/.test(name) || !fs.accessSync(this.root + '/' + name)) return null;
    try { const bytes = files.read(this.root + '/' + name, 16777216);
      return bytes.length === size && await files.hash(bytes) === hash ? bytes : null;
    } catch (_) { return null; }
  }
  async validateMembers(members: ConfigurationZipMember[]): Promise<UserConfiguration> {
    const manifest = members.find((m: ConfigurationZipMember): boolean => m.name === 'manifest.json'); if (!manifest) throw new Error('CONFIG_INCOMPLETE');
    const value = JSON.parse(files.text(manifest.bytes)) as UserConfiguration; validateUserConfiguration(value);
    const names = new Set<string>(); members.forEach((m: ConfigurationZipMember): void => { if (names.has(m.name)) throw new Error('CONFIG_INVALID'); names.add(m.name); });
    for (const asset of value.assets) { const member = members.find((m: ConfigurationZipMember): boolean => m.name === asset.path);
      if (!member || member.bytes.length !== asset.size || await files.hash(member.bytes) !== asset.sha256) throw new Error('CONFIG_HASH_MISMATCH'); }
    if (members.length !== new Set<string>(value.assets.map((a: ConfigurationAsset): string => a.path)).size + 1) throw new Error('CONFIG_INVALID');
    return value;
  }
  private async applyEntries(value: UserConfiguration): Promise<void> {
    for (const registration of CONFIGURATION_REGISTRY) {
      const module = value.modules.find((m: ConfigurationModule): boolean => m.name === registration.name); if (!module) throw new Error('CONFIG_INCOMPLETE');
      const store = this.stores.get(registration.store); if (!store) throw new Error('CONFIG_STORAGE_UNAVAILABLE');
      for (const key of registration.keys) {
        const entry = module.entries.find((e: ConfigurationEntry): boolean => e.store === registration.store && e.key === key);
        if (!entry) { this.restoreExpected.set(registration.store + '/' + key, 'absent'); await store.delete(key); continue; }
        let data = entry.value;
        if (key === 'global_settings_v1') {
          const settings = JSON.parse(data as string) as Record<string, string | boolean | number>;
          const asset = value.assets.find((a: ConfigurationAsset): boolean => a.target === 'wallpaper');
          settings['pageBackgroundUri'] = asset ? this.root + '/' + asset.path : ''; data = JSON.stringify(settings);
        }
        if (key === 'fonts_v1') data = JSON.stringify(value.assets.filter((a: ConfigurationAsset): boolean => a.target.startsWith('font:'))
          .map((a: ConfigurationAsset): ImportedFont => { return { name: a.target.slice(5), path: this.fontsRoot + '/' + a.target.slice(5) }; }));
        this.restoreExpected.set(registration.store + '/' + key, JSON.stringify(data));
        await store.put(key, data);
      }
      await store.flush();
    }
  }
  private async applyConcurrent(changes: RestoreChange[]): Promise<void> {
    for (const change of changes) {
      if (!CONFIGURATION_REGISTRY.some((r: ConfigurationRegistration): boolean => r.store === change.store && r.keys.includes(change.key))) throw new Error('CONFIG_INVALID');
      const store = this.stores.get(change.store); if (!store) throw new Error('CONFIG_STORAGE_UNAVAILABLE');
      if (change.removed) await store.delete(change.key); else await store.put(change.key, change.value);
      await store.flush();
    }
  }
  async restore(members: ConfigurationZipMember[], expectedRevision: number): Promise<void> {
    const value = await this.validateMembers(members);
    if (expectedRevision !== this.state.revision || this.restoring || this.blocked) throw new Error('CONFIG_LOCAL_CHANGED');
    const before = await this.snapshot();
    if (expectedRevision !== this.state.revision) throw new Error('CONFIG_LOCAL_CHANGED');
    files.write(this.root + '/restore-before.zip', encodeConfigurationZip(before));
    const fontUndo: RestoreFont[] = []; files.ensure(this.root + '/restore-fonts');
    const beforeValue = await this.validateMembers(before);
    const fontNames = new Set<string>();
    for (const asset of beforeValue.assets.concat(value.assets)) if (asset.target.startsWith('font:')) fontNames.add(asset.target.slice(5));
    for (const name of fontNames) {
      const path = this.fontsRoot + '/' + name;
      const existed = fs.accessSync(path); if (existed) files.write(this.root + '/restore-fonts/' + name, files.read(path, 16777216));
      fontUndo.push({ name: name, existed: existed });
    }
    const journal: RestoreJournal = { phase: 'applying', before: 'restore-before.zip', previousState: JSON.parse(JSON.stringify(this.state)) as ConfigurationState, fonts: fontUndo };
    files.json(this.root + '/restore-journal.json', JSON.stringify(journal)); this.restoring = true;
    this.restoreJournal = journal; this.restoreExpected.clear();
    this.observed.forEach((v: string, k: string): void => { this.restoreExpected.set(k, v); });
    try {
      for (const member of members) if (member.name !== 'manifest.json') files.write(this.root + '/' + member.name, member.bytes);
      for (const name of fontNames) {
        if (value.assets.some((a: ConfigurationAsset): boolean => a.target === 'font:' + name)) continue;
        const path = this.fontsRoot + '/' + name;
        if (fs.accessSync(path)) fs.unlinkSync(path);
      }
      // Restore only registered user fonts; never replace a whole Wine prefix.
      for (const asset of value.assets.filter((a: ConfigurationAsset): boolean => a.target.startsWith('font:'))) {
        const directory = this.fontsRoot; files.ensure(directory);
        files.write(directory + '/' + asset.target.slice(5), files.read(this.root + '/' + asset.path, 16777216));
      }
      await this.applyEntries(value);
      if (journal.changes?.length) throw new Error('CONFIG_LOCAL_CHANGED');
      this.state.restored = true; this.state.revision++; this.persistState();
      journal.phase = 'committed'; files.json(this.root + '/restore-journal.json', JSON.stringify(journal));
      for (const listener of this.restoreListeners) await listener();
      fs.unlinkSync(this.root + '/restore-journal.json'); this.localStatus = '应用配置已恢复';
    } catch (error) {
      if (journal.phase === 'applying') {
        const changes = journal.changes ?? []; this.restoreJournal = null;
        await this.applyEntries(await this.validateMembers(before)); await this.applyConcurrent(changes);
        this.rollbackFonts(journal.fonts); this.state = journal.previousState;
        if (changes.length) { this.state.userModified = true; this.state.revision++; }
        this.persistState();
        for (const listener of this.restoreListeners) await listener();
        fs.unlinkSync(this.root + '/restore-journal.json');
        if (changes.length) { this.emit(); this.scheduleLocal(); }
      }
      throw new Error((error as Error).message);
    } finally { this.restoring = false; this.restoreJournal = null; this.restoreExpected.clear(); }
    this.scheduleLocal();
  }
  async backupLocal(): Promise<void> {
    if (this.blocked || this.restoring || !this.hasLocalUserData()) return;
    if (this.writing) { this.pending = true; return; }
    this.writing = true;
    try {
      const revision = this.state.revision, members = await this.snapshot();
      await this.validateMembers(members); const archive = encodeConfigurationZip(members);
      if (revision !== this.state.revision) { this.pending = true; return; }
      // Keep a unified sandbox snapshot and readable module projections for future migrations.
      const manifest = members[0]; files.write(this.root + '/manifest.json', manifest.bytes);
      const value = JSON.parse(files.text(manifest.bytes)) as UserConfiguration;
      value.modules.forEach((m: ConfigurationModule): void => files.json(this.root + '/' + m.name + '.json', JSON.stringify(m)));
      files.ensure(this.libraryRoot + '/.pomelo'); const path = this.libraryRoot + '/.pomelo/app-settings-backup.zip';
      if (fs.accessSync(path)) {
        const previous = files.read(path, 75497472); await this.validateMembers(decodeConfigurationZip(previous));
        const digest = await files.hash(archive);
        if (await files.hash(previous) === digest) { this.rememberLocalBackup('本地配置备份已更新', digest); return; }
      }
      const temporary = path + '.next'; files.write(temporary, archive); await this.validateMembers(decodeConfigurationZip(files.read(temporary, 75497472)));
      if (revision !== this.state.revision) { this.pending = true; return; }
      fs.renameSync(temporary, path); this.rememberLocalBackup('本地配置备份已更新', await files.hash(archive));
      console.info('[UserConfiguration] local backup committed bytes=' + archive.length + ' revision=' + revision);
    } catch (error) { this.rememberLocalBackup((error as Error).message === 'CONFIG_REQUIRES_UPDATE' ? '备份需要更新应用，原文件已保留' : '本地备份未完成，原文件已保留');
      const message = (error as Error).message;
      console.warn('[UserConfiguration] local backup failed: ' + (/^[A-Z_0-9]{1,64}$/.test(message) ? message : 'FAILED'));
      throw new Error(message); }
    finally { this.writing = false; if (this.pending) { this.pending = false; this.scheduleLocal(); } }
  }
}
