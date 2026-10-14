import { InputProfile, INPUT_PROFILE_SCHEMA_VERSION } from './AppModels';
import { validateCloudProfile } from './ValueAddedModels';

export type ConfigurationValue = string | number | boolean;
export interface ConfigurationEntry { store: string; key: string; value: ConfigurationValue; }
export interface ConfigurationModule { name: string; schemaVersion: number; entries: ConfigurationEntry[]; }
export interface ConfigurationAsset { path: string; sha256: string; size: number; target: string; }
export interface UserConfiguration {
  formatVersion: number; minReaderVersion: number; minWriterVersion: number;
  writtenByAppVersion: string; requiredFeatures: string[];
  modules: ConfigurationModule[]; assets: ConfigurationAsset[];
}
export interface ConfigurationRegistration { name: string; schemaVersion: number; store: string; keys: string[]; }
interface PortableIndexValidation { directory: string; appId: string; executable: string; executableSha256: string; game: PortableGame; blocked: boolean; }
// Credentials, purchase verdicts, sync journals, generated catalog and Wine prefix are deliberately unregistered.
export const CONFIGURATION_REGISTRY: ConfigurationRegistration[] = [
  { name: 'settings', schemaVersion: 1, store: 'vintage_pomelo_pro', keys: ['global_settings_v1', 'winehua_render_mode_v1', 'platform_defaults_v7'] },
  { name: 'settings', schemaVersion: 1, store: 'input_settings', keys: ['touchpadEnabled', 'gamepadEnabled', 'keyboardEnabled', 'cursorSpeed', 'padButtons', 'padZones', 'keyboardPos', 'layoutEditIntroShown'] },
  { name: 'settings', schemaVersion: 1, store: 'desktop_mode', keys: ['mode'] },
  { name: 'settings', schemaVersion: 1, store: 'wine_settings', keys: ['lang'] },
  { name: 'settings', schemaVersion: 1, store: 'vd_exit_btn', keys: ['x', 'y'] },
  { name: 'settings', schemaVersion: 1, store: 'steam_local_client', keys: ['compatibility'] },
  { name: 'input_profiles', schemaVersion: INPUT_PROFILE_SCHEMA_VERSION, store: 'vintage_pomelo_input_profiles', keys: ['profiles_v6', 'deleted_profiles_v1'] },
  { name: 'library', schemaVersion: 1, store: 'vintage_pomelo_pro', keys: ['app_settings_v1', 'library_path_v1', 'recent_launches_v1', 'recent_launches_v2', 'play_time_v1', 'console_bento_state_v1'] },
  { name: 'library', schemaVersion: 1, store: 'pomelo_game_config', keys: ['index_v1'] },
  { name: 'resources', schemaVersion: 1, store: 'pomelo_config_resources', keys: ['fonts_v1'] }
];
export function configurationRelativePath(value: string): string {
  if (typeof value !== 'string' || value.length > 1024 || value.includes('\\') || value.startsWith('/') ||
    value.includes(':') || /[\u0000-\u001f]/.test(value) || value.split('/').some((p: string): boolean => !p || p === '.' || p === '..')) {
    throw new Error('CONFIG_UNSAFE_PATH');
  }
  return value;
}
export function validateUserConfiguration(value: UserConfiguration): void {
  if (!value || value.formatVersion !== 1 || value.minReaderVersion > 1 || value.minWriterVersion > 1 ||
    !Array.isArray(value.requiredFeatures) || value.requiredFeatures.some((f: string): boolean => !['portable-games-v1', 'input-v6', 'assets-v1'].includes(f))) {
    throw new Error('CONFIG_REQUIRES_UPDATE');
  }
  if (value.minReaderVersion !== 1 || value.minWriterVersion !== 1 || typeof value.writtenByAppVersion !== 'string' ||
    !Array.isArray(value.modules) || !Array.isArray(value.assets) || value.assets.length > 256 || value.modules.length > 4) throw new Error('CONFIG_INVALID');
  const seen = new Set<string>();
  value.modules.forEach((module: ConfigurationModule): void => {
    if (!module || seen.has(module.name) || !Array.isArray(module.entries) || module.entries.length > 64) throw new Error('CONFIG_INVALID');
    seen.add(module.name);
    const registrations = CONFIGURATION_REGISTRY.filter((r: ConfigurationRegistration): boolean => r.name === module.name);
    if (!registrations.length || module.schemaVersion !== registrations[0].schemaVersion) throw new Error('CONFIG_REQUIRES_UPDATE');
    const keys = new Set<string>();
    module.entries.forEach((e: ConfigurationEntry): void => {
      if (!e || !registrations.some((r: ConfigurationRegistration): boolean => r.store === e.store && r.keys.includes(e.key)) ||
        keys.has(e.store + '/' + e.key) || !['string', 'number', 'boolean'].includes(typeof e.value) ||
        (typeof e.value === 'number' && !Number.isFinite(e.value))) throw new Error('CONFIG_INVALID');
      keys.add(e.store + '/' + e.key);
      if (e.key === 'profiles_v6') {
        const profiles = JSON.parse(e.value as string) as InputProfile[];
        if (!Array.isArray(profiles) || profiles.length > 2048) throw new Error('CONFIG_INVALID');
        const ids = new Set<string>();
        profiles.forEach((p: InputProfile): void => { validateCloudProfile(p); if (ids.has(p.id)) throw new Error('CONFIG_INVALID'); ids.add(p.id); });
      }
      if (e.key === 'index_v1') {
        const records = JSON.parse(e.value as string) as PortableIndexValidation[];
        if (!Array.isArray(records) || records.length > 2048) throw new Error('CONFIG_INVALID');
        records.forEach((r: PortableIndexValidation): void => {
          if (!r || typeof r.directory !== 'string' || typeof r.appId !== 'string' || typeof r.executable !== 'string' ||
            r.directory.length > 2048 || r.appId.length > 128 || typeof r.blocked !== 'boolean') throw new Error('CONFIG_INVALID');
          configurationRelativePath(r.executable); validatePortableGame(r.game);
        });
      }
    });
  });
  if (seen.size !== 4) throw new Error('CONFIG_INCOMPLETE');
  const paths = new Map<string, string>(); const targets = new Set<string>(); let total = 0;
  value.assets.forEach((asset: ConfigurationAsset): void => {
    configurationRelativePath(asset.path);
    if (!/^assets\/[a-f0-9]{64}\.[a-z0-9]{1,8}$/.test(asset.path) || targets.has(asset.target) || !/^[a-f0-9]{64}$/.test(asset.sha256) ||
      !Number.isSafeInteger(asset.size) || asset.size < 1 || asset.size > 16 * 1024 * 1024 ||
      !(asset.target === 'wallpaper' || /^font:[^/\\:]{1,128}$/.test(asset.target))) throw new Error('CONFIG_INVALID');
    if (paths.has(asset.path) && paths.get(asset.path) !== asset.sha256 + ':' + asset.size) throw new Error('CONFIG_INVALID');
    paths.set(asset.path, asset.sha256 + ':' + asset.size); targets.add(asset.target); total += asset.size;
  });
  if (total > 64 * 1024 * 1024) throw new Error('CONFIG_QUOTA');
}

