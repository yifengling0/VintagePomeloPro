import { requireThat } from './errors.mjs';

export const CONFIGURATION_SCOPE = 'user-settings';
export const CONFIGURATION_BYTES = 262144;
const keys = {
  vintage_pomelo_pro: ['global_settings_v1', 'winehua_render_mode_v1', 'platform_defaults_v7'],
  input_settings: ['touchpadEnabled', 'gamepadEnabled', 'keyboardEnabled', 'cursorSpeed', 'padButtons', 'padZones', 'keyboardPos', 'layoutEditIntroShown'],
  desktop_mode: ['mode'], wine_settings: ['lang'], vd_exit_btn: ['x', 'y'], steam_local_client: ['compatibility'],
  vintage_pomelo_input_profiles: ['profiles_v6', 'deleted_profiles_v1']
};
const globalKeys = ['defaultDisplayMode','wineLanguage','desktopOrientation','desktopWindowMode','keepScreenOn',
  'box64Preset','desktopInputProfileId','viewMode','uiSkin','consoleCardLayout','consolePalette','themeMode','useGlassEffect',
  'virtualInputEnabled','showVirtualControls','autoHideVirtualControls','controlBarDirection','controlBarPositionX',
  'controlBarPositionY','virtualMouseConfig','controlOpacity','gamepadDeadzone','controllerOutputMode','outputScale',
  'd3dRendererPreference','preserveUserDataOnReset','showFpsHud','performanceHud'];
export function validateCloudSettings(value) {
  requireThat(value?.formatVersion === 1 && value.minReaderVersion === 1 && value.minWriterVersion === 1 &&
    Array.isArray(value.requiredFeatures) && value.requiredFeatures.length === 1 && value.requiredFeatures[0] === 'cloud-settings-v1' &&
    Array.isArray(value.assets) && value.assets.length === 0 && Array.isArray(value.modules) && value.modules.length === 4,
    'CONFIG_SCOPE_UNSUPPORTED', 400);
  const modules = new Set(), entries = new Set();
  for (const module of value.modules) {
    requireThat(['settings','input_profiles','library','resources'].includes(module?.name) && !modules.has(module.name) &&
      module.schemaVersion === (module.name === 'input_profiles' ? 6 : 1) && Array.isArray(module.entries) && module.entries.length <= 64,
      'CONFIG_INVALID');
    modules.add(module.name);
    if (['library','resources'].includes(module.name)) requireThat(module.entries.length === 0, 'CONFIG_SCOPE_UNSUPPORTED');
    for (const entry of module.entries) {
      const input = entry?.store === 'vintage_pomelo_input_profiles';
      requireThat((module.name === 'input_profiles') === input && Object.hasOwn(keys, entry?.store) && keys[entry.store].includes(entry.key) &&
        !entries.has(entry.store + '/' + entry.key) && ['string','number','boolean'].includes(typeof entry.value) &&
        (typeof entry.value !== 'number' || Number.isFinite(entry.value)), 'CONFIG_SCOPE_UNSUPPORTED');
      entries.add(entry.store + '/' + entry.key);
      if (entry.key === 'global_settings_v1') {
        let settings; try { settings = JSON.parse(entry.value); } catch { requireThat(false, 'CONFIG_INVALID'); }
        requireThat(settings && !Array.isArray(settings) && typeof settings === 'object' &&
          Object.keys(settings).every(key => globalKeys.includes(key)), 'CONFIG_SCOPE_UNSUPPORTED');
      }
    }
  }
}
