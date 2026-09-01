import preferences from '@ohos.data.preferences';
import { normalizePerformanceHud } from '../model/PerformanceHudSettings';
import { common } from '@kit.AbilityKit';
import { DeviceCapabilityPolicy } from '../common/DeviceCapabilityPolicy';
import {
  AppSettings,
  AppDescriptor,
  ControlBarDirection,
  D3DRendererPreference,
  DesktopWindowMode,
  DisplayMode,
  GlobalSettings,
  WineLanguage,
  ViewMode,
  cloneVirtualMouseConfig,
  createDefaultAppSettings,
  createDefaultGlobalSettings
} from '../model/AppModels';

const STORE_NAME = 'vintage_pomelo_pro';
const GLOBAL_KEY = 'global_settings_v1';
const APP_SETTINGS_KEY = 'app_settings_v1';
const LIBRARY_PATH_KEY = 'library_path_v1';
const CATALOG_CACHE_KEY = 'catalog_cache_v1';
const RECENT_LAUNCHES_KEY = 'recent_launches_v1';
const RENDER_MODE_KEY = 'winehua_render_mode_v1';
// v6 briefly selected single-app on tablets.  Keep the product's desktop
// workflow as the default and migrate only those tablet installations back.
const PLATFORM_DEFAULTS_KEY = 'platform_defaults_v7';

export class AppSettingsStore {
  private static instance: AppSettingsStore | null = null;
  private store: preferences.Preferences | null = null;
  private appSettings: Map<string, AppSettings> = new Map<string, AppSettings>();
  private globalSettings: GlobalSettings = createDefaultGlobalSettings();
  // 最近启动的 appId 列表 (最新在前), 持久化, 供"最近"分类使用
  private recentLaunches: string[] = [];
  private libraryPath: string = '';
  private managedWineWindowsHost: boolean = false;
  private renderMode: string = 'dxvk_1_10';

  static getInstance(): AppSettingsStore {
    if (!AppSettingsStore.instance) AppSettingsStore.instance = new AppSettingsStore();
    return AppSettingsStore.instance;
  }

  async initialize(context: common.UIAbilityContext): Promise<void> {
    this.managedWineWindowsHost = this.detectManagedWineWindowsHost();
    if (this.store) return;
    this.store = await preferences.getPreferences(context, STORE_NAME);
    this.loadGlobal();
    await this.applyPlatformDefaults();
    this.loadApps();
    this.libraryPath = this.store.getSync(LIBRARY_PATH_KEY, '') as string;
    this.loadRecentLaunches();
    this.loadRenderMode();
  }

  private normalizeRenderMode(value: string): string {
    if (value === 'dxvk_2_6' || value === 'dxvk_1_10' || value === 'virgl') {
      return value;
    }
    if (value === 'dxvk_modern_2_6') {
      return 'dxvk_2_6';
    }
    if (value === 'dxvk_legacy') {
      return 'dxvk_1_10';
    }
    // 旧版 "auto" 与未知值一律落到 1.10，不再按 GPU 自动挑 2.6。
    return 'dxvk_1_10';
  }

  private dxvkPreferenceOf(mode: string): string {
    if (mode === 'dxvk_2_6') {
      return 'dxvk_modern_2_6';
    }
    if (mode === 'virgl') {
      return 'wined3d';
    }
    return 'dxvk_legacy';
  }

  private publishRenderMode(mode: string): void {
    AppStorage.setOrCreate<string>('winehua.render.mode', mode);
    const preference = this.dxvkPreferenceOf(mode);
    AppStorage.setOrCreate<string>('winehua.dxvk.preference', preference);
    AppStorage.setOrCreate<string>('winehua.dxvk.backend', preference);
  }

  private loadRenderMode(): void {
    if (!this.store) {
      this.publishRenderMode(this.renderMode);
      return;
    }
    const raw = this.store.getSync(RENDER_MODE_KEY, 'dxvk_1_10') as string;
    this.renderMode = this.normalizeRenderMode(raw);
    this.publishRenderMode(this.renderMode);
    if (raw !== this.renderMode) {
      void this.store.put(RENDER_MODE_KEY, this.renderMode).then((): void => {
        void this.store?.flush();
      });
    }
  }

