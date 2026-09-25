import { skinColor } from '../service/SkinColors';
import { common, ConfigurationConstant } from '@kit.AbilityKit';
import router from '@ohos.router';
import promptAction from '@ohos.promptAction';
import { inputMethod } from '@kit.IMEKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';
import testNapi from 'libentry.so';
import { AppSearchBar } from '../components/SearchBar';
import { AppCard } from '../components/AppCard';
import { ConsoleGameCard } from '../components/ConsoleGameCard';
import { ConsoleRingItem } from '../components/ConsoleRingItem';
import { GameDetailSheet } from '../components/GameDetailSheet';
import { FileBrowserView } from '../components/FileBrowserView';
import { BreakpointSystem, BreakpointType, isWideBreakpoint } from '../common/BreakpointSystem';
import { bottomNavigationLayout } from '../model/BottomNavigationLayout';
import {
  AppDescriptor,
  AppSource,
  ConsoleCardLayout,
  ConsolePalette,
  DesktopWindowMode,
  DisplayMode,
  EngineState,
  GlobalSettings,
  LaunchKind,
  RuntimeSession,
  ScreenOrientationSetting,
  ThemeMode,
  SortMode,
  UiSkin,
  ViewMode,
  cloneAppSettings,
  normalizeLaunchPath,
  resolveRunningAppIds
} from '../model/AppModels';
import { AppCatalogService } from '../service/AppCatalogService';
import { InputOverlayWindowCoordinator } from '../service/InputOverlayWindowCoordinator';
import { AppSessionService, LaunchConflictKind } from '../service/AppSessionService';
import { AppSettingsStore } from '../service/AppSettingsStore';
import { GameArtworkService, ArtworkListener } from '../service/GameArtworkService';
import { MetadataScraper } from '../service/MetadataScraper';
import { LauncherAdapterRegistry } from '../service/LauncherAdapterRegistry';
import { ThemeSettingsListener, ThemeSettingsService } from '../service/ThemeSettingsService';
import { WineEngineService } from '../service/WineEngineService';
import { WineWindowManager } from '../service/WineWindowManager';
import { InputRouter } from '../service/InputRouter';
import { EVDEV_KEY_ENTER } from '../common/KeyMap';
import { GamepadManager, GamepadNavigationListener } from '../service/GamepadManager';
import { ConsoleUiSound } from '../service/ConsoleUiSound';
import { InputProfileService } from '../service/InputProfileService';
import { WineEngineBlockingOverlay } from '../components/WineEngineBlockingOverlay';
import { DesktopLayer } from '../components/DesktopLayer';
import { DeviceCapabilityPolicy } from '../common/DeviceCapabilityPolicy';
import { LengthMetrics, window } from '@kit.ArkUI';
import deviceInfo from '@ohos.deviceInfo';
import {
  applyDesktopFullscreenShell,
  applyStatusOnlyImmersiveShell,
  getMiniTitleContentTopInset,
  getStatusOnlyWindowInsets,
  observeStatusOnlyWindowInsets
} from '../common/StatusOnlyImmersiveShell';
import {
  DividerMode,
  DividerShowType,
  HdsNavigation,
  HdsNavigationMenuContentOptions,
  HdsNavigationTitleMode,
  HdsTabs,
  HdsTabsController,
  ScrollEffectType,
  hdsMaterial
} from '@kit.UIDesignKit';

type CatalogFilter = 'recent' | 'builtin' | 'windows' | 'running' | 'favorite';

/** Virtual data keeps the native horizontal List far away from either edge. */
class ConsoleRingDataSource implements IDataSource {
  static readonly VIRTUAL_TOTAL: number = 3000;
  private apps: AppDescriptor[] = [];
  private listeners: DataChangeListener[] = [];

  setApps(apps: AppDescriptor[]): void {
    this.apps = apps;
    this.listeners.forEach((listener: DataChangeListener): void => listener.onDataReloaded());
  }

  totalCount(): number {
    if (this.apps.length <= 2) return this.apps.length;
    return ConsoleRingDataSource.VIRTUAL_TOTAL;
  }

  getData(index: number): AppDescriptor {
    const count = this.apps.length;
    return this.apps[((index % count) + count) % count];
  }

  registerDataChangeListener(listener: DataChangeListener): void {
    if (this.listeners.indexOf(listener) < 0) this.listeners.push(listener);
  }

  unregisterDataChangeListener(listener: DataChangeListener): void {
    const index = this.listeners.indexOf(listener);
    if (index >= 0) this.listeners.splice(index, 1);
  }
}

const DOMAIN = 0x0000;

const PHONE_NAV_RECENT: number = 0;
const PHONE_NAV_BUILTIN: number = 1;
const PHONE_NAV_WINDOWS: number = 2;
const PHONE_NAV_FAVORITE: number = 3;
const PHONE_NAV_RUNNING: number = 4;
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

interface ScrollOffsetSnapshot {
  xOffset: number;
  yOffset: number;
}

// 浮窗模式: 小窗始终按横屏桌面比例显示 (手机竖持时也是横条，点开再转横屏全屏)
const FLOATING_W: number = 320;
const FLOATING_W_PHONE: number = 248;

@Entry
@Component
struct Index {
  @StorageLink('overlayEditorRequest') @Watch('onOverlayEditorRequest') private overlayEditorRequest: string = '';
  @StorageLink('libraryTitleButtonInsetVp') private titleButtonInsetVp: number = 0;
  @State private apps: AppDescriptor[] = [];
  @State private visibleApps: AppDescriptor[] = [];
  @State private searchQuery: string = '';
  @State private filter: CatalogFilter = 'builtin';
  @State private breakpoint: BreakpointType = 'lg';
  @State private sidebarCollapsed: boolean = false;
  @State private consoleSort: SortMode = SortMode.RECENT;
  @State private consoleOrderedApps: AppDescriptor[] = [];
  @State private consoleFocusIndex: number = 0;
  @State private consoleRingOffset: number = 0;
  @State private consoleRingLayoutRevision: number = 0;
  private consoleRingVirtualIndex: number = 0;
  @State private consoleRingLocating: boolean = false;
  private consoleRingMotionArmed: boolean = false;
  private consoleRingMeasuredIndex: number = 0;
  private consoleRingLocateGeneration: number = 0;
  private consoleRingRelayoutGeneration: number = 0;
  private consoleRingAnchorAppId: string = '';
  private consoleRingScroller: Scroller = new Scroller();
  private consoleGridScroller: Scroller = new Scroller();
  private consoleRingData: ConsoleRingDataSource = new ConsoleRingDataSource();
  @State private consoleFocusAreaWidth: number = 0;
  @State private consoleFocusAreaHeight: number = 0;
  @State private consoleSelectedAppId: string = '';
  @State private consoleDetailApp: AppDescriptor | null = null;
  @State private consoleDetailVisible: boolean = false;
  @State private consoleSearchActive: boolean = false;
  private consoleSearchRequested: boolean = false;
  @State private artworkRevision: number = 0;
  @State private settings: GlobalSettings = AppSettingsStore.getInstance().getGlobalSettings();
  @State private engineState: EngineState = EngineState.STOPPED;
  @State private engineMessage: string = 'Wine 引擎未启动';
  @State private scanning: boolean = false;
  @State private refreshing: boolean = false;
  @State private runningAppIds: Set<string> = new Set<string>();
  @State private phoneNavIndex: number = PHONE_NAV_BUILTIN;
  @State private consoleNavIndex: number = 4;
  @State private viewportWidth: number = 0;
  @State private viewportHeight: number = 0;
  @State private titleContentTopInsetVp: number = 40;
  @State private statusBarHeightVp: number = 0;
  @State private cutoutLeftInsetVp: number = 0;
  @State private cutoutRightInsetVp: number = 0;
  @State private navBarInsetVp: number = 0;
  private context: common.UIAbilityContext = getContext(this) as common.UIAbilityContext;
  private phoneNavController: HdsTabsController = new HdsTabsController();
  private consoleNavController: HdsTabsController = new HdsTabsController();
  private builtinScroller: Scroller = new Scroller();
  private windowsScroller: Scroller = new Scroller();
  private favoriteScroller: Scroller = new Scroller();
  private runningScroller: Scroller = new Scroller();
  private recentScroller: Scroller = new Scroller();
  private scrollOffsets: Record<string, ScrollOffsetSnapshot> = {};
  private breakpointCallback: ((value: BreakpointType) => void) | null = null;
  private engineCallback: ((state: EngineState, message: string) => void) | null = null;
  private settingsCallback: ThemeSettingsListener | null = null;
  private refreshTimer: number = -1;
  private artworkListener: ArtworkListener | null = null;
  private consoleNavigationListener: GamepadNavigationListener | null = null;
  private consoleNavigationActive: boolean = false;
  private consolePageVisible: boolean = false;
  private consoleLastNavigationAt: number = 0;
  private consoleIgnoreCardClickUntil: number = 0;
  private consoleCategoryInitialized: boolean = false;
  private consoleBackgroundCache: Map<string, string> = new Map<string, string>();
  private playStartTimes: Map<string, number> = new Map<string, number>();
  private disposeWindowInsetObserver: (() => void) | null = null;
  private catalogInitialized: boolean = false;
  // 启动请求串行队列：后续点击加入队列，条件满足（会话建立/明确失败）后
  // 依次放行，不再用“正在处理上一次启动”拒绝用户，也不会被一次未完成的
  // 启动永久锁死（每次启动都以真实事件终止）。
  private launchChain: Promise<void> = Promise.resolve();
  // 浮窗模式: 桌面由 Index 承载 (main_ui 同构) — 悬浮窗态小窗缩略 / 全屏态铺满
  @StorageProp('desktopActive') desktopActive: boolean = false;
  @State @Watch('onLauncherVisibleChange') private launcherVisible: boolean = true;
  // 浮窗模式: overlay 缩小按钮经此通知 Index 缩回悬浮窗
  @StorageProp('desktopLauncherVisible') @Watch('onDesktopLauncherVisibleChange') desktopLauncherVisible: boolean = false;
  // 浮窗模式: 启动程序时 (auto-promote/resume) 通知 Index 进全屏显示程序窗口
  @StorageProp('desktopEnterFullscreen') @Watch('onDesktopEnterFullscreenChange') desktopEnterFullscreen: boolean = false;
  @State private floatPosX: number = 0;
  @State private floatPosY: number = 0;
  private floatPanStartX: number = 0;
  private floatPanStartY: number = 0;
  private transitioning: boolean = false;
  /** 浮窗全屏或缩回未竖屏稳定前隐藏库页 HDS/底栏，避免标题盖住按键。 */
  @State private libraryChromeReady: boolean = true;
  private screenW: number = 0;
  private screenH: number = 0;
  @State private floatH: number = 135;
  private floatPosPlaced: boolean = false;

  private isPhone(): boolean {
    // Width-based chrome: MatePad 11.5s portrait is md (~720vp) and must not
    // keep the in-between tablet header. xs/sm/md → phone shell + bottom nav;
    // lg/xl → pad landscape / PC sidebar.
    return !isWideBreakpoint(this.breakpoint);
  }

  private isNarrowPhone(): boolean {
    return this.breakpoint === 'xs';
  }

  private isPhoneLandscape(): boolean {
    return this.isPhone() && this.viewportWidth > this.viewportHeight && this.viewportHeight > 0;
  }

  // ── 浮窗模式: 桌面由 Index 承载 (main_ui 同构) ──
  // 显示条件: 浮窗模式 + 平板/手机 + 桌面 root 存在 (desktopActive)。
  // 悬浮窗态 (launcherVisible=true): 小窗缩略 + 提示文字, 点击放大/按住拖动;
  // 全屏态 (launcherVisible=false): 铺满当前画面, 缩小按钮缩回小窗。
  private showDesktopLayer(): boolean {
    if (this.settings.desktopWindowMode !== DesktopWindowMode.FLOATING) return false;
    if (!DeviceCapabilityPolicy.isTabletDevice() && !DeviceCapabilityPolicy.isPhoneDevice()) return false;
    if (!this.desktopActive) return false;
    return WineWindowManager.getInstance().getDesktopRootId() > 0;
  }

  private floatingWidth(): number {
    const preferred = DeviceCapabilityPolicy.isPhoneDevice() ? FLOATING_W_PHONE : FLOATING_W;
    const maxW = Math.max(160, this.screenW - 24);
    return Math.min(preferred, maxW);
  }

  /** 与 Wine 横屏桌面同比例，避免竖持手机把小窗拉成竖屏。 */
  private floatingHeight(): number {
    const longSide = Math.max(this.screenW, this.screenH);
    const shortSide = Math.min(this.screenW, this.screenH);
    const ratio = (longSide > 0 && shortSide > 0) ? longSide / shortSide : 16 / 9;
    return Math.max(88, Math.round(this.floatingWidth() / ratio));
  }

  private floatTopReserve(): number {
    return Math.max(12, this.titleContentTopInsetVp);
  }

  private floatBottomReserve(): number {
    if (this.isPhone()) {
      return this.isPhoneLandscape() ? 72 : 80;
    }
    return Math.max(12, this.navBarInsetVp);
  }

  private floatMinX(): number {
    return 0;
  }

  private floatMinY(): number {
    return this.floatTopReserve();
  }

  private floatMaxX(): number {
    return Math.max(this.floatMinX(), this.screenW - this.floatingWidth());
  }

  private floatMaxY(): number {
    return Math.max(this.floatMinY(), this.screenH - this.floatingHeight() - this.floatBottomReserve());
  }

  private clampFloatPos(): void {
    this.floatPosX = Math.min(Math.max(this.floatPosX, this.floatMinX()), this.floatMaxX());
    this.floatPosY = Math.min(Math.max(this.floatPosY, this.floatMinY()), this.floatMaxY());
  }

  private placeDefaultFloatPos(): void {
    this.floatH = this.floatingHeight();
    this.floatPosX = Math.max(12, this.screenW - this.floatingWidth() - 12);
    this.floatPosY = this.floatMaxY();
    this.clampFloatPos();
  }

  private libraryViewportReady(): boolean {
    if (this.screenW <= 0 || this.screenH <= 0) {
      return false;
    }
    // 手机缩回尚未完成时 viewport 可能仍是横屏；此时 clamp 会把竖屏坐标压坏。
    if (this.transitioning && DeviceCapabilityPolicy.isPhoneDevice() && this.screenW > this.screenH) {
      return false;
    }
    return true;
  }

  private ensureFloatPos(): void {
    if (!this.libraryViewportReady()) {
      return;
    }
    this.floatH = this.floatingHeight();
    if (!this.floatPosPlaced) {
      this.placeDefaultFloatPos();
      this.floatPosPlaced = true;
      return;
    }
    this.clampFloatPos();
  }

  private resolveMainWindow(): window.Window | null {
    return WineWindowManager.getInstance().getMainWindow();
  }

  private animateLauncherVisible(v: boolean, onFinish?: () => void, keepTransitioning: boolean = false): void {
    this.transitioning = true;
    this.getUIContext().animateTo({
      duration: 300,
      curve: Curve.LinearOutSlowIn,
      onFinish: () => {
        if (!keepTransitioning) {
          this.transitioning = false;
        }
        if (onFinish) {
          onFinish();
        }
      }
    }, () => {
      this.launcherVisible = v;
    });
  }

  private libraryChromeVisible(): boolean {
    if (!this.showDesktopLayer()) {
      return true;
    }
    if (!this.launcherVisible) {
      return false;
    }
    return this.libraryChromeReady;
  }

  // 缩放过渡动画: 显式动画包裹 launcherVisible 翻转, 宽/高/位置/圆角一次过渡
  // 放大: 锁横屏 → 动画 → 结束后再 show overlay。缩小: 先藏 overlay, 立刻锁竖屏。
  private setLauncherVisible(v: boolean): void {
    if (this.transitioning) {
      return;
    }
    if (!v) {
      if (!this.launcherVisible) {
        return;
      }
      this.transitioning = true;
      this.libraryChromeReady = false;
      this.prepareFloatingFullscreen().then((): void => {
        this.expandFloatingDesktop();
      }).catch((): void => {
        this.expandFloatingDesktop();
      });
      return;
    }
    if (this.launcherVisible && this.libraryChromeReady) {
      return;
    }
    this.shrinkFloatingDesktop();
  }

