import { WineImeCoordinator } from '../service/WineImeCoordinator';
import { X11ClipboardService } from '../service/X11ClipboardService';
import { GamescopeRuntimeService } from '../service/GamescopeRuntimeService';
import { AccountAutomaticSyncService } from '../service/valueadded/AccountAutomaticSyncService';
import { ValueAddedService } from '../service/valueadded/ValueAddedService';
import { UserConfigurationRepository } from '../service/UserConfigurationRepository';
import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '../service/LogService';
import { AppLog } from '../service/LogService';
import { display, window, router } from '@kit.ArkUI';
import { WineWindowManager } from '../service/WineWindowManager';
import { InputOverlayWindowCoordinator } from '../service/InputOverlayWindowCoordinator';
import { AppCatalogService, GamesDirectoryStatus } from '../service/AppCatalogService';
import { DeviceCapabilityPolicy } from '../common/DeviceCapabilityPolicy';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;

export default class EntryAbility extends UIAbility {
  private mainWindow: window.Window | null = null;
  private titleButtonRectCallback: ((rect: window.TitleButtonRect) => void) | null = null;
  private windowRectCallback: (() => void) | null = null;
  // A routed page can briefly change the window layout while it enters or
  // leaves an editor. That transaction reports an empty title-button rect on
  // some tablet/desktop shells even though the controls return afterwards.
  // Retain the last real safe area for the lifetime of this main window.
  private lastTitleButtonInsetVp: number = 0;
  private membershipDelivery: string | null = null;
  private membershipRouteTimer: number = -1;
  private membershipRouteAttempts: number = 0;
  private applyDiagnosticWant(want: Want): void {
    // Explicit, non-persistent host diagnostics; unrelated Wants leave the
    // current setting alone. No Wine environment or per-frame trace needed.
    const value = want.parameters?.['winehua.frameDiagnostics'];
    if (value === '1' || value === true) testNapi.setFrameLoopDiagnostics(true);
    else if (value === '0' || value === false) testNapi.setFrameLoopDiagnostics(false);
  }
  private rememberMembershipWant(want: Want): void {
    if (want.action !== 'com.vintagepomelo.action.MEMBERSHIP') return;
    const delivery = want.parameters?.deliveryId;
    this.membershipDelivery = typeof delivery === 'string' && /^[a-f0-9]{64}$/.test(delivery) ? delivery : '';
    this.membershipRouteAttempts = 0;
  }
  private openMembershipWant(): void {
    if (this.membershipDelivery === null || !this.mainWindow) return;
    if (this.membershipRouteTimer !== -1) clearTimeout(this.membershipRouteTimer);
    // loadContent can finish before the first page mounts its router. Defer
    // navigation until the next UI turn and retain the request on failure.
    this.membershipRouteTimer = setTimeout((): void => {
      this.membershipRouteTimer = -1;
      this.routeMembershipWant();
    }, 100);
  }
  private routeMembershipWant(): void {
    if (this.membershipDelivery === null || !this.mainWindow) return;
    const delivery = this.membershipDelivery; this.membershipDelivery = null;
    this.membershipRouteAttempts++;
    // Push launches can arrive before the global router has an active scope.
    // Route through the Ability's own window, including a cold application start.
    void this.mainWindow.getUIContext().getRouter().pushUrl({ url: 'pages/SystemSettings',
      params: { section: 'sync', membership: 'expanded', deliveryId: delivery } })
      .catch((): void => {
        if (this.membershipDelivery === null) this.membershipDelivery = delivery;
        if (this.membershipRouteAttempts < 5) this.openMembershipWant();
        else hilog.warn(DOMAIN, 'EntryAbility', 'Membership notification route unavailable');
      });
  }

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    // 日志服务最先初始化，保证后续 ArkTS/native 日志都能实时落盘。
    AppLog.init();
    this.applyDiagnosticWant(want);
    DeviceCapabilityPolicy.startPcModeMonitoring(this.context);
    hilog.info(DOMAIN, 'EntryAbility', '%{public}s', 'Ability onCreate');
    const useForkNcp = DeviceCapabilityPolicy.isPhoneDevice();
    testNapi.setPhoneMode(useForkNcp);
    GamescopeRuntimeService.initialize();
    hilog.info(DOMAIN, 'EntryAbility', 'NCP backend=%{public}s deviceType=%{public}s',
      useForkNcp ? 'fork' : 'system', DeviceCapabilityPolicy.deviceType());
    this.ensureDownloadGamesDirectory('onCreate');
    this.rememberMembershipWant(want);
  }

  private ensureDownloadGamesDirectory(trigger: string): void {
    const status: GamesDirectoryStatus = AppCatalogService.getInstance().ensureGamesDirectory();
    if (!status.ready) {
      hilog.error(DOMAIN, 'EntryAbility',
        'Download games directory unavailable during %{public}s: %{public}s', trigger, status.error);
    }
  }

  onDestroy(): void {
    DeviceCapabilityPolicy.stopPcModeMonitoring(this.context);
    if (this.membershipRouteTimer !== -1) clearTimeout(this.membershipRouteTimer);
    this.membershipRouteTimer = -1;
    hilog.info(DOMAIN, 'EntryAbility', '%{public}s', 'Ability onDestroy');
  }

  onNewWant(want: Want): void {
    this.applyDiagnosticWant(want);
    this.rememberMembershipWant(want);
    if (this.mainWindow) this.openMembershipWant();
    // A singleton Ability may receive the next launch without another
    // onCreate. Re-check the app-owned Download directory on every handoff.
    this.ensureDownloadGamesDirectory('onNewWant');
    const parameters = want.parameters ?? {};
    if (parameters.overlayRoute === 'input-layout' || parameters.overlayRoute === 'key-mapping') {
      // 覆盖层属于 Desktop Ability，不能直接把它的子窗口替换为编辑器。
      // 回到应用库主窗口再进入编辑页，Wine 会话则保持后台可恢复。
      AppStorage.setOrCreate('overlayEditorRequest', JSON.stringify({
        route: String(parameters.overlayRoute),
        profileId: String(parameters.profileId ?? 'default'),
        profileName: String(parameters.profileName ?? '默认键鼠手柄'),
        appId: String(parameters.appId ?? ''),
        baseProfileId: String(parameters.baseProfileId ?? 'default'),
        returnAbilityName: String(parameters.returnAbilityName ?? ''),
        returnSessionId: String(parameters.returnSessionId ?? ''),
        returnToplevelId: String(parameters.returnToplevelId ?? '0')
      }));
    }
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    void UserConfigurationRepository.getInstance().initialize(this.context).then((): void => {
      this.createConfiguredWindowStage(windowStage);
    }).catch((error: Error): void => {
      hilog.error(DOMAIN, 'EntryAbility', 'Configuration bootstrap failed, preserving backups');
      this.createConfiguredWindowStage(windowStage);
    });
  }
  private createConfiguredWindowStage(windowStage: window.WindowStage): void {
    // Main window is created, set main page for this ability
    hilog.info(DOMAIN, 'EntryAbility', '%{public}s', 'Ability onWindowStageCreate');
    this.ensureDownloadGamesDirectory('onWindowStageCreate');

    // 初始化多窗口管理器
    WineWindowManager.getInstance().init(windowStage, this.context);

    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, 'EntryAbility', 'Failed to load the content. Cause: %{public}s', JSON.stringify(err));
        return;
      }
      // loadContent 成功后隐藏窗口装饰
      let mainWin = windowStage.getMainWindowSync();
      if (mainWin) {
        this.mainWindow = mainWin;
        WineImeCoordinator.getInstance().observeWindow(0, mainWin);
        WineImeCoordinator.getInstance().foreground(0, mainWin);
        DeviceCapabilityPolicy.enableSensorRotation(mainWin, 'EntryAbility');
        mainWin.setWindowDecorVisible(false);
        if (DeviceCapabilityPolicy.isPhoneDevice()) {
          // HDS navigation owns the status-bar material. Extend the ArkUI
          // scene behind it while keeping the status icons visible.
          mainWin.setWindowLayoutFullScreen(true).catch((error: Error) => {
            hilog.warn(DOMAIN, 'EntryAbility', 'Phone immersive layout unavailable: %{public}s', JSON.stringify(error));
          });
          mainWin.setWindowSystemBarEnable(['status']).catch((error: Error) => {
            hilog.warn(DOMAIN, 'EntryAbility', 'Phone system bar policy unavailable: %{public}s', JSON.stringify(error));
          });
        }
        if (this.isPcWindowHost()) {
          // A 2-in-1 keeps the system three-button area even when the visual
          // title decoration is hidden. Re-enable that area's native drag and
          // double-click handling so the borderless library window still
          // behaves like a normal PC window.
          try {
            mainWin.setWindowDecorHeight(48);
            mainWin.setWindowTitleMoveEnabled(true);
          } catch (error) {
            hilog.warn(DOMAIN, 'EntryAbility', 'PC title drag unavailable: %{public}s', JSON.stringify(error));
          }
        }
        // The gesture navigation indicator otherwise reserves a light strip
        // below every routed page. Gesture navigation remains available.
        mainWin.setSpecificSystemBarEnabled('navigationIndicator', false).catch(() => {});
        this.observeTitleButtonInset(mainWin);
        this.windowRectCallback = (): void => {
          InputOverlayWindowCoordinator.getInstance().syncBounds().catch(() => {});
        };
        try {
          mainWin.on('windowRectChange', this.windowRectCallback);
        } catch (error) {
          hilog.warn(DOMAIN, 'EntryAbility', 'windowRectChange unavailable: %{public}s',
            JSON.stringify(error));
        }
      }
      hilog.info(DOMAIN, 'EntryAbility', 'Succeeded in loading the content.');
      this.openMembershipWant();
    });
  }

  private isPcWindowHost(): boolean {
    return DeviceCapabilityPolicy.usesManagedWineWindows();
  }

  onWindowStageDestroy(): void {
    // Main window is destroyed, release UI related resources
    hilog.info(DOMAIN, 'EntryAbility', '%{public}s', 'Ability onWindowStageDestroy');
    if (this.mainWindow && this.titleButtonRectCallback) {
      try {
        this.mainWindow.off('windowTitleButtonRectChange', this.titleButtonRectCallback);
      } catch (_) {
      }
    }
    if (this.mainWindow && this.windowRectCallback) {
      try {
        this.mainWindow.off('windowRectChange', this.windowRectCallback);
      } catch (_) {
      }
    }
    this.titleButtonRectCallback = null;
    this.windowRectCallback = null;
    WineImeCoordinator.getInstance().unobserveWindow(this.mainWindow);
    this.mainWindow = null;
    AppStorage.setOrCreate('libraryTitleButtonInsetVp', 0);
  }

  private observeTitleButtonInset(mainWindow: window.Window): void {
    this.lastTitleButtonInsetVp = 0;
    AppStorage.setOrCreate('libraryTitleButtonInsetVp', 0);
    this.titleButtonRectCallback = (rect: window.TitleButtonRect): void => {
      this.updateTitleButtonInset(rect);
    };
    try {
      this.updateTitleButtonInset(mainWindow.getTitleButtonRect());
      mainWindow.on('windowTitleButtonRectChange', this.titleButtonRectCallback);
    } catch (error) {
      this.publishTitleButtonInset();
      hilog.warn(DOMAIN, 'EntryAbility', 'Title button safe area unavailable: %{public}s', JSON.stringify(error));
    }
  }

  private updateTitleButtonInset(rect: window.TitleButtonRect): void {
    try {
      if (rect.width <= 0) {
        // Window layout and orientation changes can send a transient empty
        // rect. Do not clear a previously measured desktop/tablet title area,
        // otherwise returning from Settings or an input editor makes every
        // routed library page lose its right-side avoidance.
        this.publishTitleButtonInset();
        return;
      }
      const density = Math.max(1, display.getDefaultDisplaySync().densityPixels);
      this.lastTitleButtonInsetVp = Math.ceil((Math.max(0, rect.right) + Math.max(0, rect.width)) / density) + 12;
      this.publishTitleButtonInset();
    } catch (_) {
      this.publishTitleButtonInset();
    }
  }

  private publishTitleButtonInset(): void {
    AppStorage.setOrCreate('libraryTitleButtonInsetVp', this.lastTitleButtonInsetVp);
  }

  onForeground(): void {
    X11ClipboardService.foreground(this.context);
    if (this.mainWindow) WineImeCoordinator.getInstance().foreground(0, this.mainWindow);
    DeviceCapabilityPolicy.refreshPcMode(this.context);
    AccountAutomaticSyncService.getInstance().setForeground(true);
    ValueAddedService.getInstance().onForeground();
    // Ability has brought to foreground
    hilog.info(DOMAIN, 'EntryAbility', '%{public}s', 'Ability onForeground');
    this.ensureDownloadGamesDirectory('onForeground');
  }

  onBackground(): void {
    X11ClipboardService.background(this.context);
    WineImeCoordinator.getInstance().background(this.mainWindow);
    AccountAutomaticSyncService.getInstance().setForeground(false);
    ValueAddedService.getInstance().onBackground();
    // Ability has back to background
    hilog.info(DOMAIN, 'EntryAbility', '%{public}s', 'Ability onBackground');
  }
}