  getRenderMode(): string {
    return this.renderMode;
  }

  async setRenderMode(mode: string): Promise<void> {
    const selected = this.normalizeRenderMode(mode);
    this.renderMode = selected;
    this.publishRenderMode(selected);
    if (!this.store) return;
    await this.store.put(RENDER_MODE_KEY, selected);
    await this.store.flush();
  }

  /** Restore the v6 tablet default without overwriting other device preferences. */
  private async applyPlatformDefaults(): Promise<void> {
    if (!this.store) return;
    const applied = this.store.getSync(PLATFORM_DEFAULTS_KEY, '') as string;
    if (applied === '1') return;
    if (!DeviceCapabilityPolicy.isTabletDevice()) {
      await this.store.put(PLATFORM_DEFAULTS_KEY, '1');
      await this.store.flush();
      return;
    }
    const current = this.globalSettings;
    this.globalSettings = {
      defaultDisplayMode: DisplayMode.DESKTOP,
      wineLanguage: current.wineLanguage ?? WineLanguage.CHINESE,
      desktopOrientation: current.desktopOrientation,
      desktopWindowMode: current.desktopWindowMode ?? DesktopWindowMode.FULLSCREEN,
      keepScreenOn: current.keepScreenOn,
      box64Preset: current.box64Preset,
      desktopInputProfileId: current.desktopInputProfileId,
      viewMode: current.viewMode,
      themeMode: current.themeMode,
      useGlassEffect: current.useGlassEffect,
      usePageBackground: current.usePageBackground,
      pageBackgroundUri: current.pageBackgroundUri,
      virtualInputEnabled: current.virtualInputEnabled,
      showVirtualControls: current.showVirtualControls,
      autoHideVirtualControls: current.autoHideVirtualControls,
      controlBarDirection: current.controlBarDirection,
      controlBarPositionX: current.controlBarPositionX,
      controlBarPositionY: current.controlBarPositionY,
      virtualMouseConfig: cloneVirtualMouseConfig(current.virtualMouseConfig),
      controlOpacity: current.controlOpacity,
      gamepadDeadzone: current.gamepadDeadzone,
      controllerOutputMode: current.controllerOutputMode ?? 'dinput',
      outputScale: current.outputScale,
      d3dRendererPreference: current.d3dRendererPreference,
      preserveUserDataOnReset: current.preserveUserDataOnReset,
      showFpsHud: current.showFpsHud,
      performanceHud: normalizePerformanceHud(current.performanceHud)
    };
    await this.store.put(GLOBAL_KEY, JSON.stringify(this.globalSettings));
    await this.store.put(PLATFORM_DEFAULTS_KEY, '1');
    await this.store.flush();
  }

  private detectManagedWineWindowsHost(): boolean {
    // Device model is commonly just "emulator" for both tablet and PC HVDs.
    // Window topology must follow the declared device class so the tablet
    // emulator keeps the composed Wine desktop while 2-in-1/PC stays multiton.
    return DeviceCapabilityPolicy.usesManagedWineWindows();
  }

  /** PC/2-in-1 exposes every Wine toplevel through a multiton ArkUI window. */
  usesManagedWineWindows(): boolean {
    // WineWindowManager is created by EntryAbility before the settings store
    // finishes its asynchronous preference load. Detect the host on demand so
    // the first native callback never observes the temporary tablet default.
    return this.managedWineWindowsHost || this.detectManagedWineWindowsHost();
  }

