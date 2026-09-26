import { common, ConfigurationConstant } from '@kit.AbilityKit';
import { ConsolePalette, GlobalSettings, ThemeMode, UiSkin } from '../model/AppModels';
import { AppSettingsStore } from './AppSettingsStore';
import { skinFollowsSystem } from './SkinColors';

export type ThemeSettingsListener = (settings: GlobalSettings) => void;

export class ThemeSettingsService {
  private static instance: ThemeSettingsService | null = null;
  private listeners: Set<ThemeSettingsListener> = new Set<ThemeSettingsListener>();
  private context: common.UIAbilityContext | null = null;

  static getInstance(): ThemeSettingsService {
    if (!ThemeSettingsService.instance) ThemeSettingsService.instance = new ThemeSettingsService();
    return ThemeSettingsService.instance;
  }

  async initialize(context: common.UIAbilityContext): Promise<void> {
    this.context = context;
    await AppSettingsStore.getInstance().initialize(context);
    this.applyColorMode(AppSettingsStore.getInstance().getGlobalSettings());
  }

  getSettings(): GlobalSettings {
    return AppSettingsStore.getInstance().getGlobalSettings();
  }

  async update(settings: GlobalSettings): Promise<void> {
    this.applyColorMode(settings);
    await AppSettingsStore.getInstance().saveGlobalSettings(settings);
    this.listeners.forEach((listener: ThemeSettingsListener) => listener(settings));
  }

  subscribe(listener: ThemeSettingsListener): void {
    this.listeners.add(listener);
  }

  unsubscribe(listener: ThemeSettingsListener): void {
    this.listeners.delete(listener);
  }

  private applyColorMode(settings: GlobalSettings): void {
    if (!this.context) return;
    let colorMode = ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET;
    if (skinFollowsSystem(settings)) {
      colorMode = ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET;
    } else if (settings.uiSkin === UiSkin.CONSOLE) {
      colorMode = settings.consolePalette === ConsolePalette.PEARL ?
        ConfigurationConstant.ColorMode.COLOR_MODE_LIGHT : ConfigurationConstant.ColorMode.COLOR_MODE_DARK;
    } else {
      if (settings.themeMode === ThemeMode.LIGHT) colorMode = ConfigurationConstant.ColorMode.COLOR_MODE_LIGHT;
      if (settings.themeMode === ThemeMode.DARK) colorMode = ConfigurationConstant.ColorMode.COLOR_MODE_DARK;
    }
    try {
      this.context.getApplicationContext().setColorMode(colorMode);
    } catch (_) {
    }
  }
}
