import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '../service/LogService';
import { AppLog } from '../service/LogService';
import { display, window } from '@kit.ArkUI';
import { WineWindowManager } from '../service/WineWindowManager';
import { InputOverlayWindowCoordinator } from '../service/InputOverlayWindowCoordinator';
import { AppCatalogService, GamesDirectoryStatus } from '../service/AppCatalogService';
import { DeviceCapabilityPolicy } from '../common/DeviceCapabilityPolicy';
import { AppSettingsStore } from '../service/AppSettingsStore';
import { WineEngineService } from '../service/WineEngineService';
import { AppSessionService } from '../service/AppSessionService';
import { SmokeRunner } from '../service/SmokeRunner';
import { SmokeRequest } from '../common/SmokeTypes';
import { NativeLaunchResult, WinePresentationMode } from '../model/AppModels';
import testNapi from 'libentry.so';

const DOMAIN = 0x0000;

interface AutomationEnvironmentPair {
  key: string;
  value: string;
}

export default class EntryAbility extends UIAbility {
  private mainWindow: window.Window | null = null;
  private titleButtonRectCallback: ((rect: window.TitleButtonRect) => void) | null = null;
  private windowRectCallback: (() => void) | null = null;
  // A routed page can briefly change the window layout while it enters or
  // leaves an editor. That transaction reports an empty title-button rect on
  // some tablet/desktop shells even though the controls return afterwards.
  // Retain the last real safe area for the lifetime of this main window.
  private lastTitleButtonInsetVp: number = 0;
  private pendingAutomationWant: Want | null = null;
  private automationLaunchStarted: boolean = false;

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    // 日志服务最先初始化，保证后续 ArkTS/native 日志都能实时落盘。
    AppLog.init();
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onCreate');
    const useForkNcp = DeviceCapabilityPolicy.isPhoneDevice();
    testNapi.setPhoneMode(useForkNcp);
    hilog.info(DOMAIN, 'EntryAbility', 'NCP backend=%{public}s deviceType=%{public}s',
      useForkNcp ? 'fork' : 'system', DeviceCapabilityPolicy.deviceType());
    this.ensureDownloadGamesDirectory('onCreate');
    this.rememberAutomationWant(want);
  }

  private ensureDownloadGamesDirectory(trigger: string): void {
    const status: GamesDirectoryStatus = AppCatalogService.getInstance().ensureGamesDirectory();
    if (!status.ready) {
      hilog.error(DOMAIN, 'EntryAbility',
        'Download games directory unavailable during %{public}s: %{public}s', trigger, status.error);
    }
  }

  onDestroy(): void {
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onDestroy');
  }

  onNewWant(want: Want): void {
    // A singleton Ability may receive the next launch without another
    // onCreate. Re-check the app-owned Download directory on every handoff.
    this.ensureDownloadGamesDirectory('onNewWant');
    this.rememberAutomationWant(want);
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
    this.scheduleAutomationLaunch();
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    // Main window is created, set main page for this ability
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onWindowStageCreate');
    this.ensureDownloadGamesDirectory('onWindowStageCreate');

    // 初始化多窗口管理器
    WineWindowManager.getInstance().init(windowStage, this.context);

    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, 'testTag', 'Failed to load the content. Cause: %{public}s', JSON.stringify(err));
        return;
      }
      // loadContent 成功后隐藏窗口装饰
      let mainWin = windowStage.getMainWindowSync();
      if (mainWin) {
        this.mainWindow = mainWin;
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
      hilog.info(DOMAIN, 'testTag', 'Succeeded in loading the content.');
      this.scheduleAutomationLaunch();
    });
  }

  private rememberAutomationWant(want: Want): void {
    const mode = String(want.parameters?.['winehua.mode'] ?? '');
    if (mode !== 'game' && mode !== 'smoke') return;
    this.pendingAutomationWant = want;
    hilog.info(DOMAIN, 'EntryAbility', 'automation Want accepted mode=%{public}s', mode);
  }

  private scheduleAutomationLaunch(): void {
    if (!this.mainWindow || !this.pendingAutomationWant || this.automationLaunchStarted) return;
    const want = this.pendingAutomationWant;
    this.pendingAutomationWant = null;
    this.automationLaunchStarted = true;
    setTimeout((): void => {
      this.runAutomationLaunch(want).then((): void => {
        this.finishAutomationLaunch();
      }).catch((error: Error): void => {
        hilog.error(DOMAIN, 'EntryAbility', 'automation launch failed: %{public}s', JSON.stringify(error));
        this.finishAutomationLaunch();
      });
    }, 800);
  }

  private finishAutomationLaunch(): void {
    this.automationLaunchStarted = false;
    if (this.pendingAutomationWant) this.scheduleAutomationLaunch();
  }

  private decodeAutomationPayload(value: string): string {
    try {
      return decodeURIComponent(value);
    } catch (_) {
      return value;
    }
  }

  private parseAutomationArguments(value: string): string[] {
    if (!value) return [];
    try {
      const raw = JSON.parse(this.decodeAutomationPayload(value)) as string[];
      const result: string[] = [];
      for (let i = 0; i < raw.length; i++) {
        if (typeof raw[i] === 'string') result.push(raw[i]);
      }
      return result;
    } catch (_) {
      return [];
    }
  }

  private parseAutomationEnvironment(value: string): string[] {
    if (!value) return [];
    try {
      const raw = JSON.parse(this.decodeAutomationPayload(value)) as AutomationEnvironmentPair[];
      const result: string[] = [];
      for (let i = 0; i < raw.length; i++) {
        const pair = raw[i];
        if (pair && typeof pair.key === 'string' && typeof pair.value === 'string' && pair.key.length > 0) {
          result.push(`${pair.key}=${pair.value}`);
        }
      }
      return result;
    } catch (_) {
      return [];
    }
  }

  private automationWorkingDirectory(executable: string): string {
    const separator = Math.max(executable.lastIndexOf('/'), executable.lastIndexOf('\\'));
    return separator > 0 ? executable.substring(0, separator) : '';
  }

  private async runAutomationLaunch(want: Want): Promise<void> {
    const parameters = want.parameters ?? {};
    if (String(parameters['winehua.mode'] ?? '') === 'smoke') {
      await this.runSmokeLaunch(want);
      return;
    }
    const executable = String(parameters['winehua.game_path'] ?? '');
    if (!executable) throw new Error('winehua.game_path is empty');

    const backend = String(parameters['winehua.d3d_backend'] ?? 'dxvk_legacy');
    const renderMode = backend === 'dxvk_modern_2_6' ? 'dxvk_2_6' :
      (backend === 'wined3d' ? 'virgl' : 'dxvk_1_10');
    const settingsStore = AppSettingsStore.getInstance();
    await settingsStore.initialize(this.context);
    await settingsStore.setRenderMode(renderMode);

    const argumentsValue = this.parseAutomationArguments(
      String(parameters['winehua.game_args_json'] ?? ''));
    const environment = this.parseAutomationEnvironment(
      String(parameters['winehua.d3d_env_json'] ?? ''));
    const homeDirectory = '/storage/Users/currentUser/Download/com.vintage.pomelopro';
    const engine = WineEngineService.getInstance();
    engine.initialize(this.context);
    engine.selectHostGraphicsBackendForExecutable(executable);
    const graphicsExperiment = String(parameters['winehua.graphics_experiment'] ?? '').trim();
    if (graphicsExperiment.length > 0 &&
      !engine.selectHostGraphicsExperimentForLab(graphicsExperiment)) {
      throw new Error(`Unsupported graphics LAB experiment: ${graphicsExperiment}`);
    }
    const settings = settingsStore.getGlobalSettings();
    const ready = await engine.ensureReady(settings.defaultDisplayMode,
      WinePresentationMode.DESKTOP, homeDirectory);
    if (!ready) throw new Error('Wine engine did not become ready');

    // READY can precede desktop-root registration by a few frames. Giving the
    // Wayland desktop a short event-driven settling window keeps the first game
    // toplevel from racing the root surface on cold prefix startup.
    await new Promise<void>((resolve: () => void): void => {
      setTimeout((): void => resolve(), 1000);
    });
    const result = engine.launchExecutable(executable, argumentsValue,
      this.automationWorkingDirectory(executable), environment) as NativeLaunchResult;
    if (!result || result.pid <= 0) throw new Error('runWineExe returned no process');
    AppSessionService.getInstance().registerDiagnosticLaunch(executable, result,
      settings.defaultDisplayMode, WinePresentationMode.DESKTOP);
    hilog.info(DOMAIN, 'EntryAbility',
      'automation launched pid=%{public}d backend=%{public}s graphicsPolicy=%{public}s exe=%{public}s envCount=%{public}d',
      result.pid, backend, graphicsExperiment || '(product)', executable, environment.length);
  }

  /**
   * WineHua smoke Want protocol adapted behind the product UI.  The runner is
   * a service and uses the same Native runWineProgram policy as normal product
   * launches; no upstream sidebar or page replaces the product launcher.
   */
  private async runSmokeLaunch(want: Want): Promise<void> {
    const parameters = want.parameters ?? {};
    const settingsStore = AppSettingsStore.getInstance();
    await settingsStore.initialize(this.context);
    const engine = WineEngineService.getInstance();
    engine.initialize(this.context);
    const settings = settingsStore.getGlobalSettings();
    const homeDirectory = '/storage/Users/currentUser/Download/com.vintage.pomelopro';
    const requestedSeconds = Number(parameters['winehua.long_seconds'] ?? 3600);
    const request: SmokeRequest = {
      suite: String(parameters['winehua.suite'] ?? 'core'),
      runId: String(parameters['winehua.run_id'] ?? `manual-${Date.now()}`),
      prefix: String(parameters['winehua.prefix'] ?? 'reuse') === 'clean' ? 'clean' : 'reuse',
      longSeconds: Number.isFinite(requestedSeconds) && requestedSeconds > 0 ? requestedSeconds : 3600,
      source: 'want'
    };
    const ready = await engine.ensureSmokeReady(settings.defaultDisplayMode,
      WinePresentationMode.DESKTOP, homeDirectory, request.prefix || 'reuse');
    if (!ready) throw new Error('Wine engine did not become ready for smoke');
    SmokeRunner.getInstance().start(request);
    hilog.info(DOMAIN, 'EntryAbility',
      'smoke runner accepted suite=%{public}s runId=%{public}s prefix=%{public}s',
      request.suite, request.runId, request.prefix || 'reuse');
  }

  private isPcWindowHost(): boolean {
    return DeviceCapabilityPolicy.usesManagedWineWindows();
  }

  onWindowStageDestroy(): void {
    // Main window is destroyed, release UI related resources
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onWindowStageDestroy');
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
    // Ability has brought to foreground
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onForeground');
    this.ensureDownloadGamesDirectory('onForeground');
  }

  onBackground(): void {
    // Ability has back to background
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onBackground');
  }
}