  private loadGlobal(): void {
    if (!this.store) return;
    const raw = this.store.getSync(GLOBAL_KEY, '') as string;
    if (!raw) {
      // 全新安装: 无任何存储值时也要应用设备默认 —— 平板默认切边
      // (圆角屏避让), 否则桌面会以全屏出生, 圆角遮挡开始菜单。
      this.globalSettings.desktopWindowMode =
        this.resolveDesktopWindowModeDefault(this.globalSettings.desktopWindowMode);
      return;
    }
    try {
      const stored = JSON.parse(raw) as GlobalSettings;
      const defaults = createDefaultGlobalSettings();
      this.globalSettings = {
        defaultDisplayMode: stored.defaultDisplayMode ?? defaults.defaultDisplayMode,
        wineLanguage: stored.wineLanguage === WineLanguage.ENGLISH ?
          WineLanguage.ENGLISH : WineLanguage.CHINESE,
        desktopOrientation: stored.desktopOrientation ?? defaults.desktopOrientation,
        desktopWindowMode: stored.desktopWindowMode ??
          this.resolveDesktopWindowModeDefault(defaults.desktopWindowMode),
        keepScreenOn: stored.keepScreenOn ?? defaults.keepScreenOn,
        box64Preset: stored.box64Preset ?? defaults.box64Preset,
        desktopInputProfileId: stored.desktopInputProfileId ?? defaults.desktopInputProfileId,
        viewMode: stored.viewMode ?? defaults.viewMode,
        themeMode: stored.themeMode ?? defaults.themeMode,
        useGlassEffect: stored.useGlassEffect ?? defaults.useGlassEffect,
        usePageBackground: stored.usePageBackground ?? defaults.usePageBackground,
        pageBackgroundUri: stored.pageBackgroundUri ?? defaults.pageBackgroundUri,
        virtualInputEnabled: stored.virtualInputEnabled ?? defaults.virtualInputEnabled,
        showVirtualControls: stored.showVirtualControls ?? defaults.showVirtualControls,
        autoHideVirtualControls: stored.autoHideVirtualControls ?? defaults.autoHideVirtualControls,
        controlBarDirection: stored.controlBarDirection ?? ControlBarDirection.AUTO,
        controlBarPositionX: Math.max(0, Math.min(1,
          stored.controlBarPositionX ?? defaults.controlBarPositionX)),
        controlBarPositionY: Math.max(0, Math.min(1,
          stored.controlBarPositionY ?? defaults.controlBarPositionY)),
        virtualMouseConfig: cloneVirtualMouseConfig(stored.virtualMouseConfig ?? defaults.virtualMouseConfig),
        controlOpacity: stored.controlOpacity ?? defaults.controlOpacity,
        gamepadDeadzone: stored.gamepadDeadzone ?? defaults.gamepadDeadzone,
        controllerOutputMode: (stored.controllerOutputMode === 'keyboard_legacy')
          ? 'keyboard_legacy' : 'dinput',
        outputScale: stored.outputScale ?? defaults.outputScale,
        d3dRendererPreference: stored.d3dRendererPreference ?? D3DRendererPreference.DXVK,
        preserveUserDataOnReset: stored.preserveUserDataOnReset ?? defaults.preserveUserDataOnReset,
        showFpsHud: stored.showFpsHud ?? false,
        performanceHud: normalizePerformanceHud(stored.performanceHud)
      };
    } catch (_) {
      this.globalSettings = createDefaultGlobalSettings();
    }
  }

  /** Rounded-screen tablets keep the desktop inside the safe area by default so
   * the start menu is never clipped; other device classes stay immersive. */
  private resolveDesktopWindowModeDefault(fallback: DesktopWindowMode): DesktopWindowMode {
    return DeviceCapabilityPolicy.isTabletDevice() ? DesktopWindowMode.SAFE_AREA : fallback;
  }

  private loadApps(): void {
    if (!this.store) return;
    const raw = this.store.getSync(APP_SETTINGS_KEY, '') as string;
    if (!raw) return;
    try {
      const values = JSON.parse(raw) as AppSettings[];
      values.forEach((value: AppSettings) => this.appSettings.set(value.appId, this.normalizeAppSettings(value)));
    } catch (_) {
      this.appSettings.clear();
    }
  }