  private expandFloatingDesktop(): void {
    const manager = WineWindowManager.getInstance();
    manager.setFloatingLayoutAnimating(true);
    let finished = false;
    const finishExpand = (): void => {
      if (finished) {
        return;
      }
      finished = true;
      manager.setFloatingLayoutAnimating(false);
      manager.setDesktopLauncherVisible(false);
      this.showFloatingOverlay().then((): void => {
        this.transitioning = false;
      }).catch((): void => {
        this.transitioning = false;
      });
    };
    this.animateLauncherVisible(false, finishExpand, true);
    setTimeout((): void => {
      if (!finished) {
        finishExpand();
      }
    }, 800);
  }

  private shrinkFloatingDesktop(): void {
    this.transitioning = true;
    this.libraryChromeReady = false;
    const manager = WineWindowManager.getInstance();
    manager.setDesktopLauncherVisible(true);
    manager.setFloatingLayoutAnimating(true);
    void this.runShrinkSequence(manager).catch((): void => {
      this.ensureFloatPos();
      this.transitioning = false;
      manager.setFloatingLayoutAnimating(false);
      this.libraryChromeReady = true;
      this.launcherVisible = true;
    });
  }

  private async runShrinkSequence(manager: WineWindowManager): Promise<void> {
    try {
      await this.hideFloatingOverlay();
    } catch (_) {
    }
    let animDone = false;
    let orientDone = false;
    const finishShrink = (): void => {
      if (!animDone || !orientDone) {
        return;
      }
      this.ensureFloatPos();
      this.transitioning = false;
      manager.setFloatingLayoutAnimating(false);
      this.libraryChromeReady = true;
      this.launcherVisible = true;
    };
    this.animateLauncherVisible(true, (): void => {
      animDone = true;
      finishShrink();
    }, true);
    setTimeout((): void => {
      if (!animDone) {
        animDone = true;
        finishShrink();
      }
    }, 800);
    try {
      await this.restoreLibraryOrientation();
    } catch (_) {
    }
    orientDone = true;
    finishShrink();
  }

  private async prepareFloatingFullscreen(): Promise<void> {
    const win = this.resolveMainWindow();
    if (!win) {
      return;
    }
    // 库页 status-only 壳会把状态栏/系统标题留在窗口上。放大前先换成
    // 桌面全屏壳；转屏后再刷一次，避免方向事务把系统栏拉回来。
    await applyDesktopFullscreenShell(win);
    if (DeviceCapabilityPolicy.isPhoneDevice()) {
      const setting = this.settings.desktopOrientation ?? ScreenOrientationSetting.SENSOR_LANDSCAPE;
      await DeviceCapabilityPolicy.applyDesktopOrientation(win, setting, 'Index-float');
      await DeviceCapabilityPolicy.waitForRequestedOrientation(win, setting, 'Index-float');
      await applyDesktopFullscreenShell(win);
      WineWindowManager.getInstance().prepareOutputForOrientation(setting);
    }
  }

  private async restoreLibraryOrientation(): Promise<void> {
    const win = this.resolveMainWindow();
    if (!win) {
      return;
    }
    if (DeviceCapabilityPolicy.isPhoneDevice()) {
      await DeviceCapabilityPolicy.applyPortraitOrientation(win, 'Index-float');
      await DeviceCapabilityPolicy.waitForRequestedOrientation(win, ScreenOrientationSetting.PORTRAIT,
        'Index-float');
      DeviceCapabilityPolicy.enableSensorRotation(win, 'Index-float');
    }
    await this.restoreLibraryWindowShell(win);
  }

  private async restoreLibraryWindowShell(win: window.Window): Promise<void> {
    try {
      await win.setWindowDecorVisible(false);
    } catch (_) {
    }
    await applyStatusOnlyImmersiveShell(win);
    if (DeviceCapabilityPolicy.usesManagedWineWindows()) {
      try {
        win.setWindowDecorHeight(48);
        win.setWindowTitleMoveEnabled(true);
      } catch (_) {
      }
    }
    this.startWindowInsetObserver(win);
    this.refreshPageInsets(win);
  }

  private onLauncherVisibleChange(): void {
    AppStorage.setOrCreate('desktopFloatingFullscreen', !this.launcherVisible);
  }

  private onDesktopLauncherVisibleChange(): void {
    // home 按钮 (overlay) 点击 → 缩回悬浮窗
    if (this.desktopLauncherVisible && !this.launcherVisible) {
      this.setLauncherVisible(true);
      // 重置标志: 下次 home 点击才能再次触发 Watch
      // (AppStorage.setOrCreate 相同值不触发, 不重置则第二次点 home 失效)
      AppStorage.setOrCreate('desktopLauncherVisible', false);
    }
  }

  private onDesktopEnterFullscreenChange(): void {
    // 启动程序 (auto-promote/resume) → 进全屏显示程序窗口
    if (this.desktopEnterFullscreen && this.launcherVisible && this.showDesktopLayer()) {
      this.setLauncherVisible(false);
    }
    // 重置标志: 下次启动程序才能再次触发 Watch
    AppStorage.setOrCreate('desktopEnterFullscreen', false);
  }

  private async hideFloatingOverlay(): Promise<void> {
    AppStorage.setOrCreate('desktopFloatingFullscreen', false);
    await InputOverlayWindowCoordinator.getInstance().setVisible(false);
  }

  private async showFloatingOverlay(): Promise<void> {
    AppStorage.setOrCreate('desktopFloatingFullscreen', true);
    await this.attachFloatingOverlay();
  }

  private async attachFloatingOverlay(): Promise<void> {
    const manager = WineWindowManager.getInstance();
    const stage = manager.getMainWindowStage();
    const win = manager.getMainWindow();
    if (!stage || !win) {
      return;
    }
    const rootId = manager.getDesktopRootId();
    if (rootId <= 0) {
      return;
    }
    await InputOverlayWindowCoordinator.getInstance().attach(
      stage, win, 'desktop-' + rootId, DisplayMode.DESKTOP, rootId,
      this.settings.desktopInputProfileId || 'default', this.settings.showVirtualControls,
      this.settings.controlOpacity, this.settings.virtualInputEnabled, true);
  }

  private usesDarkAppearance(): boolean {
    if (this.settings.uiSkin === UiSkin.CONSOLE) return this.settings.consolePalette !== ConsolePalette.PEARL;
    if (this.settings.themeMode === ThemeMode.DARK) return true;
    if (this.settings.themeMode === ThemeMode.LIGHT) return false;
    try {
      return this.context.config.colorMode === ConfigurationConstant.ColorMode.COLOR_MODE_DARK;
    } catch (_) {
      return false;
    }
  }

  private navigationTitleBackground(scrolled: boolean): string {
    // Match VintagePomelo's HDS glass alpha. The page surface remains visible
    // below the title instead of becoming a white opaque strip.
    if (this.usesDarkAppearance()) return scrolled ? '#80121821' : '#26121821';
    return scrolled ? '#99FFFFFF' : '#18FFFFFF';
  }

  private navigationControlBackground(): string {
    return this.usesDarkAppearance() ? '#30FFFFFF' : '#28FFFFFF';
  }

  private navigationTitleColor(): string {
    return this.usesDarkAppearance() ? '#F4F7FB' : '#182431';
  }

  private contentPadding(): number {
    if (this.isPhoneLandscape()) return 12;
    if (this.breakpoint === 'xs') return 10;
    if (this.isPhone()) return 16;
    return 28;
  }

  // The HDS title is an overlay. Put its height into the scroll content so
  // the initial gap moves away with the first swipe instead of staying fixed.
  private phoneTitleBarScrollInset(): number {
    return this.titleContentTopInsetVp;
  }

  // -- IME: Wine 文本框聚焦拉起系统软键盘 (对齐上游 54d188d) --
  private imeController: inputMethod.InputMethodController | null = null;

  private setupIme(): void {
    testNapi.setImeCallback((active: number, x: number, y: number, w: number, h: number) => {
      hilog.info(DOMAIN, 'IME', '[cb] active=%{public}d rect=[%{public}d,%{public}d %{public}dx%{public}d]',
        active, x, y, w, h);
      if (active === 1) {
        this.imeAttach();
      } else {
        this.imeDetach();
      }
    });
  }

  private imeAttach(): void {
    try {
      const controller = inputMethod.getController();
      this.imeController = controller;
      // 每次激活都 attach; 先清掉已注册回调, 否则 on() 累积导致重复注入
      controller.off('insertText');
      controller.off('deleteLeft');
      controller.off('deleteRight');
      controller.off('sendKeyboardStatus');
      controller.off('setPreviewText');
      controller.off('sendFunctionKey');
      const attr: inputMethod.InputAttribute = {
        textInputType: inputMethod.TextInputType.TEXT,
        // NEWLINE: 软键盘显示"换行"键。软键盘回车在本设备走 sendFunctionKey
        // (onKeyEvent 收不到), 由 sendFunctionKey 回调注入回车 down+up 实现换行。
        enterKeyType: inputMethod.EnterKeyType.NEWLINE
      };
      const cfg: inputMethod.TextConfig = { inputAttribute: attr };
      controller.attach(true, cfg).then(() => {
        controller.on('insertText', (text: string) => {
          testNapi.wineTextInputCommit(text);   // 提交文本 → Wine commit_string
        });
        controller.on('deleteLeft', (len: number) => {
          testNapi.imeBackspace();              // 退格 → Wine delete_surrounding
        });
        controller.on('deleteRight', (len: number) => {
          testNapi.imeBackspace();
        });
        controller.on('sendKeyboardStatus', (status: inputMethod.KeyboardStatus) => {
          hilog.info(DOMAIN, 'IME', '[keyboardStatus] %{public}d', status);
        });
        controller.on('setPreviewText', (text: string, range: inputMethod.Range) => {
          testNapi.wineTextInputPreedit(text);  // 预上屏 → Wine preedit
        });
        // 软键盘回车 (DONE/NEWLINE 功能键): 本设备 onKeyEvent 收不到软键盘回车,
        // 在 sendFunctionKey 回调直接向激活窗口注入回车 down+up 实现换行
        controller.on('sendFunctionKey', (fk: inputMethod.FunctionKey) => {
          hilog.info(DOMAIN, 'IME-TEST', '[sendFunctionKey] enterKeyType=%{public}d (0 UNSPEC 6 DONE 8 NEWLINE)',
            fk.enterKeyType);
          if (fk.enterKeyType === inputMethod.EnterKeyType.DONE ||
              fk.enterKeyType === inputMethod.EnterKeyType.NEWLINE) {
            const router = InputRouter.getInstance();
            if (router.getActiveToplevel() > 0) {
              router.sendKey(EVDEV_KEY_ENTER, true);
              router.sendKey(EVDEV_KEY_ENTER, false);
            }
          }
        });
      }).catch((err: BusinessError) => {
        hilog.error(DOMAIN, 'IME', '[attach] FAILED code=%{public}d msg=%{public}s', err.code, err.message);
      });
    } catch (e) {
      hilog.error(DOMAIN, 'IME', '[attach] EXCEPTION: %{public}s', JSON.stringify(e));
    }
  }

  private imeDetach(): void {
    const c = this.imeController;
    if (c) {
      try {
        c.off('insertText');
        c.off('deleteLeft');
        c.off('deleteRight');
        c.off('sendKeyboardStatus');
        c.off('setPreviewText');
        c.off('sendFunctionKey');
        c.hideTextInput();
        c.detach();
      } catch (e) {
        hilog.error(DOMAIN, 'IME', '[detach] EXCEPTION: %{public}s', JSON.stringify(e));
      }
      this.imeController = null;
    }
  }

  aboutToAppear(): void {
    this.setupIme();
    this.initialize();
    BreakpointSystem.initialize();
    this.breakpointCallback = (value: BreakpointType): void => {
      this.breakpoint = value;
      if (!isWideBreakpoint(value)) {
        this.phoneNavIndex = this.phoneNavIndexForFilter(this.filter);
      }
    };
    BreakpointSystem.subscribe(this.breakpointCallback);
    this.engineCallback = (state: EngineState, message: string): void => {
      this.engineState = state;
      this.engineMessage = message;
      this.refreshRunningState();
    };
    WineEngineService.getInstance().subscribe(this.engineCallback);
    this.refreshTimer = setInterval(() => this.refreshRunningState(), 1500);
    this.artworkListener = (_appId: string, _path: string): void => {
      this.consoleBackgroundCache.delete(_appId);
      this.artworkRevision++;
    };
    GameArtworkService.getInstance().subscribe(this.artworkListener);
  }

  aboutToDisappear(): void {
    this.detachConsoleNavigation();
    if (this.breakpointCallback) BreakpointSystem.unsubscribe(this.breakpointCallback);
    if (this.engineCallback) WineEngineService.getInstance().unsubscribe(this.engineCallback);
    if (this.settingsCallback) ThemeSettingsService.getInstance().unsubscribe(this.settingsCallback);
    if (this.refreshTimer >= 0) clearInterval(this.refreshTimer);
    if (this.artworkListener) GameArtworkService.getInstance().unsubscribe(this.artworkListener);
    this.stopWindowInsetObserver();
  }

  onPageShow(): void {
    this.consolePageVisible = true;
    this.settings = AppSettingsStore.getInstance().getGlobalSettings();
    this.consoleBackgroundCache.clear();
    this.syncConsoleNavigation();
    this.preferGamesForConsole();
    if (this.settings.uiSkin === UiSkin.CONSOLE) {
      this.consoleSearchActive = false;
      this.consoleSearchRequested = false;
      try { inputMethod.getController().stopInputSession(); } catch (_) {}
      // A pushed settings page keeps this Index instance and its List alive.
      // Rebuild the ring from the selected app when the page becomes visible
      // again, because the hidden List may still finish a native fling using
      // geometry from before the route transition or an orientation change.
      this.scheduleConsoleRingRelayout();
      this.scrollConsoleGridSelection(this.consoleFocusIndex, false, 180);
      setTimeout((): void => this.focusConsoleLauncher(), 300);
      setTimeout((): void => this.focusConsoleLauncher(), 850);
    }
    if (this.catalogInitialized && this.settings.uiSkin !== UiSkin.CONSOLE) {
      // Wine/File Explorer can add, rename or remove programs while this page
      // is hidden. Reconcile the catalog whenever it becomes visible again.
      void this.rescan().then((): void => {
        setTimeout((): void => this.restoreScrollOffset(this.filter), 0);
      });
    } else {
      this.refreshRunningState();
    }
    if (!(this.showDesktopLayer() && !this.launcherVisible)) {
      void this.applyPageWindowShell();
    }
  }

  onPageHide(): void {
    this.consolePageVisible = false;
    this.detachConsoleNavigation();
    this.suspendConsoleRingForPageTransition();
    this.captureScrollOffsets();
    this.stopWindowInsetObserver();
  }

  private captureScrollOffsets(): void {
    this.scrollOffsets['recent'] = this.safeScrollOffset(this.recentScroller);
    this.scrollOffsets['builtin'] = this.safeScrollOffset(this.builtinScroller);
    this.scrollOffsets['windows'] = this.safeScrollOffset(this.windowsScroller);
    this.scrollOffsets['favorite'] = this.safeScrollOffset(this.favoriteScroller);
    this.scrollOffsets['running'] = this.safeScrollOffset(this.runningScroller);
  }

  private safeScrollOffset(scroller: Scroller): ScrollOffsetSnapshot {
    try {
      const offset = scroller.currentOffset();
      return { xOffset: offset.xOffset, yOffset: offset.yOffset };
    } catch (_) {
      return { xOffset: 0, yOffset: 0 };
    }
  }

  private restoreScrollOffset(filter: CatalogFilter): void {
    const saved = this.scrollOffsets[filter];
    if (!saved) return;
    try {
      this.catalogScroller(filter).scrollTo({ xOffset: saved.xOffset, yOffset: saved.yOffset });
    } catch (_) {
    }
  }

  private async applyPageWindowShell(): Promise<void> {
    try {
      const pageWindow = this.resolveMainWindow();
      if (!pageWindow) {
        return;
      }
      await applyStatusOnlyImmersiveShell(pageWindow);
      this.startWindowInsetObserver(pageWindow);
    } catch (_) {
    }
  }

  private refreshPageInsets(pageWindow: window.Window): void {
    this.titleContentTopInsetVp = getMiniTitleContentTopInset(pageWindow);
    // Wide layouts do not render HDS MINI. Offset them by the physical status
    // inset only, independently from the narrow-layout title height.
    const insets = getStatusOnlyWindowInsets(pageWindow);
    this.statusBarHeightVp = insets.top;
    this.cutoutLeftInsetVp = insets.left;
    this.cutoutRightInsetVp = insets.right;
    this.navBarInsetVp = insets.bottom;
    if (this.launcherVisible && !this.transitioning) {
      this.ensureFloatPos();
    }
  }

