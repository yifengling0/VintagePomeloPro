import { common, ConfigurationConstant } from '@kit.AbilityKit';
import router from '@ohos.router';
import promptAction from '@ohos.promptAction';
import { inputMethod } from '@kit.IMEKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';
import testNapi from 'libentry.so';
import { AppSearchBar } from '../components/SearchBar';
import { AppCard } from '../components/AppCard';
import { FileBrowserView } from '../components/FileBrowserView';
import { BreakpointSystem, BreakpointType, isWideBreakpoint } from '../common/BreakpointSystem';
import { bottomNavigationLayout } from '../model/BottomNavigationLayout';
import {
  AppDescriptor,
  AppSource,
  DesktopWindowMode,
  DisplayMode,
  EngineState,
  GlobalSettings,
  LaunchKind,
  RuntimeSession,
  ScreenOrientationSetting,
  ThemeMode,
  ViewMode,
  normalizeLaunchPath,
  resolveRunningAppIds
} from '../model/AppModels';
import { AppCatalogService } from '../service/AppCatalogService';
import { InputOverlayWindowCoordinator } from '../service/InputOverlayWindowCoordinator';
import { AppSessionService, LaunchConflictKind } from '../service/AppSessionService';
import { AppSettingsStore } from '../service/AppSettingsStore';
import { LauncherAdapterRegistry } from '../service/LauncherAdapterRegistry';
import { ThemeSettingsListener, ThemeSettingsService } from '../service/ThemeSettingsService';
import { WineEngineService } from '../service/WineEngineService';
import { WineWindowManager } from '../service/WineWindowManager';
import { InputRouter } from '../service/InputRouter';
import { EVDEV_KEY_ENTER } from '../common/KeyMap';
import { GamepadManager } from '../service/GamepadManager';
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
  @State private settings: GlobalSettings = AppSettingsStore.getInstance().getGlobalSettings();
  @State private engineState: EngineState = EngineState.STOPPED;
  @State private engineMessage: string = 'Wine 引擎未启动';
  @State private scanning: boolean = false;
  @State private refreshing: boolean = false;
  @State private runningAppIds: Set<string> = new Set<string>();
  /** 收藏标记是卡面上的可见状态, 随运行态一起刷新, 避免每张卡片各查一次设置。 */
  @State private favoriteIds: Set<string> = new Set<string>();
  @State private phoneNavIndex: number = PHONE_NAV_BUILTIN;
  @State private viewportWidth: number = 0;
  @State private viewportHeight: number = 0;
  @State private titleContentTopInsetVp: number = 40;
  @State private statusBarHeightVp: number = 0;
  @State private navBarInsetVp: number = 0;
  private context: common.UIAbilityContext = getContext(this) as common.UIAbilityContext;
  private phoneNavController: HdsTabsController = new HdsTabsController();
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
      // 库页只做横板: 从桌面退回库页时直接回到"仅横屏自动旋转",
      // 不再先转竖屏再放开 (横板 UI 下那一步只会闪一下竖屏)。
      DeviceCapabilityPolicy.applyLauncherLandscapeRotation(win, 'Index-float');
      await DeviceCapabilityPolicy.waitForRequestedOrientation(win, ScreenOrientationSetting.SENSOR_LANDSCAPE,
        'Index-float');
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
          testNapi.wineTextInputPreedit(text, 0, text.length);  // 预上屏 → Wine preedit
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
  }

  aboutToDisappear(): void {
    if (this.breakpointCallback) BreakpointSystem.unsubscribe(this.breakpointCallback);
    if (this.engineCallback) WineEngineService.getInstance().unsubscribe(this.engineCallback);
    if (this.settingsCallback) ThemeSettingsService.getInstance().unsubscribe(this.settingsCallback);
    if (this.refreshTimer >= 0) clearInterval(this.refreshTimer);
    this.stopWindowInsetObserver();
  }

  onPageShow(): void {
    this.settings = AppSettingsStore.getInstance().getGlobalSettings();
    if (this.catalogInitialized) {
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
      WineWindowManager.getInstance().setOutputScalePercent(settings.outputScale);
      GamepadManager.getInstance().setDeadzone(settings.gamepadDeadzone);
      GamepadManager.getInstance().setOutputMode(settings.controllerOutputMode ?? 'dinput');
    };
    ThemeSettingsService.getInstance().subscribe(this.settingsCallback);
    await AppCatalogService.getInstance().initialize(this.context);
    AppSessionService.getInstance().initialize(this.context);
    LauncherAdapterRegistry.getInstance().initialize(this.context);
    WineEngineService.getInstance().initialize(this.context);
    await InputProfileService.getInstance().initialize(this.context);
    GamepadManager.getInstance().initialize(this.settings.gamepadDeadzone,
      this.settings.controllerOutputMode ?? 'dinput');
    await this.rescan();
    this.catalogInitialized = true;
  }

  private async rescan(): Promise<void> {
    if (this.scanning) return;
    this.scanning = true;
    try {
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
    this.runningAppIds = running;
    this.refreshFavoriteIds();
    this.visibleApps = this.filterApps(this.searchQuery, this.filter, running);
  }

  private runningFilterLabel(): string {
    const n = this.runningAppIds.size;
    return n > 0 ? `运行中 (${n})` : '运行中';
  }

  /**
   * 收藏状态只在应用设置页里改, 但返回本页时 onPageShow → rescan 会走到这里,
   * 因此和运行态共用同一个刷新入口即可覆盖全部时机。
   */
  private refreshFavoriteIds(): void {
    const store = AppSettingsStore.getInstance();
    const next: Set<string> = new Set<string>();
    this.apps.forEach((app: AppDescriptor): void => {
      if (store.getAppSettings(app.id).favorite) next.add(app.id);
    });
    this.favoriteIds = next;
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
    this.captureScrollOffsets();
    this.filter = value;
    this.visibleApps = this.filterApps(this.searchQuery, value);
    setTimeout((): void => this.restoreScrollOffset(value), 0);
    if (syncPhoneNavigation && this.isPhone()) {
      const index = this.phoneNavIndexForFilter(value);
      if (index !== this.phoneNavIndex) {
        this.phoneNavIndex = index;
        this.phoneNavController.changeIndex(index);
      }
    }
    if (value === 'windows' && this.catalogInitialized) void this.rescan();
  }

  private openSidebarPage(url: string): void {
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

  /**
   * 封面墙列数。库页只做横板, 所以按窄屏 (<960vp, 含手机横屏) / 宽屏两档:
   * 竖版瓦片比横版窄, 同宽度下比网格模式多排一列而不挤压封面。
   */
  private coverWallColumns(): string {
    if (this.isPhone()) return '1fr 1fr 1fr';
    if (this.breakpoint === 'xl') return '1fr 1fr 1fr 1fr 1fr 1fr';
    return '1fr 1fr 1fr 1fr';
  }

  /**
   * 封面墙瓦片高度 (竖版, 约 3:4)。高度必须跟着列数走: 列多格窄就降高度,
   * 否则封面上下会被裁掉 — 调整上面的列数模板时这里要同步改。
   */
  private coverWallHeight(): number {
    if (this.isPhone()) return 300;
    if (this.breakpoint === 'xl') return 240;
    return 280;
  }

  private gridColumns(): string {
    if (this.catalogCardViewMode() === ViewMode.COMPACT) return '1fr';
    if (this.catalogCardViewMode() === ViewMode.COVER) return this.coverWallColumns();
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
    // 封面墙把标题压进美术里, 没有独立标题行, 只留瓦片高度和阴影内边距。
    if (this.catalogCardViewMode() === ViewMode.COVER) return this.coverWallHeight() + 16;
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
        { text: (this.settings.viewMode === ViewMode.COVER ? '✓ ' : '') + '封面墙', color: $r('sys.color.ohos_id_color_text_primary') },
        { text: (this.settings.viewMode === ViewMode.COMPACT ? '✓ ' : '') + '紧凑列表', color: $r('sys.color.ohos_id_color_text_primary') },
        { text: (this.settings.viewMode === ViewMode.LARGE ? '✓ ' : '') + '大封面', color: $r('sys.color.ohos_id_color_text_primary') },
        { text: (this.settings.viewMode === ViewMode.FILES ? '✓ ' : '') + '文件浏览', color: $r('sys.color.ohos_id_color_text_primary') }
      ]
    }).then((result) => {
      if (result.index === 0) this.setViewMode(ViewMode.GRID);
      else if (result.index === 1) this.setViewMode(ViewMode.COVER);
      else if (result.index === 2) this.setViewMode(ViewMode.COMPACT);
      else if (result.index === 3) this.setViewMode(ViewMode.LARGE);
      else if (result.index === 4) this.setViewMode(ViewMode.FILES);
    }).catch(() => {});
  }

  private phoneViewModeIcon(): Resource {
    if (this.settings.viewMode === ViewMode.COMPACT) return $r('sys.symbol.list_bullet');
    if (this.settings.viewMode === ViewMode.LARGE) return $r('sys.symbol.rectangle_grid_1x2');
    if (this.settings.viewMode === ViewMode.FILES) return $r('sys.symbol.folder');
    if (this.settings.viewMode === ViewMode.COVER) return $r('sys.symbol.rectangle_grid_2x1');
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
        .fontColor([this.filter === value ? $r('app.color.accent') : $r('app.color.text_secondary')])
      if (!this.sidebarCollapsed) {
        Text(label).fontSize(14).fontColor($r('app.color.text_primary'))
          .fontWeight(this.filter === value ? FontWeight.Medium : FontWeight.Normal)
      }
    }
    .width('100%')
    .height(48)
    .padding({ left: 15, right: 12 })
    .borderRadius(12)
    .backgroundColor(this.filter === value ? $r('app.color.selected_background') : '#00000000')
    .id('library-nav-' + value)
    .onClick(() => this.selectFilter(value))
  }

  @Builder
  private SideBar() {
    Column() {
      Row() {
        if (!this.sidebarCollapsed) {
          Column({ space: this.isNarrowPhone() ? 2 : 3 }) {
            Text('旧柚Pro').fontSize(this.isNarrowPhone() ? 16 : 18).fontWeight(FontWeight.Bold).fontColor($r('app.color.text_primary'))
            Text(this.apps.length + ' 个程序').fontSize(this.isNarrowPhone() ? 9 : 11).fontColor($r('app.color.text_secondary'))
          }.alignItems(HorizontalAlign.Start).layoutWeight(1)
        }
        Button({ type: ButtonType.Circle }) {
          SymbolGlyph(this.sidebarCollapsed ? $r('sys.symbol.chevron_right') : $r('sys.symbol.chevron_left'))
            .fontSize(16).fontColor([$r('app.color.text_primary')])
        }
          .width(48).height(48)
          .backgroundColor($r('app.color.search_background'))
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
          .backgroundColor($r('app.color.search_background')).fontColor($r('app.color.text_primary')).borderRadius(16)
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
            SymbolGlyph($r('sys.symbol.gearshape')).fontSize(19).fontColor([$r('app.color.text_secondary')])
            if (!this.sidebarCollapsed) {
              Text('系统设置').fontSize(14).fontColor($r('app.color.text_primary'))
            }
          }
          .width('100%').height(48).padding({ left: 15 }).borderRadius(12)
          .id('library-open-system-settings')
          .onClick((): void => this.openSidebarPage('pages/SystemSettings'))
          Row({ space: 12 }) {
            SymbolGlyph($r('sys.symbol.info_circle')).fontSize(19).fontColor([$r('app.color.text_secondary')])
            if (!this.sidebarCollapsed) {
              Text('帮助').fontSize(14).fontColor($r('app.color.text_primary'))
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
    .backgroundColor($r('app.color.sidebar_background'))
    .backgroundBlurStyle(BlurStyle.COMPONENT_THICK)
  }

  @Builder
  private Header() {
    Column() {
      Row() {
        Column({ space: 3 }) {
          Text(this.filterTitle())
            .fontSize(28).fontWeight(FontWeight.Bold).fontColor($r('app.color.text_primary'))
          Text(this.filterDescription()).fontSize(13).fontColor(this.engineState === EngineState.ERROR ? '#B42318' : $r('app.color.text_secondary'))
        }.alignItems(HorizontalAlign.Start).layoutWeight(1)
        Row({ space: 4 }) {
          Button({ type: ButtonType.Circle }) { SymbolGlyph($r('sys.symbol.square_grid_2x2')).fontSize(17).fontColor([$r('app.color.text_primary')]) }
            .width(48).height(48).backgroundColor(this.settings.viewMode === ViewMode.GRID ? $r('app.color.selected_background') : $r('app.color.search_background')).onClick(() => this.setViewMode(ViewMode.GRID))
          Button({ type: ButtonType.Circle }) { SymbolGlyph($r('sys.symbol.rectangle_grid_2x1')).fontSize(17).fontColor([$r('app.color.text_primary')]) }
            .width(48).height(48).backgroundColor(this.settings.viewMode === ViewMode.COVER ? $r('app.color.selected_background') : $r('app.color.search_background')).onClick(() => this.setViewMode(ViewMode.COVER))
          Button({ type: ButtonType.Circle }) { SymbolGlyph($r('sys.symbol.list_bullet')).fontSize(17).fontColor([$r('app.color.text_primary')]) }
            .width(48).height(48).backgroundColor(this.settings.viewMode === ViewMode.COMPACT ? $r('app.color.selected_background') : $r('app.color.search_background')).onClick(() => this.setViewMode(ViewMode.COMPACT))
          Button({ type: ButtonType.Circle }) { SymbolGlyph($r('sys.symbol.rectangle_grid_1x2')).fontSize(17).fontColor([$r('app.color.text_primary')]) }
            .width(48).height(48).backgroundColor(this.settings.viewMode === ViewMode.LARGE ? $r('app.color.selected_background') : $r('app.color.search_background')).onClick(() => this.setViewMode(ViewMode.LARGE))
          Button({ type: ButtonType.Circle }) { SymbolGlyph($r('sys.symbol.folder')).fontSize(17).fontColor([$r('app.color.text_primary')]) }
            .width(48).height(48).backgroundColor(this.settings.viewMode === ViewMode.FILES ? $r('app.color.selected_background') : $r('app.color.search_background')).onClick(() => this.setViewMode(ViewMode.FILES))
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
      favorite: this.favoriteIds.has(app.id),
      coverWallHeight: this.coverWallHeight(),
      useGlassEffect: this.settings.useGlassEffect,
      narrowPhone: this.isPhone(),
      onLaunch: (target: AppDescriptor): void => { this.launch(target); },
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
        .fontColor([$r('app.color.text_secondary')])
      Text(this.emptyTitle(filter)).fontSize(this.isNarrowPhone() ? 16 : 20)
        .fontWeight(FontWeight.Medium).fontColor($r('app.color.text_primary'))
      Text(this.emptyDescription(filter)).fontSize(this.isNarrowPhone() ? 11 : 13)
        .fontColor($r('app.color.text_secondary')).textAlign(TextAlign.Center)
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
          Text('正在扫描应用目录…').fontColor($r('app.color.text_secondary'))
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
        Text('正在扫描应用目录…').fontColor($r('app.color.text_secondary'))
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
          Text('正在扫描应用目录…').fontColor($r('app.color.text_secondary'))
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
        .fontColor([this.phoneNavIndex === index ? $r('app.color.accent') : $r('app.color.text_secondary')])
      Text(label)
        .fontSize(10)
        .fontWeight(this.phoneNavIndex === index ? FontWeight.Medium : FontWeight.Normal)
        .fontColor(this.phoneNavIndex === index ? $r('app.color.accent') : $r('app.color.text_secondary'))
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
      gradientMask: { maskColor: $r('app.color.page_background') },
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
      WineEngineBlockingOverlay()
    }
    .width('100%').height('100%')
    .backgroundColor($r('app.color.page_background'))
    .onAreaChange((_oldValue: Area, area: Area): void => {
      this.viewportWidth = area.width as number;
      this.viewportHeight = area.height as number;
      if (this.viewportWidth > 0 && this.viewportHeight > 0) {
        this.screenW = this.viewportWidth;
        this.screenH = this.viewportHeight;
        if (this.launcherVisible && !this.transitioning) {
          this.ensureFloatPos();
        }
      }
      BreakpointSystem.updateFromWidth(this.viewportWidth);
    })
  }

  build() {
    Stack() {
      this.PageSurface()
    }
    .width('100%').height('100%')
  }
}