  private normalizeAppSettings(value: AppSettings): AppSettings {
    const defaults = createDefaultAppSettings(value.appId);
    return {
      appId: value.appId,
      customTitle: value.customTitle ?? defaults.customTitle,
      customCover: value.customCover ?? defaults.customCover,
      favorite: value.favorite ?? defaults.favorite,
      box64Preset: value.box64Preset ?? defaults.box64Preset,
      executable: value.executable ?? defaults.executable,
      arguments: value.arguments ?? defaults.arguments,
      workingDirectory: value.workingDirectory ?? defaults.workingDirectory,
      displayModeOverride: value.displayModeOverride ?? defaults.displayModeOverride,
      inputProfileId: value.inputProfileId ?? defaults.inputProfileId,
      prefixId: value.prefixId ?? defaults.prefixId
    };
  }

  getGlobalSettings(): GlobalSettings {
    return this.globalSettings;
  }

  async saveGlobalSettings(value: GlobalSettings): Promise<void> {
    this.globalSettings = value;
    if (!this.store) return;
    await this.store.put(GLOBAL_KEY, JSON.stringify(value));
    await this.store.flush();
  }

  async setViewMode(value: ViewMode): Promise<void> {
    const current = this.globalSettings;
    await this.saveGlobalSettings({
      defaultDisplayMode: current.defaultDisplayMode,
      wineLanguage: current.wineLanguage,
      desktopOrientation: current.desktopOrientation,
      desktopWindowMode: current.desktopWindowMode,
      keepScreenOn: current.keepScreenOn,
      box64Preset: current.box64Preset,
      desktopInputProfileId: current.desktopInputProfileId,
      viewMode: value,
      themeMode: current.themeMode,
      useGlassEffect: current.useGlassEffect,
      usePageBackground: current.usePageBackground,
      pageBackgroundUri: current.pageBackgroundUri,
      virtualInputEnabled: current.virtualInputEnabled,
      showVirtualControls: current.showVirtualControls,
      autoHideVirtualControls: current.autoHideVirtualControls,
      controlBarDirection: current.controlBarDirection,
      controlBarPositionX: current.controlBarPositionX,
      controlBarPositionY: current.controlBarPositionY,
      virtualMouseConfig: cloneVirtualMouseConfig(current.virtualMouseConfig),
      controlOpacity: current.controlOpacity,
      gamepadDeadzone: current.gamepadDeadzone,
      controllerOutputMode: current.controllerOutputMode ?? 'dinput',
      outputScale: current.outputScale,
      d3dRendererPreference: current.d3dRendererPreference,
      preserveUserDataOnReset: current.preserveUserDataOnReset,
      showFpsHud: current.showFpsHud,
      performanceHud: normalizePerformanceHud(current.performanceHud)
    });
  }

  async setDefaultDisplayMode(value: DisplayMode): Promise<void> {
    const current = this.globalSettings;
    await this.saveGlobalSettings({
      defaultDisplayMode: value,
      wineLanguage: current.wineLanguage,
      desktopOrientation: current.desktopOrientation,
      desktopWindowMode: current.desktopWindowMode,
      keepScreenOn: current.keepScreenOn,
      box64Preset: current.box64Preset,
      desktopInputProfileId: current.desktopInputProfileId,
      viewMode: current.viewMode,
      themeMode: current.themeMode,
      useGlassEffect: current.useGlassEffect,
      usePageBackground: current.usePageBackground,
      pageBackgroundUri: current.pageBackgroundUri,
      virtualInputEnabled: current.virtualInputEnabled,
      showVirtualControls: current.showVirtualControls,
      autoHideVirtualControls: current.autoHideVirtualControls,
      controlBarDirection: current.controlBarDirection,
      controlBarPositionX: current.controlBarPositionX,
      controlBarPositionY: current.controlBarPositionY,
      virtualMouseConfig: cloneVirtualMouseConfig(current.virtualMouseConfig),
      controlOpacity: current.controlOpacity,
      gamepadDeadzone: current.gamepadDeadzone,
      controllerOutputMode: current.controllerOutputMode ?? 'dinput',
      outputScale: current.outputScale,
      d3dRendererPreference: current.d3dRendererPreference,
      preserveUserDataOnReset: current.preserveUserDataOnReset,
      showFpsHud: current.showFpsHud,
      performanceHud: normalizePerformanceHud(current.performanceHud)
    });
  }

