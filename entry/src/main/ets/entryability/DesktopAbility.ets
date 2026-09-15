import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '../service/LogService';
import { window } from '@kit.ArkUI';
import testNapi from 'libentry.so';
import { BusinessError } from '@kit.BasicServicesKit';
import { WineWindowManager } from '../service/WineWindowManager';
import { GamepadManager } from '../service/GamepadManager';
import { InputRouter } from '../service/InputRouter';
import { InputDispatcher } from '../service/InputDispatcher';
import { InputOverlayWindowCoordinator } from '../service/InputOverlayWindowCoordinator';
import { AppSettingsStore } from '../service/AppSettingsStore';
import { DesktopWindowMode, DisplayMode, ScreenOrientationSetting } from '../model/AppModels';
import { EngineState } from '../model/AppModels';
import { WineEngineService } from '../service/WineEngineService';
import { DeviceCapabilityPolicy } from '../common/DeviceCapabilityPolicy';

const DOMAIN = 0x0000;
const TAG = 'DWA';

export default class DesktopAbility extends UIAbility {
  private toplevelId: number = 0;
  private stage: window.WindowStage | null = null;
  private mainWindow: window.Window | null = null;
  private windowSizeCallback: ((size: window.Size) => void) | null = null;
  private windowResizeGeneration: number = 0;
  private desktopOrientation: ScreenOrientationSetting = ScreenOrientationSetting.SENSOR_LANDSCAPE;
  private lastAppliedWindowMode: string = '';
  private immersiveMaximized: boolean = false;
  private vdStatusAttached: boolean = false;

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    try {
      const params = want.parameters ?? {};
      this.toplevelId = Number(params.toplevelId) || 0;
      const requestedOrientation = String(params.desktopOrientation ?? '');
      if (requestedOrientation === ScreenOrientationSetting.LANDSCAPE ||
        requestedOrientation === ScreenOrientationSetting.PORTRAIT ||
        requestedOrientation === ScreenOrientationSetting.SENSOR_LANDSCAPE) {
        this.desktopOrientation = requestedOrientation as ScreenOrientationSetting;
      } else {
        // Ability launches can come from an older caller without the new
        // parameter. Read persisted settings only as a compatibility fallback.
        this.desktopOrientation = AppSettingsStore.getInstance().getGlobalSettings().desktopOrientation
          ?? ScreenOrientationSetting.SENSOR_LANDSCAPE;
      }
      AppStorage.setOrCreate('desktopToplevelId', this.toplevelId);
      WineWindowManager.getInstance().registerDesktopAbility(this.context);
      hilog.info(DOMAIN, TAG, '[DWA] onCreate: tl=%{public}d orientation=%{public}s',
        this.toplevelId, this.desktopOrientation);
    } catch (e) {
      hilog.error(DOMAIN, TAG, '[DWA] ERR onCreate: %{public}s', JSON.stringify(e));
    }
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    this.stage = windowStage;
    hilog.info(DOMAIN, TAG, '[DWA] onWindowStageCreate tl=%{public}d', this.toplevelId);
    let initialWindow: window.Window | null = null;
    try {
      initialWindow = windowStage.getMainWindowSync();
      if (initialWindow) {
        this.mainWindow = initialWindow;
        // Wait for the orientation transaction before constructing the ArkUI
        // scene. Otherwise the scene and native Wine output are born with the
        // default portrait rectangle and can remain a white, stale surface.
        DeviceCapabilityPolicy.applyDesktopOrientation(initialWindow, this.desktopOrientation, TAG)
          .then(() => this.loadDesktopContent(windowStage))
          .catch((error: Error) => {
            hilog.warn(DOMAIN, TAG, '[DWA] orientation failed, loading content: %{public}s', JSON.stringify(error));
            this.loadDesktopContent(windowStage);
          });
        return;
      }
    } catch (e) {
      hilog.warn(DOMAIN, TAG, '[DWA] pre-load window setup failed: %{public}s', JSON.stringify(e));
    }
    this.loadDesktopContent(windowStage);
  }

  private async loadDesktopContent(windowStage: window.WindowStage): Promise<void> {
    windowStage.loadContent('pages/DesktopWindow', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, '[DWA] loadContent FAILED: %{public}s', JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, TAG, '[DWA] loadContent OK tl=%{public}d', this.toplevelId);

      this.immersiveMaximized = false;
      this.vdStatusAttached = false;
      AppStorage.setOrCreate('vdImmersive', false);

      // 沉浸模式: 全屏 + 隐藏状态栏和导航栏
      void this.finishDesktopWindowSetup(windowStage);
    });
  }

  private async finishDesktopWindowSetup(windowStage: window.WindowStage): Promise<void> {
    try {
        const mw = windowStage.getMainWindowSync();
        if (mw) {
          this.mainWindow = mw;
          testNapi.registerHostWindow(mw.getWindowProperties().id);
          // The orientation request and the immersive layout are asynchronous.
          // Measuring windowRect before both transactions settle can feed the
          // old portrait rectangle into Wayland and leave a white strip after
          // a landscape launch. Apply the window chrome first, then wait for
          // the final rectangle before the first compositor resize.
          await this.applyDesktopWindowMode(mw);
          await DeviceCapabilityPolicy.waitForRequestedOrientation(mw, this.desktopOrientation, TAG);
          this.applyKeepScreenOn(mw);

          // 输出尺寸以 XComponent surface 为唯一权威 (onSurfaceChanged → 
          // syncOutputSize/notifyToplevelResize)。窗口 rect 在非全屏布局下
          // 仍是完整窗口尺寸, 且不包含底部手势导航区, 用来同步输出会与
          // surface 错配 → 桌面被压缩/白边/触摸错位。
          if (DeviceCapabilityPolicy.usesForkNativeChildProcess()) {
            this.windowSizeCallback = (size: window.Size): void => {
              this.synchronizeDesktopWindow(size.width, size.height);
            };
            mw.on('windowSizeChange', this.windowSizeCallback);
            const initialRect = mw.getWindowProperties().windowRect;
            this.synchronizeDesktopWindow(initialRect.width, initialRect.height);
          }
          mw.on('windowRectChange', () => {
            InputOverlayWindowCoordinator.getInstance().syncBounds().catch(() => {});
          });
          this.attachInputOverlay();
        }
      } catch (e) {
        hilog.error(DOMAIN, TAG, '[DWA] immersive ERR: %{public}s', JSON.stringify(e));
      }
  }

  /**
   * Fullscreen stretches the Wine desktop edge-to-edge and hides the system
   * bars; on rounded-screen tablets that clips the start menu. Safe-area mode
   * keeps the window inside the system inset so corners and bars stay visible.
   */
  private async applyDesktopWindowMode(target: window.Window): Promise<void> {
    // PC 虚拟桌面 (VIRTUAL): 独立窗口承载 Wine 桌面 — 保留系统标题栏,
    // 默认浮动 (可拖动/缩放/最小化/关闭), 与上游 VirtualDesktopAbility 同语义。
    // 用户可自行最大化 → 沉浸全屏 (maximize 时系统隐藏标题栏, renderer letterbox
    // 等比呈现, 无需 ArkTS 额外动作)。全屏布局/安全区仅 tablet 桌面生效。
    if (DeviceCapabilityPolicy.usesManagedWineWindows() &&
      WineWindowManager.getInstance().isDesktopMode) {
      try {
        await target.setWindowLayoutFullScreen(false);
        try {
          target.setWindowTitle('Wine 桌面');
        } catch (_) {
        }
        // 保留系统标题栏: 沉浸全屏由 maximize(DISABLE_HOVER) 隐藏, 还原后
        // 标题栏回来供关闭/最小化。不要 setWindowDecorVisible(false),
        // 也不要用 resize(屏幕) 冒充全屏 (会和 immersive 抢几何、弄歪鼠标)。
        await Promise.all([
          target.setSpecificSystemBarEnabled('status', false),
          target.setSpecificSystemBarEnabled('navigationIndicator', false)
        ]);
        await target.setWindowBackgroundColor('#000000');
        this.attachVirtualDesktopStatus(target);
        this.immersiveMaximized = true;
        WineWindowManager.getInstance().enterImmersiveFullscreen(target, 0);
        AppStorage.setOrCreate('vdImmersive', true);
        hilog.info(DOMAIN, TAG, '[DWA] pc-virtual auto immersive (DISABLE_HOVER)');
      } catch (e) {
        hilog.warn(DOMAIN, TAG, '[DWA] pc-virtual immersive setup partial: %{public}s',
          JSON.stringify(e));
      }
      this.lastAppliedWindowMode = 'pc-virtual';
      hilog.info(DOMAIN, TAG, '[DWA] pc virtual desktop: immersive fullscreen');
      return;
    }
    const mode = AppSettingsStore.getInstance().getGlobalSettings().desktopWindowMode
      ?? DesktopWindowMode.FULLSCREEN;
    this.lastAppliedWindowMode = mode;
    if (mode === DesktopWindowMode.SAFE_AREA) {
      // 切边模式同样隐藏系统标题栏并让桌面铺满: 保留系统栏会把可见区
      // 压到 2560x1430, 而 Wine 桌面按 1280x800 布局 (任务栏在 y=780),
      // 底部 85px (start 栏) 会被切掉, 且内容整体偏移导致鼠标位置错位。
      // 圆角裁剪如需规避, 后续可加小幅内边距 (corner inset), 不再显示系统栏。
      await target.setWindowLayoutFullScreen(true);
      await Promise.all([
        target.setSpecificSystemBarEnabled('status', false),
        target.setSpecificSystemBarEnabled('navigationIndicator', false)
      ]);
      // 系统栏区域保持黑色，避免安全区模式出现白色边条。
      try {
        await target.setWindowBackgroundColor('#000000');
      } catch (_) {
      }
      const rect = target.getWindowProperties().windowRect;
      hilog.info(DOMAIN, TAG,
        '[DWA] safe-area layout applied rect=%{public}dx%{public}d@(%{public}d,%{public}d)',
        rect.width, rect.height, rect.left, rect.top);
    } else {
      await target.setWindowLayoutFullScreen(true);
      await Promise.all([
        target.setSpecificSystemBarEnabled('status', false),
        target.setSpecificSystemBarEnabled('navigationIndicator', false)
      ]);
      try {
        await target.setWindowBackgroundColor('#000000');
      } catch (_) {
      }
      hilog.info(DOMAIN, TAG, '[DWA] desktop window fullscreen layout applied');
    }
  }

  onForeground(): void {
    hilog.info(DOMAIN, TAG, '[DWA] onForeground tl=%{public}d', this.toplevelId);
    WineWindowManager.getInstance().markDesktopAbilityForeground(this.toplevelId);
    InputRouter.getInstance().setActiveToplevel(WineWindowManager.getInstance().getDesktopRootId());
    // 虚拟桌面宿主进后台后 surface 不可呈现, 才暂停 GPU; 回前台再画。
    const rootId = WineWindowManager.getInstance().getDesktopRootId();
    if (rootId > 0) {
      try { testNapi.setToplevelVisible(rootId, true); } catch (_) {}
      try { testNapi.setRendererPaused(rootId, false); } catch (_) {}
      try { testNapi.refreshRenderer(rootId); } catch (_) {}
    }
    this.attachInputOverlay();
    if (this.mainWindow) {
      this.applyKeepScreenOn(this.mainWindow);
    }
  }

  onNewWant(want: Want): void {
    const newId = Number(want.parameters?.toplevelId) || 0;
    hilog.info(DOMAIN, TAG, '[DWA] onNewWant tl: %{public}d -> %{public}d', this.toplevelId, newId);
    // 无参数的 activate 只是把 singleton Ability 带回前台，不能覆盖已知 root。
    if (newId > 0) {
      this.toplevelId = newId;
      AppStorage.setOrCreate('desktopToplevelId', newId);
    }
    // A settings change while the singleton desktop ability stays alive must
    // still reach the window; re-apply only when the mode actually differs.
    const currentMode = AppSettingsStore.getInstance().getGlobalSettings().desktopWindowMode
      ?? DesktopWindowMode.FULLSCREEN;
    AppStorage.setOrCreate('desktopWindowMode', currentMode);
    if (currentMode !== this.lastAppliedWindowMode && this.mainWindow) {
      this.applyDesktopWindowMode(this.mainWindow).catch((error: Error) => {
        hilog.warn(DOMAIN, TAG, '[DWA] re-apply window mode failed: %{public}s', JSON.stringify(error));
      });
    }
    if (this.mainWindow) {
      this.applyKeepScreenOn(this.mainWindow);
    }
    this.attachInputOverlay();
  }

  onBackground(): void {
    hilog.info(DOMAIN, TAG, '[DWA] onBackground tl=%{public}d', this.toplevelId);
    WineWindowManager.getInstance().markDesktopAbilityBackground(this.toplevelId);
    GamepadManager.getInstance().releaseAll();
    InputDispatcher.getInstance().releaseAll('desktop-background');
    InputRouter.getInstance().releaseAllKeys();
    InputOverlayWindowCoordinator.getInstance().setVisible(false).catch(() => {});
    // 后台: 桌面窗口 surface 不可呈现, 暂停 GPU, 避免 vsync/swap 阻塞。
    const rootId = WineWindowManager.getInstance().getDesktopRootId();
    if (rootId > 0) {
      try { testNapi.setToplevelVisible(rootId, false); } catch (_) {}
      try { testNapi.setRendererPaused(rootId, true); } catch (_) {}
    }
  }

  onDestroy(): void {
    hilog.info(DOMAIN, TAG, '[DWA] onDestroy tl=%{public}d', this.toplevelId);
    try {
      if (this.mainWindow && this.windowSizeCallback) {
        this.mainWindow.off('windowSizeChange', this.windowSizeCallback);
      }
      this.windowResizeGeneration++;
      this.windowSizeCallback = null;
      GamepadManager.getInstance().releaseAll();
      InputDispatcher.getInstance().releaseAll('desktop-destroy');
      InputRouter.getInstance().releaseAllKeys();
      WineWindowManager.getInstance().markDesktopAbilityStopped(this.toplevelId);
      WineWindowManager.getInstance().unregisterDesktopAbility();
      // 关闭 ArkTS 宿主并不等于关闭 Wine explorer。保留 desktop root 后，
      // 应用库中的“Wine 桌面”卡片可以重新拉起同一会话和渲染 surface。
      WineWindowManager.getInstance().unregisterAbility(this.toplevelId);
      InputOverlayWindowCoordinator.getInstance().detach('desktop-ability-destroy').catch(() => {});
      if (this.mainWindow) {
        this.mainWindow.setWindowKeepScreenOn(false).catch(() => {});
      }
    } catch (e) {
      hilog.error(DOMAIN, TAG, '[DWA] ERR onDestroy: %{public}s', JSON.stringify(e));
    }
  }

  /**
   * PC 虚拟桌面: 最大化 → 沉浸全屏 (DISABLE_HOVER), 还原后标题栏回归。
   * WindowStatusType: 1=FULL_SCREEN 2=MAXIMIZE 4=FLOATING。
   */
  private attachVirtualDesktopStatus(target: window.Window): void {
    if (this.vdStatusAttached) return;
    this.vdStatusAttached = true;
    target.on('windowStatusChange', (statusType: number) => {
      hilog.info(DOMAIN, TAG, '[DWA] windowStatusChange status=%{public}d immersive=%{public}s',
        statusType, this.immersiveMaximized ? 'yes' : 'no');
      if (statusType === window.WindowStatusType.MAXIMIZE ||
        statusType === window.WindowStatusType.FULL_SCREEN) {
        if (!this.immersiveMaximized) {
          this.immersiveMaximized = true;
          WineWindowManager.getInstance().enterImmersiveFullscreen(target, 0);
        }
        AppStorage.setOrCreate('vdImmersive', true);
      } else {
        if (this.immersiveMaximized) {
          this.immersiveMaximized = false;
        }
        AppStorage.setOrCreate('vdImmersive', false);
      }
    });
  }

  /** 桌面打开期间按设置保持屏幕常亮，避免游玩中自动锁屏。 */
  private applyKeepScreenOn(target: window.Window): void {
    try {
      const keepScreenOn = AppSettingsStore.getInstance().getGlobalSettings().keepScreenOn;
      target.setWindowKeepScreenOn(keepScreenOn).catch((error: Error) => {
        hilog.warn(DOMAIN, TAG, '[DWA] setWindowKeepScreenOn(%{public}s) failed: %{public}s',
          keepScreenOn ? 'on' : 'off', JSON.stringify(error));
      });
    } catch (e) {
      hilog.warn(DOMAIN, TAG, '[DWA] applyKeepScreenOn ERR: %{public}s', JSON.stringify(e));
    }
  }

  private attachInputOverlay(): void {
    // PC/2-in-1 desktop mode is a multiton collection of native Harmony
    // windows. It relies on physical input and must not create a screen
    // overlay for every Wine window.
    if (AppSettingsStore.getInstance().usesManagedWineWindows()) {
      InputOverlayWindowCoordinator.getInstance().detach('pc-desktop-uses-physical-input').catch(() => {});
      return;
    }
    if (!this.stage || !this.mainWindow) return;
    const settings = AppSettingsStore.getInstance().getGlobalSettings();
    const rootId = WineWindowManager.getInstance().getDesktopRootId() || this.toplevelId;
    InputOverlayWindowCoordinator.getInstance().attach(
      this.stage, this.mainWindow, 'desktop-' + rootId, DisplayMode.DESKTOP, rootId,
      settings.desktopInputProfileId || 'default', settings.showVirtualControls, settings.controlOpacity,
      settings.virtualInputEnabled,
      true).catch(() => {});
  }

  private synchronizeDesktopWindow(width: number, height: number): void {
    if (width <= 0 || height <= 0) return;
    WineWindowManager.getInstance().syncOutputSize(width, height);
    InputOverlayWindowCoordinator.getInstance().syncBounds().catch(() => {});
    const generation = ++this.windowResizeGeneration;
    // Rotation delivers several transient sizes. Re-sample the real window
    // after layout and animation settle so both the Wine root and overlay use
    // the final landscape/portrait rectangle.
    [160, 480].forEach((delay: number): void => {
      setTimeout((): void => {
        if (generation !== this.windowResizeGeneration || !this.mainWindow) return;
        const rect = this.mainWindow.getWindowProperties().windowRect;
        WineWindowManager.getInstance().syncOutputSize(rect.width, rect.height);
        InputOverlayWindowCoordinator.getInstance().syncBounds().catch(() => {});
      }, delay);
    });
  }
}
