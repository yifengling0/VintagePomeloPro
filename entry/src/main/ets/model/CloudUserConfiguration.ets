import { ConfigurationEntry, ConfigurationModule, UserConfiguration, validateUserConfiguration } from './UserConfiguration';

export const CLOUD_CONFIGURATION_BYTES: number = 262144;
export const CLOUD_CONFIGURATION_SCOPE: string = 'user-settings';
export const CLOUD_GLOBAL_SETTINGS_KEYS: string[] = [
  'defaultDisplayMode', 'wineLanguage', 'desktopOrientation', 'desktopWindowMode', 'keepScreenOn',
  'box64Preset', 'desktopInputProfileId', 'viewMode', 'uiSkin', 'consoleCardLayout', 'consolePalette',
  'themeMode', 'useGlassEffect', 'virtualInputEnabled', 'showVirtualControls', 'autoHideVirtualControls',
  'controlBarDirection', 'controlBarPositionX', 'controlBarPositionY', 'virtualMouseConfig', 'controlOpacity',
  'gamepadDeadzone', 'controllerOutputMode', 'outputScale', 'd3dRendererPreference', 'preserveUserDataOnReset',
  'showFpsHud', 'performanceHud'
];

export function isCloudConfigurationKey(module: string, key: string): boolean {
  return module === 'settings' || module === 'input_profiles';
}

export function cloudGlobalSettings(raw: string): string {
  const original = JSON.parse(raw) as Record<string, string | boolean | number>;
  const settings: Record<string, string | boolean | number> = {};
  for (const key of CLOUD_GLOBAL_SETTINGS_KEYS) if (Object.keys(original).includes(key)) settings[key] = original[key];
  return JSON.stringify(settings);
}

/** Cloud backups carry settings only. The complete local backup retains its own format. */
export function projectCloudUserConfiguration(value: UserConfiguration): UserConfiguration {
  validateUserConfiguration(value);
  const modules: ConfigurationModule[] = value.modules.map((module: ConfigurationModule): ConfigurationModule => {
    const entries: ConfigurationEntry[] = [];
    if (isCloudConfigurationKey(module.name, '')) {
      for (const entry of module.entries) entries.push({ store: entry.store, key: entry.key,
        value: entry.key === 'global_settings_v1' ? cloudGlobalSettings(entry.value as string) : entry.value });
    }
    return { name: module.name, schemaVersion: module.schemaVersion, entries: entries };
  });
  return { formatVersion: 1, minReaderVersion: 1, minWriterVersion: 1, writtenByAppVersion: value.writtenByAppVersion,
    requiredFeatures: ['cloud-settings-v1'], modules: modules, assets: [] };
}

export function validateCloudUserConfiguration(value: UserConfiguration): void {
  validateUserConfiguration(value);
  if (value.assets.length || !value.requiredFeatures.includes('cloud-settings-v1')) throw new Error('CONFIG_SCOPE_UNSUPPORTED');
  for (const module of value.modules) {
    if (!isCloudConfigurationKey(module.name, '') && module.entries.length) throw new Error('CONFIG_SCOPE_UNSUPPORTED');
    for (const entry of module.entries) {
      if (entry.key === 'global_settings_v1' && cloudGlobalSettings(entry.value as string) !== entry.value) throw new Error('CONFIG_SCOPE_UNSUPPORTED');
    }
  }
}
