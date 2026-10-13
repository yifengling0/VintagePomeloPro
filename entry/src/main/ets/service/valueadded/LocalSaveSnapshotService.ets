import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { cryptoFramework } from '@kit.CryptoArchitectureKit';
import util from '@ohos.util';
import testNapi from 'libentry.so';
import { DeviceCapabilityPolicy } from '../../common/DeviceCapabilityPolicy';
import { AppDescriptor, EngineState, LaunchKind, NativeProcessInfo } from '../../model/AppModels';
import { SaveManifest, SaveManifestEntry, safeRelativeSavePath, validateSaveEntries } from '../../model/ValueAddedModels';
import { CLOUD_CHUNK_BYTES, CloudSaveManifest, CloudUploadPlan, CloudSaveFile, CloudSaveChunk, validateCloudSaveManifest } from '../../model/CloudSaveModels';
import { BackupGameSummary, GameBackupState, SaveLocation, SaveLocationIndex, SavePreview, sameSaveContent, savePreview } from '../../model/SaveBackupModels';
import { WineEngineService } from '../WineEngineService';
import { AppCatalogService } from '../AppCatalogService';

interface SteamMarker { appId: number; source: string; name: string; }
interface RestoreJournal { appId: string; selectedPath: string; stagedName: string; previousName: string; originalExists: boolean; phase: string; }
interface CloudImportState { manifest: CloudSaveManifest; appId: string; selectedPath: string; }
interface FileDigest { size: number; modifiedAt: number; sha256: string; }

