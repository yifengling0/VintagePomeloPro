/// <reference path="../ohos.systemShare.d.ts" />
import router from '@ohos.router';
import { PerformanceHudField, PerformanceHudSettings, hasPerformanceHudContent,
  normalizePerformanceHud, setPerformanceHudField } from '../model/PerformanceHudSettings';
import promptAction from '@ohos.promptAction';
import fs from '@ohos.file.fs';
import { bundleManager, common, ConfigurationConstant, Want } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { uniformTypeDescriptor as utd } from '@kit.ArkData';
import { systemShare } from '@kit.ShareKit';
import { fileUri, picker } from '@kit.CoreFileKit';
import testNapi from 'libentry.so';
import { BreakpointSystem, BreakpointType } from '../common/BreakpointSystem';
import {
  BOX64_PRESET_IDS,
  ControlBarDirection,
  D3DRendererPreference,
  DesktopWindowMode,
  DisplayMode,
  EngineState,
  GlobalSettings,
  InputProfile,
  INPUT_TEMPLATES,
  InputTemplate,
  ThemeMode,
  WineLanguage,
  ViewMode,
  VirtualMouseConfig,
  ScreenOrientationSetting,
  box64PresetDescription,
  box64PresetLabel,
  cloneVirtualMouseConfig,
  createDefaultGlobalSettings,
  inputTemplateForProfileId
} from '../model/AppModels';
import { ThemeSettingsService } from '../service/ThemeSettingsService';
import { EngineStateListener, WineEngineService } from '../service/WineEngineService';
import { GamepadManager } from '../service/GamepadManager';
import { WineWindowManager } from '../service/WineWindowManager';
import { DesktopModeService, MODE_VIRTUAL, MODE_FUSION } from '../service/DesktopModeService';
import { AppSessionService } from '../service/AppSessionService';
import { InputOverlayWindowCoordinator } from '../service/InputOverlayWindowCoordinator';
import { AppCatalogService } from '../service/AppCatalogService';
import { AppLog } from '../service/LogService';
import { hilog } from '../service/LogService';
import { InputProfileService } from '../service/InputProfileService';
import { AppSettingsStore } from '../service/AppSettingsStore';
import { LengthMetrics, window } from '@kit.ArkUI';
import pasteboard from '@ohos.pasteboard';
import deviceInfo from '@ohos.deviceInfo';
import { DeviceCapabilityPolicy } from '../common/DeviceCapabilityPolicy';
import {
  applyStatusOnlyImmersiveShell,
  getMiniTitleContentTopInset,
  getStatusOnlyWindowInsets,
  observeStatusOnlyWindowInsets
} from '../common/StatusOnlyImmersiveShell';
import {
  DividerShowType,
  HdsNavigation,
  HdsNavigationMenuContentOptions,
  HdsNavigationTitleMode,
  ScrollEffectType
} from '@kit.UIDesignKit';

const DOMAIN = 0x0000;

type SettingsSection = 'display' | 'appearance' | 'input' | 'engine' | 'performance' | 'logs' | 'about';

/** native extractFontZip 的返回结构 (与 libentry Index.d.ts 保持一致)。 */
interface FontZipExtractResult {
  ok: boolean;
  fonts: number;
  bad: number;
  firstBadExt: string;
  error: string;
}

const DEVICE_SDK_API_VERSION: number = (() => {
  try {
    return deviceInfo.sdkApiVersion as number;
  } catch (_) {
    return 0;
  }
})();
const SUPPORTS_HDS_FLOATING: boolean =
  canIUse('SystemCapability.UIDesign.HDSComponent.Core') && DEVICE_SDK_API_VERSION >= 23;
const HDS_MATERIAL_TYPE_IMMERSIVE: number = 101;
const HDS_MATERIAL_LEVEL_EXQUISITE: number = 0;

@CustomDialog
struct InputProfilePickerDialog {
  controller?: CustomDialogController;
  profiles: InputProfile[] = [];
  selectedId: string = 'default';
  compact: boolean = true;
  onSelect: (profileId: string) => void = (_profileId: string): void => {};
  onDelete: (profile: InputProfile) => void = (_profile: InputProfile): void => {};

  private builtInProfiles(): InputProfile[] {
    return this.profiles.filter((profile: InputProfile): boolean =>
      inputTemplateForProfileId(profile.id) !== undefined);
  }

  private customProfiles(): InputProfile[] {
    return this.profiles.filter((profile: InputProfile): boolean =>
      inputTemplateForProfileId(profile.id) === undefined);
  }

  private choose(profileId: string): void {
    if (this.controller) this.controller.close();
    this.onSelect(profileId);
  }

  private remove(profile: InputProfile): void {
    if (this.controller) this.controller.close();
    this.onDelete(profile);
  }

  @Builder
  private BuiltInChoice(profile: InputProfile) {
    Button() {
      Row({ space: 10 }) {
        SymbolGlyph(profile.id === this.selectedId ?
          $r('sys.symbol.checkmark_circle_fill') : $r('sys.symbol.circle'))
          .fontSize(21)
          .fontColor([profile.id === this.selectedId ?
            $r('app.color.accent') : $r('app.color.text_secondary')])
        Column({ space: 3 }) {
          Text(profile.name).fontSize(14).fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary')).maxLines(1).width('100%')
          Text(inputTemplateForProfileId(profile.id)?.description ?? '内置输入方案')
            .fontSize(10).fontColor($r('app.color.text_secondary')).maxLines(2).width('100%')
        }.alignItems(HorizontalAlign.Start).layoutWeight(1)
      }.width('100%')
    }
    .width(this.compact ? '100%' : '48%')
    .height(this.compact ? 70 : 78)
    .padding(11).borderRadius(14)
    .backgroundColor(profile.id === this.selectedId ?
      $r('app.color.selected_background') : $r('app.color.search_background'))
    .border({ width: 1, color: profile.id === this.selectedId ?
      $r('app.color.accent') : '#14000000' })
    .margin({ bottom: 8 })
    .id('picker-input-profile-' + profile.id)
    .onClick((): void => this.choose(profile.id))
  }

  @Builder
  private CustomChoice(profile: InputProfile) {
    Row({ space: 8 }) {
      Button() {
        Row({ space: 10 }) {
          SymbolGlyph(profile.id === this.selectedId ?
            $r('sys.symbol.checkmark_circle_fill') : $r('sys.symbol.circle'))
            .fontSize(21)
            .fontColor([profile.id === this.selectedId ?
              $r('app.color.accent') : $r('app.color.text_secondary')])
          Column({ space: 3 }) {
            Text(profile.name).fontSize(14).fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.text_primary')).maxLines(1).width('100%')
            Text(profile.id === this.selectedId ? '我的方案 · 使用中' : '我的方案')
              .fontSize(10).fontColor(profile.id === this.selectedId ?
                $r('app.color.accent') : $r('app.color.text_secondary')).width('100%')
          }.alignItems(HorizontalAlign.Start).layoutWeight(1)
        }.width('100%')
      }
      .layoutWeight(1).height(62).padding({ left: 10, right: 6 })
      .backgroundColor(Color.Transparent)
      .onClick((): void => this.choose(profile.id))
      Button('删除')
        .width(58).height(36).margin({ right: 8 }).borderRadius(10).fontSize(11)
        .backgroundColor('#18D94848').fontColor('#D94848')
        .id('picker-delete-input-profile-' + profile.id)
        .onClick((): void => this.remove(profile))
    }
    .width('100%').height(62).borderRadius(14)
    .backgroundColor(profile.id === this.selectedId ?
      $r('app.color.selected_background') : $r('app.color.search_background'))
    .margin({ bottom: 8 })
  }

  build() {
    Column({ space: 12 }) {
      Row() {
        Column({ space: 3 }) {
          Text('切换输入方案').fontSize(20).fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary')).width('100%')
          Text('内置方案可直接使用；自己的方案可在这里切换或删除。')
            .fontSize(11).fontColor($r('app.color.text_secondary')).width('100%')
        }.alignItems(HorizontalAlign.Start).layoutWeight(1)
        Button('关闭').height(38).borderRadius(12).fontSize(12)
          .backgroundColor($r('app.color.search_background')).fontColor($r('app.color.text_primary'))
          .onClick((): void => { if (this.controller) this.controller.close(); })
      }.width('100%')

      Scroll() {
        Column({ space: 6 }) {
          Text('内置方案').fontSize(12).fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_secondary')).width('100%')
          Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.SpaceBetween }) {
            ForEach(this.builtInProfiles(), (profile: InputProfile): void => {
              this.BuiltInChoice(profile)
            }, (profile: InputProfile): string => profile.id)
          }.width('100%')

          Text('我的方案').fontSize(12).fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_secondary')).width('100%').margin({ top: 2 })
          if (this.customProfiles().length === 0) {
            Text('还没有自定义方案，可关闭后点击“新建方案”。')
              .fontSize(11).fontColor($r('app.color.text_secondary')).width('100%')
              .padding(12).backgroundColor($r('app.color.search_background')).borderRadius(12)
          } else {
            ForEach(this.customProfiles(), (profile: InputProfile): void => {
              this.CustomChoice(profile)
            }, (profile: InputProfile): string => profile.id)
          }
        }.width('100%').alignItems(HorizontalAlign.Start)
      }
      .width('100%')
      .constraintSize({ maxHeight: this.compact ? 500 : 590 })
      .scrollBar(BarState.Auto)
    }
    .width(this.compact ? '94%' : '82%')
    .constraintSize({ maxWidth: 720 })
    .padding(this.compact ? 16 : 22)
    .backgroundColor($r('app.color.card_background'))
    .borderRadius(22)
  }
}

@Entry
@Component
struct SystemSettingsPage {
  @StorageLink('libraryTitleButtonInsetVp') private titleButtonInsetVp: number = 0;
  @State private settings: GlobalSettings = createDefaultGlobalSettings();
  // 浮窗模式独立开关: 关闭时恢复此前的全屏/切边选择
  private nonFloatingDesktopMode: DesktopWindowMode = DesktopWindowMode.FULLSCREEN;
  @State private breakpoint: BreakpointType = 'lg';
  @State private section: SettingsSection = 'display';
  @State private logBusy: boolean = false;
  @State private engineMessage: string = 'Wine 引擎未启动';
  @State private engineBusy: boolean = false;
  @State private engineOperationMessage: string = '';
  @State private inputProfiles: InputProfile[] = [];
  @State private showProfileCreator: boolean = false;
  @State private profileCreatorBaseId: string = 'current';
  @State private profileCreatorName: string = '';
  @State private profileCreating: boolean = false;
  @State private statusBarHeightVp: number = 0;
  @State private titleContentTopInsetVp: number = 40;
  @State private navBarInsetVp: number = 0;
  @State private appVersion: string = '版本信息读取中…';
  // host GPU 名称 + DXVK2.6 支持判定 (马良 920 以下不支持, 经 testNapi.getHostGpuName)
  @State private gpuName: string = '';
  @State private gpuSupportDxvk26: boolean = true;
  // 渲染模式 (dxvk_1_10/dxvk_2_6/virgl): @State 保证按钮选中态即时刷新
  @State private renderMode: string = 'dxvk_1_10';
  @State private fontImporting: boolean = false;
  @State private fontImportStage: string = '';
  @State private fontImportProgress: number = 0;
  @State private hubStateText: string = '等待手柄输入…';
  private context: common.UIAbilityContext = getContext(this) as common.UIAbilityContext;
  private settingsScroller: Scroller = new Scroller();
  private callback: ((value: BreakpointType) => void) | null = null;
  private engineCallback: EngineStateListener | null = null;
  private sidebarTarget: SettingsSection | null = null;
  private disposeWindowInsetObserver: (() => void) | null = null;
  private inputProfilePickerController: CustomDialogController | null = null;
  private hubPollTimer: number = -1;

  private isNarrowPhone(): boolean {
    return this.breakpoint === 'xs';
  }

  private usesDarkAppearance(): boolean {
    if (this.settings.themeMode === ThemeMode.DARK) return true;
    if (this.settings.themeMode === ThemeMode.LIGHT) return false;
    try {
      return this.context.config.colorMode === ConfigurationConstant.ColorMode.COLOR_MODE_DARK;
    } catch (_) {
      return false;
    }
  }

  private navigationTitleBackground(scrolled: boolean): string {
    if (this.usesDarkAppearance()) return scrolled ? '#80121821' : '#26121821';
    return scrolled ? '#99FFFFFF' : '#18FFFFFF';
  }

  private navigationControlBackground(): string {
    return this.usesDarkAppearance() ? '#30FFFFFF' : '#28FFFFFF';
  }

  private navigationTitleColor(): string {
    return this.usesDarkAppearance() ? '#F4F7FB' : '#182431';
  }

  private groupSurfaceColor(): string | Resource {
    if (!this.settings.useGlassEffect) return $r('app.color.card_background');
    return this.usesDarkAppearance() ? '#3A202833' : '#DDF5F9FD';
  }

  private groupBorderColor(): string {
    if (!this.settings.useGlassEffect) return '#14000000';
    return this.usesDarkAppearance() ? '#52FFFFFF' : '#8CFFFFFF';
  }

  private cardPadding(): number {
    return this.isNarrowPhone() ? 12 : 22;
  }

  private settingsTopInset(): number {
    return this.titleContentTopInsetVp;
  }

  private settingsSidePadding(): number {
    if (this.isNarrowPhone()) return 10;
    if (this.breakpoint === 'sm') return 16;
    if (this.breakpoint === 'md') return 20;
    return 28;
  }

  private settingsContentMaxWidth(): number {
    if (this.breakpoint === 'xl') return 1080;
    if (this.breakpoint === 'lg') return 960;
    return 100000;
  }

  private isManagedWindowsHost(): boolean {
    return AppSettingsStore.getInstance().usesManagedWineWindows();
  }

  private titleButtonMargin(): number {
    return this.titleButtonInsetVp > 0 ? this.titleButtonInsetVp + 44 : 0;
  }

  aboutToAppear(): void {
    this.settings = ThemeSettingsService.getInstance().getSettings();
    BreakpointSystem.initialize();
    this.callback = (value: BreakpointType): void => {
      this.breakpoint = value;
    };
    BreakpointSystem.subscribe(this.callback);
    this.engineCallback = (_state: EngineState, message: string): void => {
      this.engineMessage = message;
    };
    WineEngineService.getInstance().subscribe(this.engineCallback);
    this.loadInputProfiles();
    this.loadApplicationVersion();
    this.loadGpuInfo();
    this.renderMode = AppSettingsStore.getInstance().getRenderMode()
      || AppStorage.get<string>('winehua.render.mode')
      || 'dxvk_1_10';
    this.startHubPoll();
  }

  onPageShow(): void {
    this.settings = ThemeSettingsService.getInstance().getSettings();
    // 初始化非浮窗模式 (全屏/切边): 浮窗模式下 UI 高亮显示此前的选择,
    // 关闭浮窗开关时恢复该模式。
    if (this.settings.desktopWindowMode !== DesktopWindowMode.FLOATING) {
      this.nonFloatingDesktopMode = this.settings.desktopWindowMode;
    }
    this.loadInputProfiles();
    this.loadApplicationVersion();
    this.restoreSettingsWindowChrome();
  }

  onPageHide(): void {
    this.stopWindowInsetObserver();
  }

  private loadApplicationVersion(): void {
    try {
      const info = bundleManager.getBundleInfoForSelfSync(
        bundleManager.BundleFlag.GET_BUNDLE_INFO_DEFAULT);
      this.appVersion = '版本 ' + info.versionName + '（' + info.versionCode.toString() + '）';
    } catch (_) {
      this.appVersion = '版本信息不可用';
    }
  }