  private startWindowInsetObserver(pageWindow: window.Window): void {
    this.stopWindowInsetObserver();
    this.disposeWindowInsetObserver = observeStatusOnlyWindowInsets(pageWindow, () => {
      this.refreshPageInsets(pageWindow);
    });
  }

  private stopWindowInsetObserver(): void {
    if (this.disposeWindowInsetObserver) {
      this.disposeWindowInsetObserver();
      this.disposeWindowInsetObserver = null;
    }
  }

  private onOverlayEditorRequest(): void {
    const raw = this.overlayEditorRequest;
    if (!raw) return;
    AppStorage.setOrCreate('overlayEditorRequest', '');
    try {
      const request = JSON.parse(raw) as Record<string, string>;
      router.pushUrl({
        url: 'pages/InputLayoutEditor',
        params: {
          profileId: request.profileId || 'default',
          profileName: request.profileName || '默认键鼠手柄',
          appId: request.appId || '',
          baseProfileId: request.baseProfileId || 'default',
          returnAbilityName: request.returnAbilityName || '',
          returnSessionId: request.returnSessionId || '',
          returnToplevelId: request.returnToplevelId || '0'
        }
      });
    } catch (_) {
    }
  }

  private async initialize(): Promise<void> {
    await ThemeSettingsService.getInstance().initialize(this.context);
    this.settings = ThemeSettingsService.getInstance().getSettings();
    WineWindowManager.getInstance().setOutputScalePercent(this.settings.outputScale);
    this.settingsCallback = (settings: GlobalSettings): void => {
      this.settings = settings;
      this.syncConsoleNavigation();
      WineWindowManager.getInstance().setOutputScalePercent(settings.outputScale);
      GamepadManager.getInstance().setDeadzone(settings.gamepadDeadzone);
      GamepadManager.getInstance().setOutputMode(settings.controllerOutputMode ?? 'dinput');
    };
    ThemeSettingsService.getInstance().subscribe(this.settingsCallback);
    await AppCatalogService.getInstance().initialize(this.context);
    await GameArtworkService.getInstance().initialize(this.context);
    AppSessionService.getInstance().initialize(this.context);
    LauncherAdapterRegistry.getInstance().initialize(this.context);
    WineEngineService.getInstance().initialize(this.context);
    await InputProfileService.getInstance().initialize(this.context);
    GamepadManager.getInstance().initialize(this.settings.gamepadDeadzone,
      this.settings.controllerOutputMode ?? 'dinput');
    ConsoleUiSound.getInstance().initialize(this.context);
    await this.rescan();
    this.preferGamesForConsole();
    this.catalogInitialized = true;
  }

  private async rescan(): Promise<void> {
    if (this.scanning) return;
    this.scanning = true;
    try {
      this.consoleBackgroundCache.clear();
      this.apps = await AppCatalogService.getInstance().scan();
      this.refreshRunningState();
    } finally {
      this.scanning = false;
    }
  }

  private async refreshFromPull(): Promise<void> {
    // Refresh's two-way binding updates `refreshing` before onRefreshing is
    // invoked. Keep it separate from the actual scan mutex; sharing one flag
    // made rescan() observe itself as already running and return forever with
    // the refresh indicator still active.
    this.refreshing = true;
    try {
      await this.rescan();
    } finally {
      this.refreshing = false;
    }
  }

  private refreshRunningState(): void {
    const sessionService = AppSessionService.getInstance();
    const running = resolveRunningAppIds(this.apps, sessionService.getRunningAppIds(),
      sessionService.getRunningPaths());
    const now = Date.now();
    running.forEach((id: string): void => {
      if (!this.playStartTimes.has(id)) this.playStartTimes.set(id, now);
    });
    this.playStartTimes.forEach((startedAt: number, id: string): void => {
      if (running.has(id)) return;
      this.playStartTimes.delete(id);
      void AppSettingsStore.getInstance().addPlayMinutes(id, (now - startedAt) / 60000);
    });
    if (this.runningAppIds.size !== running.size ||
      Array.from(running).some((id: string): boolean => !this.runningAppIds.has(id))) {
      this.runningAppIds = running;
    }
    const visible = this.filterApps(this.searchQuery, this.filter, running);
    if (!this.sameAppSequence(this.visibleApps, visible)) this.visibleApps = visible;
    this.syncConsoleFocus();
  }

  private sameAppSequence(left: AppDescriptor[], right: AppDescriptor[]): boolean {
    return left.length === right.length &&
      left.every((app: AppDescriptor, index: number): boolean => app === right[index]);
  }

  private runningFilterLabel(): string {
    const n = this.runningAppIds.size;
    return n > 0 ? `运行中 (${n})` : '运行中';
  }

  private pageBackgroundSource(): string {
    const value = this.settings.pageBackgroundUri;
    // DocumentViewPicker may return file://, datashare:// or another
    // provider URI. Preserve provider schemes; blindly prefixing file://
    // makes the selected image fail to load and leaves a solid white surface
    // below the translucent HDS title bar.
    if (value.indexOf('://') > 0) {
      return value;
    }
    return value.length > 0 ? 'file://' + value : '';
  }

  private filterApps(searchQuery: string, filter: CatalogFilter,
    running: Set<string> = this.runningAppIds): AppDescriptor[] {
    const query = searchQuery.trim().toLowerCase();
    const recentIds = filter === 'recent' ? AppSettingsStore.getInstance().getRecentLaunches() : [];
    const recentSet = new Set(recentIds);
    const result = this.apps.filter((app: AppDescriptor) => {
      const normalized = normalizeLaunchPath(app.launchTarget.executable);
      if (filter === 'builtin' && app.source !== AppSource.BUILTIN) return false;
      if (filter === 'windows' && app.source !== AppSource.DOWNLOAD) return false;
      if (filter === 'running' && !running.has(app.id)) return false;
      if (filter === 'favorite' && !AppSettingsStore.getInstance().getAppSettings(app.id).favorite) return false;
      if (filter === 'recent' && !recentSet.has(app.id)) return false;
      if (!query) return true;
      return app.title.toLowerCase().includes(query) || normalized.includes(query);
    });
    if (filter === 'recent') {
      // 最近启动顺序排序 (最新在前): recentIds 已是最新在前
      const order = new Map(recentIds.map((id: string, index: number) => [id, index]));
      result.sort((a: AppDescriptor, b: AppDescriptor) =>
        (order.get(a.id) ?? 999) - (order.get(b.id) ?? 999));
    }
    return result;
  }

  private appsForFilter(filter: CatalogFilter): AppDescriptor[] {
    if (filter === this.filter) {
      return this.visibleApps;
    }
    return this.filterApps(this.searchQuery, filter);
  }

  private updateSearch(value: string): void {
    this.searchQuery = value;
    this.visibleApps = this.filterApps(value, this.filter);
    this.syncConsoleFocus();
  }

  private phoneNavIndexForFilter(value: CatalogFilter): number {
    if (value === 'recent') return PHONE_NAV_RECENT;
    if (value === 'windows') return PHONE_NAV_WINDOWS;
    if (value === 'favorite') return PHONE_NAV_FAVORITE;
    if (value === 'running') return PHONE_NAV_RUNNING;
    return PHONE_NAV_BUILTIN;
  }

  private phoneFilterForIndex(index: number): CatalogFilter | null {
    if (index === PHONE_NAV_RECENT) return 'recent';
    if (index === PHONE_NAV_BUILTIN) return 'builtin';
    if (index === PHONE_NAV_WINDOWS) return 'windows';
    if (index === PHONE_NAV_FAVORITE) return 'favorite';
    if (index === PHONE_NAV_RUNNING) return 'running';
    return null;
  }

  private selectFilter(value: CatalogFilter, syncPhoneNavigation: boolean = true): void {
    this.resetConsoleRingMotion();
    if (this.settings.uiSkin === UiSkin.CONSOLE) this.consoleCategoryInitialized = true;
    this.captureScrollOffsets();
    this.filter = value;
    this.consoleFocusIndex = 0;
    this.consoleSelectedAppId = '';
    this.visibleApps = this.filterApps(this.searchQuery, value);
    this.syncConsoleFocus();
    if (this.settings.uiSkin === UiSkin.CONSOLE) {
      this.scrollConsoleGridSelection(this.consoleFocusIndex, false, 80);
    }
    setTimeout((): void => this.restoreScrollOffset(value), 0);
    if (this.settings.uiSkin === UiSkin.CONSOLE) {
      const index = this.consoleNavIndexForFilter(value);
      if (index !== this.consoleNavIndex) {
        this.consoleNavIndex = index;
        this.consoleNavController.changeIndex(index);
      }
    }
    if (syncPhoneNavigation && this.isPhone()) {
      const index = this.phoneNavIndexForFilter(value);
      if (index !== this.phoneNavIndex) {
        this.phoneNavIndex = index;
        this.phoneNavController.changeIndex(index);
      }
    }
  }

  private openSidebarPage(url: string): void {
    this.suspendConsoleRingForPageTransition();
    router.pushUrl({ url });
  }

  private filterTitle(): string {
    if (this.filter === 'recent') return '最近';
    if (this.filter === 'builtin') return '内建应用';
    if (this.filter === 'windows') return 'Windows 程序';
    if (this.filter === 'running') return '运行中的应用';
    if (this.filter === 'favorite') return '收藏的应用';
    return '内建应用';
  }

  private filterDescription(): string {
    if (this.filter === 'recent') return '最近启动过的程序，最新在前';
    if (this.filter === 'builtin') return '系统工具与内建测试程序';
    if (this.filter === 'windows') {
      const catalog = AppCatalogService.getInstance();
      return catalog.isGamesDirectoryReady() ?
        '来自 Download/com.vintage.pomelopro/games' :
        '游戏目录尚未建立，点击创建并完成一次系统确认';
    }
    if (this.filter === 'running') return '点击卡片可恢复，长按可关闭会话';
    if (this.filter === 'favorite') return '已收藏的内建应用和 Windows 程序';
    return this.engineMessage;
  }

  private emptyTitle(filter: CatalogFilter): string {
    if (this.searchQuery.trim()) return '没有匹配的应用';
    if (filter === 'recent') return '还没有启动过程序';
    if (filter === 'favorite') return '还没有收藏应用';
    if (filter === 'running') return '当前没有运行中的应用';
    if (filter === 'windows') return '尚未发现 Windows 程序';
    return '未找到内建应用';
  }

  private emptyDescription(filter: CatalogFilter): string {
    if (this.searchQuery.trim()) return '请更换关键词，或清除搜索后查看当前分类的全部内容';
    if (filter === 'recent') return '启动过的程序会出现在这里，方便快速再次打开';
    if (filter === 'favorite') return '在应用设置中开启收藏后，应用会显示在这里';
    if (filter === 'running') return '启动 Wine 桌面或程序后，可在这里恢复或关闭会话';
    if (filter === 'windows') {
      const catalog = AppCatalogService.getInstance();
      if (!catalog.isGamesDirectoryReady()) {
        return '系统需要一次确认来建立 Download/com.vintage.pomelopro，之后应用会自动创建 games';
      }
      return '将程序放入 Download/com.vintage.pomelopro/games，应用会自动扫描';
    }
    return '安装包中的内建应用清单为空，请检查应用资源';
  }

  private emptyIcon(filter: CatalogFilter): Resource {
    if (filter === 'recent') return $r('sys.symbol.clock');
    if (filter === 'favorite') return $r('sys.symbol.star');
    if (filter === 'running') return $r('sys.symbol.play_fill');
    if (filter === 'windows') return $r('sys.symbol.folder');
    return $r('sys.symbol.house_fill');
  }

  private emptyActionLabel(filter: CatalogFilter): string {
    if (this.searchQuery.trim()) return '清除搜索';
    if (filter === 'windows') {
      return AppCatalogService.getInstance().isGamesDirectoryReady() ? '重新扫描' : '创建游戏目录';
    }
    return '';
  }

  private handleEmptyAction(filter: CatalogFilter): void {
    if (this.searchQuery.trim()) {
      this.updateSearch('');
      return;
    }
    if (filter === 'windows') {
      if (AppCatalogService.getInstance().isGamesDirectoryReady()) {
        void this.rescan();
      } else {
        void this.prepareGamesDirectory();
      }
    }
  }

  private async prepareGamesDirectory(): Promise<void> {
    if (this.scanning) return;
    this.scanning = true;
    try {
      const status = await AppCatalogService.getInstance().provisionGamesDirectory();
      this.consoleBackgroundCache.clear();
      this.apps = await AppCatalogService.getInstance().scan();
      this.refreshRunningState();
      promptAction.showToast({
        message: status.ready ? '游戏目录已创建' : '未能创建游戏目录：' + status.error
      });
    } finally {
      this.scanning = false;
    }
  }

  private catalogCardViewMode(): ViewMode {
    return this.settings.viewMode === ViewMode.FILES ? ViewMode.GRID : this.settings.viewMode;
  }

  private gridColumns(): string {
    if (this.catalogCardViewMode() === ViewMode.COMPACT) return '1fr';
    // VintagePomelo keeps cover cards in two columns on phones. A single
    // flexible column makes one icon fill almost the entire compact window.
    if (this.isPhone()) return '1fr 1fr';
    if (this.breakpoint === 'xl') return '1fr 1fr 1fr 1fr 1fr';
    return '1fr 1fr 1fr';
  }

  private catalogScroller(filter: CatalogFilter): Scroller {
    if (filter === 'recent') return this.recentScroller;
    if (filter === 'windows') return this.windowsScroller;
    if (filter === 'favorite') return this.favoriteScroller;
    if (filter === 'running') return this.runningScroller;
    return this.builtinScroller;
  }

  private phoneNavigationScrollers(): Scroller[] {
    return [this.recentScroller, this.builtinScroller, this.windowsScroller, this.favoriteScroller, this.runningScroller];
  }

  private compactGridColumns(): string {
    if (this.isPhoneLandscape()) return '1fr 1fr';
    if (this.isPhone()) return '1fr';
    if (this.breakpoint === 'xl') return '1fr 1fr 1fr';
    return '1fr 1fr';
  }

  private largeGridColumns(): string {
    if (this.isPhoneLandscape()) return '1fr 1fr';
    if (this.isPhone()) return '1fr';
    if (this.breakpoint === 'xl') return '1fr 1fr 1fr';
    return '1fr 1fr';
  }

  private phoneGridItemMinHeight(): number {
    // Grid can under-measure an auto-height card while a narrow two-column
    // layout is settling. Reserve the cover, fixed title row and shadow inset
    // explicitly so the following row can never paint over this card.
    if (this.catalogCardViewMode() === ViewMode.COMPACT) return 120;
    const coverHeight = this.catalogCardViewMode() === ViewMode.LARGE ? 196 : 120;
    return coverHeight + 71 + 16;
  }

  private launch(app: AppDescriptor): Promise<void> {
    if (!app.available) {
      this.openAppSettings(app, true);
      return Promise.resolve();
    }
    const conflict = AppSessionService.getInstance().getLaunchConflict(app, this.settings);
    if (conflict !== LaunchConflictKind.NONE) {
      AlertDialog.show({
        title: '切换显示模式',
        message: '当前 Windows 会话使用另一种显示模式。继续将关闭当前会话并重建 Wayland 显示，Wine prefix 不会重置。',
        primaryButton: { value: '取消', action: (): void => {} },
        secondaryButton: {
          value: '关闭并切换',
          fontColor: '#D94848',
          action: (): void => { this.switchAndLaunch(app); }
        }
      });
      return Promise.resolve();
    }
    return this.enqueueLaunch(app, false);
  }

  private switchAndLaunch(app: AppDescriptor): Promise<void> {
    return this.enqueueLaunch(app, true);
  }

  private enqueueLaunch(app: AppDescriptor, switchMode: boolean): Promise<void> {
    const run = this.launchChain.then((): Promise<void> => this.doLaunch(app, switchMode));
    // 单次启动失败不能污染队列：后续点击仍按序执行。
    this.launchChain = run.catch((): void => {});
    return run;
  }

