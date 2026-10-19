import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import preferences from '@ohos.data.preferences';
import util from '@ohos.util';
import { AppDescriptor, AppSettings, InputProfile, cloneAppSettings, cloneInputProfile } from '../model/AppModels';
import { PortableGame, PortableEntry, PortableInput, validatePortableGame, configurationRelativePath } from '../model/UserConfiguration';
import { AppSettingsStore } from './AppSettingsStore';
import { InputProfileService } from './InputProfileService';
import { ConfigurationFiles as files } from './ConfigurationFiles';

interface PortableRecord { directory: string; appId: string; executable: string; executableSha256: string; contentFingerprint?: string; game: PortableGame; blocked: boolean; }
export class PortableGameConfigurationService {
  private static instance: PortableGameConfigurationService | null = null;
  private store: preferences.Preferences | null = null;
  private records: PortableRecord[] = [];
  private chain: Promise<void> = Promise.resolve();
  private appVersion: string = '1.4.9';
  private incompatible: boolean = false;
  dedicatedProfileId(appId: string): string {
    const record = this.records.find((r: PortableRecord): boolean => r.appId === appId);
    const entry = record?.game.entries.find((e: PortableEntry): boolean => e.executable === record.executable);
    return record && entry ? 'app-game-' + record.game.gameUid + '-' + entry.entryUid : 'app-' + appId;
  }
  static getInstance(): PortableGameConfigurationService {
    if (!PortableGameConfigurationService.instance) PortableGameConfigurationService.instance = new PortableGameConfigurationService(); return PortableGameConfigurationService.instance;
  }
  async initialize(context: common.UIAbilityContext): Promise<void> {
    if (this.store) return;
    await InputProfileService.getInstance().initialize(context);
    this.store = await preferences.getPreferences(context, 'pomelo_game_config'); this.reload();
    AppSettingsStore.getInstance().setPortableWriter((value: AppSettings): Promise<void> => this.saved(value));
    InputProfileService.getInstance().subscribeChanges((id: string, removed: boolean, source: string): void => {
      if (source !== 'local') return;
      this.chain = this.chain.catch((): void => {}).then(async (): Promise<void> => {
        for (const record of this.records.filter((r: PortableRecord): boolean => AppSettingsStore.getInstance().getAppSettings(r.appId).inputProfileId === id)) {
          const value = cloneAppSettings(AppSettingsStore.getInstance().getAppSettings(record.appId));
          if (removed) { value.inputProfileId = 'default'; await AppSettingsStore.getInstance().saveAppSettings(value, 'portable'); }
          await this.saveRecord(record, value);
        }
      });
    });
  }
  reload(): void {
    if (!this.store) return;
    this.incompatible = false;
    try { this.records = JSON.parse(this.store.getSync('index_v1', '[]') as string) as PortableRecord[];
      if (!Array.isArray(this.records) || this.records.length > 2048) throw new Error('CONFIG_INVALID');
      this.records.forEach((r: PortableRecord): void => validatePortableGame(r.game));
    } catch (_) { this.records = []; this.incompatible = true; }
  }
  async flush(): Promise<void> { await this.chain; }
  private relative(path: string, root: string): string {
    const p = path.replace(/\\/g, '/'), r = root.replace(/\\/g, '/').replace(/\/+$/, '');
    if (p === r) return '';
    if (!p.startsWith(r + '/')) return '';
    return configurationRelativePath(p.slice(r.length + 1));
  }
  private async persist(): Promise<void> {
    if (!this.store) return;
    if (this.records.length > 2048) throw new Error('CONFIG_QUOTA');
    const raw = JSON.stringify(this.records.slice().sort((a: PortableRecord, b: PortableRecord): number => a.appId.localeCompare(b.appId)));
    if (this.store.getSync('index_v1', '') === raw) return;
    await this.store.put('index_v1', raw); await this.store.flush();
  }
  private async contentFingerprint(directory: string): Promise<string> {
    try {
      const names = fs.listFileSync(directory).sort(); if (names.length > 512) return '';
      const parts: string[] = [];
      for (const name of names) {
        if (!/\.(ini|dat|xp3|pfs|pak|arc|rgssad|rgss2a|rgss3a|bin)$/i.test(name)) continue;
        const path = directory + '/' + name, stat = fs.lstatSync(path);
        if (!stat.isFile() || stat.isSymbolicLink() || stat.size < 1 || stat.size > 8388608) continue;
        parts.push(name + ':' + stat.size + ':' + await files.fileHash(path));
        if (parts.length >= 4) break;
      }
      // Generic engine EXEs are shared by different games. Never match by the EXE alone.
      return parts.length ? await files.hash(files.bytes(JSON.stringify(parts))) : '';
    } catch (_) { return ''; }
  }
  /** Called before catalog settings are resolved; directory content wins over account fallback. */
  async discover(directory: string, executable: string, oldId: string): Promise<string> {
    if (!this.store || !executable || this.incompatible) return oldId;
    let relative = this.relative(executable, directory); if (!relative) return oldId;
    const existing = this.records.find((r: PortableRecord): boolean => r.directory === directory && r.executable === relative) ??
      this.records.find((r: PortableRecord): boolean => r.directory === directory);
    const sidecar = directory + '/.pomelo/game.json'; let game: PortableGame | null = null, blocked = false;
    try { if (fs.accessSync(sidecar)) { game = JSON.parse(files.text(files.read(sidecar))) as PortableGame; validatePortableGame(game); } }
    catch (_) { blocked = true; game = null; }
    if (blocked) {
      if (existing) { existing.blocked = true; await this.persist(); return existing.appId; }
      // Do not overwrite an unknown/corrupt sidecar with a new identity or defaults.
      return oldId;
    }
    let digest = existing?.executableSha256 ?? '';
    let fingerprint = existing?.contentFingerprint;
    const hashedExecutable = existing?.executable ?? relative;
    if (!game && !existing) {
      try { digest = await files.fileHash(executable); } catch (_) {}
      fingerprint = await this.contentFingerprint(directory);
      const candidates = digest && fingerprint ? this.records.filter((r: PortableRecord): boolean => !r.blocked && r.executable === relative &&
        r.executableSha256 === digest && r.contentFingerprint === fingerprint && !fs.accessSync(r.directory)) : [];
      if (candidates.length === 1) game = JSON.parse(JSON.stringify(candidates[0].game)) as PortableGame;
    }
    if (!game && existing) game = existing.game;
    if (!game) game = { formatVersion: 1, minReaderVersion: 1, minWriterVersion: 1, writtenByAppVersion: this.appVersion,
      gameUid: util.generateRandomUUID(), inputSchemaVersion: 6, entries: [] };
    const active = game.entries.find((e: PortableEntry): boolean => e.entryUid === game?.activeEntryUid);
    if (active && fs.accessSync(directory + '/' + active.executable)) relative = active.executable;
    if (relative !== hashedExecutable) digest = '';
    let entry = game.entries.find((e: PortableEntry): boolean => e.executable === relative);
    if (!entry) {
      entry = { entryUid: util.generateRandomUUID(), executable: relative, workingDirectory: '', settings: '{}',
        input: { mode: 'global', sharedId: '', profile: null } };
      game.entries.push(entry);
    }
    // Separate simultaneously present copies. A move (old folder gone) keeps its portable identity.
    if (!game.cardEntryUid) game.cardEntryUid = entry.entryUid;
    if (!game.activeEntryUid) game.activeEntryUid = entry.entryUid;
    let baseId = 'game-' + game.gameUid + '-' + game.cardEntryUid;
    const duplicate = this.records.some((r: PortableRecord): boolean => r.appId === baseId && r.directory !== directory && fs.accessSync(r.directory));
    if (duplicate && !existing) { game.gameUid = util.generateRandomUUID(); baseId = 'game-' + game.gameUid + '-' + game.cardEntryUid; }
    const id = existing?.appId ?? baseId;
    const record: PortableRecord = { directory: directory, appId: id, executable: relative,
      executableSha256: digest, contentFingerprint: fingerprint, game: game, blocked: false };
    const settings = AppSettingsStore.getInstance();
    const directoryChanged = !!existing && JSON.stringify(existing.game) !== JSON.stringify(game);
    if (!existing || directoryChanged) {
      await settings.remapAppId(oldId, id);
      if (entry.settings !== '{}') {
        const value = JSON.parse(entry.settings) as AppSettings; value.appId = id;
        value.executable = directory + '/' + entry.executable;
        value.workingDirectory = entry.workingDirectory ? directory + '/' + entry.workingDirectory : directory;
        value.customCover = value.customCover ? directory + '/' + configurationRelativePath(value.customCover) : '';
        value.manualBackgroundPath = value.manualBackgroundPath ? directory + '/' + configurationRelativePath(value.manualBackgroundPath) : '';
        const binding = entry.input, profiles = InputProfileService.getInstance();
        if (binding.mode === 'global') value.inputProfileId = 'default';
        else if (binding.profile) {
          const profileId = binding.mode === 'dedicated' ? 'app-game-' + game.gameUid + '-' + entry.entryUid : binding.sharedId;
          if (binding.mode === 'dedicated' || !profiles.hasProfile(profileId) && !profiles.isDeleted(profileId)) {
            await profiles.saveProfile(cloneInputProfile(binding.profile, profileId), 'portable');
          }
          value.inputProfileId = profiles.isDeleted(profileId) ? 'default' : profileId;
        }
        await settings.saveAppSettings(value, 'portable');
      }
    }
    this.records = this.records.filter((r: PortableRecord): boolean =>
      !(r.directory === directory && r.appId === id) && !(r.appId === id && r.directory !== directory && !fs.accessSync(r.directory)));
    this.records.push(record);
    await this.saveRecord(record, settings.getAppSettings(id)); return id;
  }
  private saved(value: AppSettings): Promise<void> {
    this.chain = this.chain.catch((): void => {}).then(async (): Promise<void> => {
      const record = this.records.find((r: PortableRecord): boolean => r.appId === value.appId);
      if (record) await this.saveRecord(record, value);
    }); return this.chain;
  }
  private async saveRecord(record: PortableRecord, source: AppSettings): Promise<void> {
    if (record.blocked || this.incompatible) return;
    let entry = record.game.entries.find((e: PortableEntry): boolean => e.executable === record.executable); if (!entry) return;
    const settings = cloneAppSettings(source), profileId = settings.inputProfileId || 'default', profiles = InputProfileService.getInstance();
    // A launcher can be changed within one game directory. Keep each entry's data separate.
    const selected = this.relative(settings.executable, record.directory);
    if (selected && selected !== record.executable) {
      const previous = record.game.entries.find((e: PortableEntry): boolean => e.executable === selected);
      if (previous) {
        record.executable = selected;
        record.executableSha256 = '';
        record.game.activeEntryUid = previous.entryUid;
        const restored = JSON.parse(previous.settings) as AppSettings;
        restored.appId = record.appId; restored.executable = record.directory + '/' + selected;
        restored.workingDirectory = previous.workingDirectory ? record.directory + '/' + previous.workingDirectory : record.directory;
        restored.customCover = restored.customCover ? record.directory + '/' + configurationRelativePath(restored.customCover) : '';
        restored.manualBackgroundPath = restored.manualBackgroundPath ? record.directory + '/' + configurationRelativePath(restored.manualBackgroundPath) : '';
        if (previous.input.mode === 'global') restored.inputProfileId = 'default';
        else if (previous.input.profile) {
          const profileId = previous.input.mode === 'dedicated' ? 'app-game-' + record.game.gameUid + '-' + previous.entryUid : previous.input.sharedId;
          if (!profiles.isDeleted(profileId) && (previous.input.mode === 'dedicated' || !profiles.hasProfile(profileId))) await profiles.saveProfile(cloneInputProfile(previous.input.profile, profileId), 'portable');
          restored.inputProfileId = profiles.isDeleted(profileId) ? 'default' : profileId;
        }
        await AppSettingsStore.getInstance().saveAppSettings(restored, 'portable'); await this.commitRecord(record); return;
      }
      entry = { entryUid: util.generateRandomUUID(), executable: selected, workingDirectory: '', settings: '{}', input: { mode: 'global', sharedId: '', profile: null } };
      record.game.entries.push(entry); record.executable = selected; record.executableSha256 = '';
    }
    record.game.activeEntryUid = entry.entryUid;
    entry.workingDirectory = this.relative(settings.workingDirectory, record.directory);
    settings.executable = entry.executable; settings.workingDirectory = entry.workingDirectory;
    settings.customCover = this.relative(settings.customCover, record.directory);
    // Migrate manual artwork without changing the user-selected mode or deleting its source until verified.
    if (settings.manualBackgroundPath && !this.relative(settings.manualBackgroundPath, record.directory)) {
      try {
        const destination = record.directory + '/.pomelo/artwork'; files.ensure(destination);
        const path = destination + '/' + entry.entryUid + '.jpg';
        const bytes = files.read(settings.manualBackgroundPath, 12582912); files.write(path, bytes);
        if (await files.hash(files.read(path, 12582912)) !== await files.hash(bytes)) throw new Error('CONFIG_HASH_MISMATCH');
        const updated = cloneAppSettings(source); updated.manualBackgroundPath = path;
        await AppSettingsStore.getInstance().saveAppSettings(updated, 'portable'); settings.manualBackgroundPath = path;
      } catch (_) { /* Read-only games retain the original local image, excluded from cloud resources. */ }
    }
    settings.manualBackgroundPath = this.relative(settings.manualBackgroundPath, record.directory);
    settings.appId = ''; settings.inputProfileId = '';
    entry.settings = JSON.stringify(settings);
    const input: PortableInput = { mode: profileId === 'default' ? 'global' : profileId.startsWith('app-') ? 'dedicated' : 'shared', sharedId: '', profile: null };
    if (input.mode !== 'global' && profiles.hasProfile(profileId)) { input.profile = profiles.getProfile(profileId); input.sharedId = input.mode === 'shared' ? profileId : ''; }
    else if (input.mode !== 'global') {
      // Editor entry can save the binding before its first layout exists. Clone the effective global profile now.
      const base = AppSettingsStore.getInstance().getGlobalSettings().desktopInputProfileId || 'default';
      input.profile = cloneInputProfile(profiles.getProfile(base), profileId);
      await profiles.saveProfile(input.profile, 'portable');
    }
    if (input.mode === 'dedicated' && input.profile) {
      const independentId = 'app-game-' + record.game.gameUid + '-' + entry.entryUid;
      input.profile = cloneInputProfile(input.profile, independentId);
      if (source.inputProfileId !== independentId) {
        await profiles.saveProfile(input.profile, 'portable');
        const bound = cloneAppSettings(AppSettingsStore.getInstance().getAppSettings(record.appId)); bound.inputProfileId = independentId;
        await AppSettingsStore.getInstance().saveAppSettings(bound, 'portable');
      }
    }
    entry.input = input; record.game.writtenByAppVersion = this.appVersion; validatePortableGame(record.game);
    await this.commitRecord(record);
  }
  private async commitRecord(record: PortableRecord): Promise<void> {
    // Populate restored/legacy records once; an unchanged scan never rehashes the game.
    const executable = record.directory + '/' + record.executable;
    if ((!record.executableSha256 || record.contentFingerprint === undefined) && fs.accessSync(executable)) {
      try { record.executableSha256 = await files.fileHash(executable); } catch (_) {}
      record.contentFingerprint = await this.contentFingerprint(record.directory);
    }
    // Save sandbox fallback FIRST. A failed directory write never loses the user edit.
    await this.persist();
    try {
      if (!fs.accessSync(record.directory) || !fs.statSync(record.directory).isDirectory()) return;
      files.ensure(record.directory + '/.pomelo'); const path = record.directory + '/.pomelo/game.json';
      if (fs.accessSync(path)) validatePortableGame(JSON.parse(files.text(files.read(path))) as PortableGame);
      const raw = JSON.stringify(record.game);
      if (fs.accessSync(path) && files.text(files.read(path)) === raw) return;
      files.json(path, raw); validatePortableGame(JSON.parse(files.text(files.read(path))) as PortableGame);
      console.info('[PortableGameConfiguration] directory configuration committed');
    } catch (_) { /* Fallback remains part of the complete application backup. */ }
  }
}