  private async restoreSettingsWindowChrome(): Promise<void> {
    try {
      const pageWindow = await window.getLastWindow(this.context);
      await applyStatusOnlyImmersiveShell(pageWindow);
      try {
        const useLightIcons = this.usesDarkAppearance();
        await pageWindow.setWindowSystemBarProperties({
          statusBarColor: '#00000000',
          navigationBarColor: '#00000000',
          isStatusBarLightIcon: useLightIcons,
          isNavigationBarLightIcon: useLightIcons
        });
      } catch (_) {
      }
      this.startWindowInsetObserver(pageWindow);
    } catch (_) {
    }
  }

  private refreshStatusBarInset(pageWindow: window.Window): void {
    this.titleContentTopInsetVp = getMiniTitleContentTopInset(pageWindow);
    const insets = getStatusOnlyWindowInsets(pageWindow);
    this.statusBarHeightVp = insets.top;
    this.navBarInsetVp = insets.bottom;
  }

  private startWindowInsetObserver(pageWindow: window.Window): void {
    this.stopWindowInsetObserver();
    this.disposeWindowInsetObserver = observeStatusOnlyWindowInsets(pageWindow, () => {
      this.refreshStatusBarInset(pageWindow);
    });
  }

  private stopWindowInsetObserver(): void {
    if (this.disposeWindowInsetObserver) {
      this.disposeWindowInsetObserver();
      this.disposeWindowInsetObserver = null;
    }
  }

  private loadInputProfiles(): void {
    InputProfileService.getInstance().initialize(getContext(this) as common.UIAbilityContext).then((): void => {
      this.inputProfiles = InputProfileService.getInstance().getDesktopProfiles();
      if (!InputProfileService.getInstance().hasDesktopProfile(this.settings.desktopInputProfileId)) {
        this.selectDesktopProfile('default');
      }
    }).catch((): void => {});
  }

  aboutToDisappear(): void {
    if (this.callback) BreakpointSystem.unsubscribe(this.callback);
    if (this.engineCallback) WineEngineService.getInstance().unsubscribe(this.engineCallback);
    this.stopWindowInsetObserver();
    this.stopHubPoll();
  }

  onBackPress(): boolean {
    return this.engineBusy;
  }

  private async runEngineOperation(message: string, task: () => Promise<boolean>, successMessage: string): Promise<void> {
    if (this.engineBusy) return;
    this.engineBusy = true;
    this.engineOperationMessage = message;
    try {
      const succeeded = await task();
      promptAction.showToast({ message: succeeded ? successMessage : WineEngineService.getInstance().getMessage() });
    } catch (_) {
      promptAction.showToast({ message: 'Wine 引擎操作失败，请查看引擎状态' });
    } finally {
      this.engineBusy = false;
      this.engineOperationMessage = '';
    }
  }

  private restartEngine(): void {
    const engine = WineEngineService.getInstance();
    const mode = engine.getMode() ?? this.settings.defaultDisplayMode;
    const homeDirectory = AppCatalogService.getInstance().getWineHomeDirectory();
    if (!homeDirectory) {
      promptAction.showToast({
        message: '无法创建 Download/com.vintage.pomelopro/games，请先重试目录扫描'
      });
      return;
    }
    this.runEngineOperation('正在关闭 Windows 会话并重建 Wine/Wayland 服务…',
      (): Promise<boolean> => AppSessionService.getInstance().restartEngine(mode, homeDirectory),
      'Wine 引擎已重新启动');
  }

  private stopEngine(): void {
    this.runEngineOperation('正在停止全部 Windows 会话…', async (): Promise<boolean> => {
      await AppSessionService.getInstance().stopAll();
      return true;
    }, 'Wine 引擎已停止');
  }

  private resetPrefix(): void {
    const preserve = this.settings.preserveUserDataOnReset;
    this.runEngineOperation('正在重置 Wine prefix，请勿退出应用…', async (): Promise<boolean> => {
      await AppSessionService.getInstance().resetPrefix();
      return true;
    }, preserve ? 'Wine 沙盒已重建，文档和注册表已还原' : 'Wine 环境已重置');
  }

  private setPreserveUserDataOnReset(value: boolean): void {
    this.update(this.settings.defaultDisplayMode, this.settings.viewMode,
      this.settings.useGlassEffect, this.settings.usePageBackground,
      this.settings.showVirtualControls, this.settings.autoHideVirtualControls,
      this.settings.controlOpacity, this.settings.gamepadDeadzone,
      this.settings.outputScale, this.settings.themeMode,
      this.settings.controlBarDirection, this.settings.virtualMouseConfig,
      this.settings.desktopInputProfileId, this.settings.pageBackgroundUri,
      this.settings.virtualInputEnabled, this.settings.desktopOrientation,
      this.settings.controlBarPositionX, this.settings.controlBarPositionY,
      this.settings.d3dRendererPreference, this.settings.desktopWindowMode,
      this.settings.keepScreenOn, value);
  }

  private resetPrefixConfirmMessage(): string {
    if (this.settings.preserveUserDataOnReset) {
      return '将清空 Wine 沙盒并重装运行时，然后还原文档和注册表。损坏的系统目录会被重建。';
    }
    return '将删除当前 Wine prefix 中的全部数据，且无法撤销。';
  }

  private async exportLogs(): Promise<void> {
    if (this.logBusy) return;
    this.logBusy = true;
    try {
      // 打包整个 logs 目录为 zip，用系统分享面板（ShareKit）分享。
      // utd 用 general.object 而非 general.file：QQ/微信等 IM 的分享扩展
      // 只注册了 general.object/text 等泛类型（bm dump 实测 QQ 无 general.file），
      // 声明泛类型后 QQ 才会出现在面板中。
      const uri = await AppLog.exportLogsZip();
      const shareData = new systemShare.SharedData({
        utd: utd.UniformDataType.OBJECT,
        uri: uri,
        title: '旧柚Pro 日志'
      });
      const controller = new systemShare.ShareController(shareData);
      await controller.show(this.context, {
        selectionMode: systemShare.SelectionMode.SINGLE,
        previewMode: systemShare.SharePreviewMode.DETAIL
      });
    } catch (error) {
      const err = error as BusinessError;
      hilog.error(DOMAIN, 'Settings', 'systemShare failed code=%{public}d msg=%{public}s',
        err.code, err.message);
      promptAction.showToast({ message: '分享失败，日志仍保留在 下载/旧柚Pro/logs 目录' });
    } finally {
      this.logBusy = false;
    }
  }

  private clearLogs(): void {
    AlertDialog.show({
      title: '清空日志',
      message: '将删除 logs 目录下全部应用日志（含历史归档与导出快照），无法恢复。',
      primaryButton: { value: '取消', action: (): void => {} },
      secondaryButton: {
        value: '清空',
        fontColor: '#D94848',
        action: (): void => {
          AppLog.clearLogs();
          promptAction.showToast({ message: '日志已清空' });
        }
      }
    });
  }

  private update(mode: DisplayMode = this.settings.defaultDisplayMode,
    view: ViewMode = this.settings.viewMode,
    glass: boolean = this.settings.useGlassEffect,
    background: boolean = this.settings.usePageBackground,
    controls: boolean = this.settings.showVirtualControls,
    autoHide: boolean = this.settings.autoHideVirtualControls,
    opacity: number = this.settings.controlOpacity,
    deadzone: number = this.settings.gamepadDeadzone,
    scale: number = this.settings.outputScale,
    theme: ThemeMode = this.settings.themeMode,
    controlBarDirection: ControlBarDirection = this.settings.controlBarDirection,
     virtualMouseConfig: VirtualMouseConfig = cloneVirtualMouseConfig(this.settings.virtualMouseConfig),
     desktopInputProfileId: string = this.settings.desktopInputProfileId,
     pageBackgroundUri: string = this.settings.pageBackgroundUri,
      virtualInputEnabled: boolean = this.settings.virtualInputEnabled,
      desktopOrientation: ScreenOrientationSetting = this.settings.desktopOrientation,
      controlBarPositionX: number = this.settings.controlBarPositionX,
      controlBarPositionY: number = this.settings.controlBarPositionY,
      d3dRendererPreference: D3DRendererPreference = this.settings.d3dRendererPreference,
      desktopWindowMode: DesktopWindowMode = this.settings.desktopWindowMode,
      keepScreenOn: boolean = this.settings.keepScreenOn,
      preserveUserDataOnReset: boolean = this.settings.preserveUserDataOnReset,
      showFpsHud: boolean = this.settings.showFpsHud,
      performanceHud: PerformanceHudSettings = this.settings.performanceHud,
      wineLanguage: WineLanguage = this.settings.wineLanguage): void {
    const virtualInputChanged = this.settings.virtualInputEnabled !== virtualInputEnabled;
    this.settings = {
      defaultDisplayMode: mode,
      wineLanguage: wineLanguage,
       desktopOrientation: desktopOrientation,
      desktopWindowMode: desktopWindowMode,
      keepScreenOn: keepScreenOn,
      box64Preset: this.settings.box64Preset,
      desktopInputProfileId: desktopInputProfileId || 'default',
      viewMode: view,
      themeMode: theme,
      useGlassEffect: glass,
      usePageBackground: background,
      pageBackgroundUri: pageBackgroundUri,
      virtualInputEnabled: virtualInputEnabled,
      showVirtualControls: controls,
      autoHideVirtualControls: autoHide,
      controlBarDirection: controlBarDirection,
      controlBarPositionX: controlBarPositionX,
      controlBarPositionY: controlBarPositionY,
      virtualMouseConfig: cloneVirtualMouseConfig(virtualMouseConfig),
      controlOpacity: opacity,
      gamepadDeadzone: deadzone,
      controllerOutputMode: this.settings.controllerOutputMode ?? 'dinput',
      outputScale: scale,
      d3dRendererPreference: d3dRendererPreference,
      preserveUserDataOnReset: preserveUserDataOnReset,
      showFpsHud: showFpsHud,
      performanceHud: normalizePerformanceHud(performanceHud)
    };
    GamepadManager.getInstance().setDeadzone(deadzone);
    WineWindowManager.getInstance().setOutputScalePercent(scale);
    ThemeSettingsService.getInstance().update(this.settings);
    InputOverlayWindowCoordinator.getInstance().setOpacity(opacity);
    InputOverlayWindowCoordinator.getInstance().setControlsVisible(controls);
    if (virtualInputChanged) {
      InputOverlayWindowCoordinator.getInstance().setEnabled(virtualInputEnabled).catch((): void => {});
    }
  }

  private updatePageBackground(uri: string, enabled: boolean): void {
    this.update(this.settings.defaultDisplayMode, this.settings.viewMode,
      this.settings.useGlassEffect, enabled,
      this.settings.showVirtualControls, this.settings.autoHideVirtualControls,
      this.settings.controlOpacity, this.settings.gamepadDeadzone,
      this.settings.outputScale, this.settings.themeMode,
      this.settings.controlBarDirection, this.settings.virtualMouseConfig,
      this.settings.desktopInputProfileId, uri, this.settings.virtualInputEnabled);
  }

  private updateVirtualInputEnabled(enabled: boolean): void {
    this.update(this.settings.defaultDisplayMode, this.settings.viewMode,
      this.settings.useGlassEffect, this.settings.usePageBackground,
      this.settings.showVirtualControls, this.settings.autoHideVirtualControls,
      this.settings.controlOpacity, this.settings.gamepadDeadzone,
      this.settings.outputScale, this.settings.themeMode,
      this.settings.controlBarDirection, this.settings.virtualMouseConfig,
      this.settings.desktopInputProfileId, this.settings.pageBackgroundUri, enabled);
  }

  private updateDesktopOrientation(orientation: ScreenOrientationSetting): void {
    this.update(this.settings.defaultDisplayMode, this.settings.viewMode,
      this.settings.useGlassEffect, this.settings.usePageBackground,
      this.settings.showVirtualControls, this.settings.autoHideVirtualControls,
      this.settings.controlOpacity, this.settings.gamepadDeadzone,
      this.settings.outputScale, this.settings.themeMode,
      this.settings.controlBarDirection, this.settings.virtualMouseConfig,
      this.settings.desktopInputProfileId, this.settings.pageBackgroundUri,
      this.settings.virtualInputEnabled, orientation);
  }

  private updateKeepScreenOn(enabled: boolean): void {
    this.update(this.settings.defaultDisplayMode, this.settings.viewMode,
      this.settings.useGlassEffect, this.settings.usePageBackground,
      this.settings.showVirtualControls, this.settings.autoHideVirtualControls,
      this.settings.controlOpacity, this.settings.gamepadDeadzone,
      this.settings.outputScale, this.settings.themeMode,
      this.settings.controlBarDirection, this.settings.virtualMouseConfig,
      this.settings.desktopInputProfileId, this.settings.pageBackgroundUri,
      this.settings.virtualInputEnabled, this.settings.desktopOrientation,
      this.settings.controlBarPositionX, this.settings.controlBarPositionY,
      this.settings.d3dRendererPreference, this.settings.desktopWindowMode,
      enabled);
  }

  private updatePerformanceHud(field: PerformanceHudField, enabled: boolean): void {
    this.updateShowFpsHud(this.settings.showFpsHud,
      setPerformanceHudField(this.settings.performanceHud, field, enabled));
  }

  private updateShowFpsHud(enabled: boolean,
    metrics: PerformanceHudSettings = this.settings.performanceHud): void {
    this.update(this.settings.defaultDisplayMode, this.settings.viewMode,
      this.settings.useGlassEffect, this.settings.usePageBackground,
      this.settings.showVirtualControls, this.settings.autoHideVirtualControls,
      this.settings.controlOpacity, this.settings.gamepadDeadzone,
      this.settings.outputScale, this.settings.themeMode,
      this.settings.controlBarDirection, this.settings.virtualMouseConfig,
      this.settings.desktopInputProfileId, this.settings.pageBackgroundUri,
      this.settings.virtualInputEnabled, this.settings.desktopOrientation,
      this.settings.controlBarPositionX, this.settings.controlBarPositionY,
      this.settings.d3dRendererPreference, this.settings.desktopWindowMode,
      this.settings.keepScreenOn, this.settings.preserveUserDataOnReset, enabled, metrics);
  }

  /**
   * PC 桌面模式切换 (融合=每程序独立窗口 / 虚拟桌面=独立窗口承载 Wine 桌面)。
   * 持久化到 DesktopService (Preferences); 模式需重启 Wine 才生效 —
   * 直接重启应用最干净 (结束 Wine 进程并重新初始化), 弹窗给用户选择。
   * await setMode: 确保 Preferences 落盘完成再给"重启应用"入口, 否则立即
   * 重启会读到旧模式 (flush 未完成丢失写入)。
   */
  private async updateDesktopMode(mode: string): Promise<void> {
    await DesktopModeService.getInstance().setMode(mode);
    AlertDialog.show({
      title: '桌面模式已切换',
      message: '需要重启应用（结束 Wine 进程并重新初始化桌面）才能生效。是否立即重启？',
      primaryButton: {
        value: '稍后',
        action: (): void => {}
      },
      secondaryButton: {
        value: '重启应用',
        fontColor: '#D94848',
        action: (): void => { this.restartAppAfterImport(); }
      }
    });
  }

  private togglePageBackground(enabled: boolean): void {
    if (enabled && this.settings.pageBackgroundUri.length === 0) {
      this.selectPageBackground();
      return;
    }
    this.updatePageBackground(this.settings.pageBackgroundUri, enabled);
  }