  private async doLaunch(app: AppDescriptor, switchMode: boolean): Promise<void> {
    const root = AppCatalogService.getInstance().getWineHomeDirectory();
    if (!root && (app.launchTarget.kind === LaunchKind.WINE_EXE ||
      app.launchTarget.kind === LaunchKind.WINE_SYSTEM ||
      app.launchTarget.kind === LaunchKind.WINE_DESKTOP)) {
      promptAction.showToast({
        message: '无法创建 Download/com.vintage.pomelopro/games，请点击 Windows 程序中的重新扫描重试'
      });
      this.refreshRunningState();
      return;
    }
    let session: RuntimeSession | null = null;
    try {
      if (switchMode) {
        session = await AppSessionService.getInstance().switchModeAndLaunch(app, this.settings, root);
      } else {
        session = await LauncherAdapterRegistry.getInstance().launch(app, this.settings, root);
      }
      if (!session && (app.launchTarget.kind === LaunchKind.WINE_EXE ||
        app.launchTarget.kind === LaunchKind.WINE_SYSTEM ||
        app.launchTarget.kind === LaunchKind.WINE_DESKTOP)) {
        const engineMessage = WineEngineService.getInstance().getMessage();
        promptAction.showToast({
          message: engineMessage === 'Wine 引擎已就绪' ? '程序启动失败，请稍后重试' : engineMessage
        });
      }
    } catch (_) {
      promptAction.showToast({ message: '程序启动异常，请稍后重试' });
    } finally {
      this.refreshRunningState();
    }
  }

  private isEngineBusy(): boolean {
    return this.engineState === EngineState.PREPARING || this.engineState === EngineState.SWITCHING;
  }

  private openAppSettings(app: AppDescriptor, selectExecutable: boolean = false): void {
    router.pushUrl({
      url: 'pages/AppSettings',
      params: { appId: app.id, selectExecutable: selectExecutable ? 'true' : 'false' }
    });
  }

  private showAppLongPressMenu(app: AppDescriptor): void {
    const running = this.runningAppIds.has(app.id);
    if (app.source === AppSource.DOWNLOAD && !app.available) {
      promptAction.showDialog({
        title: app.title,
        message: '主程序已不存在，可重新关联或只删除卡片记录（不会删除任何文件）',
        buttons: [
          { text: '重新关联', color: '#5569D8' },
          { text: '应用设置', color: $r('sys.color.ohos_id_color_text_primary') },
          { text: '删除卡片', color: '#D94848' }
        ]
      }).then((result) => {
        if (result.index === 0) this.openAppSettings(app, true);
        else if (result.index === 1) this.openAppSettings(app);
        else if (result.index === 2) this.confirmDeleteUnavailableApp(app);
      }).catch((_error: Error) => {});
      return;
    }
    promptAction.showDialog({
      title: app.title,
      message: '请选择操作',
      buttons: [
        { text: running ? '关闭运行中的程序' : (app.available ? '启动或恢复' : '重新关联'), color: running ? '#D94848' : '#5569D8' },
        { text: '应用设置', color: $r('sys.color.ohos_id_color_text_primary') },
        { text: '取消', color: $r('sys.color.ohos_id_color_text_secondary') }
      ]
    }).then((result) => {
      if (result.index === 0) {
        if (running) {
          AppSessionService.getInstance().stopApp(app);
          this.refreshRunningState();
        } else if (app.available) this.launch(app); else this.openAppSettings(app, true);
      } else if (result.index === 1) {
        this.openAppSettings(app);
      }
    }).catch((_error: Error) => {});
  }

  private confirmDeleteUnavailableApp(app: AppDescriptor): void {
    AlertDialog.show({
      title: '删除卡片记录',
      message: `将从旧柚 Pro 中移除“${app.title}”卡片，不会删除 games 目录中的任何文件。`,
      primaryButton: { value: '取消', action: (): void => {} },
      secondaryButton: {
        value: '删除',
        fontColor: '#D94848',
        action: (): void => { this.deleteUnavailableApp(app); }
      }
    });
  }

  private async deleteUnavailableApp(app: AppDescriptor): Promise<void> {
    if (app.source !== AppSource.DOWNLOAD || app.available) return;
    await AppSettingsStore.getInstance().removeCachedApp(app.id);
    this.apps = this.apps.filter((item: AppDescriptor): boolean => item.id !== app.id);
    this.refreshRunningState();
    promptAction.showToast({ message: '卡片记录已删除' });
  }

  private async setViewMode(mode: ViewMode): Promise<void> {
    await AppSettingsStore.getInstance().setViewMode(mode);
    this.settings = AppSettingsStore.getInstance().getGlobalSettings();
  }

  private showViewModeMenu(): void {
    promptAction.showActionMenu({
      title: '卡片视图',
      buttons: [
        { text: (this.settings.viewMode === ViewMode.GRID ? '✓ ' : '') + '网格封面', color: $r('sys.color.ohos_id_color_text_primary') },
        { text: (this.settings.viewMode === ViewMode.COMPACT ? '✓ ' : '') + '紧凑列表', color: $r('sys.color.ohos_id_color_text_primary') },
        { text: (this.settings.viewMode === ViewMode.LARGE ? '✓ ' : '') + '大封面', color: $r('sys.color.ohos_id_color_text_primary') },
        { text: (this.settings.viewMode === ViewMode.FILES ? '✓ ' : '') + '文件浏览', color: $r('sys.color.ohos_id_color_text_primary') }
      ]
    }).then((result) => {
      if (result.index === 0) this.setViewMode(ViewMode.GRID);
      else if (result.index === 1) this.setViewMode(ViewMode.COMPACT);
      else if (result.index === 2) this.setViewMode(ViewMode.LARGE);
      else if (result.index === 3) this.setViewMode(ViewMode.FILES);
    }).catch(() => {});
  }

  private phoneViewModeIcon(): Resource {
    if (this.settings.viewMode === ViewMode.COMPACT) return $r('sys.symbol.list_bullet');
    if (this.settings.viewMode === ViewMode.LARGE) return $r('sys.symbol.rectangle_grid_1x2');
    if (this.settings.viewMode === ViewMode.FILES) return $r('sys.symbol.folder');
    return $r('sys.symbol.square_grid_2x2');
  }

  private phoneNavigationMenu(): HdsNavigationMenuContentOptions {
    return {
      value: [
        {
          content: {
            label: '系统设置',
            icon: $r('sys.symbol.gearshape'),
            action: (): void => { this.openSidebarPage('pages/SystemSettings'); }
          }
        },
        {
          content: {
            label: '切换卡片视图',
            icon: this.phoneViewModeIcon(),
            action: (): void => { this.showViewModeMenu(); }
          }
        },
        {
          content: {
            label: '帮助中心',
            icon: $r('sys.symbol.info_circle'),
            action: (): void => { this.openSidebarPage('pages/HelpCenter'); }
          }
        }
      ],
      maxCount: 3
    };
  }

  private phoneBottomInset(): number {
    return bottomNavigationLayout(this.isPhoneLandscape(), this.navBarInsetVp).contentInset;
  }

  private showsCompactSearch(): boolean {
    return this.isPhone() && this.breakpoint === 'md';
  }

  @Builder
  private NavItem(label: string, value: CatalogFilter, icon: Resource) {
    Row({ space: 12 }) {
      SymbolGlyph(icon).fontSize(19)
        .fontColor([this.filter === value ? skinColor('accent') : skinColor('text_secondary')])
      if (!this.sidebarCollapsed) {
        Text(label).fontSize(14).fontColor(skinColor('text_primary'))
          .fontWeight(this.filter === value ? FontWeight.Medium : FontWeight.Normal)
      }
    }
    .width('100%')
    .height(48)
    .padding({ left: 15, right: 12 })
    .borderRadius(12)
    .backgroundColor(this.filter === value ? skinColor('selected_background') : '#00000000')
    .id('library-nav-' + value)
    .onClick(() => this.selectFilter(value))
  }

  @Builder
  private SideBar() {
    Column() {
      Row() {
        if (!this.sidebarCollapsed) {
          Column({ space: this.isNarrowPhone() ? 2 : 3 }) {
            Text('旧柚Pro').fontSize(this.isNarrowPhone() ? 16 : 18).fontWeight(FontWeight.Bold).fontColor(skinColor('text_primary'))
            Text(this.apps.length + ' 个程序').fontSize(this.isNarrowPhone() ? 9 : 11).fontColor(skinColor('text_secondary'))
          }.alignItems(HorizontalAlign.Start).layoutWeight(1)
        }
        Button({ type: ButtonType.Circle }) {
          SymbolGlyph(this.sidebarCollapsed ? $r('sys.symbol.chevron_right') : $r('sys.symbol.chevron_left'))
            .fontSize(16).fontColor([skinColor('text_primary')])
        }
          .width(48).height(48)
          .backgroundColor(skinColor('search_background'))
          .border({ width: 1, color: '#12000000' })
          .id('library-toggle-sidebar')
          .onClick((): void => {
            this.sidebarCollapsed = !this.sidebarCollapsed;
          })
      }.width('100%').height(64)

      if (!this.sidebarCollapsed) {
        AppSearchBar({
          value: this.searchQuery,
          heightVp: 48,
          onValueChange: (value: string): void => { this.updateSearch(value); }
        })
        Button('重新扫描').width('100%').height(48).fontSize(13).fontWeight(FontWeight.Medium)
          .backgroundColor(skinColor('search_background')).fontColor(skinColor('text_primary')).borderRadius(16)
          .id('library-rescan')
          .onClick(() => this.rescan())
          .margin({ top: 10, bottom: 18 })
      }

      Scroll() {
        Column() {
          Column({ space: 4 }) {
            this.NavItem('最近', 'recent', $r('sys.symbol.clock'))
            this.NavItem('收藏', 'favorite', $r('sys.symbol.star'))
            this.NavItem('内建应用', 'builtin', $r('sys.symbol.house_fill'))
            this.NavItem('Windows 程序', 'windows', $r('sys.symbol.folder'))
            this.NavItem(this.runningFilterLabel(), 'running', $r('sys.symbol.play_fill'))
          }.width('100%')

          Blank()
          Row({ space: 12 }) {
            SymbolGlyph($r('sys.symbol.gearshape')).fontSize(19).fontColor([skinColor('text_secondary')])
            if (!this.sidebarCollapsed) {
              Text('系统设置').fontSize(14).fontColor(skinColor('text_primary'))
            }
          }
          .width('100%').height(48).padding({ left: 15 }).borderRadius(12)
          .id('library-open-system-settings')
          .onClick((): void => this.openSidebarPage('pages/SystemSettings'))
          Row({ space: 12 }) {
            SymbolGlyph($r('sys.symbol.info_circle')).fontSize(19).fontColor([skinColor('text_secondary')])
            if (!this.sidebarCollapsed) {
              Text('帮助').fontSize(14).fontColor(skinColor('text_primary'))
            }
          }.width('100%').height(48).padding({ left: 15 })
            .id('library-open-help')
            .onClick((): void => { this.openSidebarPage('pages/HelpCenter'); })
        }
        .width('100%')
        .constraintSize({ minHeight: '100%' })
      }
      .layoutWeight(1)
      .width('100%')
      .scrollBar(BarState.Off)
      .edgeEffect(EdgeEffect.Spring)
    }
    .width(this.sidebarCollapsed ? 72 : 264)
    .height('100%')
    .padding({
      left: this.isNarrowPhone() ? 10 : 16,
      right: this.isNarrowPhone() ? 10 : 16,
      top: this.isNarrowPhone() ? 10 : 16,
      bottom: Math.max(this.isNarrowPhone() ? 10 : 16, this.navBarInsetVp)
    })
    .backgroundColor(skinColor('sidebar_background'))
    .backgroundBlurStyle(BlurStyle.COMPONENT_THICK)
  }

  @Builder
  private Header() {
    Column() {
      Row() {
        Column({ space: 3 }) {
          Text(this.filterTitle())
            .fontSize(28).fontWeight(FontWeight.Bold).fontColor(skinColor('text_primary'))
          Text(this.filterDescription()).fontSize(13).fontColor(this.engineState === EngineState.ERROR ? '#B42318' : skinColor('text_secondary'))
        }.alignItems(HorizontalAlign.Start).layoutWeight(1)
        Row({ space: 4 }) {
          Button({ type: ButtonType.Circle }) { SymbolGlyph($r('sys.symbol.square_grid_2x2')).fontSize(17).fontColor([skinColor('text_primary')]) }
            .width(48).height(48).backgroundColor(this.settings.viewMode === ViewMode.GRID ? skinColor('selected_background') : skinColor('search_background')).onClick(() => this.setViewMode(ViewMode.GRID))
          Button({ type: ButtonType.Circle }) { SymbolGlyph($r('sys.symbol.list_bullet')).fontSize(17).fontColor([skinColor('text_primary')]) }
            .width(48).height(48).backgroundColor(this.settings.viewMode === ViewMode.COMPACT ? skinColor('selected_background') : skinColor('search_background')).onClick(() => this.setViewMode(ViewMode.COMPACT))
          Button({ type: ButtonType.Circle }) { SymbolGlyph($r('sys.symbol.rectangle_grid_1x2')).fontSize(17).fontColor([skinColor('text_primary')]) }
            .width(48).height(48).backgroundColor(this.settings.viewMode === ViewMode.LARGE ? skinColor('selected_background') : skinColor('search_background')).onClick(() => this.setViewMode(ViewMode.LARGE))
          Button({ type: ButtonType.Circle }) { SymbolGlyph($r('sys.symbol.folder')).fontSize(17).fontColor([skinColor('text_primary')]) }
            .width(48).height(48).backgroundColor(this.settings.viewMode === ViewMode.FILES ? skinColor('selected_background') : skinColor('search_background')).onClick(() => this.setViewMode(ViewMode.FILES))
        }
        .id('library-view-modes')
        // Keep the complete system-reported title-button area. PC window
        // managers add spacing between the maximize/minimize/close nodes
        // that is not always included in TitleButtonRect.width.
        .margin({ right: this.titleButtonInsetVp > 0 ? this.titleButtonInsetVp + 24 : 0 })
      }.width('100%')
    }
    .width('100%')
    .height(64)
    .justifyContent(FlexAlign.Start)
  }

  @Builder
  private AppGrid(items: AppDescriptor[]) {
    Grid() {
      ForEach(items, (app: AppDescriptor): void => {
        GridItem() {
          this.CatalogCard(app)
        }
        // Shadows extend beyond the card's measured bounds. Without this
        // inset Grid clips the last row's rounded bottom edge and shadow.
        .padding({ left: 4, right: 4, top: 4, bottom: 12 })
        .constraintSize({ minHeight: this.isPhone() ? this.phoneGridItemMinHeight() : 0 })
      }, (app: AppDescriptor): string => app.id)
    }
    .columnsTemplate(this.catalogCardViewMode() === ViewMode.COMPACT ? this.compactGridColumns() :
      (this.catalogCardViewMode() === ViewMode.LARGE ? this.largeGridColumns() : this.gridColumns()))
    .columnsGap(8)
    .rowsGap(this.isPhone() ? 8 : 4)
    .width('100%')
  }

  @Builder
  private CatalogCard(app: AppDescriptor) {
    AppCard({
      app: app,
      viewMode: this.catalogCardViewMode(),
      running: this.runningAppIds.has(app.id),
      useGlassEffect: this.settings.useGlassEffect,
      narrowPhone: this.isPhone(),
      onLaunch: (target: AppDescriptor): void => { this.showConsoleDetail(target); },
      onSettings: (target: AppDescriptor): void => { this.openAppSettings(target); },
      onLongPress: (target: AppDescriptor): void => { this.showAppLongPressMenu(target); }
    })
  }

  /**
   * Give every catalog filter its own ArkUI branch and scroll identity.
   * Reusing one non-empty Grid made ForEach keep the previous category until
   * an empty branch (usually Favorites) forced the node tree to be recreated.
   */
  @Builder
  private CatalogScroll(items: AppDescriptor[], filter: CatalogFilter) {
    Scroll(this.catalogScroller(filter)) {
      Column() {
        if (this.showsCompactSearch()) {
          AppSearchBar({
            value: this.searchQuery,
            heightVp: 44,
            onValueChange: (value: string): void => { this.updateSearch(value); }
          }).margin({ bottom: 10 })
        }
        this.AppGrid(items)
      }
      .width('100%')
      .constraintSize({ minHeight: '100%' })
      .justifyContent(FlexAlign.Start)
      // Keep the last row above the phone navigation gesture area and the
      // overlaid bottom bar. The extra inset is harmless on tablet layouts.
      .padding({
        top: this.isPhone() ? this.phoneTitleBarScrollInset() : 2,
        bottom: this.isPhone() ? this.phoneBottomInset() : Math.max(36, this.navBarInsetVp)
      })
    }
    .layoutWeight(1)
    .width('100%')
    .scrollBar(BarState.Off)
    .id('library-scroll-' + filter)
  }