export interface PortableInput { mode: string; sharedId: string; profile: InputProfile | null; }
export interface PortableEntry {
  entryUid: string; executable: string; workingDirectory: string;
  settings: string; input: PortableInput;
}
export interface PortableGame {
  formatVersion: number; minReaderVersion: number; minWriterVersion: number; writtenByAppVersion: string;
  gameUid: string; inputSchemaVersion: number; entries: PortableEntry[];
  activeEntryUid?: string; cardEntryUid?: string;
}
export function validatePortableGame(game: PortableGame): void {
  if (!game || game.formatVersion !== 1 || game.minReaderVersion !== 1 || game.minWriterVersion !== 1 ||
    game.inputSchemaVersion !== INPUT_PROFILE_SCHEMA_VERSION) throw new Error('CONFIG_REQUIRES_UPDATE');
  if (!/^[a-f0-9-]{36}$/.test(game.gameUid) || !Array.isArray(game.entries) || game.entries.length > 64) throw new Error('CONFIG_INVALID');
  for (const id of [game.activeEntryUid, game.cardEntryUid]) {
    if (id && !game.entries.some((entry: PortableEntry): boolean => entry.entryUid === id)) throw new Error('CONFIG_INVALID');
  }
  const seen = new Set<string>();
  game.entries.forEach((entry: PortableEntry): void => {
    configurationRelativePath(entry.executable);
    if (entry.workingDirectory) configurationRelativePath(entry.workingDirectory);
    if (!/^[a-f0-9-]{36}$/.test(entry.entryUid) || seen.has(entry.executable.toLowerCase()) ||
      typeof entry.settings !== 'string' || entry.settings.length > 65536 || !entry.input ||
      !['global', 'dedicated', 'shared'].includes(entry.input.mode)) throw new Error('CONFIG_INVALID');
    seen.add(entry.executable.toLowerCase());
    if (entry.input.profile) validateCloudProfile(entry.input.profile);
    if (entry.input.mode !== 'global' && !entry.input.profile) throw new Error('CONFIG_INCOMPLETE');
    JSON.parse(entry.settings);
  });
}