  private async selectPageBackground(): Promise<void> {
    try {
      const documentPicker = new picker.DocumentViewPicker(this.context);
      const options = new picker.DocumentSelectOptions();
      options.fileSuffixFilters = ['图片|.png,.jpg,.jpeg,.webp,.bmp'];
      options.maxSelectNumber = 1;
      const result = await documentPicker.select(options);
      if (!result || result.length === 0) return;
      this.updatePageBackground(result[0], true);
      promptAction.showToast({ message: '页面背景已更新' });
    } catch (_) {
      promptAction.showToast({ message: '未选择背景图片' });
    }
  }

  private clearPageBackground(): void {
    this.updatePageBackground('', false);
  }

  // ── 字体导入 (ZIP → C:\windows\Fonts) ──
  private static readonly WINE_WINDOWS_DIR: string =
    '/data/storage/el2/base/files/.wine/drive_c/windows/';

  private wineFontsDir(): string {
    try {
      if (fs.accessSync(SystemSettingsPage.WINE_WINDOWS_DIR + 'Fonts')) {
        return SystemSettingsPage.WINE_WINDOWS_DIR + 'Fonts';
      }
      if (fs.accessSync(SystemSettingsPage.WINE_WINDOWS_DIR + 'fonts')) {
        return SystemSettingsPage.WINE_WINDOWS_DIR + 'fonts';
      }
    } catch (_) {
    }
    return SystemSettingsPage.WINE_WINDOWS_DIR + 'Fonts';
  }

  private async importFontZip(): Promise<void> {
    let selected: string[] = [];
    try {
      const documentPicker = new picker.DocumentViewPicker(this.context);
      const options = new picker.DocumentSelectOptions();
      options.fileSuffixFilters = ['字体包|.zip'];
      options.maxSelectNumber = 1;
      selected = await documentPicker.select(options);
    } catch (_) {
      promptAction.showToast({ message: '未选择字体压缩包' });
      return;
    }
    if (!selected || selected.length === 0) return;
    const stamp = Date.now();
    const cacheZip = this.context.cacheDir + '/font_import_' + stamp + '.zip';
    const tmpDir = this.context.cacheDir + '/font_import_' + stamp;
    this.fontImporting = true;
    this.fontImportProgress = 0;
    try {
      this.fontImportStage = '正在复制字体包…';
      await new Promise<void>((resolve: () => void) => setTimeout(resolve, 30));
      // picker 返回 file:// URI, 先经 fs 打开再复制到应用缓存, 解压用真实路径。
      const srcFile = fs.openSync(selected[0], fs.OpenMode.READ_ONLY);
      await fs.copyFile(srcFile.fd, cacheZip);
      fs.closeSync(srcFile);
      await fs.mkdir(tmpDir, true);
      this.fontImportStage = '正在解压字体包…';
      await new Promise<void>((resolve: () => void) => setTimeout(resolve, 30));
      // native 异步解压: 后台线程执行, 避免大字体包阻塞 UI 线程导致
      // AppFreeze/ANR 杀进程。原始条目名只参与扩展名/魔数判断。
      let result: FontZipExtractResult;
      try {
        result = await testNapi.extractFontZipAsync(cacheZip, tmpDir) as FontZipExtractResult;
      } catch (_) {
        promptAction.showDialog({
          title: '解压失败',
          message: '无法解压所选文件，请确认是有效的 ZIP 压缩包。',
          buttons: [{ text: '确定', color: '#5569D8' }]
        });
        await this.removeDirRecursive(tmpDir);
        try { fs.unlinkSync(cacheZip); } catch (_) {}
        this.fontImporting = false;
        return;
      }
      if (!result.ok) {
        promptAction.showDialog({
          title: '解压失败',
          message: result.error || '无法解压所选文件，请确认是有效的 ZIP 压缩包。',
          buttons: [{ text: '确定', color: '#5569D8' }]
        });
        await this.removeDirRecursive(tmpDir);
        try { fs.unlinkSync(cacheZip); } catch (_) {}
        this.fontImporting = false;
        return;
      }
      if (result.bad > 0) {
        const badInfo = result.firstBadExt && result.firstBadExt !== '(无扩展名)'
          ? '如 .' + result.firstBadExt : result.firstBadExt;
        promptAction.showDialog({
          title: '字体包无效',
          message: '压缩包包含非字体文件' +
            (badInfo ? '（' + badInfo + '）' : '') +
            '，已取消导入，共 ' + result.bad + ' 个。',
          buttons: [{ text: '确定', color: '#5569D8' }]
        });
        await this.removeDirRecursive(tmpDir);
        try { fs.unlinkSync(cacheZip); } catch (_) {}
        this.fontImporting = false;
        return;
      }
      // native 落盘的 font-NNN.ext 均为合法字体, 直接列目录即可。
      const fonts: string[] = await fs.listFile(tmpDir);
      if (fonts.length === 0) {
        promptAction.showDialog({
          title: '字体包无效',
          message: '压缩包中没有字体文件。',
          buttons: [{ text: '确定', color: '#5569D8' }]
        });
        await this.removeDirRecursive(tmpDir);
        try { fs.unlinkSync(cacheZip); } catch (_) {}
        this.fontImporting = false;
        return;
      }
      this.fontImportStage = '';
      this.fontImportProgress = 0;
      // 确认对话框由系统管理, 先撤掉阻塞遮罩, 避免用户点外部关闭后遮罩残留。
      this.fontImporting = false;
      // 确认将删除旧字体
      AlertDialog.show({
        title: '导入字体',
        message: '将导入 ' + fonts.length + ' 个字体（含子目录），并删除 C:\\windows\\Fonts 下所有旧字体文件。' +
          '将保留原始字体文件名（中文文件名会自动转码为 UTF-8）。是否继续？',
        primaryButton: {
          value: '取消',
          action: (): void => {
            this.removeDirRecursive(tmpDir).catch((): void => {});
            try { fs.unlinkSync(cacheZip); } catch (_) {}
            this.fontImporting = false;
          }
        },
        secondaryButton: {
          value: '导入',
          fontColor: '#D94848',
          action: (): void => {
            this.applyFontImport(tmpDir, cacheZip, fonts);
          }
        }
      });
    } catch (_) {
      promptAction.showToast({ message: '导入失败，请稍后重试' });
      await this.removeDirRecursive(tmpDir);
      try { fs.unlinkSync(cacheZip); } catch (_) {}
      this.fontImporting = false;
    }
  }

  /** 把 native 已改名 (font-NNN.ext) 的字体复制到 C:\windows\Fonts。 */
  private async applyFontImport(tmpDir: string, cacheZip: string, fonts: string[]): Promise<void> {
    try {
      const fontsDir = this.wineFontsDir();
      this.fontImporting = true;
      this.fontImportProgress = 0;
      this.fontImportStage = '正在清除旧字体…';
      await new Promise<void>((resolve: () => void) => setTimeout(resolve, 30));
      await this.removeDirRecursive(fontsDir);
      await fs.mkdir(fontsDir, true);
      for (let index = 0; index < fonts.length; index++) {
        await fs.copyFile(tmpDir + '/' + fonts[index], fontsDir + '/' + fonts[index]);
        this.fontImportProgress = Math.round((index + 1) / fonts.length * 100);
        this.fontImportStage = '正在写入字体 ' + (index + 1) + '/' + fonts.length;
        await new Promise<void>((resolve: () => void) => setTimeout(resolve, 16));
      }
      await this.removeDirRecursive(tmpDir);
      try { fs.unlinkSync(cacheZip); } catch (_) {}
      this.fontImporting = false;
      this.fontImportProgress = 100;
      AlertDialog.show({
        title: '字体导入完成',
        message: '已导入 ' + fonts.length + ' 个字体到 C:\\windows\\Fonts。' +
          '字体需要重启应用（结束 App 进程）后才会生效，是否立即重启？',
        primaryButton: {
          value: '稍后',
          action: (): void => {}
        },
        secondaryButton: {
          value: '重启应用',
          fontColor: '#D94848',
          action: (): void => { this.restartAppAfterImport(); }
        }
      });
    } catch (_) {
      promptAction.showToast({ message: '字体写入失败' });
      await this.removeDirRecursive(tmpDir);
      try { fs.unlinkSync(cacheZip); } catch (_) {}
      this.fontImporting = false;
    }
  }

  /** 导入字体后辅助用户做 App 级重启, 让 Wine 重新扫描 C:\windows\Fonts。 */
  private async restartAppAfterImport(): Promise<void> {
    // restartApp 是原子化服务 (atomicservice) 专用 API；普通应用调用可能
    // 不受支持而失败。先尝试，失败则关闭应用，用户重新打开后字体生效。
    try {
      const want: Want = {
        bundleName: 'com.vintage.pomelopro',
        abilityName: 'EntryAbility'
      };
      await this.context.restartApp(want);
    } catch (_) {
      try {
        await this.context.terminateSelf();
      } catch (_) {
        promptAction.showToast({ message: '自动重启失败，请手动关闭应用后重新打开' });
      }
    }
  }

  private async removeDirRecursive(path: string): Promise<void> {
    try {
      try {
        await fs.access(path);
      } catch (_) {
        return;
      }
      const entries = await fs.listFile(path);
      for (const entry of entries) {
        const full = path + '/' + entry;
        try {
          const stat = await fs.stat(full);
          if (stat.isDirectory()) {
            await this.removeDirRecursive(full);
          } else {
            await fs.unlink(full);
          }
        } catch (_) {
        }
      }
      await fs.rmdir(path);
    } catch (_) {
    }
  }

  private updateTheme(theme: ThemeMode): void {
    this.update(this.settings.defaultDisplayMode, this.settings.viewMode,
      this.settings.useGlassEffect, this.settings.usePageBackground,
      this.settings.showVirtualControls, this.settings.autoHideVirtualControls,
      this.settings.controlOpacity, this.settings.gamepadDeadzone,
      this.settings.outputScale, theme);
  }

  private updateControlBarDirection(direction: ControlBarDirection): void {
    this.update(this.settings.defaultDisplayMode, this.settings.viewMode,
      this.settings.useGlassEffect, this.settings.usePageBackground,
      this.settings.showVirtualControls, this.settings.autoHideVirtualControls,
      this.settings.controlOpacity, this.settings.gamepadDeadzone,
      this.settings.outputScale, this.settings.themeMode, direction);
  }

  /** host GPU 名称 + DXVK2.6 支持判定: 马良 (Mali-Gxxx) 920 以下不支持 DXVK2.6。 */
  private loadGpuInfo(): void {
    try {
      const name = (testNapi.getHostGpuName() as string) ?? '';
      this.gpuName = name;
      // 马良 GPU: "Maleoon 910" / "Mali-G920" 等。920 以下不支持 DXVK2.6。
      const m = /(?:Maleoon|Mali-?G)\s*(\d+)/i.exec(name);
      if (m) {
        this.gpuSupportDxvk26 = parseInt(m[1], 10) >= 920;
      } else {
        // 非马良 (Adreno/自研) 或未知: 默认支持 (按实际能力)
        this.gpuSupportDxvk26 = true;
      }
      hilog.info(DOMAIN, 'Settings', 'GPU=%{public}s dxvk26=%{public}s',
        name, this.gpuSupportDxvk26 ? 'yes' : 'no');
    } catch (_) {
      this.gpuSupportDxvk26 = true;
    }
  }

  /** 渲染模式: dxvk_1_10 / dxvk_2_6 / virgl。默认 1.10；2.6 仅 920+ 可选（实验）。 */
  private updateRenderMode(mode: string): void {
    const selected = mode === 'dxvk_2_6' || mode === 'dxvk_1_10' || mode === 'virgl' ? mode : 'dxvk_1_10';
    const current = AppSettingsStore.getInstance().getRenderMode()
      || AppStorage.get<string>('winehua.render.mode')
      || 'dxvk_1_10';
    if (current === selected) return;
    this.renderMode = selected;
    promptAction.showToast({ message: '渲染模式已切换，正在重启引擎…' });
    void AppSettingsStore.getInstance().setRenderMode(selected).then((): void => {
      this.restartEngine();
    });
  }

  /** DXVK 版本芯片与渲染模式共用同一持久化项。 */
  private updateDxvkVersion(version: string): void {
    this.updateRenderMode(version === 'dxvk_modern_2_6' ? 'dxvk_2_6' : 'dxvk_1_10');
  }

  private updateD3DRendererPreference(preference: D3DRendererPreference): void {
    if (preference === this.settings.d3dRendererPreference) return;
    this.update(this.settings.defaultDisplayMode, this.settings.viewMode,
      this.settings.useGlassEffect, this.settings.usePageBackground,
      this.settings.showVirtualControls, this.settings.autoHideVirtualControls,
      this.settings.controlOpacity, this.settings.gamepadDeadzone,
      this.settings.outputScale, this.settings.themeMode,
      this.settings.controlBarDirection, this.settings.virtualMouseConfig,
      this.settings.desktopInputProfileId, this.settings.pageBackgroundUri,
      this.settings.virtualInputEnabled, this.settings.desktopOrientation,
      this.settings.controlBarPositionX, this.settings.controlBarPositionY, preference);
    promptAction.showToast({ message: '渲染策略已保存；重启 Wine 引擎后生效' });
  }

  private updateBox64Preset(preset: string): void {
    if (preset === this.settings.box64Preset) return;
    this.settings = {
      defaultDisplayMode: this.settings.defaultDisplayMode,
      wineLanguage: this.settings.wineLanguage,
      desktopOrientation: this.settings.desktopOrientation,
      desktopWindowMode: this.settings.desktopWindowMode,
      keepScreenOn: this.settings.keepScreenOn,
      box64Preset: preset,
      desktopInputProfileId: this.settings.desktopInputProfileId,
      viewMode: this.settings.viewMode,
      themeMode: this.settings.themeMode,
      useGlassEffect: this.settings.useGlassEffect,
      usePageBackground: this.settings.usePageBackground,
      pageBackgroundUri: this.settings.pageBackgroundUri,
      virtualInputEnabled: this.settings.virtualInputEnabled,
      showVirtualControls: this.settings.showVirtualControls,
      autoHideVirtualControls: this.settings.autoHideVirtualControls,
      controlBarDirection: this.settings.controlBarDirection,
      controlBarPositionX: this.settings.controlBarPositionX,
      controlBarPositionY: this.settings.controlBarPositionY,
      virtualMouseConfig: cloneVirtualMouseConfig(this.settings.virtualMouseConfig),
      controlOpacity: this.settings.controlOpacity,
      gamepadDeadzone: this.settings.gamepadDeadzone,
      controllerOutputMode: this.settings.controllerOutputMode ?? 'dinput',
      outputScale: this.settings.outputScale,
      d3dRendererPreference: this.settings.d3dRendererPreference,
      preserveUserDataOnReset: this.settings.preserveUserDataOnReset,
      showFpsHud: this.settings.showFpsHud,
      performanceHud: normalizePerformanceHud(this.settings.performanceHud)
    };
    void ThemeSettingsService.getInstance().update(this.settings);
    promptAction.showToast({ message: '兼容性预设已保存，新启动的程序生效' });
  }

  private startHubPoll(): void {
    this.stopHubPoll();
    this.refreshHubState();
    this.hubPollTimer = setInterval((): void => this.refreshHubState(), 250);
  }

  private stopHubPoll(): void {
    if (this.hubPollTimer >= 0) {
      clearInterval(this.hubPollTimer);
      this.hubPollTimer = -1;
    }
  }