  @Builder
  private EmptyLibraryState(filter: CatalogFilter) {
    Column({ space: 12 }) {
      SymbolGlyph(this.emptyIcon(filter)).fontSize(this.isNarrowPhone() ? 34 : 48)
        .fontColor([skinColor('text_secondary')])
      Text(this.emptyTitle(filter)).fontSize(this.isNarrowPhone() ? 16 : 20)
        .fontWeight(FontWeight.Medium).fontColor(skinColor('text_primary'))
      Text(this.emptyDescription(filter)).fontSize(this.isNarrowPhone() ? 11 : 13)
        .fontColor(skinColor('text_secondary')).textAlign(TextAlign.Center)
        .constraintSize({ maxWidth: this.isNarrowPhone() ? 240 : 420 })
      if (this.emptyActionLabel(filter)) {
        Button(this.emptyActionLabel(filter)).height(48).borderRadius(24)
          .id('library-empty-action-' + filter)
          .onClick((): void => this.handleEmptyAction(filter))
      }
    }
    .layoutWeight(1)
    .justifyContent(this.isPhone() ? FlexAlign.Start : FlexAlign.Center)
    .padding({ top: this.isNarrowPhone() ? 18 : (this.breakpoint === 'sm' ? 30 : 0),
      bottom: Math.max(this.isNarrowPhone() ? 72 : (this.breakpoint === 'sm' ? 96 : 0), this.navBarInsetVp) })
  }

  @Builder
  private CatalogEmptyScroll(filter: CatalogFilter) {
    Scroll(this.catalogScroller(filter)) {
      Column() {
        if (this.showsCompactSearch()) {
          AppSearchBar({
            value: this.searchQuery,
            heightVp: 44,
            onValueChange: (value: string): void => { this.updateSearch(value); }
          }).margin({ bottom: 10 })
        }
        this.EmptyLibraryState(filter)
      }
      .width('100%')
      .constraintSize({ minHeight: '100%' })
      .padding({
        top: this.isPhone() ? this.phoneTitleBarScrollInset() : 2,
        bottom: this.isPhone() ? this.phoneBottomInset() : Math.max(36, this.navBarInsetVp)
      })
    }
    .layoutWeight(1)
    .width('100%')
    .scrollBar(BarState.Off)
    .id('library-empty-scroll-' + filter)
  }

  @Builder
  private WindowsRefreshContent() {
    Refresh({ refreshing: $$this.refreshing }) {
      if (this.settings.viewMode === ViewMode.FILES) {
        FileBrowserView({
          topInsetVp: this.isPhone() ? this.phoneTitleBarScrollInset() : 0,
          bottomInsetVp: this.isPhone() ? this.phoneBottomInset() : 0
        })
      } else if (this.scanning) {
        Row({ space: 10 }) {
          LoadingProgress().width(20).height(20)
          Text('正在扫描应用目录…').fontColor(skinColor('text_secondary'))
        }.layoutWeight(1).justifyContent(FlexAlign.Center)
      } else if (this.appsForFilter('windows').length === 0) {
        this.CatalogEmptyScroll('windows')
      } else {
        this.CatalogScroll(this.appsForFilter('windows'), 'windows')
      }
    }
    .layoutWeight(1)
    .width('100%')
    .onRefreshing((): void => { void this.refreshFromPull(); })
    .id('windows-library-refresh')
  }

  @Builder
  private LibraryPane(filter: CatalogFilter) {
    if (filter === 'windows') {
      this.WindowsRefreshContent()
    } else if (this.scanning) {
      Row({ space: 10 }) {
        LoadingProgress().width(20).height(20)
        Text('正在扫描应用目录…').fontColor(skinColor('text_secondary'))
      }.margin({ top: this.isNarrowPhone() ? 12 : 22 })
    } else if (this.appsForFilter(filter).length === 0) {
      this.CatalogEmptyScroll(filter)
    } else if (filter === 'builtin') {
      this.CatalogScroll(this.appsForFilter(filter), 'builtin')
    } else if (filter === 'running') {
      this.CatalogScroll(this.appsForFilter(filter), 'running')
    } else if (filter === 'recent') {
      this.CatalogScroll(this.appsForFilter(filter), 'recent')
    } else {
      this.CatalogScroll(this.appsForFilter(filter), 'favorite')
    }
  }

  @Builder
  private Library() {
    Column() {
      if (!this.isPhone()) {
        this.Header()
      }
      if (this.filter === 'windows') {
        this.WindowsRefreshContent()
      } else if (this.scanning) {
        Row({ space: 10 }) {
          LoadingProgress().width(20).height(20)
          Text('正在扫描应用目录…').fontColor(skinColor('text_secondary'))
        }.margin({ top: this.isNarrowPhone() ? 12 : 22 })
      } else if (this.visibleApps.length === 0) {
        this.CatalogEmptyScroll(this.filter)
      } else if (this.filter === 'builtin') {
        this.CatalogScroll(this.visibleApps, 'builtin')
      } else if (this.filter === 'running') {
        this.CatalogScroll(this.visibleApps, 'running')
      } else if (this.filter === 'recent') {
        this.CatalogScroll(this.visibleApps, 'recent')
      } else {
        this.CatalogScroll(this.visibleApps, 'favorite')
      }
    }
    .layoutWeight(1)
    .height('100%')
    .padding({
      left: this.contentPadding(),
      right: this.contentPadding(),
      top: this.isPhone() ? 0 : 16
    })
  }

  @Builder
  private PhoneNavigationTab(label: string, icon: Resource, index: number) {
    Column({ space: 2 }) {
      SymbolGlyph(icon)
        .fontSize(20)
        .fontColor([this.phoneNavIndex === index ? skinColor('accent') : skinColor('text_secondary')])
      Text(label)
        .fontSize(10)
        .fontWeight(this.phoneNavIndex === index ? FontWeight.Medium : FontWeight.Normal)
        .fontColor(this.phoneNavIndex === index ? skinColor('accent') : skinColor('text_secondary'))
        .maxLines(1)
    }
    .width('100%')
    // HDS owns the padded inner slot (barHeight minus 8vp on the tested SDK).
    // A fixed bar-sized child overflows that slot and shifts the content down.
    .height('100%')
    .justifyContent(FlexAlign.Center)
    .alignItems(HorizontalAlign.Center)
    .id('library-nav-tab-' + index)
  }

  @Builder
  private PhoneNavigationContents() {
    TabContent() {
      Column() {
        this.LibraryPane('recent')
      }
      .width('100%')
      .height('100%')
      .layoutWeight(1)
      .padding({ left: this.contentPadding(), right: this.contentPadding() })
    }
      .tabBar(this.PhoneNavigationTab('最近', $r('sys.symbol.clock'), PHONE_NAV_RECENT))
    TabContent() {
      Column() {
        this.LibraryPane('builtin')
      }
      .width('100%')
      .height('100%')
      .layoutWeight(1)
      .padding({ left: this.contentPadding(), right: this.contentPadding() })
    }
      .tabBar(this.PhoneNavigationTab('内建', $r('sys.symbol.house_fill'), PHONE_NAV_BUILTIN))
    TabContent() {
      Column() {
        this.LibraryPane('windows')
      }
      .width('100%')
      .height('100%')
      .layoutWeight(1)
      .padding({ left: this.contentPadding(), right: this.contentPadding() })
    }
      .tabBar(this.PhoneNavigationTab('Windows', $r('sys.symbol.folder'), PHONE_NAV_WINDOWS))
    TabContent() {
      Column() {
        this.LibraryPane('favorite')
      }
      .width('100%')
      .height('100%')
      .layoutWeight(1)
      .padding({ left: this.contentPadding(), right: this.contentPadding() })
    }
      .tabBar(this.PhoneNavigationTab('收藏', $r('sys.symbol.star'), PHONE_NAV_FAVORITE))
    TabContent() {
      Column() {
        this.LibraryPane('running')
      }
      .width('100%')
      .height('100%')
      .layoutWeight(1)
      .padding({ left: this.contentPadding(), right: this.contentPadding() })
    }
      .tabBar(this.PhoneNavigationTab(this.runningFilterLabel(), $r('sys.symbol.play_fill'), PHONE_NAV_RUNNING))
  }

  @Builder
  private PhoneNavigation() {
    HdsTabs({
      barPosition: BarPosition.End,
      index: this.phoneNavIndex,
      controller: this.phoneNavController
    }) {
      this.PhoneNavigationContents()
    }
    .vertical(false)
    .barHeight(bottomNavigationLayout(this.isPhoneLandscape(), this.navBarInsetVp).barHeight)
    .scrollable(this.breakpoint === 'xs' || this.breakpoint === 'sm')
    .animationDuration(300)
    .barOverlap(true)
    .divider({ mode: DividerMode.FOLLOW_SCROLL })
    .barBackgroundBlurStyle(BlurStyle.COMPONENT_THICK)
    .barFloatingStyle({
      barBottomMargin: bottomNavigationLayout(this.isPhoneLandscape(), this.navBarInsetVp).bottomMargin,
      barSideMargin: bottomNavigationLayout(this.isPhoneLandscape(), this.navBarInsetVp).sideMargin,
      gradientMask: { maskColor: skinColor('page_background') },
      systemMaterialEffect: {
        materialType: hdsMaterial.MaterialType.ADAPTIVE,
        materialLevel: hdsMaterial.MaterialLevel.ADAPTIVE
      },
      adaptToHandedness: false,
      thermoCtrl: true
    })
    .onChange((index: number): void => {
      const target = this.phoneFilterForIndex(index);
      if (target === null) return;
      this.phoneNavIndex = index;
      this.selectFilter(target, false);
    })
    .width('100%')
    .height('100%')
    .id('library-bottom-bar')
  }