/** Verified local snapshots of an automatically recognized catalog-game save directory. */
export class LocalSaveSnapshotService {
  private static instance: LocalSaveSnapshotService | null = null;
  private busy: boolean = false;
  private automaticBusy: boolean = false;
  private automaticContext: common.UIAbilityContext | null = null;
  initialize(context: common.UIAbilityContext): void {
    if (!DeviceCapabilityPolicy.supportsValueAddedFeatures() || this.automaticContext) return;
    this.automaticContext = context;
    WineEngineService.getInstance().subscribe((state: EngineState): void => {
      if (state === EngineState.STOPPED) {
        setTimeout((): void => { void this.runAutomatic(context); }, 750);
      }
    });
    void this.runAutomatic(context);
  }
  static getInstance(): LocalSaveSnapshotService {
    if (!LocalSaveSnapshotService.instance) LocalSaveSnapshotService.instance = new LocalSaveSnapshotService();
    return LocalSaveSnapshotService.instance;
  }
  private guard(): void {
    if (!DeviceCapabilityPolicy.supportsValueAddedFeatures()) throw new Error('FEATURE_HIDDEN');
  }
  private ensureStopped(): void {
    // Native ownership can include children launched outside the catalog. Fail closed rather than stopAll.
    if (WineEngineService.getInstance().getState() !== EngineState.STOPPED) throw new Error('SAVE_ENGINE_RUNNING');
    const processes = testNapi.getProcessList() as NativeProcessInfo[];
    if (processes.some((process: NativeProcessInfo): boolean => process.state === 'running')) {
      throw new Error('SAVE_ENGINE_RUNNING');
    }
  }
  private root(context: common.UIAbilityContext): string {
    this.guard();
    const path = context.filesDir + '/local_save_snapshots';
    if (!fs.accessSync(path)) fs.mkdirSync(path);
    return this.canonicalDirectory(path);
  }
  private canonicalDirectory(path: string): string {
    if (fs.lstatSync(path).isSymbolicLink()) throw new Error('UNSAFE_SAVE_PATH');
    const file = fs.openSync(path, fs.OpenMode.READ_ONLY | fs.OpenMode.DIR | fs.OpenMode.NOFOLLOW);
    try { return file.path; } finally { fs.closeSync(file); }
  }
  private child(root: string, relative: string, directory: boolean = false): string {
    safeRelativeSavePath(relative);
    let path = root;
    const parts = relative.split('/');
    for (let index = 0; index < parts.length; index++) {
      path += '/' + parts[index];
      const stat = fs.lstatSync(path);
      if (stat.isSymbolicLink() || (index < parts.length - 1 && !stat.isDirectory())) throw new Error('UNSAFE_SAVE_PATH');
    }
    const file = fs.openSync(path, fs.OpenMode.READ_ONLY | fs.OpenMode.NOFOLLOW | (directory ? fs.OpenMode.DIR : 0));
    try {
      // CoreFileKit File.path for filesystem paths resolves realpath, not a URI path.
      if (!file.path.startsWith(root + '/')) throw new Error('UNSAFE_SAVE_PATH');
      return file.path;
    } finally { fs.closeSync(file); }
  }
  eligibleGames(): AppDescriptor[] {
    this.guard();
    return AppCatalogService.getInstance().getApps().filter((app: AppDescriptor): boolean =>
      app.available && !app.steam && app.launchTarget.kind === LaunchKind.WINE_EXE && !this.isSteamGame(app));
  }
  private isSteamGame(app: AppDescriptor): boolean {
    try {
      const marker = app.directory + '/.steam-game.json';
      if (!fs.accessSync(marker)) return false;
      // Any marker is conservatively excluded; a verified marker only establishes Steam provenance, not cloud success.
      if (fs.lstatSync(marker).isSymbolicLink() || fs.statSync(marker).size > 8192) return true;
      const parsed = JSON.parse(fs.readTextSync(marker)) as SteamMarker;
      return parsed.source === 'steam' && Number.isInteger(parsed.appId) && parsed.appId > 0 || fs.accessSync(marker);
    } catch (_) { return true; }
  }
  private game(appId: string): AppDescriptor {
    const app = this.eligibleGames().find((candidate: AppDescriptor): boolean => candidate.id === appId);
    if (!app) throw new Error('SAVE_GAME_NOT_ELIGIBLE');
    return app;
  }
  /** Bounded suggestions only. A name or extension is evidence for a candidate, never consent to back it up. */
  private candidates(appId: string): string[] {
    this.guard();
    const root = this.canonicalDirectory(this.game(appId).directory), found: string[] = [];
    const queue: string[] = ['']; let examined = 0, incomplete = false;
    while (queue.length && examined < 96 && found.length < 12) {
      const relative = queue.shift() ?? '';
      examined++;
      try {
        const current = relative ? this.child(root, relative, true) : root;
        const names = fs.listFileSync(current);
        if (names.length > 256) { incomplete = true; continue; }
        const leaf = relative.slice(relative.lastIndexOf('/') + 1);
        let likely = /^(saves?|savedata|savegames?|saved games|存档|存档文件)$/i.test(leaf);
        for (const name of names) {
          if (name.startsWith('.')) continue;
          try {
            safeRelativeSavePath(name);
            const stat = fs.lstatSync(current + '/' + name);
            if (stat.isSymbolicLink()) continue;
            if (stat.isFile() && /\.(sav|save|lsd|rvdata2?|rpgsave)$/i.test(name)) likely = true;
            if (stat.isDirectory() && relative.split('/').length < 3 &&
              !/^(assets?|resources?|textures?|movies?|videos?|audio|sound|node_modules|cache|logs?)$/i.test(name)) {
              if (queue.length + examined >= 96) incomplete = true;
              else queue.push(relative ? relative + '/' + name : name);
            }
          } catch (_) { }
        }
        if (relative && likely) found.push(relative);
      } catch (_) { incomplete = true; }
    }
    return incomplete || queue.length > 0 ? [] : found;
  }
  private locations(context: common.UIAbilityContext): SaveLocationIndex {
    const path = this.root(context) + '/locations.json';
    if (!fs.accessSync(path)) return { schemaVersion: 1, locations: [] };
    if (fs.lstatSync(path).isSymbolicLink() || fs.statSync(path).size > 262144) throw new Error('INVALID_SAVE_LOCATION');
    const value = JSON.parse(fs.readTextSync(path)) as SaveLocationIndex;
    if (value.schemaVersion !== 1 || !Array.isArray(value.locations) || value.locations.length > 256) throw new Error('INVALID_SAVE_LOCATION');
    const ids = new Set<string>();
    for (const location of value.locations) {
      if (typeof location.appId !== 'string' || !location.appId || location.appId.length > 256 ||
        !Number.isFinite(location.updatedAt) || ids.has(location.appId)) throw new Error('INVALID_SAVE_LOCATION');
      safeRelativeSavePath(location.selectedPath); ids.add(location.appId);
    }
    return value;
  }
  configuredPath(context: common.UIAbilityContext, appId: string): string {
    this.guard();
    const location = this.locations(context).locations.find((entry: SaveLocation): boolean => entry.appId === appId);
    // Existing snapshots remain usable; the latest explicitly selected path seeds the first visit after upgrading.
    return location?.selectedPath ?? this.list(context, appId)[0]?.selectedPath ?? '';
  }
  private saveLocation(context: common.UIAbilityContext, location: SaveLocation): void {
    const index = this.locations(context);
    index.locations = index.locations.filter((entry: SaveLocation): boolean => entry.appId !== location.appId);
    if (index.locations.length >= 256) throw new Error('SAVE_QUOTA');
    index.locations.push(location);
    this.writeJson(this.root(context) + '/locations.json', JSON.stringify(index));
  }
  backupState(context: common.UIAbilityContext, appId: string): GameBackupState {
    this.guard();
    const location = this.locations(context).locations.find((entry: SaveLocation): boolean => entry.appId === appId);
    if (!this.eligibleGames().some((app: AppDescriptor): boolean => app.id === appId)) {
      return { supported: false, enabled: false, selectedPath: '', reason: '此游戏暂不支持旧柚存档备份' };
    }
    const paths = this.candidates(appId).filter((path: string, _index: number, all: string[]): boolean =>
      !all.some((parent: string): boolean => parent !== path && path.startsWith(parent + '/')));
    if (paths.length !== 1) return { supported: false, enabled: false, selectedPath: '',
      reason: paths.length ? '发现多个可能的存档位置，暂不启用备份' : '尚未识别到存档。游戏保存一次后可重新检测' };
    if (!this.recognizableFolder(appId, paths[0])) return { supported: false, enabled: false, selectedPath: '',
      reason: '尚不能可靠识别存档文件，此游戏暂不支持备份' };
    if (location?.enabled && location.selectedPath !== paths[0]) return { supported: false, enabled: false, selectedPath: '',
      reason: '游戏的存档位置发生变化，备份已暂停' };
    return { supported: true, enabled: location?.enabled === true, selectedPath: paths[0],
      reason: location?.lastStatus ?? '' };
  }
  private recognizableFolder(appId: string, selectedPath: string): boolean {
    const root = this.canonicalDirectory(this.game(appId).directory);
    const queue: string[] = [selectedPath]; let fileCount = 0, directoryCount = 0, bytes = 0, evidence = false;
    const dedicated = /^(saves?|savedata|savegames?|saved games|存档|存档文件)$/i.test(selectedPath.slice(selectedPath.lastIndexOf('/') + 1));
    while (queue.length) {
      if (++directoryCount > 128) return false;
      const current = this.child(root, queue.shift() ?? selectedPath, true);
      const names = fs.listFileSync(current);
      if (names.length > 1024) return false;
      for (const name of names) {
        safeRelativeSavePath(name);
        const relative = current.slice(root.length + 1) + '/' + name, stat = fs.lstatSync(current + '/' + name);
        if (stat.isSymbolicLink()) return false;
        if (stat.isDirectory()) { if (relative.split('/').length > 16) return false; queue.push(relative); }
        else {
          if (!stat.isFile() || ++fileCount > 1024 || stat.size > 67108864 ||
            /\.(exe|dll|com|bat|cmd|pak|pck|xp3|rpa|vpk|mp4|ogg|wav)$/i.test(name)) return false;
          bytes += stat.size; if (bytes > 268435456) return false;
          if (/\.(sav|save|lsd|rvdata2?|rpgsave)$/i.test(name) ||
            dedicated && /\.(dat|bin|json|xml|vndata|ksd|sd)$/i.test(name)) evidence = true;
        }
      }
    }
    return evidence && fileCount > 0;
  }
  private async gameIdentity(appId: string): Promise<string> {
    const app = this.game(appId), root = this.canonicalDirectory(app.directory);
    const executable = app.launchTarget.executable.replace(/\\/g, '/');
    if (!executable.startsWith(root + '/')) throw new Error('UNSAFE_SAVE_PATH');
    const path = this.child(root, executable.slice(root.length + 1));
    const identity = await this.digest(path, root);
    return identity.size + ':' + identity.sha256;
  }
  async setBackupEnabled(context: common.UIAbilityContext, appId: string, enabled: boolean): Promise<void> {
    this.guard();
    if (this.busy || this.automaticBusy) throw new Error('BUSY');
    const old = this.locations(context).locations.find((entry: SaveLocation): boolean => entry.appId === appId);
    if (!enabled) {
      if (old) { old.enabled = false; old.updatedAt = Date.now(); old.lastStatus = '自动备份已关闭'; this.saveLocation(context, old); }
      return;
    }
    const state = this.backupState(context, appId);
    if (!state.supported) throw new Error('SAVE_NOT_RECOGNIZED');
    const identity = await this.gameIdentity(appId);
    this.saveLocation(context, { appId: appId, selectedPath: state.selectedPath, updatedAt: Date.now(),
      gameTitle: this.game(appId).title, gameIdentity: identity, enabled: true, lastStatus: '等待首次备份' });
  }
  /** Library scans can temporarily lose access. Pause automatic work, retain history, and require explicit re-enabling on return. */
  reconcileCatalog(context: common.UIAbilityContext): void {
    this.guard();
    const installed = this.eligibleGames();
    for (const entry of this.locations(context).locations) {
      if (entry.enabled && !installed.some((app: AppDescriptor): boolean => app.id === entry.appId)) {
        entry.enabled = false; entry.lastStatus = '游戏已移除或不可访问，已有备份仍保留'; this.saveLocation(context, entry);
      }
    }
  }
  summaries(context: common.UIAbilityContext): BackupGameSummary[] {
    this.guard();
    const installed = this.eligibleGames(), snapshots = this.list(context, ''), locations = this.locations(context).locations;
    const ids = new Set<string>(); snapshots.forEach((entry: SaveManifest): void => { ids.add(entry.appId); });
    locations.forEach((entry: SaveLocation): void => { ids.add(entry.appId); });
    const result: BackupGameSummary[] = [];
    ids.forEach((appId: string): void => {
      const app = installed.find((entry: AppDescriptor): boolean => entry.id === appId);
      const location = locations.find((entry: SaveLocation): boolean => entry.appId === appId);
      const versions = snapshots.filter((entry: SaveManifest): boolean => entry.appId === appId);
      result.push({ appId: appId, title: app?.title ?? location?.gameTitle ?? versions[0]?.gameTitle ?? '已移除的游戏',
        installed: !!app, enabled: !!app && location?.enabled === true,
        selectedPath: location?.selectedPath ?? versions[0]?.selectedPath ?? '', count: versions.length,
        latestAt: versions[0]?.createdAt ?? 0,
        totalBytes: versions.reduce((sum: number, version: SaveManifest): number => sum + version.files.reduce((total: number, file: SaveManifestEntry): number => total + file.size, 0), 0),
        status: app ? location?.lastStatus ?? '' : '游戏已移除或不可访问，备份仍保留' });
    });
    return result.sort((left: BackupGameSummary, right: BackupGameSummary): number => right.latestAt - left.latestAt);
  }
  async runAutomatic(context: common.UIAbilityContext): Promise<void> {
    if (this.automaticBusy || this.busy || !DeviceCapabilityPolicy.supportsValueAddedFeatures()) return;
    try { this.ensureStopped(); } catch (_) { return; }
    this.automaticBusy = true;
    try {
      this.reconcileCatalog(context);
      for (const entry of this.locations(context).locations) {
        if (!entry.enabled) continue;
        try {
          const state = this.backupState(context, entry.appId);
          if (!state.supported || state.selectedPath !== entry.selectedPath) throw new Error('SAVE_NOT_RECOGNIZED');
          if (await this.gameIdentity(entry.appId) !== entry.gameIdentity) throw new Error('SAVE_IDENTITY_CHANGED');
          const before = this.list(context, entry.appId)[0]?.snapshotId ?? '';
          const snapshot = await this.create(context, entry.appId, entry.selectedPath);
          entry.lastStatus = snapshot.snapshotId === before ? '存档没有变化' : '自动备份完成';
        } catch (error) {
          const message = (error as Error).message;
          entry.lastStatus = message === 'SAVE_ENGINE_RUNNING' || message === 'BUSY' ? '等待运行环境空闲后备份' :
            message === 'SAVE_IDENTITY_CHANGED' ? '游戏主程序已变化，请重新开启备份' :
            message === 'SAVE_NOT_RECOGNIZED' ? '存档位置无法可靠识别，备份已暂停' :
            message === 'SAVE_STORAGE_FULL' ? '本地备份空间已满，请清理不需要的版本' : '备份未完成，旧版本仍保留';
          if (message === 'SAVE_IDENTITY_CHANGED' || message === 'SAVE_NOT_RECOGNIZED') entry.enabled = false;
        }
        this.saveLocation(context, entry);
      }
    } catch (_) { /* Startup and game launch never depend on backup I/O succeeding. */ }
    finally { this.automaticBusy = false; }
  }
  hasInterruptedRestore(context: common.UIAbilityContext): boolean {
    return fs.accessSync(this.root(context) + '/restore-journal.json');
  }
  async preview(context: common.UIAbilityContext, appId: string, selectedPath: string, snapshotId: string = ''): Promise<SavePreview> {
    this.guard(); this.ensureStopped(); safeRelativeSavePath(selectedPath);
    if (this.busy) throw new Error('BUSY');
    this.busy = true;
    try {
      const root = this.canonicalDirectory(this.game(appId).directory);
      const exists = fs.accessSync(root + '/' + selectedPath);
      if (!snapshotId && !exists) throw new Error('SAVE_DIRECTORY_MISSING');
      const current = exists ? await this.scan(root, selectedPath) : [];
      validateSaveEntries(current, !!snapshotId);
      if (!snapshotId) return savePreview(selectedPath, current, current);
      const snapshot = this.list(context, appId).find((entry: SaveManifest): boolean => entry.snapshotId === snapshotId);
      if (!snapshot || snapshot.selectedPath !== selectedPath) throw new Error('INVALID_SAVE_MANIFEST');
      if (snapshot.gameIdentity && snapshot.gameIdentity !== await this.gameIdentity(appId)) throw new Error('SAVE_IDENTITY_CHANGED');
      const source = this.canonicalDirectory(this.root(context) + '/' + snapshotId + '/files');
      for (const file of snapshot.files) {
        this.ensureStopped();
        const digest = await this.digest(this.child(source, file.relativePath), source);
        if (digest.sha256 !== file.sha256 || digest.size !== file.size) throw new Error('SAVE_HASH_MISMATCH');
      }
      return savePreview(selectedPath, current, snapshot.files, !exists);
    } finally { this.busy = false; }
  }
  private async digest(path: string, expectedRoot: string, copyTo: string = ''): Promise<FileDigest> {
    const input = fs.openSync(path, fs.OpenMode.READ_ONLY | fs.OpenMode.NOFOLLOW);
    let output: fs.File | null = null;
    try {
      if (!input.path.startsWith(expectedRoot + '/')) throw new Error('UNSAFE_SAVE_PATH');
      const before = fs.statSync(input.fd);
      if (!before.isFile() || before.size > 67108864) throw new Error('SAVE_QUOTA');
      if (copyTo) output = fs.openSync(copyTo, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.NOFOLLOW);
      const md = cryptoFramework.createMd('SHA256');
      const buffer = new ArrayBuffer(65536);
      let total = 0;
      while (true) {
        const count = fs.readSync(input.fd, buffer);
        if (count === 0) break;
        total += count;
        if (total > 67108864) throw new Error('SAVE_QUOTA');
        const bytes = new Uint8Array(buffer.slice(0, count));
        await md.update({ data: bytes });
        if (output) {
          let written = 0;
          while (written < count) {
            const step = fs.writeSync(output.fd, bytes.buffer.slice(written));
            if (step <= 0) throw new Error('SAVE_WRITE_FAILED');
            written += step;
          }
        }
      }
      const after = fs.statSync(input.fd);
      if (before.size !== after.size || before.mtime !== after.mtime || total !== before.size ||
        input.path !== path) throw new Error('SAVE_CHANGED');
      if (output) fs.fsyncSync(output.fd);
      const hash = await md.digest();
      return { size: total, modifiedAt: before.mtime,
        sha256: Array.from(hash.data).map((value: number): string => value.toString(16).padStart(2, '0')).join('') };
    } finally { if (output) fs.closeSync(output); fs.closeSync(input); }
  }
  private async scan(root: string, selected: string, relative: string = '', entries: SaveManifestEntry[] = [],
    visited: string[] = []): Promise<SaveManifestEntry[]> {
    this.ensureStopped();
    const current = relative ? this.child(root, selected + '/' + relative, true) : this.child(root, selected, true);
    visited.push(current);
    if (visited.length > 1024 || relative.split('/').length > 24) throw new Error('SAVE_QUOTA');
    const names = fs.listFileSync(current);
    if (names.length > 2048) throw new Error('SAVE_QUOTA');
    for (const name of names.sort()) {
      const childRelative = relative ? relative + '/' + name : name;
      safeRelativeSavePath(childRelative);
      const path = this.child(root, selected + '/' + childRelative);
      const stat = fs.lstatSync(path);
      if (stat.isDirectory()) await this.scan(root, selected, childRelative, entries, visited);
      else {
        if (!stat.isFile() || entries.length >= 1024) throw new Error('SAVE_QUOTA');
        const digest = await this.digest(path, root);
        entries.push({ relativePath: childRelative, size: digest.size, modifiedAt: digest.modifiedAt, sha256: digest.sha256 });
        validateSaveEntries(entries);
      }
    }
    return entries;
  }
  private writeJson(path: string, value: string): void {
    const temporary = path + '.tmp-' + util.generateRandomUUID();
    const file = fs.openSync(temporary, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.NOFOLLOW);
    try {
      const bytes = new util.TextEncoder().encodeInto(value);
      let written = 0;
      while (written < bytes.length) {
        const count = fs.writeSync(file.fd, bytes.buffer.slice(written));
        if (count <= 0) throw new Error('SAVE_WRITE_FAILED');
        written += count;
      }
      fs.fsyncSync(file.fd);
    } finally { fs.closeSync(file); }
    fs.renameSync(temporary, path);
  }
  private ensureParents(root: string, relative: string): void {
    const parts = safeRelativeSavePath(relative).split('/');
    parts.pop(); let path = root;
    for (const part of parts) {
      path += '/' + part;
      if (!fs.accessSync(path)) fs.mkdirSync(path);
      if (fs.lstatSync(path).isSymbolicLink() || !fs.statSync(path).isDirectory()) throw new Error('UNSAFE_SAVE_PATH');
    }
  }
  private async snapshotInternal(context: common.UIAbilityContext, appId: string, selectedPath: string, allowEmpty: boolean = false, expected?: SavePreview): Promise<SaveManifest> {
    this.ensureStopped(); safeRelativeSavePath(selectedPath);
    if (this.hasInterruptedRestore(context)) throw new Error('SAVE_RECOVERY_REQUIRED');
    const app = this.game(appId), approvedRoot = this.canonicalDirectory(app.directory);
    const identity = await this.gameIdentity(appId);
    this.child(approvedRoot, selectedPath, true);
    const first = await this.scan(approvedRoot, selectedPath);
    validateSaveEntries(first, allowEmpty);
    if (expected && (expected.selectedPath !== selectedPath || !sameSaveContent(first, expected.files))) throw new Error('SAVE_CHANGED');
    await new Promise<void>((resolve: () => void): void => { setTimeout(resolve, 350); });
    const second = await this.scan(approvedRoot, selectedPath);
    if (JSON.stringify(first) !== JSON.stringify(second)) throw new Error('SAVE_CHANGED');
    if (!allowEmpty) {
      const latest = this.list(context, appId).find((entry: SaveManifest): boolean => entry.selectedPath === selectedPath);
      if (latest && latest.gameIdentity === identity && sameSaveContent(latest.files, second)) {
        let intact = true;
        try {
          const backupRoot = this.canonicalDirectory(this.root(context) + '/' + latest.snapshotId + '/files');
          for (const file of latest.files) {
            const saved = await this.digest(this.child(backupRoot, file.relativePath), backupRoot);
            if (saved.sha256 !== file.sha256 || saved.size !== file.size) { intact = false; break; }
          }
        } catch (_) { intact = false; }
        this.ensureStopped();
        if (intact) return latest;
      }
    }
    const id = util.generateRandomUUID();
    const base = this.root(context), staging = base + '/pending-' + id;
    const storedBytes = this.list(context, '').reduce((total: number, version: SaveManifest): number =>
      total + version.files.reduce((sum: number, file: SaveManifestEntry): number => sum + file.size, 0), 0);
    if (storedBytes + second.reduce((sum: number, file: SaveManifestEntry): number => sum + file.size, 0) > 1073741824) throw new Error('SAVE_STORAGE_FULL');
    fs.mkdirSync(staging);
    try {
      fs.mkdirSync(staging + '/files');
      for (const entry of second) {
        this.ensureStopped();
        this.ensureParents(staging + '/files', entry.relativePath);
        const source = this.child(approvedRoot, selectedPath + '/' + entry.relativePath);
        const copied = await this.digest(source, approvedRoot, staging + '/files/' + entry.relativePath);
        if (copied.sha256 !== entry.sha256 || copied.size !== entry.size || copied.modifiedAt !== entry.modifiedAt) {
          throw new Error('SAVE_CHANGED');
        }
      }
      const finalScan = await this.scan(approvedRoot, selectedPath);
      if (JSON.stringify(finalScan) !== JSON.stringify(second)) throw new Error('SAVE_CHANGED');
      if (identity !== await this.gameIdentity(appId)) throw new Error('SAVE_IDENTITY_CHANGED');
      const manifest: SaveManifest = { schemaVersion: 1, snapshotId: id, appId: app.id, gameTitle: app.title, gameIdentity: identity,
        rootAlias: 'install', selectedPath: selectedPath, createdAt: Date.now(), origin: allowEmpty ? 'before-restore' : 'manual', files: second };
      this.ensureStopped();
      this.writeJson(staging + '/manifest.json', JSON.stringify(manifest));
      fs.renameSync(staging, base + '/' + id);
      return manifest;
    } catch (error) {
      try { if (fs.accessSync(staging)) this.removeTransferTree(staging); } catch (_) { }
      throw new Error((error as Error).message);
    }
  }
  async create(context: common.UIAbilityContext, appId: string, selectedPath: string, expected?: SavePreview): Promise<SaveManifest> {
    this.guard();
    if (this.busy) throw new Error('BUSY');
    this.busy = true;
    try { return await this.snapshotInternal(context, appId, selectedPath, false, expected); } finally { this.busy = false; }
  }
  list(context: common.UIAbilityContext, appId: string): SaveManifest[] {
    this.guard();
    const base = this.root(context), result: SaveManifest[] = [];
    for (const id of fs.listFileSync(base)) {
      if (!/^[a-f0-9-]{36}$/.test(id)) continue;
      try {
        const path = this.child(base, id + '/manifest.json');
        if (fs.statSync(path).size > 524288) continue;
        const manifest = JSON.parse(fs.readTextSync(path)) as SaveManifest;
        if ((!appId || manifest.appId === appId) && manifest.snapshotId === id && manifest.schemaVersion === 1 && manifest.rootAlias === 'install') {
          safeRelativeSavePath(manifest.selectedPath); validateSaveEntries(manifest.files, true); result.push(manifest);
        }
      } catch (_) { }
    }
    return result.sort((a: SaveManifest, b: SaveManifest): number => b.createdAt - a.createdAt);
  }
  deleteSnapshot(context: common.UIAbilityContext, snapshotId: string, appId: string): void {
    this.guard();
    if (this.busy || this.automaticBusy) throw new Error('BUSY');
    if (this.hasInterruptedRestore(context)) throw new Error('SAVE_RECOVERY_REQUIRED');
    if (!this.list(context, appId).some((entry: SaveManifest): boolean => entry.snapshotId === snapshotId)) throw new Error('INVALID_SAVE_MANIFEST');
    const directory = this.child(this.root(context), snapshotId, true);
    // Only private snapshot files are removed. Game installation and active saves are never deletion targets.
    this.removeTransferTree(directory);
  }
  recoverInterrupted(context: common.UIAbilityContext): void {
    this.guard(); this.ensureStopped();
    if (this.busy) throw new Error('BUSY');
    const journalPath = this.root(context) + '/restore-journal.json';
    if (!fs.accessSync(journalPath)) return;
    if (fs.lstatSync(journalPath).isSymbolicLink() || fs.statSync(journalPath).size > 4096) throw new Error('SAVE_RECOVERY_REQUIRED');
    const journal = JSON.parse(fs.readTextSync(journalPath)) as RestoreJournal;
    safeRelativeSavePath(journal.selectedPath);
    if (!/^\.vp-restore-[a-f0-9-]{36}$/.test(journal.stagedName) ||
      !/^\.vp-before-[a-f0-9-]{36}$/.test(journal.previousName)) throw new Error('SAVE_RECOVERY_REQUIRED');
    const root = this.canonicalDirectory(this.game(journal.appId).directory);
    const split = journal.selectedPath.lastIndexOf('/');
    const parent = split < 0 ? root : this.child(root, journal.selectedPath.slice(0, split), true);
    const selected = root + '/' + journal.selectedPath;
    const previous = parent + '/' + journal.previousName;
    if (!fs.accessSync(selected)) {
      if (fs.accessSync(previous)) {
        if (fs.lstatSync(previous).isSymbolicLink()) throw new Error('SAVE_RECOVERY_REQUIRED');
        this.ensureStopped(); fs.renameSync(previous, selected);
      } else if (journal.originalExists !== false) throw new Error('SAVE_RECOVERY_REQUIRED');
    } else {
      this.child(root, journal.selectedPath, true);
    }
    // If both directories exist, a swap had completed. Keep both; no user data is deleted.
    fs.unlinkSync(journalPath);
  }
  private cloudDirectory(context: common.UIAbilityContext, id: string): string {
    if (!/^[a-f0-9-]{36}$/.test(id)) throw new Error('INVALID_SAVE_MANIFEST');
    return this.child(this.root(context), 'transfer-' + id, true);
  }
  private writeBytes(path: string, bytes: Uint8Array): void {
    const file = fs.openSync(path, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.NOFOLLOW | fs.OpenMode.TRUNC);
    try {
      let written = 0;
      while (written < bytes.length) {
        const count = fs.writeSync(file.fd, bytes.buffer.slice(written));
        if (count <= 0) throw new Error('SAVE_WRITE_FAILED');
        written += count;
      }
      fs.fsyncSync(file.fd);
    } finally { fs.closeSync(file); }
  }
  private async bytesHash(bytes: Uint8Array): Promise<string> {
    const md = cryptoFramework.createMd('SHA256');
    await md.update({ data: bytes });
    const result = await md.digest();
    return Array.from(result.data).map((value: number): string => value.toString(16).padStart(2, '0')).join('');
  }
  /** Pack concatenated file bytes into bounded blocks; no archive extraction or device paths reach the cloud. */
  async prepareCloudUpload(context: common.UIAbilityContext, snapshotId: string, appId: string, assertCurrent: () => void): Promise<CloudUploadPlan> {
    this.guard();
    if (this.busy) throw new Error('BUSY');
    this.busy = true;
    const id = util.generateRandomUUID();
    try {
      assertCurrent();
      const snapshot = this.list(context, appId).find((item: SaveManifest): boolean => item.snapshotId === snapshotId);
      if (!snapshot) throw new Error('INVALID_SAVE_MANIFEST');
      const directory = this.root(context) + '/transfer-' + id;
      fs.mkdirSync(directory);
      const sourceRoot = this.canonicalDirectory(this.root(context) + '/' + snapshotId + '/files');
      const files: CloudSaveFile[] = [], chunks: CloudSaveChunk[] = [];
      let offset = 0, used = 0;
      let packed = new Uint8Array(CLOUD_CHUNK_BYTES);
      for (const entry of snapshot.files) {
        assertCurrent();
        const source = this.child(sourceRoot, entry.relativePath), file = fs.openSync(source, fs.OpenMode.READ_ONLY | fs.OpenMode.NOFOLLOW);
        const md = cryptoFramework.createMd('SHA256');
        let size = 0;
        try {
          if (!file.path.startsWith(sourceRoot + '/')) throw new Error('UNSAFE_SAVE_PATH');
          while (true) {
            const buffer = new ArrayBuffer(65536), count = fs.readSync(file.fd, buffer);
            if (count === 0) break;
            size += count;
            if (size > entry.size) throw new Error('SAVE_CHANGED');
            const bytes = new Uint8Array(buffer.slice(0, count));
            await md.update({ data: bytes });
            assertCurrent();
            let read = 0;
            while (read < bytes.length) {
              const step = Math.min(CLOUD_CHUNK_BYTES - used, bytes.length - read);
              packed.set(bytes.slice(read, read + step), used); used += step; read += step;
              if (used === CLOUD_CHUNK_BYTES) {
                const hash = await this.bytesHash(packed);
                assertCurrent();
                this.writeBytes(directory + '/' + chunks.length, packed);
                chunks.push({ index: chunks.length, size: used, sha256: hash });
                packed = new Uint8Array(CLOUD_CHUNK_BYTES); used = 0;
              }
            }
          }
        } finally { fs.closeSync(file); }
        const digest = await md.digest();
        const hash = Array.from(digest.data).map((value: number): string => value.toString(16).padStart(2, '0')).join('');
        if (size !== entry.size || hash !== entry.sha256) throw new Error('SAVE_HASH_MISMATCH');
        files.push({ relativePath: entry.relativePath, size: size, sha256: hash, offset: offset }); offset += size;
      }
      if (used > 0) {
        const tail = packed.slice(0, used), hash = await this.bytesHash(tail);
        this.writeBytes(directory + '/' + chunks.length, tail);
        chunks.push({ index: chunks.length, size: used, sha256: hash });
      }
      const manifest: CloudSaveManifest = { schemaVersion: 1, files: files, chunks: chunks, totalBytes: offset };
      validateCloudSaveManifest(manifest);
      assertCurrent();
      this.writeJson(directory + '/cloud.json', JSON.stringify(manifest));
      return { transferId: id, manifest: manifest };
    } catch (error) {
      try { this.cleanupCloudTransfer(context, id); } catch (_) { }
      throw new Error((error as Error).message);
    } finally { this.busy = false; }
  }
  async readCloudChunk(context: common.UIAbilityContext, transferId: string, chunk: CloudSaveChunk): Promise<ArrayBuffer> {
    this.guard();
    if (!Number.isInteger(chunk.index) || chunk.index < 0 || chunk.index >= 64 || chunk.size < 1 || chunk.size > CLOUD_CHUNK_BYTES) {
      throw new Error('INVALID_SAVE_MANIFEST');
    }
    const directory = this.cloudDirectory(context, transferId), path = this.child(directory, '' + chunk.index);
    if (fs.statSync(path).size !== chunk.size) throw new Error('SAVE_HASH_MISMATCH');
    const file = fs.openSync(path, fs.OpenMode.READ_ONLY | fs.OpenMode.NOFOLLOW);
    const bytes = new Uint8Array(chunk.size); let read = 0;
    try {
      while (read < bytes.length) {
        const buffer = new ArrayBuffer(Math.min(65536, bytes.length - read)), count = fs.readSync(file.fd, buffer);
        if (count <= 0) throw new Error('SAVE_HASH_MISMATCH');
        bytes.set(new Uint8Array(buffer.slice(0, count)), read); read += count;
      }
    } finally { fs.closeSync(file); }
    if (await this.bytesHash(bytes) !== chunk.sha256) throw new Error('SAVE_HASH_MISMATCH');
    return bytes.buffer;
  }
  beginCloudImport(context: common.UIAbilityContext, manifest: CloudSaveManifest, appId: string, selectedPath: string): string {
    this.guard(); this.game(appId); safeRelativeSavePath(selectedPath); validateCloudSaveManifest(manifest);
    const id = util.generateRandomUUID(), directory = this.root(context) + '/transfer-' + id;
    fs.mkdirSync(directory);
    const state: CloudImportState = { manifest: manifest, appId: appId, selectedPath: selectedPath };
    try { this.writeJson(directory + '/import.json', JSON.stringify(state)); return id; }
    catch (error) {
      try { this.cleanupCloudTransfer(context, id); } catch (_) { }
      throw new Error((error as Error).message);
    }
  }
  async writeCloudChunk(context: common.UIAbilityContext, transferId: string, chunk: CloudSaveChunk, buffer: ArrayBuffer): Promise<void> {
    this.guard();
    if (buffer.byteLength !== chunk.size || chunk.size > CLOUD_CHUNK_BYTES || !Number.isInteger(chunk.index) || chunk.index < 0 || chunk.index >= 64 ||
      await this.bytesHash(new Uint8Array(buffer)) !== chunk.sha256) throw new Error('SAVE_HASH_MISMATCH');
    this.writeBytes(this.cloudDirectory(context, transferId) + '/' + chunk.index, new Uint8Array(buffer));
  }
  async finishCloudImport(context: common.UIAbilityContext, transferId: string, assertCurrent: () => void): Promise<SaveManifest> {
    this.guard();
    if (this.busy) throw new Error('BUSY');
    this.busy = true;
    try {
      const directory = this.cloudDirectory(context, transferId);
      const statePath = this.child(directory, 'import.json');
      if (fs.statSync(statePath).size > 524288) throw new Error('INVALID_SAVE_MANIFEST');
      const state = JSON.parse(fs.readTextSync(statePath)) as CloudImportState;
      validateCloudSaveManifest(state.manifest); this.game(state.appId); safeRelativeSavePath(state.selectedPath);
      fs.mkdirSync(directory + '/files');
      let chunkIndex = 0, chunkOffset = 0, packed = new Uint8Array(0);
      const entries: SaveManifestEntry[] = [];
      for (const entry of state.manifest.files) {
        assertCurrent();
        this.ensureParents(directory + '/files', entry.relativePath);
        const target = directory + '/files/' + entry.relativePath;
        const file = fs.openSync(target, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.NOFOLLOW);
        const md = cryptoFramework.createMd('SHA256'); let written = 0;
        try {
          while (written < entry.size) {
            if (chunkOffset === packed.length) {
              packed = new Uint8Array(await this.readCloudChunk(context, transferId, state.manifest.chunks[chunkIndex++]));
              assertCurrent(); chunkOffset = 0;
            }
            const step = Math.min(entry.size - written, packed.length - chunkOffset);
            const bytes = packed.slice(chunkOffset, chunkOffset + step);
            await md.update({ data: bytes });
            assertCurrent();
            let position = 0;
            while (position < bytes.length) {
              const count = fs.writeSync(file.fd, bytes.buffer.slice(position));
              if (count <= 0) throw new Error('SAVE_WRITE_FAILED'); position += count;
            }
            written += step; chunkOffset += step;
          }
          fs.fsyncSync(file.fd);
        } finally { fs.closeSync(file); }
        const digest = await md.digest();
        const hash = Array.from(digest.data).map((value: number): string => value.toString(16).padStart(2, '0')).join('');
        if (hash !== entry.sha256) throw new Error('SAVE_HASH_MISMATCH');
        entries.push({ relativePath: entry.relativePath, size: entry.size, sha256: hash, modifiedAt: Date.now() });
      }
      const id = util.generateRandomUUID();
      const manifest: SaveManifest = { schemaVersion: 1, snapshotId: id, appId: state.appId, rootAlias: 'install',
        selectedPath: state.selectedPath, createdAt: Date.now(), gameTitle: this.game(state.appId).title, gameIdentity: await this.gameIdentity(state.appId), origin: 'cloud-download', files: entries };
      assertCurrent();
      for (const chunk of state.manifest.chunks) fs.unlinkSync(this.child(directory, '' + chunk.index));
      fs.unlinkSync(this.child(directory, 'import.json'));
      this.writeJson(directory + '/manifest.json', JSON.stringify(manifest));
      fs.renameSync(directory, this.root(context) + '/' + id); return manifest;
    } finally { this.busy = false; }
  }
  cleanupCloudTransfer(context: common.UIAbilityContext, transferId: string): void {
    this.guard();
    if (!/^[a-f0-9-]{36}$/.test(transferId)) throw new Error('INVALID_SAVE_MANIFEST');
    const path = this.root(context) + '/transfer-' + transferId;
    if (!fs.accessSync(path)) return;
    const directory = this.cloudDirectory(context, transferId);
    this.removeTransferTree(directory);
  }
  cleanupCloudTransfers(context: common.UIAbilityContext): void {
    this.guard();
    if (this.busy) throw new Error('BUSY');
    for (const name of fs.listFileSync(this.root(context))) {
      if (/^transfer-[a-f0-9-]{36}$/.test(name)) this.cleanupCloudTransfer(context, name.slice(9));
    }
  }
  private removeTransferTree(directory: string): void {
    for (const name of fs.listFileSync(directory)) {
      safeRelativeSavePath(name);
      const path = directory + '/' + name, stat = fs.lstatSync(path);
      if (stat.isDirectory() && !stat.isSymbolicLink()) this.removeTransferTree(path);
      else fs.unlinkSync(path);
    }
    fs.rmdirSync(directory);
  }
  async restore(context: common.UIAbilityContext, snapshotId: string, appId: string, expected?: SavePreview): Promise<void> {
    this.guard(); this.ensureStopped();
    if (this.busy) throw new Error('BUSY');
    this.busy = true;
    try {
      const manifest = this.list(context, appId).find((item: SaveManifest): boolean => item.snapshotId === snapshotId);
      if (!manifest) throw new Error('INVALID_SAVE_MANIFEST');
      if (manifest.gameIdentity && manifest.gameIdentity !== await this.gameIdentity(appId)) throw new Error('SAVE_IDENTITY_CHANGED');
      const approvedRoot = this.canonicalDirectory(this.game(appId).directory);
      this.ensureParents(approvedRoot, manifest.selectedPath);
      const originalExists = fs.accessSync(approvedRoot + '/' + manifest.selectedPath);
      const selected = originalExists ? this.child(approvedRoot, manifest.selectedPath, true) : approvedRoot + '/' + manifest.selectedPath;
      if (expected) {
        const current = originalExists ? await this.scan(approvedRoot, manifest.selectedPath) : [];
        if (expected.selectedPath !== manifest.selectedPath || expected.destinationMissing === originalExists ||
          !sameSaveContent(expected.files, current)) throw new Error('SAVE_CHANGED');
      }
      // An unresolved interrupted restore is never overwritten. The intact previous directory stays available.
      const journalPath = this.root(context) + '/restore-journal.json';
      if (fs.accessSync(journalPath)) throw new Error('SAVE_RECOVERY_REQUIRED');
      const beforeRestore = originalExists ? await this.snapshotInternal(context, appId, manifest.selectedPath, true) : null;
      const parent = selected.slice(0, selected.lastIndexOf('/'));
      const id = util.generateRandomUUID();
      const stagedName = '.vp-restore-' + id, previousName = '.vp-before-' + id;
      const staged = parent + '/' + stagedName, previous = parent + '/' + previousName;
      fs.mkdirSync(staged);
      const sourceRoot = this.canonicalDirectory(this.root(context) + '/' + snapshotId + '/files');
      for (const entry of manifest.files) {
        this.ensureStopped(); this.ensureParents(staged, entry.relativePath);
        const source = this.child(sourceRoot, entry.relativePath);
        const copied = await this.digest(source, sourceRoot, staged + '/' + entry.relativePath);
        if (copied.sha256 !== entry.sha256 || copied.size !== entry.size) throw new Error('SAVE_HASH_MISMATCH');
      }
      this.ensureStopped();
      if (beforeRestore) {
        const currentFiles = await this.scan(approvedRoot, manifest.selectedPath);
        if (JSON.stringify(currentFiles) !== JSON.stringify(beforeRestore.files)) throw new Error('SAVE_CHANGED');
      } else if (fs.accessSync(selected)) throw new Error('SAVE_CHANGED');
      const journal: RestoreJournal = { appId: appId, selectedPath: manifest.selectedPath, stagedName: stagedName,
        previousName: previousName, originalExists: originalExists, phase: 'prepared' };
      if (manifest.gameIdentity && manifest.gameIdentity !== await this.gameIdentity(appId)) throw new Error('SAVE_IDENTITY_CHANGED');
      this.writeJson(journalPath, JSON.stringify(journal));
      this.ensureStopped();
      if (originalExists) fs.renameSync(selected, previous);
      try {
        this.ensureStopped(); fs.renameSync(staged, selected);
      } catch (error) {
        if (originalExists) fs.renameSync(previous, selected);
        fs.unlinkSync(journalPath);
        throw new Error((error as Error).message);
      }
      // Retain the pre-restore tree as well as the snapshot. Never delete game data to finish this operation.
      fs.unlinkSync(journalPath);
    } finally { this.busy = false; }
  }
}