  private refreshHubState(): void {
    try {
      this.hubStateText = testNapi.controllerGetStateText(0) + '  ' +
        GamepadManager.getInstance().getRumbleStatusText();
    } catch (_) {
      this.hubStateText = 'Hub 不可用';
    }
  }

  private updateControllerOutputMode(mode: 'dinput' | 'keyboard_legacy'): void {
    if (this.settings.controllerOutputMode === mode) return;
    this.settings = {
      defaultDisplayMode: this.settings.defaultDisplayMode,
      wineLanguage: this.settings.wineLanguage,
      desktopOrientation: this.settings.desktopOrientation,
      desktopWindowMode: this.settings.desktopWindowMode,
      keepScreenOn: this.settings.keepScreenOn,
      box64Preset: this.settings.box64Preset,
      desktopInputProfileId: this.settings.desktopInputProfileId,
      viewMode: this.settings.viewMode,
      themeMode: this.settings.themeMode,
      useGlassEffect: this.settings.useGlassEffect,
      usePageBackground: this.settings.usePageBackground,
      pageBackgroundUri: this.settings.pageBackgroundUri,
      virtualInputEnabled: this.settings.virtualInputEnabled,
      showVirtualControls: this.settings.showVirtualControls,
      autoHideVirtualControls: this.settings.autoHideVirtualControls,
      controlBarDirection: this.settings.controlBarDirection,
      controlBarPositionX: this.settings.controlBarPositionX,
      controlBarPositionY: this.settings.controlBarPositionY,
      virtualMouseConfig: cloneVirtualMouseConfig(this.settings.virtualMouseConfig),
      controlOpacity: this.settings.controlOpacity,
      gamepadDeadzone: this.settings.gamepadDeadzone,
      controllerOutputMode: mode,
      outputScale: this.settings.outputScale,
      d3dRendererPreference: this.settings.d3dRendererPreference,
      preserveUserDataOnReset: this.settings.preserveUserDataOnReset,
      showFpsHud: this.settings.showFpsHud,
      performanceHud: normalizePerformanceHud(this.settings.performanceHud)
    };
    GamepadManager.getInstance().setOutputMode(mode);
    void ThemeSettingsService.getInstance().update(this.settings);
    promptAction.showToast({
      message: mode === 'dinput'
        ? '已切换 Wine 手柄（XInput/DirectInput）；请重新启动 Wine 会话后生效'
        : '已切换键盘模拟；请重新启动 Wine 会话后生效'
    });
  }

  private updateWineLanguage(language: WineLanguage): void {
    if (language === this.settings.wineLanguage) return;
    this.update(this.settings.defaultDisplayMode, this.settings.viewMode,
      this.settings.useGlassEffect, this.settings.usePageBackground,
      this.settings.showVirtualControls, this.settings.autoHideVirtualControls,
      this.settings.controlOpacity, this.settings.gamepadDeadzone,
      this.settings.outputScale, this.settings.themeMode,
      this.settings.controlBarDirection, this.settings.virtualMouseConfig,
      this.settings.desktopInputProfileId, this.settings.pageBackgroundUri,
      this.settings.virtualInputEnabled, this.settings.desktopOrientation,
      this.settings.controlBarPositionX, this.settings.controlBarPositionY,
      this.settings.d3dRendererPreference, this.settings.desktopWindowMode,
      this.settings.keepScreenOn, this.settings.preserveUserDataOnReset,
      this.settings.showFpsHud, this.settings.performanceHud, language);
    promptAction.showToast({ message: '系统语言已保存；重启 Wine 引擎后生效' });
  }

  private updateDesktopWindowMode(mode: DesktopWindowMode): void {
    if (mode === this.settings.desktopWindowMode) return;
    // 记录非浮窗选择 (全屏/切边), 浮窗开关关闭时恢复
    if (mode !== DesktopWindowMode.FLOATING) {
      this.nonFloatingDesktopMode = mode;
    }
    this.update(this.settings.defaultDisplayMode, this.settings.viewMode,
      this.settings.useGlassEffect, this.settings.usePageBackground,
      this.settings.showVirtualControls, this.settings.autoHideVirtualControls,
      this.settings.controlOpacity, this.settings.gamepadDeadzone,
      this.settings.outputScale, this.settings.themeMode,
      this.settings.controlBarDirection, this.settings.virtualMouseConfig,
      this.settings.desktopInputProfileId, this.settings.pageBackgroundUri,
      this.settings.virtualInputEnabled, this.settings.desktopOrientation,
      this.settings.controlBarPositionX, this.settings.controlBarPositionY,
      this.settings.d3dRendererPreference, mode);
    promptAction.showToast({ message: '桌面窗口显示已保存；重新打开桌面窗口后生效' });
  }

  /** 浮窗模式独立开关: 开启=桌面由 Index 承载 (悬浮窗↔全屏切换), 关闭=恢复全屏/切边。 */
  private updateFloatingMode(enabled: boolean): void {
    if (enabled) {
      this.updateDesktopWindowMode(DesktopWindowMode.FLOATING);
    } else {
      this.updateDesktopWindowMode(this.nonFloatingDesktopMode);
    }
  }

  /** 桌面窗口显示选中态: 浮窗模式下高亮显示当前非浮窗选择 (关闭浮窗后恢复的模式)。 */
  private isDesktopWindowModeSelected(mode: DesktopWindowMode): boolean {
    if (this.settings.desktopWindowMode === DesktopWindowMode.FLOATING) {
      return this.nonFloatingDesktopMode === mode;
    }
    return this.settings.desktopWindowMode === mode;
  }

  private updateVirtualMouse(sensitivity: number = this.settings.virtualMouseConfig.mouseSensitivity,
    showCursor: boolean = this.settings.virtualMouseConfig.showCursor): void {
    const mouse = cloneVirtualMouseConfig(this.settings.virtualMouseConfig);
    mouse.mouseSensitivity = Math.max(50, Math.min(200, Math.round(sensitivity / 10) * 10));
    mouse.showCursor = showCursor;
    this.update(this.settings.defaultDisplayMode, this.settings.viewMode,
      this.settings.useGlassEffect, this.settings.usePageBackground,
      this.settings.showVirtualControls, this.settings.autoHideVirtualControls,
      this.settings.controlOpacity, this.settings.gamepadDeadzone,
      this.settings.outputScale, this.settings.themeMode,
      this.settings.controlBarDirection, mouse);
  }

  private selectDesktopProfile(profileId: string): void {
    this.update(this.settings.defaultDisplayMode, this.settings.viewMode,
      this.settings.useGlassEffect, this.settings.usePageBackground,
      this.settings.showVirtualControls, this.settings.autoHideVirtualControls,
      this.settings.controlOpacity, this.settings.gamepadDeadzone,
      this.settings.outputScale, this.settings.themeMode,
      this.settings.controlBarDirection, this.settings.virtualMouseConfig, profileId);
  }

  private openProfileCreator(baseId: string = 'current'): void {
    this.showProfileCreator = true;
    this.selectProfileCreatorBase(baseId);
  }

  private selectProfileCreatorBase(baseId: string): void {
    this.profileCreatorBaseId = baseId;
    if (baseId === 'current') {
      this.profileCreatorName = (this.selectedProfileName() + ' 副本').slice(0, 40);
      return;
    }
    const templateId = baseId.startsWith('template:') ? baseId.slice('template:'.length) : baseId;
    const template = INPUT_TEMPLATES.find((item: InputTemplate): boolean => item.id === templateId);
    this.profileCreatorName = ((template?.name ?? '自定义') + ' 方案').slice(0, 40);
  }

  private normalizedProfileCreatorName(): string {
    return this.profileCreatorName.replace(/\s+/g, ' ').trim();
  }

  private profileCreatorError(): string {
    const name = this.normalizedProfileCreatorName();
    if (name.length === 0) return '请输入方案名称';
    if (name.length > 40) return '方案名称不能超过 40 个字符';
    if (InputProfileService.getInstance().hasDesktopProfileName(name)) return '已有同名方案，请换一个名称';
    return '';
  }

  private async createNamedProfile(): Promise<void> {
    if (this.profileCreating) return;
    const error = this.profileCreatorError();
    if (error.length > 0) {
      promptAction.showToast({ message: error });
      return;
    }
    this.profileCreating = true;
    try {
      const service = InputProfileService.getInstance();
      const name = this.normalizedProfileCreatorName();
      let profile: InputProfile;
      if (this.profileCreatorBaseId === 'current') {
        profile = await service.duplicateProfile(this.settings.desktopInputProfileId || 'default', name);
      } else {
        const templateId = this.profileCreatorBaseId.startsWith('template:') ?
          this.profileCreatorBaseId.slice('template:'.length) : this.profileCreatorBaseId;
        profile = await service.createProfileFromTemplate(name, templateId);
      }
      this.inputProfiles = service.getDesktopProfiles();
      this.selectDesktopProfile(profile.id);
      this.showProfileCreator = false;
      promptAction.showToast({ message: '已创建并切换到“' + profile.name + '”' });
    } catch (_) {
      promptAction.showToast({ message: '创建方案失败，请重试' });
    } finally {
      this.profileCreating = false;
    }
  }

  private requestRemoveInputProfile(profile: InputProfile): void {
    if (inputTemplateForProfileId(profile.id)) return;
    AlertDialog.show({
      title: '删除输入方案',
      message: '确定删除“' + profile.name + '”吗？只会删除这份按键配置，不会影响游戏文件。',
      primaryButton: { value: '取消', action: (): void => {} },
      secondaryButton: { value: '删除', fontColor: '#D94848', action: (): void => {
        const wasActive = this.settings.desktopInputProfileId === profile.id;
        InputProfileService.getInstance().removeProfile(profile.id).then((removed: boolean): void => {
          if (!removed) {
            promptAction.showToast({ message: '内置方案不能删除' });
            return;
          }
          this.inputProfiles = InputProfileService.getInstance().getDesktopProfiles();
          if (wasActive) this.selectDesktopProfile('default');
          promptAction.showToast({ message: '已删除“' + profile.name + '”' });
        }).catch((): void => {
          promptAction.showToast({ message: '删除方案失败' });
        });
      } }
    });
  }

  private async importInputProfile(): Promise<void> {
    try {
      const documentPicker = new picker.DocumentViewPicker(this.context);
      const result = await documentPicker.select({ maxSelectNumber: 1 });
      if (!result || result.length === 0) return;
      const uri = result[0];
      // 文档选择器返回的可能是 file://docs/ 这类虚拟 URI，直接交给
      // readTextSync 在部分设备上会报错；先尝试原样读，失败再转成
      // 真实路径重试（与导出侧 FileUri(uri).path 保持一致）。
      let content = '';
      try {
        content = fs.readTextSync(uri);
      } catch (readError) {
        try {
          const realPath = new fileUri.FileUri(uri).path;
          hilog.warn(DOMAIN, 'Settings', 'import .icp read via raw URI failed, retry path=%{public}s err=%{public}s',
            realPath, JSON.stringify(readError));
          content = fs.readTextSync(realPath);
        } catch (pathError) {
          hilog.error(DOMAIN, 'Settings', 'import .icp read failed uri=%{public}s err=%{public}s',
            uri, JSON.stringify(pathError));
          throw new Error('read .icp failed');
        }
      }
      const service = InputProfileService.getInstance();
      const profile = await service.importProfileFromIcp(content);
      this.inputProfiles = service.getDesktopProfiles();
      this.selectDesktopProfile(profile.id);
      const total = profile.elements.length;
      promptAction.showToast({ message: '已导入 ' + profile.name + '（' +
        total.toString() + ' 个控件）' });
    } catch (error) {
      hilog.error(DOMAIN, 'Settings', 'importInputProfile failed: %{public}s', JSON.stringify(error));
      promptAction.showToast({ message: '导入失败：请选择有效的 .icp / JSON 文件' });
    }
  }

  private async exportCurrentInputProfile(): Promise<void> {
    try {
      const service = InputProfileService.getInstance();
      const path = await service.exportProfileToFile(this.settings.desktopInputProfileId || 'default');
      promptAction.showToast({ message: '已导出：' + path });
    } catch (_) {
      promptAction.showToast({ message: '导出失败：请检查 Download 目录是否可写' });
    }
  }

  private resetSelectedBuiltInProfile(): void {
    const profileId = this.settings.desktopInputProfileId || 'default';
    const template = inputTemplateForProfileId(profileId);
    if (!template) return;
    AlertDialog.show({
      title: '恢复内置方案',
      message: '“' + template.profileName + '”将恢复为出厂布局，对它的修改会丢失；我的方案不受影响。',
      primaryButton: { value: '取消', action: (): void => {} },
      secondaryButton: { value: '恢复', fontColor: '#D94848', action: (): void => {
        InputProfileService.getInstance().resetBuiltInProfile(profileId).then((): void => {
          this.inputProfiles = InputProfileService.getInstance().getDesktopProfiles();
          promptAction.showToast({ message: '“' + template.profileName + '”已恢复' });
        }).catch((): void => {
          promptAction.showToast({ message: '恢复失败' });
        });
      } }
    });
  }

  private restoreDefaults(): void {
    const defaults = createDefaultGlobalSettings();
    this.settings = defaults;
    GamepadManager.getInstance().setDeadzone(defaults.gamepadDeadzone);
    WineWindowManager.getInstance().setOutputScalePercent(defaults.outputScale);
    ThemeSettingsService.getInstance().update(defaults);
    InputOverlayWindowCoordinator.getInstance().setOpacity(defaults.controlOpacity);
    InputOverlayWindowCoordinator.getInstance().setControlsVisible(defaults.showVirtualControls);
    InputOverlayWindowCoordinator.getInstance().setEnabled(defaults.virtualInputEnabled).catch((): void => {});
  }

  @Builder
  private SectionButton(label: string, section: SettingsSection, icon: Resource) {
    Row({ space: 12 }) {
      SymbolGlyph(icon).fontSize(18).fontColor([$r('app.color.text_primary')])
      Text(label).fontSize(14).fontColor($r('app.color.text_primary'))
        .fontWeight(this.section === section ? FontWeight.Medium : FontWeight.Regular)
    }
      .width('100%').height(48).padding({ left: 14, right: 14 }).borderRadius(16)
      .backgroundColor(this.section === section ? $r('app.color.selected_background') : '#00FFFFFF')
      .border({ width: this.section === section ? 1 : 0, color: this.groupBorderColor() })
      .id('settings-nav-' + section)
      .onClick(() => this.selectSection(section))
  }

  @Builder
  private SectionChip(label: string, section: SettingsSection, icon: Resource) {
    Button() {
      Row({ space: 7 }) {
        SymbolGlyph(icon).fontSize(16)
          .fontColor([this.section === section ? $r('app.color.accent') : $r('app.color.text_secondary')])
        Text(label).fontSize(13).fontWeight(this.section === section ? FontWeight.Medium : FontWeight.Regular)
          .fontColor(this.section === section ? $r('app.color.accent') : $r('app.color.text_primary'))
      }
    }
    .height(40)
    .padding({ left: 13, right: 13 })
    .borderRadius(20)
    .backgroundColor(this.section === section ? $r('app.color.selected_background') : '#18FFFFFF')
    .border({ width: 1, color: this.section === section ? '#38FFFFFF' : '#22FFFFFF' })
    .onClick((): void => this.selectSection(section))
  }

  private sectionIndex(section: SettingsSection): number {
    if (section === 'display') return 0;
    if (section === 'appearance') return 1;
    if (section === 'input') return 2;
    if (section === 'engine') return 3;
    if (section === 'performance') return 4;
    if (section === 'logs') return 5;
    return 6;
  }