  @Builder
  private PhoneLibraryNavigation() {
    HdsNavigation() {
      this.PhoneNavigation()
    }
    .mode(NavigationMode.Stack)
    .titleBar({
      padding: { start: LengthMetrics.vp(14), end: LengthMetrics.vp(14) },
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
              mainTitleColor: skinColor('text_primary'),
              subTitleColor: skinColor('text_secondary')
            },
            menuStyle: {
              backgroundColor: this.navigationControlBackground(),
              iconColor: skinColor('text_primary')
            },
            backIconStyle: {
              backgroundColor: this.navigationControlBackground(),
              iconColor: skinColor('text_primary')
            }
          }
        },
        scrollEffectStyle: {
          backgroundStyle: {
            backgroundColor: this.navigationTitleBackground(true)
          },
          contentStyle: {
            titleStyle: {
              mainTitleColor: skinColor('text_primary'),
              subTitleColor: skinColor('text_secondary')
            },
            menuStyle: {
              backgroundColor: this.navigationControlBackground(),
              iconColor: skinColor('text_primary')
            },
            backIconStyle: {
              backgroundColor: this.navigationControlBackground(),
              iconColor: skinColor('text_primary')
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
          mainTitle: '旧柚Pro',
          subTitle: this.filterTitle()
        },
        menu: this.phoneNavigationMenu(),
        divider: { showType: DividerShowType.OFF }
      },
      avoidLayoutSafeArea: true,
    })
    .titleMode(HdsNavigationTitleMode.MINI)
    .hideBackButton(true)
    .hideToolBar(true)
    .systemBarStyle(
      { statusBarContentColor: this.navigationTitleColor() },
      { statusBarContentColor: this.navigationTitleColor() }
    )
    .width('100%')
    .height('100%')
    .backgroundColor('#00FFFFFF')
    .bindToScrollable(this.phoneNavigationScrollers())
    .expandSafeArea([SafeAreaType.SYSTEM, SafeAreaType.CUTOUT],
      [SafeAreaEdge.TOP, SafeAreaEdge.BOTTOM, SafeAreaEdge.START, SafeAreaEdge.END])
    .id('library-phone-navigation')
  }

  private consoleNavIndexForFilter(value: CatalogFilter): number {
    if (value === 'recent') return 0;
    if (value === 'favorite') return 1;
    if (value === 'windows') return 2;
    if (value === 'running') return 3;
    return 4;
  }

  private consoleFilterForNavIndex(index: number): CatalogFilter {
    if (index === 0) return 'recent';
    if (index === 1) return 'favorite';
    if (index === 2) return 'windows';
    if (index === 3) return 'running';
    return 'builtin';
  }

  onBackPress(): boolean {
    if (this.consoleDetailVisible) {
      this.consoleDetailVisible = false;
      return true;
    }
    return false;
  }

  private focusConsoleLauncher(): void {
    if (!this.consolePageVisible || this.settings.uiSkin !== UiSkin.CONSOLE ||
      this.consoleDetailVisible || this.consoleSearchRequested) return;
    const focused = focusControl.requestFocus('console-launcher');
    hilog.info(DOMAIN, 'ConsoleFocus', 'launcher focus=%{public}s', focused.toString());
    if (focused) {
      this.consoleSearchActive = false;
      try { inputMethod.getController().stopInputSession(); } catch (_) {}
    }
  }

  private consoleApps(): AppDescriptor[] {
    return this.consoleOrderedApps;
  }

  private orderedConsoleApps(): AppDescriptor[] {
    const result = this.visibleApps.slice();
    const store = AppSettingsStore.getInstance();
    if (this.consoleSort === SortMode.NAME) {
      result.sort((a: AppDescriptor, b: AppDescriptor): number => a.title.localeCompare(b.title));
    } else if (this.consoleSort === SortMode.PLAY_TIME) {
      result.sort((a: AppDescriptor, b: AppDescriptor): number =>
        store.getPlayMinutes(b.id) - store.getPlayMinutes(a.id));
    } else {
      result.sort((a: AppDescriptor, b: AppDescriptor): number =>
        store.getLastPlayedAt(b.id) - store.getLastPlayedAt(a.id));
    }
    return result;
  }

  private syncConsoleFocus(): void {
    const apps = this.orderedConsoleApps();
    const changed = !this.sameAppSequence(this.consoleOrderedApps, apps);
    if (changed) {
      this.consoleOrderedApps = apps;
      this.consoleRingData.setApps(apps);
    }
    if (apps.length === 0) {
      if (this.consoleFocusIndex !== 0) this.consoleFocusIndex = 0;
      if (this.consoleSelectedAppId) this.consoleSelectedAppId = '';
      this.consoleRingAnchorAppId = '';
      return;
    }
    const selected = apps.findIndex((app: AppDescriptor): boolean => app.id === this.consoleSelectedAppId);
    const next = selected >= 0 ? selected : 0;
    if (this.consoleFocusIndex !== next) this.consoleFocusIndex = next;
    if (this.consoleSelectedAppId !== apps[next].id) this.consoleSelectedAppId = apps[next].id;
    this.consoleRingAnchorAppId = apps[next].id;
    if (changed) this.locateConsoleRing(next);
  }

  private preferGamesForConsole(): void {
    if (this.settings.uiSkin !== UiSkin.CONSOLE || this.consoleCategoryInitialized) return;
    if (this.apps.some((app: AppDescriptor): boolean => app.source === AppSource.DOWNLOAD)) {
      this.selectFilter('windows', false);
    }
  }

  private consoleFocusApp(): AppDescriptor | null {
    const apps = this.consoleApps();
    if (apps.length === 0) return null;
    return apps[Math.max(0, Math.min(this.consoleFocusIndex, apps.length - 1))];
  }

  private consoleBackground(app: AppDescriptor): string {
    if (this.artworkRevision < 0) return '';
    const cached = this.consoleBackgroundCache.get(app.id);
    if (cached !== undefined) return cached;
    const path = GameArtworkService.getInstance().resolve(app);
    this.consoleBackgroundCache.set(app.id, path);
    return path;
  }

  private consoleHasArtworkBackground(): boolean {
    const app = this.consoleFocusApp();
    return !!app && (this.consoleBackground(app).length > 0 || app.coverPath.length > 0);
  }

  private consoleContentWidth(): number {
    return Math.max(180, Math.min(1600, this.viewportWidth - 32));
  }

  private consoleCompactLandscape(): boolean {
    return this.viewportWidth > this.viewportHeight && this.viewportHeight < 650;
  }

  private consoleImmersiveHeader(): boolean {
    return this.consoleCompactLandscape() ||
      this.settings.consoleCardLayout === ConsoleCardLayout.FOCUS_3D;
  }

  private consoleGridColumns(tile: boolean): number {
    const minimum = tile ? 152 : 220;
    const maxColumns = tile ? 6 : (this.viewportHeight > this.viewportWidth ? 2 : 4);
    return Math.max(1, Math.min(maxColumns,
      Math.floor((this.consoleContentWidth() + 12) / (minimum + 12))));
  }

  private consoleGridTemplate(tile: boolean): string {
    const count = this.consoleGridColumns(tile);
    const parts: string[] = [];
    for (let index = 0; index < count; index++) parts.push('1fr');
    return parts.join(' ');
  }

  private consoleGridCardWidth(tile: boolean): number {
    const count = this.consoleGridColumns(tile);
    return Math.max(140, (this.consoleContentWidth() - (count - 1) * 12) / count - 2);
  }

  private consoleRingVisibleCount(): number {
    const width = this.consoleFocusAreaWidth || this.consoleContentWidth();
    const portrait = this.viewportHeight > this.viewportWidth;
    // ArkUI measures these values in vp, so the physical card remains
    // comfortable across densities. Fractional counts reveal the next card
    // while breakpoints reserve more width for the focused card.
    if (portrait) {
      if (width < 480) return 1.58;
      if (width < 700) return 2.08;
      if (width < 900) return 2.30;
      return 2.70;
    }
    if (width < 600) return 1.72;
    if (width < 900) return 2.65;
    if (width < 1280) return 5.05;
    if (width < 1700) return 4.80;
    return 5.80;
  }

  private consoleRingCellWidth(): number {
    const width = this.consoleFocusAreaWidth || this.consoleContentWidth();
    return width / this.consoleRingVisibleCount();
  }

  private consoleRingLeadingInset(): number {
    if (this.consoleApps().length > 2) return 0;
    const width = this.consoleFocusAreaWidth || this.consoleContentWidth();
    return Math.max(0, (width - this.consoleRingCellWidth()) / 2);
  }

  private consoleRingInitialIndex(realIndex: number = 0): number {
    const count = this.consoleApps().length;
    if (count <= 2) return Math.max(0, Math.min(count - 1, realIndex));
    const middle = Math.floor(ConsoleRingDataSource.VIRTUAL_TOTAL / 2);
    const base = middle - (middle % count);
    return base + Math.max(0, Math.min(count - 1, realIndex));
  }

  private consoleRingCenterVirtualIndex(offset: number = this.consoleRingOffset): number {
    const cell = this.consoleRingCellWidth();
    const inset = this.consoleRingLeadingInset();
    const viewport = this.consoleFocusAreaWidth || this.consoleContentWidth();
    return Math.round((offset + viewport / 2 - inset - cell / 2) / cell);
  }

  private consoleRingOffsetForIndex(index: number): number {
    return Math.max(0, this.consoleRingLeadingInset() + index * this.consoleRingCellWidth() +
      this.consoleRingCellWidth() / 2 -
      (this.consoleFocusAreaWidth || this.consoleContentWidth()) / 2);
  }

  private currentConsoleRingOffset(fallback: number): number {
    // A lazy List estimates the size of unbuilt items. Its absolute offset can
    // differ from index * cellWidth after jumps, resizing or reversed flings.
    // Anchor the 3D transform to an actual visible ListItem instead.
    try {
      const index = this.consoleRingMeasuredIndex;
      const rect = this.consoleRingScroller.getItemRect(index);
      if (Number.isFinite(rect.x) && rect.width > 0) {
        return this.consoleRingLeadingInset() + index * this.consoleRingCellWidth() - rect.x;
      }
    } catch (_) {}
    return fallback;
  }

  private consoleRingDelta(index: number): number {
    const cell = this.consoleRingCellWidth();
    const inset = this.consoleRingLeadingInset();
    const viewport = this.consoleFocusAreaWidth || this.consoleContentWidth();
    const center = inset + index * cell + cell / 2 - this.consoleRingOffset;
    return (center - viewport / 2) / cell;
  }

  private syncConsoleRingFocus(index: number, playSound: boolean = false,
    commitAnchor: boolean = false): void {
    const apps = this.consoleApps();
    if (apps.length === 0) return;
    const total = this.consoleRingData.totalCount();
    const safe = Math.max(0, Math.min(total - 1, index));
    const real = ((safe % apps.length) + apps.length) % apps.length;
    this.consoleRingVirtualIndex = safe;
    if (commitAnchor) this.consoleRingAnchorAppId = apps[real].id;
    if (real === this.consoleFocusIndex && this.consoleSelectedAppId === apps[real].id) return;
    this.consoleFocusIndex = real;
    this.consoleSelectedAppId = apps[real].id;
    if (playSound) ConsoleUiSound.getInstance().focus();
    GameArtworkService.getInstance().enqueueIfMissing(apps[real]);
  }

  private locateConsoleRing(realIndex: number = this.consoleFocusIndex): void {
    const count = this.consoleRingData.totalCount();
    if (count === 0 || this.settings.consoleCardLayout !== ConsoleCardLayout.FOCUS_3D) return;
    const target = this.consoleRingInitialIndex(realIndex);
    const targetAppId = this.consoleApps()[realIndex]?.id ?? '';
    const generation = ++this.consoleRingLocateGeneration;
    this.consoleRingLocating = true;
    this.consoleRingMotionArmed = false;
    let attempts = 0;
    const locate = (): void => {
      if (generation !== this.consoleRingLocateGeneration) return;
      attempts++;
      try { this.consoleRingScroller.scrollToIndex(target, false, ScrollAlign.CENTER); } catch (_) {}
      const expected = this.consoleRingOffsetForIndex(target);
      // Use the target while native layout is pending, then measure the item
      // before allowing gestures against the new viewport geometry.
      this.consoleRingOffset = expected;
      this.syncConsoleRingFocus(target, false, true);
      if (targetAppId) this.consoleRingAnchorAppId = targetAppId;
      if (attempts >= 5) {
        // Keep callbacks isolated until the native layout transaction lands.
        setTimeout((): void => {
          if (generation !== this.consoleRingLocateGeneration) return;
          this.consoleRingMeasuredIndex = target;
          this.consoleRingOffset = this.currentConsoleRingOffset(this.consoleRingOffsetForIndex(target));
          this.syncConsoleRingFocus(target, false, true);
          this.consoleRingLocating = false;
        }, 100);
        return;
      }
      setTimeout(locate, 70);
    };
    setTimeout(locate, 20);
  }

  private suspendConsoleRingForPageTransition(): void {
    if (this.settings.uiSkin !== UiSkin.CONSOLE ||
      this.settings.consoleCardLayout !== ConsoleCardLayout.FOCUS_3D) return;
    const total = this.consoleRingData.totalCount();
    if (total === 0) return;
    const target = Math.max(0, Math.min(total - 1, this.consoleRingCenterVirtualIndex()));
    // A non-animated jump cancels the native List fling before another page
    // covers this one. Keep callbacks suppressed until onPageShow performs a
    // fresh locate with the current viewport geometry.
    this.consoleRingLocateGeneration++;
    this.consoleRingRelayoutGeneration++;
    this.consoleRingLocating = true;
    this.consoleRingMotionArmed = false;
    this.syncConsoleRingFocus(target, false, true);
    try { this.consoleRingScroller.scrollToIndex(target, false, ScrollAlign.CENTER); } catch (_) {}
    this.consoleRingOffset = this.consoleRingOffsetForIndex(target);
  }

  private beginConsoleRingRelayout(): void {
    if (this.settings.uiSkin !== UiSkin.CONSOLE ||
      this.settings.consoleCardLayout !== ConsoleCardLayout.FOCUS_3D) return;
    const current = this.consoleFocusApp();
    if (!this.consoleRingLocating) {
      if (current) this.consoleRingAnchorAppId = current.id;
      // Stop an in-flight native fling before the List changes its item width.
      // Otherwise its old pixel velocity can advance cards after relocation.
      try {
        this.consoleRingScroller.scrollToIndex(this.consoleRingVirtualIndex, false,
          ScrollAlign.CENTER);
      } catch (_) {}
    }
    this.consoleRingLocateGeneration++;
    this.consoleRingLocating = true;
    this.consoleRingMotionArmed = false;
  }

  private scheduleConsoleRingRelayout(): void {
    this.beginConsoleRingRelayout();
    const generation = ++this.consoleRingRelayoutGeneration;
    setTimeout((): void => {
      if (generation !== this.consoleRingRelayoutGeneration ||
        this.settings.consoleCardLayout !== ConsoleCardLayout.FOCUS_3D) return;
      // LazyForEach keeps the old ListItem width across orientation changes.
      // Reload after the new area settles so item width and 3D math use one coordinate system.
      this.consoleRingLayoutRevision++;
      this.consoleRingData.setApps(this.consoleApps());
      const apps = this.consoleApps();
      const anchored = apps.findIndex((app: AppDescriptor): boolean =>
        app.id === this.consoleRingAnchorAppId);
      this.locateConsoleRing(anchored >= 0 ? anchored : this.consoleFocusIndex);
    }, 160);
  }

  private selectConsoleRingIndex(index: number, animated: boolean): void {
    const total = this.consoleRingData.totalCount();
    if (total === 0) return;
    const safe = Math.max(0, Math.min(total - 1, index));
    if (animated) this.consoleRingMotionArmed = true;
    try { this.consoleRingScroller.scrollToIndex(safe, animated, ScrollAlign.CENTER); } catch (_) {}
    if (!animated) this.syncConsoleRingFocus(safe);
  }

  private finishConsoleRingScroll(): void {
    if (!this.consolePageVisible || this.consoleRingLocating) return;
    this.consoleRingOffset = this.currentConsoleRingOffset(this.consoleRingOffset);
    this.consoleRingMotionArmed = false;
    this.syncConsoleRingFocus(this.consoleRingCenterVirtualIndex(), false, true);
    hilog.debug(DOMAIN, 'ConsoleRing', 'settled index=%{public}d residual=%{public}s',
      this.consoleRingVirtualIndex, this.consoleRingDelta(this.consoleRingVirtualIndex).toFixed(4));
    // Native CENTER snapping owns settling. Never start another scroll from
    // onScrollStop: an interrupted snap would otherwise feed back into itself.
  }

  private focusConsoleStep(step: number): void {
    const apps = this.consoleApps();
    if (apps.length === 0) return;
    if (this.settings.consoleCardLayout === ConsoleCardLayout.FOCUS_3D) {
      const direction = step > 0 ? 1 : -1;
      const target = this.consoleRingVirtualIndex + direction;
      this.syncConsoleRingFocus(target, true, true);
      this.selectConsoleRingIndex(target, true);
      return;
    }
    const next = Math.max(0, Math.min(apps.length - 1, this.consoleFocusIndex + step));
    if (next === this.consoleFocusIndex) return;
    this.consoleFocusIndex = next;
    this.consoleSelectedAppId = apps[next].id;
    ConsoleUiSound.getInstance().focus();
    GameArtworkService.getInstance().enqueueIfMissing(apps[next]);
    this.scrollConsoleGridSelection(next, true);
  }

  private scrollConsoleGridSelection(index: number = this.consoleFocusIndex,
    animated: boolean = true, delayMs: number = 0): void {
    if (this.settings.consoleCardLayout === ConsoleCardLayout.FOCUS_3D) return;
    const count = this.consoleApps().length;
    if (count === 0) return;
    const target = Math.max(0, Math.min(count - 1, index));
    setTimeout((): void => {
      if (!this.consolePageVisible ||
        this.settings.consoleCardLayout === ConsoleCardLayout.FOCUS_3D) return;
      try {
        // Center alignment keeps the highlighted card clear of the floating
        // navigation bar, including the incomplete final row.
        this.consoleGridScroller.scrollToIndex(target, animated, ScrollAlign.CENTER);
      } catch (_) {
      }
    }, delayMs);
  }

  private focusConsoleCategoryStep(step: number): void {
    const count = 5;
    const next = ((this.consoleNavIndex + step) % count + count) % count;
    const target = this.consoleFilterForNavIndex(next);
    ConsoleUiSound.getInstance().focus();
    this.consoleNavIndex = next;
    this.consoleNavController.changeIndex(next);
    if (target !== this.filter) this.selectFilter(target, false);
  }

  private resetConsoleRingMotion(): void {
    this.consoleRingLocateGeneration++;
    this.consoleRingRelayoutGeneration++;
    this.consoleRingLocating = false;
    this.consoleRingMotionArmed = false;
    this.consoleRingMeasuredIndex = 0;
    this.consoleRingAnchorAppId = '';
    this.consoleRingOffset = 0;
    this.consoleRingVirtualIndex = 0;
  }

  private focusConsoleApp(app: AppDescriptor): void {
    const index = this.consoleApps().findIndex((item: AppDescriptor): boolean => item.id === app.id);
    if (index === this.consoleFocusIndex) return;
    const count = this.consoleApps().length;
    if (this.settings.consoleCardLayout === ConsoleCardLayout.FOCUS_3D && count > 1) {
      let delta = index - this.consoleFocusIndex;
      if (delta > count / 2) delta -= count;
      if (delta < -count / 2) delta += count;
      const target = this.consoleRingVirtualIndex + delta;
      this.syncConsoleRingFocus(target, false, true);
      this.selectConsoleRingIndex(target, true);
      ConsoleUiSound.getInstance().focus();
      return;
    }
    if (index >= 0) this.consoleFocusIndex = index;
    this.consoleSelectedAppId = app.id;
    ConsoleUiSound.getInstance().focus();
    GameArtworkService.getInstance().enqueueIfMissing(app);
  }

  private consoleActivate(app: AppDescriptor): void {
    ConsoleUiSound.getInstance().select();
    if (this.runningAppIds.has(app.id)) {
      AppSessionService.getInstance().stopApp(app);
      this.refreshRunningState();
    } else {
      void this.launch(app);
    }
  }

  private showConsoleDetail(app: AppDescriptor): void {
    if (this.settings.uiSkin === UiSkin.CONSOLE) {
      ConsoleUiSound.getInstance().select();
      this.focusConsoleApp(app);
    }
    this.consoleDetailApp = app;
    this.consoleDetailVisible = true;
  }

  private syncConsoleNavigation(): void {
    if (!this.consolePageVisible || this.settings.uiSkin !== UiSkin.CONSOLE) {
      this.detachConsoleNavigation();
      return;
    }
    if (!this.consoleNavigationListener) {
      this.consoleNavigationListener = (code: number): boolean => this.handleConsoleHardware(code);
    }
    if (!this.consoleNavigationActive) {
      GamepadManager.getInstance().subscribeNavigation(this.consoleNavigationListener);
      this.consoleNavigationActive = true;
    }
  }

  private detachConsoleNavigation(): void {
    if (this.consoleNavigationActive && this.consoleNavigationListener) {
      GamepadManager.getInstance().unsubscribeNavigation(this.consoleNavigationListener);
    }
    this.consoleNavigationActive = false;
  }

  private handleConsoleHardware(code: number, fromKeyboard: boolean = false): boolean {
    if (this.settings.uiSkin !== UiSkin.CONSOLE || !this.libraryChromeVisible()) return false;
    if (code === 2070 || code === 2302) {
      if (this.consoleDetailVisible) {
        this.consoleDetailVisible = false;
        return true;
      }
      return false;
    }
    if (this.consoleDetailVisible) {
      if ((code === 2054 || code === 2016 || code === 2050 || code === 2301) &&
        this.consoleDetailApp) {
        const app = this.consoleDetailApp;
        this.consoleDetailVisible = false;
        this.consoleActivate(app);
        return true;
      }
      return false;
    }
    if (fromKeyboard && this.consoleSearchActive) return false;
    if (code === 2307 || code === 2308) {
      this.focusConsoleCategoryStep(code === 2307 ? -1 : 1);
      return true;
    }
    const now = Date.now();
    const direction = code === 2014 || code === 2012 ? -1 :
      (code === 2015 || code === 2013 ? 1 : 0);
    if (direction !== 0) {
      if (now - this.consoleLastNavigationAt < 75) return true;
      this.consoleLastNavigationAt = now;
      const columns = this.consoleGridColumns(this.settings.consoleCardLayout === ConsoleCardLayout.XBOX_TILES);
      const vertical = code === 2012 || code === 2013;
      this.focusConsoleStep(this.settings.consoleCardLayout === ConsoleCardLayout.FOCUS_3D ?
        direction : direction * (vertical ? columns : 1));
      return true;
    }
    const focused = this.consoleFocusApp();
    if (!focused) return false;
    if (code === 2054 || code === 2016 || code === 2050 || code === 2301) {
      this.showConsoleDetail(focused);
      return true;
    }
    if (code === 2025 || code === 2304 || code === 2305) {
      this.showConsoleDetail(focused);
      return true;
    }
    return false;
  }

  private toggleConsoleFavorite(app: AppDescriptor): void {
    const store = AppSettingsStore.getInstance();
    const next = cloneAppSettings(store.getAppSettings(app.id));
    next.favorite = !next.favorite;
    void store.saveAppSettings(next).then((): void => {
      this.apps = this.apps.slice();
      this.refreshRunningState();
    });
  }

  private async scrapeConsoleCover(app: AppDescriptor): Promise<void> {
    if (app.source !== AppSource.DOWNLOAD || !app.directory) return;
    const store = AppSettingsStore.getInstance();
    const key = store.getSgdbApiKey();
    if (store.getAppSettings(app.id).customCover) {
      promptAction.showToast({ message: '已有手动封面；可在游戏设置中更换' });
      return;
    }
    promptAction.showToast({ message: '正在查找封面…' });
    const result = await MetadataScraper.getInstance().scrapeCover(key, app.title, app.directory);
    promptAction.showToast({ message: result.ok ? '封面已更新' : result.detail });
    if (result.ok) await this.rescan();
  }

  private consoleLastPlayed(appId: string): string {
    const at = AppSettingsStore.getInstance().getLastPlayedAt(appId);
    return at > 0 ? '上次游玩 ' + new Date(at).toLocaleDateString() : '';
  }

  private consolePlayTime(appId: string): string {
    const minutes = AppSettingsStore.getInstance().getPlayMinutes(appId);
    if (minutes <= 0) return '';
    if (minutes < 60) return '累计 ' + Math.max(1, Math.floor(minutes)).toString() + ' 分钟';
    return '累计 ' + (minutes / 60).toFixed(1) + ' 小时';
  }

  private cycleConsoleSort(): void {
    const modes: SortMode[] = [SortMode.RECENT, SortMode.NAME, SortMode.PLAY_TIME];
    const next = (modes.indexOf(this.consoleSort) + 1) % modes.length;
    this.consoleSort = modes[next];
    this.consoleFocusIndex = 0;
    this.consoleSelectedAppId = '';
    this.syncConsoleFocus();
  }

  private consoleSortLabel(): string {
    if (this.consoleSort === SortMode.NAME) return '名称';
    if (this.consoleSort === SortMode.PLAY_TIME) return '时长';
    return '最近';
  }

  private consoleImageSource(path: string): string {
    return path.startsWith('file://') ? path : 'file://' + path;
  }

  @Builder
  private ConsoleNavButton(label: string, target: CatalogFilter, icon: Resource) {
    Button() {
      Row({ space: 10 }) {
        SymbolGlyph(icon).fontSize(17)
          .fontColor([this.filter === target ? skinColor('accent') : skinColor('text_secondary')])
        Text(label).fontSize(14)
          .fontColor(this.filter === target ? skinColor('text_primary') : skinColor('text_secondary'))
      }
      .width('100%')
    }
    .width('100%').height(46).borderRadius(14)
    .backgroundColor(this.filter === target ? skinColor('selected_background') : '#00000000')
    .onClick((): void => this.selectFilter(target, false))
  }

  @Builder
  private ConsoleSideBar() {
    Column({ space: 8 }) {
      Text('旧柚 Pro').fontSize(22).fontWeight(FontWeight.Bold)
        .fontColor(skinColor('text_primary')).width('100%').margin({ bottom: 22 })
      this.ConsoleNavButton('最近', 'recent', $r('sys.symbol.clock'))
      this.ConsoleNavButton('内建应用', 'builtin', $r('sys.symbol.square_grid_2x2'))
      this.ConsoleNavButton('Windows 程序', 'windows', $r('sys.symbol.gamecontroller'))
      this.ConsoleNavButton('收藏', 'favorite', $r('sys.symbol.star'))
      this.ConsoleNavButton(this.runningFilterLabel(), 'running', $r('sys.symbol.play_fill'))
      Blank()
      Button('系统设置').width('100%').height(44).borderRadius(14)
        .backgroundColor(skinColor('search_background')).fontColor(skinColor('text_primary'))
        .onClick((): void => this.openSidebarPage('pages/SystemSettings'))
    }
    .width(220).height('100%')
    .padding({ left: 16, right: 16, top: 24, bottom: 24 })
    .backgroundColor(skinColor('sidebar_background'))
    .id('console-sidebar')
  }

  @Builder
  private ConsoleBottomTab(label: string, icon: Resource, index: number) {
    Column({ space: 2 }) {
      SymbolGlyph(icon).fontSize(20)
        .fontColor([this.consoleNavIndex === index ? skinColor('accent') : skinColor('text_secondary')])
      Text(label).fontSize(10).maxLines(1)
        .fontWeight(this.consoleNavIndex === index ? FontWeight.Medium : FontWeight.Normal)
        .fontColor(this.consoleNavIndex === index ? skinColor('accent') : skinColor('text_secondary'))
    }
    .width('100%').height('100%')
    .justifyContent(FlexAlign.Center).alignItems(HorizontalAlign.Center)
  }

  @Builder
  private ConsoleBottomNav() {
    HdsTabs({
      barPosition: BarPosition.End,
      index: this.consoleNavIndex,
      controller: this.consoleNavController
    }) {
      TabContent() { Column() }.tabBar(this.ConsoleBottomTab('最近', $r('sys.symbol.clock'), 0))
      TabContent() { Column() }.tabBar(this.ConsoleBottomTab('收藏', $r('sys.symbol.star'), 1))
      TabContent() { Column() }.tabBar(this.ConsoleBottomTab('Game', $r('sys.symbol.gamecontroller'), 2))
      TabContent() { Column() }.tabBar(this.ConsoleBottomTab(this.runningFilterLabel(), $r('sys.symbol.play_fill'), 3))
      TabContent() { Column() }.tabBar(this.ConsoleBottomTab('Home', $r('sys.symbol.house_fill'), 4))
    }
    .vertical(false)
    .barHeight(bottomNavigationLayout(this.viewportWidth > this.viewportHeight,
      this.navBarInsetVp).barHeight)
    .scrollable(false)
    .animationDuration(250)
    .barOverlap(true)
    .divider({ mode: DividerMode.FOLLOW_SCROLL })
    .barBackgroundBlurStyle(BlurStyle.COMPONENT_THICK)
    .barFloatingStyle({
      barBottomMargin: bottomNavigationLayout(this.viewportWidth > this.viewportHeight,
        this.navBarInsetVp).bottomMargin,
      barSideMargin: bottomNavigationLayout(this.viewportWidth > this.viewportHeight,
        this.navBarInsetVp).sideMargin,
      // Keep only the floating material bar visible. The launcher content is
      // drawn behind it, so an opaque gradient backplate would look like a
      // full-width footer and cut the cards off before the window edge.
      gradientMask: { maskColor: '#00000000', maskHeight: 0 },
      systemMaterialEffect: {
        materialType: hdsMaterial.MaterialType.ADAPTIVE,
        materialLevel: hdsMaterial.MaterialLevel.ADAPTIVE
      },
      adaptToHandedness: false,
      thermoCtrl: true
    })
    .onChange((index: number): void => {
      const target = this.consoleFilterForNavIndex(index);
      this.consoleNavIndex = index;
      if (target !== this.filter) this.selectFilter(target, false);
    })
    .width('100%')
    .height(bottomNavigationLayout(this.viewportWidth > this.viewportHeight,
      this.navBarInsetVp).containerHeight)
    .id('console-bottom-nav')
  }

  @Builder
  private ConsoleHeader() {
    Row({ space: 10 }) {
      if (this.consoleContentWidth() >= (this.consoleCompactLandscape() ? 620 : 520)) {
        Column({ space: 2 }) {
          Text('游戏库').fontSize(20).fontWeight(FontWeight.Bold)
            .fontColor(skinColor('text_primary'))
          if (!this.consoleCompactLandscape()) {
            Text(this.filterTitle()).fontSize(11).fontColor(skinColor('text_secondary'))
          }
        }.width(this.consoleCompactLandscape() ? 74 : 100)
          .alignItems(HorizontalAlign.Start)
      }
      TextInput({ placeholder: '搜索游戏…', text: this.searchQuery })
        .height(this.consoleCompactLandscape() ? 38 : 42).borderRadius(20)
        .layoutWeight(1)
        .constraintSize({ minWidth: 120, maxWidth: this.consoleCompactLandscape() ? 320 : 560 })
        .backgroundColor(skinColor('search_background'))
        .fontColor(skinColor('text_primary'))
        .enableKeyboardOnFocus(false)
        .onChange((value: string): void => this.updateSearch(value))
        .onTouch((event: TouchEvent): void => {
          if (event.type === TouchType.Down) this.consoleSearchRequested = true;
        })
        .onFocus((): void => {
          if (this.consoleSearchRequested) this.consoleSearchActive = true;
          else setTimeout((): void => this.focusConsoleLauncher(), 0);
        })
        .onBlur((): void => {
          this.consoleSearchActive = false;
          this.consoleSearchRequested = false;
        })
        .onClick((): void => {
          this.consoleSearchRequested = true;
          this.consoleSearchActive = true;
          focusControl.requestFocus('console-search');
          void inputMethod.getController().showTextInput().catch((): void => {});
        })
        .id('console-search')
      Button(this.consoleSortLabel()).height(40).borderRadius(20)
        .padding({ left: 10, right: 10 })
        .backgroundColor(skinColor('search_background')).fontColor(skinColor('text_primary'))
        .onClick((): void => this.cycleConsoleSort())
      Button({ type: ButtonType.Circle }) {
        SymbolGlyph($r('sys.symbol.arrow_clockwise')).fontSize(18)
          .fontColor([skinColor('text_primary')])
      }
      .width(40).height(40).backgroundColor(skinColor('search_background'))
      .enabled(!this.scanning)
      .onClick((): void => { void this.rescan(); })
      .accessibilityText('刷新游戏库')
      .id('console-refresh')
      Button({ type: ButtonType.Circle }) {
        SymbolGlyph($r('sys.symbol.gearshape')).fontSize(18)
          .fontColor([skinColor('text_primary')])
      }
      .width(40).height(40).backgroundColor(skinColor('search_background'))
      .onClick((): void => this.openSidebarPage('pages/SystemSettings'))
    }
    .width(Math.min(1100, this.consoleContentWidth()))
    .justifyContent(this.consoleCompactLandscape() ? FlexAlign.SpaceBetween : FlexAlign.Start)
    .padding({ left: this.consoleImmersiveHeader() ? 0 : 14,
      right: this.consoleImmersiveHeader() ? 0 : 14,
      top: this.consoleImmersiveHeader() ? 0 : 12,
      bottom: this.consoleImmersiveHeader() ? 0 : 12 })
    .borderRadius(this.consoleImmersiveHeader() ? 0 : 26)
    .backgroundColor(this.consoleImmersiveHeader() ? '#00000000' : skinColor('card_glass_background'))
    .backgroundBlurStyle(this.consoleImmersiveHeader() ? BlurStyle.NONE : BlurStyle.COMPONENT_ULTRA_THICK)
    .border({ width: this.consoleImmersiveHeader() ? 0 : 1, color: '#44FFFFFF' })
    .shadow({ radius: this.consoleImmersiveHeader() ? 0 : 22,
      color: '#44000000', offsetX: 0, offsetY: this.consoleImmersiveHeader() ? 0 : 5 })
    .margin({ top: this.consoleImmersiveHeader() ? 4 : 12,
      bottom: this.consoleImmersiveHeader() ? 2 : 8 })
    .id('console-header')
  }

  @Builder
  private ConsoleRingList() {
      List({ space: 0, scroller: this.consoleRingScroller }) {
        LazyForEach(this.consoleRingData, (app: AppDescriptor, index: number): void => {
          ListItem() {
            Column() {
              ConsoleRingItem({
                app: app,
                virtualIndex: index,
                scrollOffset: $consoleRingOffset,
                cellWidth: this.consoleRingCellWidth(),
                viewportWidth: this.consoleFocusAreaWidth || this.consoleContentWidth(),
                leadingInset: this.consoleRingLeadingInset(),
                areaHeight: this.consoleFocusAreaHeight,
                viewportHeight: this.viewportHeight,
                artworkRevision: $artworkRevision,
                running: this.runningAppIds.has(app.id),
                favorite: AppSettingsStore.getInstance().getAppSettings(app.id).favorite,
                lastPlayed: this.consoleLastPlayed(app.id),
                onSelect: (_target: AppDescriptor): void => {
                  if (Date.now() >= this.consoleIgnoreCardClickUntil) {
                    this.selectConsoleRingIndex(index, true);
                  }
                },
                onLaunch: (target: AppDescriptor): void => {
                  if (Date.now() >= this.consoleIgnoreCardClickUntil) this.showConsoleDetail(target);
                },
                onDetails: (target: AppDescriptor): void => this.showConsoleDetail(target),
                onVisible: (target: AppDescriptor): void =>
                  GameArtworkService.getInstance().enqueueIfMissing(target)
              })
            }
            .width(this.consoleRingCellWidth()).height('100%')
            .justifyContent(FlexAlign.Center).alignItems(HorizontalAlign.Center)
          }
          .width(this.consoleRingCellWidth()).height('100%')
          .zIndex(100 - Math.min(90, Math.abs(this.consoleRingDelta(index)) * 10))
        }, (app: AppDescriptor, index: number): string =>
          `${app.id}#${index}#${this.consoleRingLayoutRevision}`)
      }
      .listDirection(Axis.Horizontal)
      .scrollBar(BarState.Off)
      .cachedCount(3)
      .edgeEffect(EdgeEffect.None)
      .scrollSnapAlign(ScrollSnapAlign.CENTER)
      .enableScrollInteraction(!this.consoleRingLocating)
      .contentStartOffset(this.consoleRingLeadingInset())
      .contentEndOffset(this.consoleRingLeadingInset())
      .width('100%').layoutWeight(1).clip(true)
      .onTouch((event: TouchEvent): void => {
        if (event.type === TouchType.Down && !this.consoleRingLocating) {
          this.consoleRingMotionArmed = true;
          this.consoleRingOffset = this.currentConsoleRingOffset(this.consoleRingOffset);
        }
      })
      .onScrollIndex((_start: number, _end: number, center: number): void => {
        this.consoleRingMeasuredIndex = center;
      })
      .onDidScroll((_scrollOffset: number, _scrollState: ScrollState): void => {
        if (!this.consolePageVisible || this.consoleRingLocating) return;
        this.consoleIgnoreCardClickUntil = Date.now() + 180;
        this.consoleRingOffset = this.currentConsoleRingOffset(this.consoleRingOffset);
        this.syncConsoleRingFocus(this.consoleRingCenterVirtualIndex(), this.consoleRingMotionArmed, true);
      })
      .onScrollStop((): void => this.finishConsoleRingScroll())
      .onAreaChange((_oldValue: Area, area: Area): void => {
        const width = area.width as number;
        const height = area.height as number;
        let changed = false;
        if (Math.abs(width - this.consoleFocusAreaWidth) > 1) {
          this.consoleFocusAreaWidth = width;
          changed = true;
        }
        if (Math.abs(height - this.consoleFocusAreaHeight) > 1) {
          this.consoleFocusAreaHeight = height;
          changed = true;
        }
        if (changed) this.scheduleConsoleRingRelayout();
      })
      .id('console-focus-flow')
  }

  @Builder
  private ConsoleRingCaption(compactLandscape: boolean) {
      Row({ space: 16 }) {
        Button('‹').width(44).height(44).borderRadius(22)
          .backgroundColor(skinColor('card_glass_background')).fontColor(skinColor('text_primary'))
          .onClick((): void => this.focusConsoleStep(-1))
        Column({ space: 4 }) {
          Text(this.consoleFocusApp()?.title ?? '')
            .fontSize(compactLandscape ? 18 : (this.consoleContentWidth() < 520 ? 21 : 30))
            .fontWeight(FontWeight.Bold).fontColor(skinColor('text_primary'))
            .maxLines(compactLandscape ? 2 : 1)
            .textAlign(TextAlign.Center).width('100%')
          Text(this.consoleFocusApp() ? this.consolePlayTime(this.consoleFocusApp()!.id) : '')
            .fontSize(12).fontColor(skinColor('text_secondary')).maxLines(1)
        }.layoutWeight(1).alignItems(HorizontalAlign.Center)
        Button('›').width(44).height(44).borderRadius(22)
          .backgroundColor(skinColor('card_glass_background')).fontColor(skinColor('text_primary'))
          .onClick((): void => this.focusConsoleStep(1))
      }
      .width(compactLandscape ? Math.min(300, this.consoleContentWidth() * 0.34) :
        Math.min(650, this.consoleContentWidth())).height(68)
      .justifyContent(FlexAlign.Center)
  }

  @Builder
  private ConsoleFocusFlow() {
    if (this.consoleCompactLandscape()) {
      Column() {
        this.ConsoleRingList()
      }
      .width('100%').layoutWeight(1)
      .padding({ top: 4,
        bottom: bottomNavigationLayout(true, this.navBarInsetVp).contentInset })
    } else {
      Column({ space: 4 }) {
        this.ConsoleRingList()
        this.ConsoleRingCaption(false)
      }
      .width('100%').layoutWeight(1).alignItems(HorizontalAlign.Center)
    // The focused title and its controls must stay readable above the
    // floating tab bar. This clearance is local to the focus presentation;
    // the launcher background and scrolling card layouts still extend to the
    // bottom edge behind the navigation material.
      .padding({
        top: 6,
        bottom: bottomNavigationLayout(this.viewportWidth > this.viewportHeight,
          this.navBarInsetVp).contentInset
      })
    }
  }

  @Builder
  private ConsoleTileHero(compactLandscape: boolean) {
    if (this.consoleFocusApp()) {
      ConsoleGameCard({
        app: this.consoleFocusApp()!,
        layout: ConsoleCardLayout.XBOX_TILES,
        backgroundPath: this.consoleBackground(this.consoleFocusApp()!),
        focused: true,
        running: this.runningAppIds.has(this.consoleFocusApp()!.id),
        favorite: AppSettingsStore.getInstance().getAppSettings(this.consoleFocusApp()!.id).favorite,
        cardWidth: compactLandscape ? this.consoleContentWidth() * 0.35 :
          Math.min(900, this.consoleContentWidth() - 24),
        cardHeight: compactLandscape ? Math.min(220, this.viewportHeight * 0.46) :
          Math.max(145, Math.min(200, this.viewportHeight * 0.20)),
        onLaunch: (app: AppDescriptor): void => this.showConsoleDetail(app),
        onDetails: (app: AppDescriptor): void => this.showConsoleDetail(app),
        onVisible: (app: AppDescriptor): void => GameArtworkService.getInstance().enqueueIfMissing(app)
      })
    }
  }

  private consoleCompactTileWidth(): number {
    return Math.max(130, (this.consoleContentWidth() * 0.63 - 44) / 3);
  }

  @Builder
  private ConsoleGridBody(tile: boolean, compactLandscape: boolean) {
    Grid(this.consoleGridScroller) {
      ForEach(this.consoleApps(), (app: AppDescriptor): void => {
        GridItem() {
          Column() {
            ConsoleGameCard({
              app: app,
              layout: tile ? ConsoleCardLayout.XBOX_TILES : ConsoleCardLayout.GALLERY,
              backgroundPath: this.consoleBackground(app),
              focused: this.consoleSelectedAppId === app.id,
              running: this.runningAppIds.has(app.id),
              favorite: AppSettingsStore.getInstance().getAppSettings(app.id).favorite,
              lastPlayed: this.consoleLastPlayed(app.id),
              cardWidth: compactLandscape ? this.consoleCompactTileWidth() : this.consoleGridCardWidth(tile),
              cardHeight: compactLandscape ? Math.min(132, this.consoleCompactTileWidth() * 0.60) :
                (tile ? Math.min(178, this.consoleGridCardWidth(tile) * 0.60) :
                  Math.min(420, this.consoleGridCardWidth(tile) * 1.15)),
              onSelect: (target: AppDescriptor): void => this.showConsoleDetail(target),
              onLaunch: (target: AppDescriptor): void => this.showConsoleDetail(target),
              onDetails: (target: AppDescriptor): void => this.showConsoleDetail(target),
              onVisible: (target: AppDescriptor): void => GameArtworkService.getInstance().enqueueIfMissing(target)
            })
          }.width('100%').alignItems(HorizontalAlign.Center)
        }
      }, (app: AppDescriptor): string => app.id)
    }
    .columnsTemplate(compactLandscape ? '1fr 1fr 1fr' : this.consoleGridTemplate(tile))
    .columnsGap(12).rowsGap(14).width('100%').layoutWeight(1)
    .padding({ left: compactLandscape ? 0 : 12, right: compactLandscape ? 0 : 12,
      top: compactLandscape ? 0 : 8, bottom: compactLandscape ? 0 : 112 })
    .scrollBar(BarState.Off)
    .focusable(true)
  }

  @Builder
  private ConsoleGrid(tile: boolean) {
    if (tile && this.consoleCompactLandscape()) {
      Row({ space: 12 }) {
        this.ConsoleTileHero(true)
        Column() {
          this.ConsoleGridBody(true, true)
        }.layoutWeight(1).height('100%')
      }
      .width('100%').layoutWeight(1).alignItems(VerticalAlign.Center)
      .padding({ left: 12, right: 12, top: 4,
        bottom: bottomNavigationLayout(true, this.navBarInsetVp).contentInset })
      .id('console-xbox-grid')
    } else {
      Column({ space: 10 }) {
        if (tile) this.ConsoleTileHero(false)
        this.ConsoleGridBody(tile, false)
      }
      .width('100%').layoutWeight(1).alignItems(HorizontalAlign.Center)
      .id(tile ? 'console-xbox-grid' : 'console-gallery-grid')
    }
  }

  @Builder
  private ConsoleContent() {
    if (this.scanning) {
      Column() {
        LoadingProgress().width(32).height(32)
      }.width('100%').layoutWeight(1).justifyContent(FlexAlign.Center)
    } else if (this.visibleApps.length === 0) {
      ForEach([this.filter], (category: CatalogFilter): void => {
        Column({ space: 14 }) {
          Text(this.emptyTitle(category)).fontSize(20).fontWeight(FontWeight.Bold)
            .fontColor(skinColor('text_primary'))
          Text(this.emptyDescription(category)).fontSize(13)
            .fontColor(skinColor('text_secondary')).textAlign(TextAlign.Center)
          Button('重新扫描').height(42).borderRadius(20)
            .backgroundColor(skinColor('accent')).fontColor('#FFFFFFFF')
            .onClick((): void => { void this.rescan(); })
        }.width('100%').layoutWeight(1).justifyContent(FlexAlign.Center)
      }, (category: CatalogFilter): string => category)
    } else if (this.settings.consoleCardLayout === ConsoleCardLayout.FOCUS_3D) {
      this.ConsoleFocusFlow()
    } else if (this.settings.consoleCardLayout === ConsoleCardLayout.GALLERY) {
      this.ConsoleGrid(false)
    } else {
      this.ConsoleGrid(true)
    }
  }

  @Builder
  private ConsoleDetailOverlay() {
    if (this.consoleDetailVisible && this.consoleDetailApp) {
      Stack() {
        if (this.consoleBackground(this.consoleDetailApp!).length > 0) {
          Image(this.consoleImageSource(this.consoleBackground(this.consoleDetailApp!)))
            .width('100%').height('100%').objectFit(ImageFit.Cover).blur(28).opacity(0.55)
        }
        Column().width('100%').height('100%').backgroundColor('#D90B0B13')
        GameDetailSheet({
          app: this.consoleDetailApp!,
          backgroundPath: this.consoleBackground(this.consoleDetailApp!),
          isRunning: this.runningAppIds.has(this.consoleDetailApp!.id),
          isFavorite: AppSettingsStore.getInstance().getAppSettings(this.consoleDetailApp!.id).favorite,
          lastPlayedText: this.consoleLastPlayed(this.consoleDetailApp!.id),
          playTimeText: this.consolePlayTime(this.consoleDetailApp!.id),
          compact: this.viewportWidth < 620,
          shortLandscape: this.consoleCompactLandscape(),
          topInset: Math.max(56, this.statusBarHeightVp),
          leftInset: this.cutoutLeftInsetVp,
          rightInset: this.cutoutRightInsetVp,
          callbacks: {
            onLaunch: (app: AppDescriptor): void => {
              this.consoleDetailVisible = false;
              this.consoleActivate(app);
            },
            onClose: (): void => { this.consoleDetailVisible = false; },
            onToggleFavorite: (app: AppDescriptor): void => this.toggleConsoleFavorite(app),
            onOpenSettings: (app: AppDescriptor): void => {
              this.consoleDetailVisible = false;
              this.openAppSettings(app);
            },
            onScrapeCover: (app: AppDescriptor): void => { void this.scrapeConsoleCover(app); }
          }
        })
      }
      .width('100%').height('100%')
      .backgroundColor(skinColor('page_background'))
      .parallelGesture(PanGesture({ direction: PanDirection.Horizontal })
        .onActionEnd((event: GestureEvent): void => {
          if (Math.abs(event.offsetX) > 90 && Math.abs(event.offsetX) > Math.abs(event.offsetY) * 1.2) {
            this.consoleDetailVisible = false;
          }
        }))
      .id('console-game-detail')
    }
  }

  @Builder
  private ConsoleLayout() {
    Stack({ alignContent: Alignment.Bottom }) {
      if (this.consoleHasArtworkBackground() && this.consoleFocusApp()) {
        if (this.consoleBackground(this.consoleFocusApp()!).length > 0) {
          Image(this.consoleImageSource(this.consoleBackground(this.consoleFocusApp()!)))
            .width('100%').height('100%').objectFit(ImageFit.Cover)
            .blur(32).opacity(0.55).scale({ x: 1.12, y: 1.12 })
            .hitTestBehavior(HitTestMode.None)
        } else if (this.consoleFocusApp()!.coverPath.length > 0) {
          Image(this.consoleImageSource(this.consoleFocusApp()!.coverPath))
            .width('100%').height('100%').objectFit(ImageFit.Cover)
            .blur(42).opacity(0.38).scale({ x: 1.12, y: 1.12 })
            .hitTestBehavior(HitTestMode.None)
        }
        Column().width('100%').height('100%').backgroundColor('#330A071A')
          .hitTestBehavior(HitTestMode.None)
      } else {
        Column().width('100%').height('100%')
          .linearGradient({ angle: 140, colors: [
            [skinColor('page_background'), 0],
            [skinColor('selected_background'), 0.58],
            [skinColor('page_background'), 1]
          ] })
          .hitTestBehavior(HitTestMode.None)
      }
      Column() {
        this.ConsoleHeader()
        this.ConsoleContent()
      }
      .width(this.consoleContentWidth()).height('100%')
      .alignItems(HorizontalAlign.Center)
      .padding({ top: Math.max(8, this.statusBarHeightVp) })
      Column() {
        this.ConsoleBottomNav()
      }
      .width('100%').height(bottomNavigationLayout(this.viewportWidth > this.viewportHeight,
        this.navBarInsetVp).containerHeight)
      .justifyContent(FlexAlign.End).alignItems(HorizontalAlign.Center)
    }
    .width('100%').height('100%')
    .clip(true)
    .backgroundColor(skinColor('page_background'))
    .focusable(true).defaultFocus(true)
    .onAppear((): void => {
      setTimeout((): void => this.focusConsoleLauncher(), 120);
    })
    .id('console-launcher')
  }

  @Builder
  private PageSurface() {
    Stack({ alignContent: Alignment.Bottom }) {
      if (this.settings.usePageBackground && this.settings.pageBackgroundUri.length > 0) {
        Image(this.pageBackgroundSource())
          .width('100%').height('100%')
          .objectFit(ImageFit.Cover)
          .opacity(0.72)
          .hitTestBehavior(HitTestMode.None)
          // 浮窗模式全屏态: 桌面铺满, 隐藏启动器背景
          .visibility(this.libraryChromeVisible() ? Visibility.Visible : Visibility.None)
          .id('library-page-background')
      }
      if (this.settings.uiSkin === UiSkin.CONSOLE) {
        Column() { this.ConsoleLayout() }
          .width('100%').height('100%')
          .visibility(this.libraryChromeVisible() ? Visibility.Visible : Visibility.None)
      } else {
        Row() {
          if (!this.isPhone()) {
            this.SideBar()
          }
          if (this.isPhone()) {
            this.PhoneLibraryNavigation()
          } else {
            this.Library()
          }
        }
        .width('100%')
        .height('100%')
        .padding({ top: this.isPhone() ? 0 : this.statusBarHeightVp + 16 })
        .visibility(this.libraryChromeVisible() ? Visibility.Visible : Visibility.None)
      }
      // 浮窗模式: 桌面由 Index 承载 — 悬浮窗态小窗缩略 / 全屏态铺满当前画面
      if (this.showDesktopLayer()) {
        Stack() {
          DesktopLayer({ floating: this.launcherVisible })
          if (this.launcherVisible) {
            // 悬浮窗态交互层: 全尺寸透明 Block, 轻点放大 / 按住拖动
            Stack() {
              Row({ space: 4 }) {
                Text('点击放大 · 按住拖动').fontSize(11).fontColor(Color.White)
              }
              .width('100%').justifyContent(FlexAlign.Center)
              .padding({ top: 3, bottom: 3 })
              .backgroundColor('#66000000')
              .position({ x: 0, y: this.floatingHeight() - 26 })
              .hitTestBehavior(HitTestMode.None)
            }
            .width('100%').height('100%')
            .onClick(() => { this.setLauncherVisible(false); })
            .gesture(
              PanGesture()
                .onActionStart(() => {
                  this.floatPanStartX = this.floatPosX;
                  this.floatPanStartY = this.floatPosY;
                })
                .onActionUpdate((ev: GestureEvent) => {
                  if (this.transitioning) return;
                  this.floatPosX = Math.min(Math.max(this.floatPanStartX + ev.offsetX, this.floatMinX()),
                    this.floatMaxX());
                  this.floatPosY = Math.min(Math.max(this.floatPanStartY + ev.offsetY, this.floatMinY()),
                    this.floatMaxY());
                })
            )
            .hitTestBehavior(HitTestMode.Block)
          }
        }
        .width(this.launcherVisible ? this.floatingWidth() : '100%')
        .height(this.launcherVisible ? this.floatingHeight() : '100%')
        .position(this.launcherVisible ? { x: this.floatPosX, y: this.floatPosY } : { x: 0, y: 0 })
        .borderRadius(this.launcherVisible ? 12 : 0)
        .clip(true)
        .border({ width: this.launcherVisible ? 1 : 0, color: '#66FFFFFF' })
        .shadow({ radius: this.launcherVisible ? 12 : 0, color: '#66000000', offsetX: 0, offsetY: this.launcherVisible ? 4 : 0 })
        .id('desktop-layer-container')
      }
      if (this.libraryChromeVisible()) {
        this.ConsoleDetailOverlay()
      }
      WineEngineBlockingOverlay()
    }
    .width('100%').height('100%')
    .backgroundColor(skinColor('page_background'))
    .onAreaChange((_oldValue: Area, area: Area): void => {
      const width = area.width as number;
      const height = area.height as number;
      const geometryChanged = Math.abs(width - this.viewportWidth) > 1 ||
        Math.abs(height - this.viewportHeight) > 1;
      if (geometryChanged) this.beginConsoleRingRelayout();
      this.viewportWidth = width;
      this.viewportHeight = height;
      if (this.viewportWidth > 0 && this.viewportHeight > 0) {
        this.screenW = this.viewportWidth;
        this.screenH = this.viewportHeight;
        if (this.launcherVisible && !this.transitioning) {
          this.ensureFloatPos();
        }
      }
      BreakpointSystem.updateFromWidth(this.viewportWidth);
      if (geometryChanged) {
        this.scheduleConsoleRingRelayout();
        this.scrollConsoleGridSelection(this.consoleFocusIndex, false, 180);
      }
    })
  }

  build() {
    Stack() {
      this.PageSurface()
    }
    .width('100%').height('100%')
    .focusable(true)
    .defaultFocus(true)
    .onKeyEvent((event: KeyEvent): void => {
      if (event.type === KeyType.Down && this.handleConsoleHardware(event.keyCode, true)) {
        event.stopPropagation();
      }
    })
  }
}