  async setControlBarPosition(x: number, y: number): Promise<void> {
    const current = this.globalSettings;
    await this.saveGlobalSettings({
      defaultDisplayMode: current.defaultDisplayMode,
      wineLanguage: current.wineLanguage,
      desktopOrientation: current.desktopOrientation,
      desktopWindowMode: current.desktopWindowMode,
      keepScreenOn: current.keepScreenOn,
      box64Preset: current.box64Preset,
      desktopInputProfileId: current.desktopInputProfileId,
      viewMode: current.viewMode,
      themeMode: current.themeMode,
      useGlassEffect: current.useGlassEffect,
      usePageBackground: current.usePageBackground,
      pageBackgroundUri: current.pageBackgroundUri,
      virtualInputEnabled: current.virtualInputEnabled,
      showVirtualControls: current.showVirtualControls,
      autoHideVirtualControls: current.autoHideVirtualControls,
      controlBarDirection: current.controlBarDirection,
      controlBarPositionX: Math.max(0, Math.min(1, x)),
      controlBarPositionY: Math.max(0, Math.min(1, y)),
      virtualMouseConfig: cloneVirtualMouseConfig(current.virtualMouseConfig),
      controlOpacity: current.controlOpacity,
      gamepadDeadzone: current.gamepadDeadzone,
      controllerOutputMode: current.controllerOutputMode ?? 'dinput',
      outputScale: current.outputScale,
      d3dRendererPreference: current.d3dRendererPreference,
      preserveUserDataOnReset: current.preserveUserDataOnReset,
      showFpsHud: current.showFpsHud,
      performanceHud: normalizePerformanceHud(current.performanceHud)
    });
  }

  /** 持久化虚拟按键开关: 桌面重开时按用户最后设定恢复, 不再每次自动弹出。 */
  async setShowVirtualControls(value: boolean): Promise<void> {
    const current = this.globalSettings;
    await this.saveGlobalSettings({
      defaultDisplayMode: current.defaultDisplayMode,
      wineLanguage: current.wineLanguage,
      desktopOrientation: current.desktopOrientation,
      desktopWindowMode: current.desktopWindowMode,
      keepScreenOn: current.keepScreenOn,
      box64Preset: current.box64Preset,
      desktopInputProfileId: current.desktopInputProfileId,
      viewMode: current.viewMode,
      themeMode: current.themeMode,
      useGlassEffect: current.useGlassEffect,
      usePageBackground: current.usePageBackground,
      pageBackgroundUri: current.pageBackgroundUri,
      virtualInputEnabled: current.virtualInputEnabled,
      showVirtualControls: value,
      autoHideVirtualControls: current.autoHideVirtualControls,
      controlBarDirection: current.controlBarDirection,
      controlBarPositionX: current.controlBarPositionX,
      controlBarPositionY: current.controlBarPositionY,
      virtualMouseConfig: cloneVirtualMouseConfig(current.virtualMouseConfig),
      controlOpacity: current.controlOpacity,
      gamepadDeadzone: current.gamepadDeadzone,
      controllerOutputMode: current.controllerOutputMode ?? 'dinput',
      outputScale: current.outputScale,
      d3dRendererPreference: current.d3dRendererPreference,
      preserveUserDataOnReset: current.preserveUserDataOnReset,
      showFpsHud: current.showFpsHud,
      performanceHud: normalizePerformanceHud(current.performanceHud)
    });
  }