  private sectionAt(index: number): SettingsSection {
    if (index <= 0) return 'display';
    if (index === 1) return 'appearance';
    if (index === 2) return 'input';
    if (index === 3) return 'engine';
    if (index === 4) return 'performance';
    if (index === 5) return 'logs';
    return 'about';
  }

  private selectSection(section: SettingsSection): void {
    this.section = section;
    // The List reports every intermediate first-visible item while a smooth
    // sidebar jump is in progress. Keep the clicked item highlighted until
    // that programmatic scroll stops instead of flashing through each section.
    this.sidebarTarget = section;
    try {
      this.settingsScroller.scrollToIndex(this.sectionIndex(section), true, ScrollAlign.START);
    } catch (_) {
      this.sidebarTarget = null;
    }
  }

  @Builder
  private SwitchRow(title: string, description: string, enabled: boolean,
    onChange: (value: boolean) => void, id: string = '') {
    Row() {
      Column({ space: 4 }) {
        Text(title).fontSize(15).fontWeight(FontWeight.Medium).fontColor($r('app.color.text_primary'))
        Text(description).fontSize(11).fontColor($r('app.color.text_secondary'))
      }.alignItems(HorizontalAlign.Start).layoutWeight(1)
      Toggle({ type: ToggleType.Switch, isOn: enabled }).onChange((value: boolean) => onChange(value))
    }
    .width('100%')
    .padding({ left: 14, right: 14, top: 13, bottom: 13 })
    .backgroundColor($r('app.color.search_background'))
    .borderRadius(14)
    .id(id)
  }

  @Builder
  private PageBackgroundChooser() {
    Column({ space: 12 }) {
      Row({ space: 14 }) {
        Stack({ alignContent: Alignment.Center }) {
          if (this.settings.pageBackgroundUri.length > 0) {
            Image(this.settings.pageBackgroundUri)
              .width('100%').height('100%').objectFit(ImageFit.Cover)
          } else {
            SymbolGlyph($r('sys.symbol.rectangle_grid_1x2')).fontSize(26)
              .fontColor([$r('app.color.text_secondary')])
          }
        }
        .width(112).height(72).borderRadius(12).clip(true)
        .backgroundColor($r('app.color.search_background'))
        .border({ width: 1, color: '#1F000000' })
        .id('page-background-preview')
        Column({ space: 4 }) {
          Text(this.settings.pageBackgroundUri.length > 0 ? '已选择背景图片' : '尚未选择背景')
            .fontSize(14).fontWeight(FontWeight.Medium).fontColor($r('app.color.text_primary'))
          Text('建议使用横向图片，应用库会自动裁切填充')
            .fontSize(11).fontColor($r('app.color.text_secondary'))
        }.alignItems(HorizontalAlign.Start).layoutWeight(1)
      }.width('100%')
      Row({ space: 10 }) {
        Button('选择图片').height(44).borderRadius(14)
          .backgroundColor($r('app.color.selected_background')).fontColor($r('app.color.accent'))
          .id('page-background-select')
          .onClick((): void => { this.selectPageBackground(); })
        if (this.settings.pageBackgroundUri.length > 0) {
          Button('移除图片').height(44).borderRadius(14)
            .backgroundColor($r('app.color.search_background')).fontColor('#D94848')
            .id('page-background-clear')
            .onClick((): void => this.clearPageBackground())
        }
      }.width('100%')
    }
    .width('100%')
    .id('page-background-chooser')
  }

  @Builder
  private ControlBarDirectionChip(label: string, direction: ControlBarDirection) {
    Button(label)
      .height(40)
      .padding({ left: 14, right: 14 })
      .borderRadius(20)
      .fontSize(13)
      .fontColor(this.settings.controlBarDirection === direction ? '#FFFFFFFF' : $r('app.color.text_primary'))
      .backgroundColor(this.settings.controlBarDirection === direction ? $r('app.color.accent') : $r('app.color.search_background'))
      .id('control-bar-direction-' + direction)
      .onClick((): void => this.updateControlBarDirection(direction))
  }

  @Builder
  private D3DRendererChip(label: string, preference: D3DRendererPreference) {
    Button(label)
      .height(42)
      .layoutWeight(1)
      .borderRadius(14)
      .fontSize(13)
      .fontColor(this.settings.d3dRendererPreference === preference ?
        '#FFFFFFFF' : $r('app.color.text_primary'))
      .backgroundColor(this.settings.d3dRendererPreference === preference ?
        $r('app.color.accent') : $r('app.color.search_background'))
      .id('d3d-renderer-' + preference)
      .onClick((): void => this.updateD3DRendererPreference(preference))
  }

  private dxvkVersionActive(version: string): boolean {
    if (version === 'dxvk_modern_2_6') {
      return this.renderMode === 'dxvk_2_6';
    }
    if (version === 'dxvk_legacy') {
      return this.renderMode === 'dxvk_1_10';
    }
    return false;
  }

  private renderModeActive(mode: string): boolean {
    const effective = (this.renderMode === 'dxvk_2_6' && !this.gpuSupportDxvk26)
      ? 'dxvk_1_10' : this.renderMode;
    return effective === mode;
  }

  @Builder
  private RenderModeChip(label: string, mode: string, enabled: boolean = true) {
    Button() {
      if (this.isNarrowPhone() && (mode === 'dxvk_2_6' || mode === 'dxvk_1_10')) {
        Column({ space: 0 }) {
          Text(mode === 'dxvk_2_6' ? 'DXVK 2.6' : 'DXVK')
            .fontSize(10)
            .fontWeight(FontWeight.Medium)
            .fontColor(!enabled ? '#666' : (this.renderModeActive(mode) ? '#FFFFFFFF' : $r('app.color.text_primary')))
            .maxLines(1)
          Text(mode === 'dxvk_2_6' ? '实验' : '1.10')
            .fontSize(13)
            .fontWeight(FontWeight.Bold)
            .fontColor(!enabled ? '#666' : (this.renderModeActive(mode) ? '#FFFFFFFF' : $r('app.color.text_primary')))
            .maxLines(1)
        }
        .width('100%')
        .alignItems(HorizontalAlign.Center)
        .justifyContent(FlexAlign.Center)
      } else {
        Text(label)
          .fontSize(13)
          .fontColor(!enabled ? '#666' : (this.renderModeActive(mode) ? '#FFFFFFFF' : $r('app.color.text_primary')))
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
    }
      .height(this.isNarrowPhone() ? 48 : 42)
      .layoutWeight(1)
      .borderRadius(14)
      .enabled(enabled)
      .backgroundColor(!enabled ? '#222' : (this.renderModeActive(mode) ? $r('app.color.accent') : $r('app.color.search_background')))
      .id('render-mode-' + mode)
      .onClick((): void => this.updateRenderMode(mode))
  }

  @Builder
  private DxvkVersionChip(label: string, version: string, enabled: boolean = true) {
    Button(label)
      .height(42)
      .layoutWeight(1)
      .borderRadius(14)
      .fontSize(13)
      .enabled(enabled)
      .fontColor(!enabled ? '#666' : (this.dxvkVersionActive(version) ? '#FFFFFFFF' : $r('app.color.text_primary')))
      .backgroundColor(!enabled ? '#222' : (this.dxvkVersionActive(version) ? $r('app.color.accent') : $r('app.color.search_background')))
      .id('dxvk-version-' + version)
      .onClick((): void => this.updateDxvkVersion(version))
  }

  private selectedProfileName(): string {
    const selected = this.inputProfiles.find((profile: InputProfile): boolean =>
      profile.id === this.settings.desktopInputProfileId);
    return selected?.name ?? '默认输入方案';
  }

  private selectedProfileDescription(): string {
    const template = inputTemplateForProfileId(this.settings.desktopInputProfileId || 'default');
    if (template) return '内置方案 · ' + template.description;
    const selected = this.inputProfiles.find((profile: InputProfile): boolean =>
      profile.id === this.settings.desktopInputProfileId);
    return '我的方案 · ' + (selected?.elements.length ?? 0).toString() + ' 个控件';
  }

  private compactProfileDialog(): boolean {
    return this.breakpoint === 'xs' || this.breakpoint === 'sm';
  }

  private openInputProfilePicker(): void {
    if (this.inputProfilePickerController) return;
    this.inputProfilePickerController = new CustomDialogController({
      builder: InputProfilePickerDialog({
        profiles: this.inputProfiles,
        selectedId: this.settings.desktopInputProfileId || 'default',
        compact: this.compactProfileDialog(),
        onSelect: (profileId: string): void => {
          this.inputProfilePickerController = null;
          if (profileId === this.settings.desktopInputProfileId) return;
          this.selectDesktopProfile(profileId);
          promptAction.showToast({ message: '已切换到“' + this.selectedProfileName() + '”' });
        },
        onDelete: (profile: InputProfile): void => {
          this.inputProfilePickerController = null;
          this.requestRemoveInputProfile(profile);
        }
      }),
      cancel: (): void => { this.inputProfilePickerController = null; },
      autoCancel: true,
      alignment: DialogAlignment.Center,
      customStyle: true
    });
    try {
      this.inputProfilePickerController.open();
    } catch (_) {
      this.inputProfilePickerController = null;
      promptAction.showToast({ message: '无法打开方案选择器' });
    }
  }

  @Builder
  private ProfileCreatorBaseChoice(baseId: string, title: string, description: string) {
    Button() {
      Column({ space: 3 }) {
        Text(title).fontSize(13).fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_primary')).maxLines(1).width('100%')
        Text(description).fontSize(10).fontColor($r('app.color.text_secondary'))
          .maxLines(2).width('100%')
      }.width('100%').alignItems(HorizontalAlign.Start)
    }
    .width(this.compactProfileDialog() ? '100%' : '48%')
    .height(66).padding(10).borderRadius(12)
    .backgroundColor(this.profileCreatorBaseId === baseId ?
      $r('app.color.selected_background') : $r('app.color.card_background'))
    .border({ width: 1, color: this.profileCreatorBaseId === baseId ?
      $r('app.color.accent') : this.groupBorderColor() })
    .margin({ right: 7, bottom: 7 })
    .id('profile-base-' + baseId.replace(':', '-'))
    .onClick((): void => this.selectProfileCreatorBase(baseId))
  }

  @Builder
  private InputProfileCreator() {
    Column({ space: 11 }) {
      Row() {
        Column({ space: 3 }) {
          Text('新建我的方案').fontSize(15).fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
          Text('先选择一个起点，再为新方案命名。创建后会自动切换。')
            .fontSize(10).fontColor($r('app.color.text_secondary'))
        }.alignItems(HorizontalAlign.Start).layoutWeight(1)
        Text('取消').fontSize(12).fontColor($r('app.color.text_secondary'))
          .onClick((): void => { this.showProfileCreator = false; })
      }.width('100%')

      Text('1  选择起点').fontSize(12).fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary')).width('100%')
      Flex({ wrap: FlexWrap.Wrap }) {
        this.ProfileCreatorBaseChoice('current', '复制当前', '保留“' + this.selectedProfileName() + '”的全部设置')
        ForEach(INPUT_TEMPLATES, (template: InputTemplate): void => {
          this.ProfileCreatorBaseChoice('template:' + template.id, template.name, template.description)
        }, (template: InputTemplate): string => template.id)
      }.width('100%')

      Text('2  输入名称').fontSize(12).fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary')).width('100%')
      TextInput({ text: this.profileCreatorName, placeholder: '例如：仙剑快捷键、我的 FPS 布局' })
        .width('100%').height(46).borderRadius(12).maxLength(40)
        .backgroundColor($r('app.color.card_background'))
        .id('new-input-profile-name')
        .onChange((value: string): void => { this.profileCreatorName = value; })
      if (this.profileCreatorError().length > 0) {
        Text(this.profileCreatorError()).fontSize(10).fontColor('#D94848').width('100%')
      }
      Button(this.profileCreating ? '正在创建…' : '创建并使用')
        .width('100%').height(46).borderRadius(12)
        .backgroundColor($r('app.color.accent')).fontColor('#FFFFFFFF')
        .enabled(!this.profileCreating && this.profileCreatorError().length === 0)
        .id('confirm-create-input-profile')
        .onClick((): void => { this.createNamedProfile(); })
    }
    .width('100%').padding(13).borderRadius(14)
    .backgroundColor($r('app.color.selected_background'))
    .border({ width: 1, color: $r('app.color.accent') })
  }

  @Builder
  private DesktopInputProfiles() {
    Column({ space: 11 }) {
      Text('输入方案').fontSize(16).fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.text_primary')).width('100%')
      Text('当前只显示正在使用的方案；需要时打开选择器快速切换。')
        .fontSize(11).fontColor($r('app.color.text_secondary')).width('100%')

      Button() {
        Row({ space: 12 }) {
          Stack({ alignContent: Alignment.Center }) {
            SymbolGlyph($r('sys.symbol.keyboard')).fontSize(23).fontColor([$r('app.color.accent')])
          }
          .width(44).height(44).borderRadius(14)
          .backgroundColor($r('app.color.selected_background'))
          Column({ space: 3 }) {
            Text(this.selectedProfileName()).fontSize(15).fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.text_primary')).maxLines(1).width('100%')
            Text(this.selectedProfileDescription()).fontSize(10)
              .fontColor($r('app.color.text_secondary')).maxLines(2).width('100%')
          }.alignItems(HorizontalAlign.Start).layoutWeight(1)
          Text('切换').fontSize(12).fontColor($r('app.color.accent'))
        }.width('100%')
      }
      .width('100%').height(72).padding({ left: 12, right: 12 }).borderRadius(14)
      .backgroundColor($r('app.color.card_background'))
      .border({ width: 1, color: this.groupBorderColor() })
      .id('open-input-profile-picker')
      .onClick((): void => this.openInputProfilePicker())

      Row({ space: 8 }) {
        Button('切换方案').layoutWeight(1).height(46).borderRadius(12)
          .backgroundColor($r('app.color.selected_background')).fontColor($r('app.color.accent'))
          .fontSize(12)
          .id('switch-input-profile')
          .onClick((): void => this.openInputProfilePicker())
        Button(this.showProfileCreator ? '收起新建' : '新建方案')
          .layoutWeight(1).height(46).borderRadius(12)
          .backgroundColor($r('app.color.selected_background')).fontColor($r('app.color.accent'))
          .fontSize(12)
          .id('open-input-profile-creator')
          .onClick((): void => {
            if (this.showProfileCreator) this.showProfileCreator = false;
            else this.openProfileCreator('current');
          })
      }.width('100%')
      if (this.showProfileCreator) {
        this.InputProfileCreator()
      }

      Row({ space: 8 }) {
        Button('导入方案').layoutWeight(1).height(44).borderRadius(12)
          .backgroundColor($r('app.color.card_background')).fontColor($r('app.color.text_primary'))
          .fontSize(12)
          .id('import-input-profile')
          .onClick((): void => { this.importInputProfile(); })
        Button('导出当前方案').layoutWeight(1).height(44).borderRadius(12)
          .backgroundColor($r('app.color.card_background')).fontColor($r('app.color.text_primary'))
          .fontSize(12)
          .id('export-input-profile')
          .onClick((): void => { this.exportCurrentInputProfile(); })
      }.width('100%')
      if (inputTemplateForProfileId(this.settings.desktopInputProfileId || 'default')) {
        Button('恢复当前内置方案')
          .width('100%').height(42).borderRadius(12)
          .backgroundColor($r('app.color.card_background')).fontColor('#D94848')
          .fontSize(12)
          .id('reset-builtin-input-profile')
          .onClick((): void => { this.resetSelectedBuiltInProfile(); })
      }
    }
    .width('100%').padding(14).backgroundColor($r('app.color.search_background')).borderRadius(14)
  }

  @Builder
  private InputLayoutEntry() {
    Row({ space: 14 }) {
      Stack({ alignContent: Alignment.Center }) {
        SymbolGlyph($r('sys.symbol.gamecontroller')).fontSize(22).fontColor([$r('app.color.accent')])
      }
      .width(48).height(48).borderRadius(16).backgroundColor($r('app.color.selected_background'))
      Column({ space: 4 }) {
        Text('编辑当前桌面方案布局').fontSize(15).fontWeight(FontWeight.Medium).fontColor($r('app.color.text_primary'))
        Text(this.selectedProfileName() + '：在画布上自由摆放按钮、摇杆、方向键与触摸板，逐元素设置绑定、形状与透明度').fontSize(11).fontColor($r('app.color.text_secondary')).maxLines(2)
      }
      .layoutWeight(1)
      .alignItems(HorizontalAlign.Start)
      SymbolGlyph($r('sys.symbol.chevron_right')).fontSize(18).fontColor([$r('app.color.text_secondary')])
    }
    .width('100%').padding(14).backgroundColor($r('app.color.search_background')).borderRadius(14)
    .id('open-input-layout-editor')
    .onClick((): void => { router.pushUrl({
      url: 'pages/InputLayoutEditor',
      params: {
        profileId: this.settings.desktopInputProfileId || 'default',
        profileName: this.selectedProfileName(),
        baseProfileId: this.settings.desktopInputProfileId || 'default'
      }
    }); })
  }

  @Builder
  private Content(section: SettingsSection) {
    Column({ space: 14 }) {
      if (section === 'display') {
        Column({ space: 16 }) {
          Text('Windows 显示').fontSize(20).fontWeight(FontWeight.Bold).width('100%')
          Text('程序统一在 Wine 桌面中运行，由游戏自行切换到全屏，无需单独适配画面拉伸。')
            .fontSize(12).fontColor($r('app.color.text_secondary')).width('100%')
           Text('桌面屏幕方向').fontSize(14).fontWeight(FontWeight.Medium)
             .fontColor($r('app.color.text_primary')).width('100%')
           Row({ space: 8 }) {
             Button('自动横屏').layoutWeight(1).height(48).borderRadius(14)
               .fontSize(this.isNarrowPhone() ? 11 : 13)
               .padding({ left: this.isNarrowPhone() ? 4 : 12, right: this.isNarrowPhone() ? 4 : 12 })
               .backgroundColor(this.settings.desktopOrientation === ScreenOrientationSetting.SENSOR_LANDSCAPE ? $r('app.color.accent') : $r('app.color.search_background'))
               .fontColor(this.settings.desktopOrientation === ScreenOrientationSetting.SENSOR_LANDSCAPE ? '#FFFFFFFF' : $r('app.color.text_primary'))
               .onClick(() => this.updateDesktopOrientation(ScreenOrientationSetting.SENSOR_LANDSCAPE))
             Button('横屏').layoutWeight(1).height(48).borderRadius(14)
               .fontSize(this.isNarrowPhone() ? 11 : 13)
               .padding({ left: this.isNarrowPhone() ? 4 : 12, right: this.isNarrowPhone() ? 4 : 12 })
               .backgroundColor(this.settings.desktopOrientation === ScreenOrientationSetting.LANDSCAPE ? $r('app.color.accent') : $r('app.color.search_background'))
               .fontColor(this.settings.desktopOrientation === ScreenOrientationSetting.LANDSCAPE ? '#FFFFFFFF' : $r('app.color.text_primary'))
               .onClick(() => this.updateDesktopOrientation(ScreenOrientationSetting.LANDSCAPE))
             Button('竖屏').layoutWeight(1).height(48).borderRadius(14)
               .fontSize(this.isNarrowPhone() ? 11 : 13)
               .padding({ left: this.isNarrowPhone() ? 4 : 12, right: this.isNarrowPhone() ? 4 : 12 })
               .backgroundColor(this.settings.desktopOrientation === ScreenOrientationSetting.PORTRAIT ? $r('app.color.accent') : $r('app.color.search_background'))
               .fontColor(this.settings.desktopOrientation === ScreenOrientationSetting.PORTRAIT ? '#FFFFFFFF' : $r('app.color.text_primary'))
               .onClick(() => this.updateDesktopOrientation(ScreenOrientationSetting.PORTRAIT))
           }.width('100%')
           Text('桌面窗口显示').fontSize(14).fontWeight(FontWeight.Medium)
             .fontColor($r('app.color.text_primary')).width('100%')
           Row({ space: 8 }) {
             Button('全屏（沉浸）').layoutWeight(1).height(48).borderRadius(14)
               .fontSize(this.isNarrowPhone() ? 11 : 13)
               .padding({ left: this.isNarrowPhone() ? 4 : 12, right: this.isNarrowPhone() ? 4 : 12 })
               .backgroundColor(this.isDesktopWindowModeSelected(DesktopWindowMode.FULLSCREEN) ? $r('app.color.accent') : $r('app.color.search_background'))
               .fontColor(this.isDesktopWindowModeSelected(DesktopWindowMode.FULLSCREEN) ? '#FFFFFFFF' : $r('app.color.text_primary'))
               .onClick(() => this.updateDesktopWindowMode(DesktopWindowMode.FULLSCREEN))
             Button('切边（安全区）').layoutWeight(1).height(48).borderRadius(14)
               .fontSize(this.isNarrowPhone() ? 11 : 13)
               .padding({ left: this.isNarrowPhone() ? 4 : 12, right: this.isNarrowPhone() ? 4 : 12 })
               .backgroundColor(this.isDesktopWindowModeSelected(DesktopWindowMode.SAFE_AREA) ? $r('app.color.accent') : $r('app.color.search_background'))
               .fontColor(this.isDesktopWindowModeSelected(DesktopWindowMode.SAFE_AREA) ? '#FFFFFFFF' : $r('app.color.text_primary'))
               .onClick(() => this.updateDesktopWindowMode(DesktopWindowMode.SAFE_AREA))
           }.width('100%')
           Text('圆角屏全屏可能遮挡开始菜单；切边模式保留状态栏与圆角安全区。').fontSize(12)
             .fontColor($r('app.color.text_secondary')).width('100%')
          if (DeviceCapabilityPolicy.isTabletDevice() || DeviceCapabilityPolicy.isPhoneDevice()) {
            this.SwitchRow('浮窗模式',
              '桌面缩成可拖动小窗，点击放大；放大在当前画面完成，不切换窗口',
              this.settings.desktopWindowMode === DesktopWindowMode.FLOATING,
              (value: boolean): void => this.updateFloatingMode(value),
              'floating-mode')
          }
          if (!DeviceCapabilityPolicy.isTabletDevice() && !DeviceCapabilityPolicy.isPhoneDevice()) {
            Text('桌面模式').fontSize(14).fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.text_primary')).width('100%')
            Row({ space: 8 }) {
              Button('融合（每程序独立窗口）').layoutWeight(1).height(48).borderRadius(14)
                .fontSize(this.isNarrowPhone() ? 11 : 13)
                .padding({ left: this.isNarrowPhone() ? 4 : 12, right: this.isNarrowPhone() ? 4 : 12 })
                .backgroundColor(DesktopModeService.getInstance().mode === MODE_FUSION ? $r('app.color.accent') : $r('app.color.search_background'))
                .fontColor(DesktopModeService.getInstance().mode === MODE_FUSION ? '#FFFFFFFF' : $r('app.color.text_primary'))
                .onClick(() => { this.updateDesktopMode(MODE_FUSION); })
              Button('虚拟桌面').layoutWeight(1).height(48).borderRadius(14)
                .fontSize(this.isNarrowPhone() ? 11 : 13)
                .padding({ left: this.isNarrowPhone() ? 4 : 12, right: this.isNarrowPhone() ? 4 : 12 })
                .backgroundColor(DesktopModeService.getInstance().mode === MODE_VIRTUAL ? $r('app.color.accent') : $r('app.color.search_background'))
                .fontColor(DesktopModeService.getInstance().mode === MODE_VIRTUAL ? '#FFFFFFFF' : $r('app.color.text_primary'))
                .onClick(() => { this.updateDesktopMode(MODE_VIRTUAL); })
            }.width('100%')
            Text('融合：每个程序独立窗口；虚拟桌面：独立窗口承载 Wine 桌面（重启引擎后生效）。').fontSize(12)
              .fontColor($r('app.color.text_secondary')).width('100%')
          }
          this.SwitchRow('桌面打开时保持亮屏',
            '游玩中自动锁屏会中断操作；关闭后恢复系统默认的自动锁屏策略',
            this.settings.keepScreenOn, (value: boolean): void => this.updateKeepScreenOn(value),
            'keep-screen-on')
          Text('输出缩放 ' + this.settings.outputScale + '%').fontSize(14).fontColor($r('app.color.text_primary')).width('100%')
           Slider({ value: this.settings.outputScale, min: 75, max: 150, step: 5 })
             // Match VintagePomelo: a press on the track must not immediately
             // commit a new value while the user is scrolling the page.
             .sliderInteractionMode(2)
             .onChange((value: number, mode: SliderChangeMode) => {
               if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                 this.update(this.settings.defaultDisplayMode, this.settings.viewMode,
                   this.settings.useGlassEffect, this.settings.usePageBackground,
                   this.settings.showVirtualControls, this.settings.autoHideVirtualControls,
                   this.settings.controlOpacity, this.settings.gamepadDeadzone, value);
               }
             })
        }
        .width('100%')
        .padding(this.cardPadding())
        .backgroundColor(this.groupSurfaceColor())
        .backgroundBlurStyle(this.settings.useGlassEffect ? BlurStyle.COMPONENT_ULTRA_THICK : BlurStyle.NONE)
        .border({ width: 1, color: this.groupBorderColor() })
        .borderRadius(20)
      } else if (section === 'appearance') {
        Column({ space: 16 }) {
          Text('外观').fontSize(20).fontWeight(FontWeight.Bold).width('100%')
          Text('主题').fontSize(12).fontColor($r('app.color.text_secondary')).width('100%')
          Row({ space: 8 }) {
            Button(this.isNarrowPhone() ? '跟随' : '跟随系统').layoutWeight(1).height(48).borderRadius(14)
              .fontSize(this.isNarrowPhone() ? 10 : 14)
              .padding({ left: this.isNarrowPhone() ? 4 : 12, right: this.isNarrowPhone() ? 4 : 12 })
              .backgroundColor(this.settings.themeMode === ThemeMode.SYSTEM ? $r('app.color.accent') : $r('app.color.search_background'))
              .fontColor(this.settings.themeMode === ThemeMode.SYSTEM ? '#FFFFFFFF' : $r('app.color.text_primary'))
              .onClick(() => this.updateTheme(ThemeMode.SYSTEM))
            Button('浅色').layoutWeight(1).height(48).borderRadius(14)
              .fontSize(this.isNarrowPhone() ? 10 : 14)
              .backgroundColor(this.settings.themeMode === ThemeMode.LIGHT ? $r('app.color.accent') : $r('app.color.search_background'))
              .fontColor(this.settings.themeMode === ThemeMode.LIGHT ? '#FFFFFFFF' : $r('app.color.text_primary'))
              .onClick(() => this.updateTheme(ThemeMode.LIGHT))
            Button('深色').layoutWeight(1).height(48).borderRadius(14)
              .fontSize(this.isNarrowPhone() ? 10 : 14)
              .backgroundColor(this.settings.themeMode === ThemeMode.DARK ? $r('app.color.accent') : $r('app.color.search_background'))
              .fontColor(this.settings.themeMode === ThemeMode.DARK ? '#FFFFFFFF' : $r('app.color.text_primary'))
              .onClick(() => this.updateTheme(ThemeMode.DARK))
          }.width('100%')
          this.SwitchRow('玻璃卡片', '默认使用 16vp 圆角和半透明材质', this.settings.useGlassEffect, (value: boolean): void => this.update(this.settings.defaultDisplayMode, this.settings.viewMode, value))
          this.SwitchRow('自定义页面背景', '选择图片后可随时开启或关闭，不会删除原图',
            this.settings.usePageBackground, (value: boolean): void => this.togglePageBackground(value),
            'page-background-enabled')
          this.PageBackgroundChooser()
          Column({ space: 10 }) {
            Text('字体').fontSize(14).fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.text_primary')).width('100%')
            Text('从 ZIP 压缩包导入字体到 Wine 的 C:\\windows\\Fonts。导入前会删除该目录下所有旧字体，导入完成后可一键重启应用生效。')
              .fontSize(11).fontColor($r('app.color.text_secondary')).width('100%')
            Button('导入字体包 (ZIP)').width('100%').height(44).borderRadius(12)
              .backgroundColor($r('app.color.accent')).fontColor('#FFFFFFFF')
              .id('font-import-button')
              .enabled(!this.fontImporting)
              .onClick((): void => { this.importFontZip(); })
          }
          .width('100%').padding(14).backgroundColor($r('app.color.search_background')).borderRadius(14)
        }
        .width('100%')
        .padding(this.cardPadding())
        .backgroundColor(this.groupSurfaceColor())
        .backgroundBlurStyle(this.settings.useGlassEffect ? BlurStyle.COMPONENT_ULTRA_THICK : BlurStyle.NONE)
        .border({ width: 1, color: this.groupBorderColor() })
        .borderRadius(20)
      } else if (section === 'input') {
        Column({ space: 16 }) {
          Text('输入与控制').fontSize(20).fontWeight(FontWeight.Bold).width('100%')
          if (this.isManagedWindowsHost()) {
            Column({ space: 6 }) {
              Text('PC 桌面默认使用实体键盘鼠标')
                .fontSize(14).fontWeight(FontWeight.Medium).fontColor($r('app.color.text_primary')).width('100%')
              Text('桌面模式不会创建屏幕控制层，虚拟输入主要面向平板触屏与手柄使用。')
                .fontSize(12).fontColor($r('app.color.text_secondary')).width('100%')
            }
            .width('100%').padding(14).backgroundColor($r('app.color.search_background')).borderRadius(14)
          }
          this.SwitchRow('启用屏幕虚拟输入',
            '关闭后不显示虚拟按键、全屏鼠标和右上角控制栏；实体外设仍可使用',
            this.settings.virtualInputEnabled, (value: boolean): void => this.updateVirtualInputEnabled(value),
            'virtual-input-enabled')
          if (this.settings.virtualInputEnabled) {
            this.SwitchRow('显示虚拟按键', '只控制十字键、摇杆、ABXY 和肩键；全屏鼠标与右上角控制栏不受影响', this.settings.showVirtualControls, (value: boolean): void => this.update(this.settings.defaultDisplayMode, this.settings.viewMode, this.settings.useGlassEffect, this.settings.usePageBackground, value))
            this.SwitchRow('外设接管时隐藏按键', '检测到实体键盘、鼠标或手柄操作后，仅隐藏虚拟按键', this.settings.autoHideVirtualControls, (value: boolean): void => this.update(this.settings.defaultDisplayMode, this.settings.viewMode, this.settings.useGlassEffect, this.settings.usePageBackground, this.settings.showVirtualControls, value))
            Column({ space: 10 }) {
              Text('控制栏方向').fontSize(14).fontWeight(FontWeight.Medium).fontColor($r('app.color.text_primary')).width('100%')
              Text('自动会根据横竖屏切换；自动隐藏会在控制栏收起 3 秒后完全隐藏，点击右上角热区即可唤出。')
                .fontSize(11).fontColor($r('app.color.text_secondary')).width('100%')
              Flex({ wrap: FlexWrap.Wrap }) {
                this.ControlBarDirectionChip('自动', ControlBarDirection.AUTO)
                this.ControlBarDirectionChip('横向', ControlBarDirection.HORIZONTAL)
                this.ControlBarDirectionChip('纵向', ControlBarDirection.VERTICAL)
                this.ControlBarDirectionChip('自动隐藏', ControlBarDirection.AUTO_HIDE)
              }.width('100%')
            }
            .width('100%').padding(14).backgroundColor($r('app.color.search_background')).borderRadius(14)
            Column({ space: 10 }) {
              Text('全屏触摸板模式').fontSize(15).fontWeight(FontWeight.Medium).fontColor($r('app.color.text_primary')).width('100%')
              Text('从 Wine 画面右上角手指图标切换。开启后单指相对移动鼠标、轻点左键；双指轻点右键、双指滑动滚轮。')
                .fontSize(11).fontColor($r('app.color.text_secondary')).width('100%')
              this.SwitchRow('显示箭头指针', '触摸板模式用箭头显示鼠标位置，箭头尖即点击点', this.settings.virtualMouseConfig.showCursor,
                (value: boolean): void => this.updateVirtualMouse(this.settings.virtualMouseConfig.mouseSensitivity, value))
              Text('轻点为左键，双指轻点为右键；轻点后再次按住并移动可拖拽窗口')
                .fontSize(12).fontColor($r('app.color.text_secondary')).width('100%')
              Text('滚动灵敏度 ' + this.settings.virtualMouseConfig.mouseSensitivity + '%')
                .fontSize(14).fontColor($r('app.color.text_primary')).width('100%')
               Slider({ value: this.settings.virtualMouseConfig.mouseSensitivity, min: 50, max: 200, step: 10 })
                 .id('virtual-mouse-sensitivity')
                 .sliderInteractionMode(2)
                 .onChange((value: number, mode: SliderChangeMode): void => {
                   if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                     this.updateVirtualMouse(value);
                   }
                 })
            }
            .width('100%').padding(14).backgroundColor($r('app.color.search_background')).borderRadius(14)
            Text('控制层透明度 ' + this.settings.controlOpacity + '%').fontSize(14).fontColor($r('app.color.text_primary')).width('100%')
             Slider({ value: this.settings.controlOpacity, min: 25, max: 100, step: 1 })
               .sliderInteractionMode(2)
               .onChange((value: number, mode: SliderChangeMode) => {
                 if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                   this.update(this.settings.defaultDisplayMode, this.settings.viewMode,
                     this.settings.useGlassEffect, this.settings.usePageBackground,
                     this.settings.showVirtualControls, this.settings.autoHideVirtualControls, value);
                 }
               })
          }
          Text('手柄兼容模式').fontSize(14).fontColor($r('app.color.text_primary')).width('100%')
          Row({ space: 10 }) {
            Button(this.settings.controllerOutputMode === 'dinput' ? 'Wine 手柄 ✓' : 'Wine 手柄')
              .fontSize(13)
              .onClick((): void => this.updateControllerOutputMode('dinput'))
            Button(this.settings.controllerOutputMode === 'keyboard_legacy' ? '键盘模拟 ✓' : '键盘模拟')
              .fontSize(13)
              .onClick((): void => this.updateControllerOutputMode('keyboard_legacy'))
            Button('测试震动')
              .fontSize(13)
              .onClick((): void => {
                const ok: boolean = GamepadManager.getInstance().testRumble();
                promptAction.showToast({
                  message: ok ? '正在测试手柄震动' : '未找到手柄马达（不会使用平板震动）'
                });
              })
          }.width('100%')
          Text('Wine 手柄经 Controller Hub 进入 DirectInput 与 XInput，并转发震动到实体手柄；键盘模拟保持旧版键鼠映射。切换后需重启会话。')
            .fontSize(12).fontColor($r('app.color.text_secondary')).width('100%')
          Text(this.hubStateText)
            .fontSize(11).fontColor($r('app.color.text_secondary')).width('100%')
          Text('手柄死区 ' + this.settings.gamepadDeadzone + '%').fontSize(14).fontColor($r('app.color.text_primary')).width('100%')
           Slider({ value: this.settings.gamepadDeadzone, min: 0, max: 40, step: 1 })
             .sliderInteractionMode(2)
             .onChange((value: number, mode: SliderChangeMode) => {
               if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                 this.update(this.settings.defaultDisplayMode, this.settings.viewMode,
                   this.settings.useGlassEffect, this.settings.usePageBackground,
                   this.settings.showVirtualControls, this.settings.autoHideVirtualControls,
                   this.settings.controlOpacity, value);
               }
             })
          this.DesktopInputProfiles()
          this.InputLayoutEntry()
        }
        .width('100%')
        .padding(this.cardPadding())
        .backgroundColor(this.groupSurfaceColor())
        .backgroundBlurStyle(this.settings.useGlassEffect ? BlurStyle.COMPONENT_ULTRA_THICK : BlurStyle.NONE)
        .border({ width: 1, color: this.groupBorderColor() })
        .borderRadius(20)
      } else if (section === 'engine') {
        Column({ space: 14 }) {
          Row({ space: 12 }) {
            Stack({ alignContent: Alignment.Center }) {
              SymbolGlyph($r('sys.symbol.arrow_clockwise')).fontSize(24).fontColor([$r('app.color.accent')])
            }
            .width(48).height(48).borderRadius(16)
            .backgroundColor($r('app.color.selected_background'))
            Column({ space: 3 }) {
              Text('Wine 引擎').fontSize(20).fontWeight(FontWeight.Bold)
                .fontColor($r('app.color.text_primary')).width('100%')
              Text(this.engineMessage).fontSize(12).fontColor($r('app.color.text_secondary'))
                .maxLines(2).textOverflow({ overflow: TextOverflow.Ellipsis }).width('100%')
            }.layoutWeight(1).alignItems(HorizontalAlign.Start)
          }
          .width('100%').padding(14).borderRadius(16)
          .backgroundColor($r('app.color.search_background'))

          Text('Windows 系统语言').fontSize(12).fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_secondary')).width('100%')
          Row({ space: 8 }) {
            Button('中文').layoutWeight(1).height(44).borderRadius(14)
              .fontSize(13)
              .backgroundColor(this.settings.wineLanguage === WineLanguage.CHINESE ?
                $r('app.color.accent') : $r('app.color.search_background'))
              .fontColor(this.settings.wineLanguage === WineLanguage.CHINESE ?
                '#FFFFFFFF' : $r('app.color.text_primary'))
              .id('wine-language-zh-cn')
              .onClick((): void => this.updateWineLanguage(WineLanguage.CHINESE))
            Button('English').layoutWeight(1).height(44).borderRadius(14)
              .fontSize(13)
              .backgroundColor(this.settings.wineLanguage === WineLanguage.ENGLISH ?
                $r('app.color.accent') : $r('app.color.search_background'))
              .fontColor(this.settings.wineLanguage === WineLanguage.ENGLISH ?
                '#FFFFFFFF' : $r('app.color.text_primary'))
              .id('wine-language-en-us')
              .onClick((): void => this.updateWineLanguage(WineLanguage.ENGLISH))
          }.width('100%')
          Text('默认中文。更改后重启 Wine 引擎生效，已有 Windows 程序会在重启时关闭。')
            .fontSize(11).fontColor('#C47A20').width('100%')

          Text('渲染模式').fontSize(12).fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_secondary')).width('100%')
          Row({ space: 6 }) {
            this.RenderModeChip('DXVK 1.10', 'dxvk_1_10')
            if (this.gpuSupportDxvk26) {
              this.RenderModeChip('DXVK 2.6（实验）', 'dxvk_2_6')
            }
            this.RenderModeChip('VirGL', 'virgl')
          }.width('100%')
          Text(this.gpuSupportDxvk26
            ? '默认使用 DXVK 1.10。当前 GPU 支持开启 DXVK 2.6（实验）。'
            : `默认使用 DXVK 1.10。当前 GPU（${this.gpuName || '未知'}）低于 920，不提供 DXVK 2.6。`)
            .fontSize(11).fontColor($r('app.color.text_secondary')).width('100%')
          Text('更改后需要重启 Wine 引擎；运行中的游戏不会被立即中断。')
            .fontSize(11).fontColor('#C47A20').width('100%')

          Text('Box64 兼容性预设').fontSize(12).fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_secondary')).width('100%')
          Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.Start }) {
            ForEach(BOX64_PRESET_IDS, (preset: string) => {
              Button(box64PresetLabel(preset))
                .height(42)
                .padding({ left: 14, right: 14 })
                .margin({ right: 8, bottom: 8 })
                .borderRadius(14)
                .fontSize(13)
                .fontColor(this.settings.box64Preset === preset ?
                  '#FFFFFFFF' : $r('app.color.text_primary'))
                .backgroundColor(this.settings.box64Preset === preset ?
                  $r('app.color.accent') : $r('app.color.search_background'))
                .id('box64-preset-' + preset)
                .onClick((): void => this.updateBox64Preset(preset))
            }, (preset: string): string => preset)
          }.width('100%')
          Text(box64PresetDescription(this.settings.box64Preset))
            .fontSize(11).fontColor($r('app.color.text_secondary')).width('100%')
          Text('预设会应用于之后启动的 Windows 程序；默认档使用系统当前已验证的兼容配置。')
            .fontSize(11).fontColor('#C47A20').width('100%')

          Text('会话维护').fontSize(12).fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_secondary')).width('100%')
          Button('重启 Wine 引擎').width('100%').height(48).borderRadius(14)
            .backgroundColor($r('app.color.accent'))
            .fontColor('#FFFFFFFF')
            .enabled(!this.engineBusy)
            .id('wine-engine-restart')
            .onClick((): void => AlertDialog.show({
              title: '重启 Wine 引擎',
              message: '这会关闭所有 Windows 程序并重建 Wine 与 Wayland 服务，但不会删除 Wine prefix。重启期间画面将被锁定。',
              primaryButton: { value: '取消', action: (): void => {} },
              secondaryButton: { value: '立即重启', action: (): void => { this.restartEngine(); } }
            }))
          Button('停止全部 Windows 会话').width('100%').height(48).borderRadius(14)
            .backgroundColor($r('app.color.search_background'))
            .fontColor($r('app.color.text_primary'))
            .enabled(!this.engineBusy)
            .id('wine-engine-stop-all')
            .onClick((): void => AlertDialog.show({
              title: '停止全部 Windows 会话',
              message: '正在运行的 Windows 程序会被关闭。',
              primaryButton: { value: '取消', action: (): void => {} },
              secondaryButton: { value: '停止', fontColor: '#D94848', action: (): void => { this.stopEngine(); } }
            }))
          Column({ space: 10 }) {
            Row({ space: 10 }) {
              SymbolGlyph($r('sys.symbol.info_circle')).fontSize(20).fontColor(['#D94848'])
              Column({ space: 3 }) {
                Text('重置 Wine 环境').fontSize(14).fontWeight(FontWeight.Medium)
                  .fontColor($r('app.color.text_primary')).width('100%')
                Text('重建 Wine 沙盒并重装运行时。默认保留文档和注册表。')
                  .fontSize(11).fontColor($r('app.color.text_secondary')).width('100%')
              }.layoutWeight(1).alignItems(HorizontalAlign.Start)
            }.width('100%')
            this.SwitchRow('保留文档和注册表',
              '开启则还原用户文档与注册表；关闭则全部清空',
              this.settings.preserveUserDataOnReset,
              (value: boolean): void => this.setPreserveUserDataOnReset(value),
              'wine-reset-preserve-user-data')
            Button('重置 Wine prefix').width('100%').height(48).borderRadius(14)
              .backgroundColor('#18D94848')
              .fontColor('#D94848')
              .enabled(!this.engineBusy)
              .id('wine-engine-reset-prefix')
              .onClick(() => AlertDialog.show({
                title: '重置 Wine 环境',
                message: this.resetPrefixConfirmMessage(),
                primaryButton: { value: '取消', action: (): void => {} },
                secondaryButton: {
                  value: '重置',
                  fontColor: '#D94848',
                  action: (): void => { this.resetPrefix(); }
                }
              }))
          }
          .width('100%').padding(14).borderRadius(16)
          .backgroundColor('#0FD94848').border({ width: 1, color: '#28D94848' })
        }
        .width('100%')
        .padding(this.cardPadding())
        .backgroundColor(this.groupSurfaceColor())
        .backgroundBlurStyle(this.settings.useGlassEffect ? BlurStyle.COMPONENT_ULTRA_THICK : BlurStyle.NONE)
        .border({ width: 1, color: this.groupBorderColor() })
        .borderRadius(20)
      } else if (section === 'performance') {
        Column({ space: 14 }) {
          Text('性能监控').fontSize(20).fontWeight(FontWeight.Bold)
            .fontColor($r('app.color.text_primary')).width('100%')
          Text('在游戏顶部以小字号横条显示，即时生效；不修改图形后端或游戏启动环境。')
            .fontSize(12).fontColor($r('app.color.text_secondary')).width('100%')
          this.SwitchRow('显示性能监控', '总开关；关闭后保留各项选择并停止采样',
            this.settings.showFpsHud, (value: boolean): void => this.updateShowFpsHud(value),
            'performance-enabled')
          Column({ space: 10 }) {
            this.SwitchRow('FPS', '宿主呈现帧率，非游戏引擎内部计数',
              this.settings.performanceHud.fps,
              (value: boolean): void => this.updatePerformanceHud('fps', value), 'performance-fps')
            this.SwitchRow('应用 CPU', '累计 Wine 子进程；100%＝一个逻辑核，200%≈两个核的负载。≥ 表示计数不完整时的下限',
              this.settings.performanceHud.appCpu,
              (value: boolean): void => this.updatePerformanceHud('appCpu', value), 'performance-app-cpu')
            this.SwitchRow('系统 CPU', '整机全部逻辑核的平均占用（0–100%），包含游戏、系统服务及其他应用；采样窗口可能与应用 CPU 不同',
              this.settings.performanceHud.systemCpu,
              (value: boolean): void => this.updatePerformanceHud('systemCpu', value), 'performance-system-cpu')
            this.SwitchRow('电池温度', '系统电池传感器温度（℃），与芯片温度不同',
              this.settings.performanceHud.batteryTemperature,
              (value: boolean): void => this.updatePerformanceHud('batteryTemperature', value), 'performance-battery-temperature')
            this.SwitchRow('热状态', '系统热管理等级，不代表具体温度或已发生降频',
              this.settings.performanceHud.thermalLevel,
              (value: boolean): void => this.updatePerformanceHud('thermalLevel', value), 'performance-thermal-level')
          }.width('100%')
          if (!hasPerformanceHudContent(this.settings.performanceHud)) {
            Text('未选择任何指标，监控层将隐藏且不采样。').fontSize(12)
              .fontColor($r('app.color.text_secondary')).width('100%')
          }
          Text('FPS 每 0.5 秒刷新，其余指标每 2 秒采样；进入后台时暂停。尚未取得数值时显示省略号，读取失败的指标自动隐藏。GPU 占用率和芯片温度暂不提供开关。')
            .fontSize(11).fontColor($r('app.color.text_secondary')).width('100%')
        }
        .width('100%').padding(this.cardPadding())
        .backgroundColor(this.groupSurfaceColor())
        .backgroundBlurStyle(this.settings.useGlassEffect ? BlurStyle.COMPONENT_ULTRA_THICK : BlurStyle.NONE)
        .border({ width: 1, color: this.groupBorderColor() }).borderRadius(20)
      } else if (section === 'logs') {
        Column({ space: 14 }) {
          Row({ space: 12 }) {
            Stack({ alignContent: Alignment.Center }) {
              SymbolGlyph($r('sys.symbol.doc_text')).fontSize(24).fontColor([$r('app.color.accent')])
            }
            .width(48).height(48).borderRadius(16)
            .backgroundColor($r('app.color.selected_background'))
            Column({ space: 3 }) {
              Text('日志管理').fontSize(20).fontWeight(FontWeight.Bold)
                .fontColor($r('app.color.text_primary')).width('100%')
              Text('应用日志按天保存，自动保留 7 天').fontSize(12)
                .fontColor($r('app.color.text_secondary')).width('100%')
            }.layoutWeight(1).alignItems(HorizontalAlign.Start)
          }
          .width('100%').padding(14).borderRadius(16)
          .backgroundColor($r('app.color.search_background'))

          Column({ space: 8 }) {
            Text('日志目录').fontSize(12).fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.text_secondary')).width('100%')
            Text('Download/com.vintage.pomelopro/logs').fontSize(12)
              .fontColor($r('app.color.text_primary')).width('100%')
            Text('记录应用自身 ArkTS 与 native 日志，按日期分文件（app-日期.log / native-日期.log），同一天超过 20MB 自动滚动，超过 7 天的文件自动清理。')
              .fontSize(11).fontColor($r('app.color.text_secondary')).width('100%')
          }
          .width('100%').padding(14).borderRadius(16)
          .backgroundColor($r('app.color.search_background'))

          Row({ space: 10 }) {
            Button('导出日志').layoutWeight(1).height(48).borderRadius(14)
              .backgroundColor($r('app.color.accent')).fontColor('#FFFFFFFF')
              .enabled(!this.logBusy)
              .id('log-export')
              .onClick((): void => { this.exportLogs(); })
            Button('清空日志').layoutWeight(1).height(48).borderRadius(14)
              .backgroundColor($r('app.color.search_background'))
              .fontColor($r('app.color.text_primary'))
              .id('log-clear')
              .onClick((): void => { this.clearLogs(); })
          }.width('100%')
        }
        .width('100%')
        .padding(this.cardPadding())
        .backgroundColor(this.groupSurfaceColor())
        .backgroundBlurStyle(this.settings.useGlassEffect ? BlurStyle.COMPONENT_ULTRA_THICK : BlurStyle.NONE)
        .border({ width: 1, color: this.groupBorderColor() })
        .borderRadius(20)
      } else {
        Column({ space: 14 }) {
          Row({ space: 12 }) {
            Stack({ alignContent: Alignment.Center }) {
              SymbolGlyph($r('sys.symbol.square_grid_2x2')).fontSize(25).fontColor([$r('app.color.accent')])
            }
            .width(52).height(52).borderRadius(17)
            .backgroundColor($r('app.color.selected_background'))
            Column({ space: 3 }) {
              Text('旧柚Pro').fontSize(22).fontWeight(FontWeight.Bold)
                .fontColor($r('app.color.text_primary')).width('100%')
              Text('Wine 应用库与 Windows 兼容引擎').fontSize(12)
                .fontColor($r('app.color.text_secondary')).width('100%')
              Text(this.appVersion).fontSize(12)
                .fontColor($r('app.color.text_secondary')).width('100%').id('app-version')
            }.layoutWeight(1).alignItems(HorizontalAlign.Start)
          }.width('100%')
          Column({ space: 8 }) {
            Text('按需启动 Wine，集中管理内建工具与 Download/games 中的 Windows 程序。')
              .fontSize(13).fontColor($r('app.color.text_primary')).width('100%')
            Text('支持桌面、多窗口、虚拟输入以及每个应用的独立运行设置。')
              .fontSize(12).fontColor($r('app.color.text_secondary')).width('100%')
          }
          .width('100%').padding(14).borderRadius(16)
          .backgroundColor($r('app.color.search_background'))
          Button('打开帮助中心').width('100%').height(48).borderRadius(14)
            .backgroundColor($r('app.color.accent')).fontColor('#FFFFFFFF')
            .onClick((): void => { router.pushUrl({ url: 'pages/HelpCenter' }); })
          Button('GitHub 开源地址').width('100%').height(48).borderRadius(14)
            .backgroundColor($r('app.color.search_background'))
            .fontColor($r('app.color.text_primary'))
            .id('about-github')
            .onClick((): void => {
              try {
                const clip = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN,
                  'https://github.com/yifengling0/VintagePomeloPro');
                pasteboard.getSystemPasteboard().setData(clip);
                promptAction.showToast({ message: '已复制开源地址到剪贴板' });
              } catch (_) {
                promptAction.showToast({ message: 'https://github.com/yifengling0/VintagePomeloPro' });
              }
            })
        }
        .width('100%')
        .padding(this.isNarrowPhone() ? 12 : 24)
        .backgroundColor(this.groupSurfaceColor())
        .backgroundBlurStyle(this.settings.useGlassEffect ? BlurStyle.COMPONENT_ULTRA_THICK : BlurStyle.NONE)
        .border({ width: 1, color: this.groupBorderColor() })
        .borderRadius(20)
      }
    }.width('100%')
  }

  @Builder
  private SettingsList(maxWidth: number, scrollUnderTitle: boolean = false) {
    List({ space: 14, scroller: this.settingsScroller }) {
      ListItem() {
        Column() {
          if (scrollUnderTitle) {
            Blank().height(this.settingsTopInset())
          }
          this.Content('display')
        }
        .width('100%')
      }
      .width('100%').id('settings-section-display')
      ListItem() { this.Content('appearance') }.width('100%').id('settings-section-appearance')
      ListItem() { this.Content('input') }.width('100%').id('settings-section-input')
      ListItem() { this.Content('engine') }.width('100%').id('settings-section-engine')
      ListItem() { this.Content('performance') }.width('100%').id('settings-section-performance')
      ListItem() { this.Content('logs') }.width('100%').id('settings-section-logs')
      ListItem() { this.Content('about') }.width('100%').id('settings-section-about')
    }
    .layoutWeight(1)
    .width('100%')
    .constraintSize({ maxWidth: maxWidth })
    .padding({
      right: this.isNarrowPhone() ? 0 : 10,
      bottom: Math.max(this.isNarrowPhone() ? 28 : 16, this.navBarInsetVp)
    })
    .scrollBar(BarState.Off)
    .edgeEffect(EdgeEffect.Spring)
    .onScrollIndex((start: number): void => {
      if (this.sidebarTarget === null) this.section = this.sectionAt(start);
    })
    .onScrollStop((): void => {
      this.sidebarTarget = null;
    })
    .id('system-settings-list')
  }

  build() {
    Stack() {
      HdsNavigation() {
        if (this.breakpoint === 'lg' || this.breakpoint === 'xl') {
          Row({ space: 22 }) {
            Column({ space: 5 }) {
              Text('设置分类').fontSize(13).fontWeight(FontWeight.Medium)
                .fontColor($r('app.color.text_secondary')).width('100%').margin({ left: 8, bottom: 6 })
              this.SectionButton('显示模式', 'display', $r('sys.symbol.rectangle_grid_1x2'))
              this.SectionButton('外观', 'appearance', $r('sys.symbol.square_grid_2x2'))
              this.SectionButton('输入与手柄', 'input', $r('sys.symbol.list_bullet'))
              this.SectionButton('Wine 引擎', 'engine', $r('sys.symbol.arrow_clockwise'))
              this.SectionButton('性能监控', 'performance', $r('sys.symbol.list_bullet'))
              this.SectionButton('日志', 'logs', $r('sys.symbol.doc_text'))
              this.SectionButton('关于', 'about', $r('sys.symbol.info_circle'))
            }
            .width(this.breakpoint === 'xl' ? 224 : 204)
            .height('100%')
            .padding({ left: 8, right: 8, top: 12, bottom: 12 })
            .backgroundColor(this.groupSurfaceColor())
            .backgroundBlurStyle(this.settings.useGlassEffect ? BlurStyle.COMPONENT_ULTRA_THICK : BlurStyle.NONE)
            .border({ width: 1, color: this.groupBorderColor() })
            .borderRadius(20)
            .hitTestBehavior(HitTestMode.Default)
            this.SettingsList(this.settingsContentMaxWidth())
          }
          .width('100%').height('100%')
          .alignItems(VerticalAlign.Top)
          .padding({
            left: this.settingsSidePadding(), right: this.settingsSidePadding(),
            top: this.settingsTopInset(), bottom: 20
          })
        } else {
          Column() {
            this.SettingsList(840, true)
          }
          .width('100%').height('100%')
          .alignItems(HorizontalAlign.Start)
          .padding({
            left: this.settingsSidePadding(), right: this.settingsSidePadding(),
            top: 0, bottom: Math.max(this.isNarrowPhone() ? 8 : 16, this.navBarInsetVp)
          })
        }
      }
      .mode(NavigationMode.Stack)
      .titleBar({
        padding: { start: LengthMetrics.vp(14), end: LengthMetrics.vp(14 + this.titleButtonMargin()) },
        style: {
          scrollEffectOpts: {
            enableScrollEffect: true,
            scrollEffectType: SUPPORTS_HDS_FLOATING ? ScrollEffectType.IMMERSIVE_GRADIENT_BLUR : ScrollEffectType.GRADUAL_BLUR,
            blurEffectiveStartOffset: LengthMetrics.vp(0),
            blurEffectiveEndOffset: LengthMetrics.vp(SUPPORTS_HDS_FLOATING ? 96 : 68)
        },
        originalStyle: {
          backgroundStyle: {
            backgroundColor: this.navigationTitleBackground(false)
          },
          contentStyle: {
              titleStyle: {
                mainTitleColor: $r('app.color.text_primary'),
                subTitleColor: $r('app.color.text_secondary')
            },
            menuStyle: {
              backgroundColor: this.navigationControlBackground(),
              iconColor: $r('app.color.text_primary')
            },
            backIconStyle: {
              backgroundColor: this.navigationControlBackground(),
              iconColor: $r('app.color.text_primary')
              }
            }
        },
        scrollEffectStyle: {
          backgroundStyle: {
            backgroundColor: this.navigationTitleBackground(true)
          },
          contentStyle: {
              titleStyle: {
                mainTitleColor: $r('app.color.text_primary'),
                subTitleColor: $r('app.color.text_secondary')
            },
            menuStyle: {
              backgroundColor: this.navigationControlBackground(),
              iconColor: $r('app.color.text_primary')
            },
            backIconStyle: {
              backgroundColor: this.navigationControlBackground(),
              iconColor: $r('app.color.text_primary')
              }
            }
        },
        systemMaterialEffect: SUPPORTS_HDS_FLOATING ? {
          materialType: HDS_MATERIAL_TYPE_IMMERSIVE,
          materialLevel: HDS_MATERIAL_LEVEL_EXQUISITE
        } : undefined
        },
        content: {
          title: {
            mainTitle: '系统设置',
            subTitle: undefined
          },
          menu: {
            value: [
              {
                content: {
                  icon: $r('sys.symbol.arrow_counterclockwise'),
                  action: (): void => { this.restoreDefaults(); }
                }
              }
            ],
            maxCount: 1
          } as HdsNavigationMenuContentOptions,
          divider: { showType: DividerShowType.OFF }
      },
      avoidLayoutSafeArea: true
      })
    .titleMode(HdsNavigationTitleMode.MINI)
    .hideToolBar(true)
    .systemBarStyle(
      { statusBarContentColor: this.navigationTitleColor() },
      { statusBarContentColor: this.navigationTitleColor() }
    )
      .bindToScrollable([this.settingsScroller])
      .width('100%').height('100%')
      .backgroundColor('#00FFFFFF')

      if (this.engineBusy) {
        Stack() {
          Column()
            .width('100%').height('100%')
            .backgroundColor('#E80B1220')
            .hitTestBehavior(HitTestMode.Block)
          Column({ space: 16 }) {
            LoadingProgress().width(56).height(56).color('#2F6BFF')
            Text('Wine 引擎维护中').fontSize(22).fontWeight(FontWeight.Bold)
              .fontColor('#101828').textAlign(TextAlign.Center)
            Text(this.engineOperationMessage).fontSize(15).fontColor('#344054')
              .textAlign(TextAlign.Center).constraintSize({ maxWidth: 420 })
            Text('请保持应用在前台，完成前所有操作已锁定。').fontSize(13).fontColor('#667085')
              .textAlign(TextAlign.Center).constraintSize({ maxWidth: 420 })
          }
          .width('86%').constraintSize({ maxWidth: 480 })
          .padding({ left: 24, right: 24, top: 28, bottom: 28 })
          .backgroundColor('#FFFFFFFF')
          .border({ width: 2, color: '#FFB8C7E8' })
          .borderRadius(20)
          .shadow({ radius: 24, color: '#66000000', offsetX: 0, offsetY: 10 })
          .hitTestBehavior(HitTestMode.Block)
          .id('wine-engine-settings-progress-panel')
        }
        .width('100%').height('100%')
        .alignContent(Alignment.Center)
        .hitTestBehavior(HitTestMode.Block)
        .zIndex(10000)
        .id('wine-engine-operation-blocker')
      }

      if (this.fontImporting) {
        Stack() {
          Column()
            .width('100%').height('100%')
            .backgroundColor('#E80B1220')
            .hitTestBehavior(HitTestMode.Block)
          Column({ space: 16 }) {
            if (this.fontImportStage.startsWith('正在解压') || this.fontImportProgress === 0) {
              LoadingProgress().width(56).height(56).color('#2F6BFF')
            } else {
              Progress({ value: this.fontImportProgress, total: 100, type: ProgressType.Linear })
                .width('100%').color('#2F6BFF')
            }
            Text('正在导入字体').fontSize(22).fontWeight(FontWeight.Bold)
              .fontColor('#101828').textAlign(TextAlign.Center)
            Text(this.fontImportStage).fontSize(15).fontColor('#344054')
              .textAlign(TextAlign.Center).constraintSize({ maxWidth: 420 })
            Text('请保持应用在前台，完成前请勿退出。').fontSize(13).fontColor('#667085')
              .textAlign(TextAlign.Center).constraintSize({ maxWidth: 420 })
          }
          .width('86%').constraintSize({ maxWidth: 480 })
          .padding({ left: 24, right: 24, top: 28, bottom: 28 })
          .backgroundColor('#FFFFFFFF')
          .border({ width: 2, color: '#FFB8C7E8' })
          .borderRadius(20)
          .shadow({ radius: 24, color: '#66000000', offsetX: 0, offsetY: 10 })
          .hitTestBehavior(HitTestMode.Block)
          .id('font-import-progress-panel')
        }
        .width('100%').height('100%')
        .alignContent(Alignment.Center)
        .hitTestBehavior(HitTestMode.Block)
        .zIndex(10000)
        .id('font-import-blocker')
      }
    }
    .width('100%').height('100%').backgroundColor($r('app.color.page_background'))
    .expandSafeArea([SafeAreaType.SYSTEM, SafeAreaType.CUTOUT],
      [SafeAreaEdge.TOP, SafeAreaEdge.BOTTOM, SafeAreaEdge.START, SafeAreaEdge.END])
  }
}