  async setBox64Preset(value: string): Promise<void> {
    const current = this.globalSettings;
    await this.saveGlobalSettings({
      defaultDisplayMode: current.defaultDisplayMode,
      wineLanguage: current.wineLanguage,
      desktopOrientation: current.desktopOrientation,
      desktopWindowMode: current.desktopWindowMode,
      keepScreenOn: current.keepScreenOn,
      box64Preset: value,
      desktopInputProfileId: current.desktopInputProfileId,
      viewMode: current.viewMode,
      themeMode: current.themeMode,
      useGlassEffect: current.useGlassEffect,
      usePageBackground: current.usePageBackground,
      pageBackgroundUri: current.pageBackgroundUri,
      virtualInputEnabled: current.virtualInputEnabled,
      showVirtualControls: current.showVirtualControls,
      autoHideVirtualControls: current.autoHideVirtualControls,
      controlBarDirection: current.controlBarDirection,
      controlBarPositionX: current.controlBarPositionX,
      controlBarPositionY: current.controlBarPositionY,
      virtualMouseConfig: cloneVirtualMouseConfig(current.virtualMouseConfig),
      controlOpacity: current.controlOpacity,
      gamepadDeadzone: current.gamepadDeadzone,
      controllerOutputMode: current.controllerOutputMode ?? 'dinput',
      outputScale: current.outputScale,
      d3dRendererPreference: current.d3dRendererPreference,
      preserveUserDataOnReset: current.preserveUserDataOnReset,
      showFpsHud: current.showFpsHud,
      performanceHud: normalizePerformanceHud(current.performanceHud)
    });
  }

  getAppSettings(appId: string): AppSettings {
    return this.appSettings.get(appId) ?? createDefaultAppSettings(appId);
  }

  async saveAppSettings(value: AppSettings): Promise<void> {
    this.appSettings.set(value.appId, this.normalizeAppSettings(value));
    if (!this.store) return;
    const values: AppSettings[] = [];
    this.appSettings.forEach((item: AppSettings) => values.push(item));
    await this.store.put(APP_SETTINGS_KEY, JSON.stringify(values));
    await this.store.flush();
  }

  async removeAppSettings(appId: string): Promise<void> {
    this.appSettings.delete(appId);
    if (!this.store) return;
    const values: AppSettings[] = [];
    this.appSettings.forEach((item: AppSettings) => values.push(item));
    await this.store.put(APP_SETTINGS_KEY, JSON.stringify(values));
    await this.store.flush();
  }

  getLibraryPath(): string {
    return this.libraryPath;
  }

  getCatalogCache(): AppDescriptor[] {
    if (!this.store) return [];
    const raw = this.store.getSync(CATALOG_CACHE_KEY, '') as string;
    if (!raw) return [];
    try {
      return JSON.parse(raw) as AppDescriptor[];
    } catch (_) {
      return [];
    }
  }

  async saveCatalogCache(apps: AppDescriptor[]): Promise<void> {
    if (!this.store) return;
    await this.store.put(CATALOG_CACHE_KEY, JSON.stringify(apps));
    await this.store.flush();
  }

  async removeCachedApp(appId: string): Promise<void> {
    const apps = this.getCatalogCache().filter((item: AppDescriptor) => item.id !== appId);
    await this.saveCatalogCache(apps);
    await this.removeAppSettings(appId);
  }

  async saveLibraryPath(path: string): Promise<void> {
    this.libraryPath = path;
    if (!this.store) return;
    await this.store.put(LIBRARY_PATH_KEY, path);
    await this.store.flush();
  }

  // ── 最近启动 (供"最近"分类, 最新在前, 持久化) ──

  private loadRecentLaunches(): void {
    if (!this.store) return;
    const raw = this.store.getSync(RECENT_LAUNCHES_KEY, '[]') as string;
    try {
      const values = JSON.parse(raw) as string[];
      this.recentLaunches = Array.isArray(values) ? values.filter((v) => typeof v === 'string') : [];
    } catch (_) {
      this.recentLaunches = [];
    }
  }

  /** 记录一次程序启动: 插入最前 (最近), 去重, 上限 20 条, 持久化。 */
  async recordRecentLaunch(appId: string): Promise<void> {
    if (!appId) return;
    this.recentLaunches = this.recentLaunches.filter((id: string) => id !== appId);
    this.recentLaunches.unshift(appId);
    if (this.recentLaunches.length > 20) {
      this.recentLaunches = this.recentLaunches.slice(0, 20);
    }
    if (!this.store) return;
    await this.store.put(RECENT_LAUNCHES_KEY, JSON.stringify(this.recentLaunches));
    await this.store.flush();
  }

  /** 最近启动的 appId 列表 (最新在前)。 */
  getRecentLaunches(): string[] {
    return this.recentLaunches.slice();
  }
}
