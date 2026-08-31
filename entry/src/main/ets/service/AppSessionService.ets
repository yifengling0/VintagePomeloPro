import { common, Want } from '@kit.AbilityKit';
import { hilog } from './LogService';
import testNapi from 'libentry.so';
import {
  BOX64_PRESET_DEFAULT,
  AppDescriptor,
  DesktopWindowMode,
  DisplayMode,
  EngineState,
  GlobalSettings,
  LaunchKind,
  NativeLaunchResult,
  NativeProcessInfo,
  NativeWineSessionInfo,
  RuntimeSession,
  SessionState,
  UNITY_COMPAT_LAUNCH_ARGS,
  WinePresentationMode,
  isUnityCompatPreset,
  normalizeCatalogLaunchIdentity,
  normalizeLaunchPath,
  resolveBox64PresetEnv,
  resolveLaunchDisplayMode,
  resolveWinePresentationMode
} from '../model/AppModels';
import { isWineInfrastructurePath } from '../model/AppCatalogRules';
import { WineEngineService } from './WineEngineService';
import { WineWindowManager } from './WineWindowManager';
import { InputOverlayWindowCoordinator } from './InputOverlayWindowCoordinator';
import { AppSettingsStore } from './AppSettingsStore';
import { AppCatalogService } from './AppCatalogService';

const DOMAIN = 0x0000;

export enum LaunchConflictKind {
  NONE = 'none',
  DISPLAY_MODE = 'display-mode'
}

interface PendingToplevelAssociation {
  sessionId: string;
  clientPid: number;
  toplevelId: number;
}

const DESKTOP_SESSION_ID = 'desktop';
const DESKTOP_APP_ID = 'builtin-desktop';
const DESKTOP_EXECUTABLE = 'explorer.exe';
/* Guest Venus/virglrenderer 共用一条命令环 (no_multi_ring)。D3D11 cube 与
 * D3D12 gears/triangle 同时跑会把 Host queue 卡住（cube 掉到 0.4 FPS）。
 * 这些内建图形烟测互斥：启动其中一个时先停掉其余。 */
const VENUS_EXCLUSIVE_APP_IDS: string[] = [
  'builtin-dxvk-cube',
  'builtin-vkd3d-gears',
  'builtin-vkd3d-triangle'
];

interface PendingLaunchRequest {
  app: AppDescriptor;
  settings: GlobalSettings;
  mode: DisplayMode;
  presentationMode: WinePresentationMode;
  homeDirectory: string;
  resolve: (result: RuntimeSession | null) => void;
}
export class AppSessionService {
  private static instance: AppSessionService | null = null;
  private context: common.UIAbilityContext | null = null;
  private sessions: Map<string, RuntimeSession> = new Map<string, RuntimeSession>();
  private pendingToplevels: PendingToplevelAssociation[] = [];
  private desktopRootWaiters: Array<(session: RuntimeSession | null) => void> = [];
  private promoteRetries: Map<string, number> = new Map<string, number>();

  static getInstance(): AppSessionService {
    if (!AppSessionService.instance) AppSessionService.instance = new AppSessionService();
    return AppSessionService.instance;
  }

  initialize(context: common.UIAbilityContext): void {
    this.context = context;
    WineWindowManager.getInstance().setSessionAssociationCallback(
      (sessionId: string, clientPid: number, toplevelId: number): void => {
        this.associateToplevel(sessionId, clientPid, toplevelId);
      });
    // 引擎明确失败/停止是桌面根等待的终止信号：条件被破坏，立即放行等待者。
    WineEngineService.getInstance().subscribe((state: EngineState): void => {
      if (state === EngineState.ERROR || state === EngineState.STOPPED) {
        this.settleDesktopRootWaiters(null);
      }
    });
  }

  resolveMode(app: AppDescriptor, settings: GlobalSettings): DisplayMode {
    return resolveLaunchDisplayMode(app.launchTarget.kind, app.displayModeOverride,
      settings.defaultDisplayMode);
  }

  resolvePresentationMode(app: AppDescriptor, settings: GlobalSettings): WinePresentationMode {
    return resolveWinePresentationMode(app.launchTarget.kind, this.resolveMode(app, settings),
      AppSettingsStore.getInstance().usesManagedWineWindows(),
      WineWindowManager.getInstance().isDesktopMode);
  }

  hasModeConflict(app: AppDescriptor, settings: GlobalSettings): boolean {
    return this.getLaunchConflict(app, settings) === LaunchConflictKind.DISPLAY_MODE;
  }

  getLaunchConflict(app: AppDescriptor, settings: GlobalSettings): LaunchConflictKind {
    if (app.launchTarget.kind !== LaunchKind.WINE_EXE &&
      app.launchTarget.kind !== LaunchKind.WINE_SYSTEM &&
      app.launchTarget.kind !== LaunchKind.WINE_DESKTOP) {
      return LaunchConflictKind.NONE;
    }
    const engine = WineEngineService.getInstance();
    const currentMode = engine.getMode();
    const currentPresentation = engine.getPresentationMode();
    if (!currentMode || !currentPresentation || engine.getState() === EngineState.STOPPED) {
      return LaunchConflictKind.NONE;
    }
    const requestedMode = this.resolveMode(app, settings);
    const requestedPresentation = this.resolvePresentationMode(app, settings);
    // 期望模式 = 当前设备+用户设置应处的模式 (applyModePolicy 的判定):
    // PC 虚拟桌面/平板 → DESKTOP; PC 融合 → MANAGED_WINDOWS。
    // 若请求与期望一致, 引擎状态不对会自动 ensureReady 切换, 不打扰用户弹窗
    // (修复: PC 虚拟桌面下每次点桌面卡片都弹"切换显示模式")。
    const expected = WineWindowManager.getInstance().isDesktopMode
      ? WinePresentationMode.DESKTOP : WinePresentationMode.MANAGED_WINDOWS;
    if (requestedPresentation === expected) {
      return LaunchConflictKind.NONE;
    }
    if ((currentMode !== requestedMode || currentPresentation !== requestedPresentation) &&
      engine.getState() !== EngineState.STOPPED) {
      return LaunchConflictKind.DISPLAY_MODE;
    }
    return LaunchConflictKind.NONE;
  }

  async launch(app: AppDescriptor, settings: GlobalSettings, homeDirectory: string): Promise<RuntimeSession | null> {
    if (!app.available) return null;
    // PC/2-in-1 融合模式: 每个 Wine 程序独立窗口, 桌面 root 目标不适用。
    // 虚拟桌面模式 (VIRTUAL, isDesktopMode=true) 与平板同逻辑 — 放行桌面。
    if (app.launchTarget.kind === LaunchKind.WINE_DESKTOP &&
      AppSettingsStore.getInstance().usesManagedWineWindows() &&
      !WineWindowManager.getInstance().isDesktopMode) return null;
    if ((app.launchTarget.kind === LaunchKind.WINE_EXE ||
      app.launchTarget.kind === LaunchKind.WINE_SYSTEM ||
      app.launchTarget.kind === LaunchKind.WINE_DESKTOP) &&
      app.launchTarget.executable.trim().length === 0) return null;
    const mode = this.resolveMode(app, settings);
    const presentationMode = this.resolvePresentationMode(app, settings);

    // “Wine 桌面”不是普通可执行文件：先检查实际 desktop root 是否仍存在。
    // root 已被关闭时只补启 explorer desktop，不重置 prefix，也不结束其它窗口。
    if (app.launchTarget.kind === LaunchKind.WINE_DESKTOP &&
      presentationMode === WinePresentationMode.DESKTOP) {
      const engine = WineEngineService.getInstance();
      const wasDesktopReady = engine.isReady() &&
        engine.getPresentationMode() === WinePresentationMode.DESKTOP;
      const ready = await engine.ensureReady(mode, presentationMode, homeDirectory);
      if (!ready) return null;
      const currentDesktop = this.syncDesktopSessionFromNative();
      if (currentDesktop && currentDesktop.toplevelId > 0) {
        // The root event already starts the singleton DesktopAbility. Calling
        // startAbility again here creates a visible PC desktop pop-up on every
        // first launch; only explicitly restore a stopped/background ability.
        if (!WineWindowManager.getInstance().isDesktopAbilityForeground(currentDesktop.toplevelId)) {
          await this.resume(currentDesktop);
        } else {
          // The singleton DesktopAbility is already the visible host for this
          // root. Calling startAbility again on PC creates a second mission/
          // popup even though the Wine session is unchanged.
          try { testNapi.activateWineSession(currentDesktop.sessionId); } catch (_) {}
          try { testNapi.raiseToplevel(currentDesktop.toplevelId); } catch (_) {}
        }
        return currentDesktop;
      }
      // 首次启动时 native launch thread 正在创建 explorer，不能抢跑出第二个 shell。
      if (!wasDesktopReady) return this.ensureDesktopSession();
      // 已就绪引擎也给短暂的 surface 重建留出时间；稳定缺 root 才恢复 shell。
      const settlingDesktop = await this.waitForDesktopRoot();
      if (settlingDesktop && settlingDesktop.toplevelId > 0) return settlingDesktop;
      const restarted = engine.restartDesktopShell();
      if (!restarted || restarted.pid <= 0) return null;
      return this.ensureDesktopSession();
    }
    let existing = this.findByAppId(app.id);
    if (!existing) {
      existing = this.findByExecutable(app.launchTarget.executable);
      // The desktop shell is also explorer.exe, but it is not a File Explorer
      // window.  Do not let path-singleton reuse turn the system Explorer card
      // into a no-op that merely raises the desktop root.
      if (existing && existing.sessionId === DESKTOP_SESSION_ID && app.id !== DESKTOP_APP_ID) {
        existing = null;
      }
    }
    if (existing) {
      await this.resume(existing);
      return existing;
    }


    // 程序/游戏：事件驱动启动。
    // - 引擎已 READY 且模式匹配 → 直接启动；
    // - 未就绪 → 挂到引擎 READY 事件，由"桌面就绪"事件触发真正启动，
    //   避免 ensureReady 返回后继续执行造成的竞态（桌面会话未入表、
    //   DesktopAbility startAbility 阻塞等）。
    const engine = WineEngineService.getInstance();
    engine.selectHostGraphicsBackendForExecutable(app.launchTarget.executable);
    if (engine.isReady() && engine.getPresentationMode() === presentationMode) {
      return this.launchReady(app, settings, mode, presentationMode, homeDirectory);
    }
    return this.deferLaunchUntilReady(app, settings, mode, presentationMode, homeDirectory);
  }

  private async launchReady(app: AppDescriptor, settings: GlobalSettings,
    mode: DisplayMode, presentationMode: WinePresentationMode,
    homeDirectory: string): Promise<RuntimeSession | null> {
    const engine = WineEngineService.getInstance();
    if (engine.getState() !== EngineState.READY ||
      engine.getPresentationMode() !== presentationMode) {
      // 状态在检查后变化：重新走事件队列。
      return this.deferLaunchUntilReady(app, settings, mode, presentationMode, homeDirectory);
    }

    const hostGraphicsBackend =
      engine.selectHostGraphicsBackendForExecutable(app.launchTarget.executable);
    if (engine.needsHostGraphicsRestart()) {
      hilog.info(DOMAIN, '[SESS]', 'launch: graphics host restart app=%{public}s backend=%{public}s',
        app.id, hostGraphicsBackend);
      this.sessions.clear();
      this.pendingToplevels = [];
      try {
        await InputOverlayWindowCoordinator.getInstance().setVisible(false);
      } catch (_) {
      }
      const hostReady = await engine.ensureReady(mode, presentationMode, homeDirectory);
      if (!hostReady) {
        hilog.error(DOMAIN, '[SESS]', 'launch: graphics host restart failed app=%{public}s',
          app.id);
        return null;
      }
    }

    if (presentationMode === WinePresentationMode.DESKTOP) {
      // 游戏/程序：前置条件是桌面根已注册（toplevelId > 0），不是仅 READY。
      // READY 事件可能先于 desktop_root/created 到达，此时直接 runWineExe
      // 会落在窗口链路未就绪的窗口期（表现为"点了没反应/进程没起来"）。
      // 这里只等注册，绝不 await resume()/startAbility——冷启动时
      // DesktopAbility 正由 desktop_root 事件启动，startAbility 可能阻塞
      // 导致启动永久挂起。
      const desktopSession = await this.waitForDesktopRootRegistered();
      if (!desktopSession || desktopSession.toplevelId <= 0) {
        hilog.warn(DOMAIN, '[SESS]', 'launch: desktop root unavailable, abort app=%{public}s',
          app.id);
        return null;
      }
      // 先跳到桌面再启动 exe：非阻塞地把桌面窗口（DWA）带到前台（不 await，
      // 冷启动时 startAbility 可能阻塞启动链）。exe 启动后其窗口出现在桌面
      // 合成里，窗口 created 后再自动切到该窗口（auto-promote）。
      if (!WineWindowManager.getInstance().isDesktopAbilityForeground(desktopSession.toplevelId)) {
        this.resume(desktopSession).catch((err: Error) => {
          hilog.warn(DOMAIN, '[SESS]', 'launch: jump-to-desktop failed err=%{public}s',
            JSON.stringify(err));
        });
      } else {
        try { testNapi.activateWineSession(desktopSession.sessionId); } catch (_) {}
        try { testNapi.raiseToplevel(desktopSession.toplevelId); } catch (_) {}
      }
    }

    this.stopVenusExclusivePeers(app);
    if (this.isVenusExclusiveSmoke(app.id)) {
      await new Promise<void>((resolve: () => void): void => {
        setTimeout((): void => { resolve(); }, 500);
      });
    }

    // 每游戏兼容性预设优先于全局预设；'default' 表示跟随全局。
    const appSettings = AppSettingsStore.getInstance().getAppSettings(app.id);
    const presetId = appSettings.box64Preset !== BOX64_PRESET_DEFAULT
      ? appSettings.box64Preset : settings.box64Preset;
    const envOverrides = resolveBox64PresetEnv(presetId);
    hilog.info(DOMAIN, '[SESS]', 'launch: app=%{public}s preset=%{public}s env=%{public}s',
      app.id, presetId, JSON.stringify(envOverrides));
    // Unity 兼容档：自动追加 -force-gfx-direct（跳过 GDI 间接渲染），
    // 已存在同名参数时不重复注入；只对选中 Unity 档的游戏生效。
    const launchArguments = app.launchTarget.arguments.slice();
    if (isUnityCompatPreset(presetId)) {
      for (const extra of UNITY_COMPAT_LAUNCH_ARGS) {
        if (!launchArguments.some((arg: string) => arg.toLowerCase() === extra.toLowerCase())) {
          launchArguments.push(extra);
        }
      }
    }
    const result = WineEngineService.getInstance().launchExecutable(
      app.launchTarget.executable, launchArguments,
      app.launchTarget.workingDirectory, envOverrides) as NativeLaunchResult;
    if (!result || result.pid <= 0) {
      hilog.error(DOMAIN, '[SESS]', 'launch: runWineExe FAILED app=%{public}s exe=%{public}s state=%{public}s',
        app.id, app.launchTarget.executable, engine.getState());
      return null;
    }
    hilog.info(DOMAIN, '[SESS]', 'launch: spawned app=%{public}s pid=%{public}d session=%{public}s',
      app.id, result.pid, result.sessionId);
    const session: RuntimeSession = {
      sessionId: result.sessionId || 'wine-' + result.pid,
      appId: app.id,
      executable: app.launchTarget.executable,
      pid: result.pid,
      displayMode: mode,
      presentationMode: presentationMode,
      state: SessionState.RUNNING,
      toplevelId: 0,
      startedAt: Date.now()
    };
    if (result.reused) {
      const nativeSession = testNapi.getWineSession(result.sessionId) as NativeWineSessionInfo | null;
      if (nativeSession) session.toplevelId = nativeSession.toplevelId;
      testNapi.activateWineSession(result.sessionId);
    }
    this.sessions.set(session.sessionId, session);
    this.resolvePendingToplevel(session);
    // 记录最近启动 (供"最近"分类, 最新在前, 持久化)
    AppSettingsStore.getInstance().recordRecentLaunch(app.id).catch(() => {});
    // created 事件可能早于会话入表到达；若关联仍缺失，直接从 native 会话补
    // toplevelId，保证二次点击 resume 能唤起窗口而不是静默 no-op。
    if (session.toplevelId <= 0) {
      try {
        const nativeSession = testNapi.getWineSession(session.sessionId) as NativeWineSessionInfo | null;
        if (nativeSession && nativeSession.toplevelId > 0) {
          session.toplevelId = nativeSession.toplevelId;
          hilog.info(DOMAIN, '[SESS]', 'launch: native toplevel fallback sid=%{public}s tl=%{public}d',
            session.sessionId, session.toplevelId);
        }
      } catch (_) {
      }
    }
    return session;
  }

  /**
   * Register a process launched by the diagnostics Want path with the normal
   * desktop session lifecycle.  The launcher intentionally accepts extra
   * per-process environment variables, so it cannot call launch() directly;
   * without this registration, however, a newly-created Wine toplevel remains
   * behind the desktop root and visual A/B tests capture a black desktop.
   */
  registerDiagnosticLaunch(executable: string, result: NativeLaunchResult,
    mode: DisplayMode, presentationMode: WinePresentationMode): RuntimeSession | null {
    if (!result || result.pid <= 0) return null;
    const session: RuntimeSession = {
      sessionId: result.sessionId || 'wine-' + result.pid,
      appId: 'diagnostic-' + result.pid,
      executable: executable,
      pid: result.pid,
      displayMode: mode,
      presentationMode: presentationMode,
      state: SessionState.RUNNING,
      toplevelId: 0,
      startedAt: Date.now()
    };
    this.sessions.set(session.sessionId, session);
    this.resolvePendingToplevel(session);
    if (session.toplevelId <= 0) {
      try {
        const nativeSession = testNapi.getWineSession(session.sessionId) as NativeWineSessionInfo | null;
        if (nativeSession && nativeSession.toplevelId > 0) session.toplevelId = nativeSession.toplevelId;
      } catch (_) {
      }
    }
    if (session.toplevelId > 0) this.promoteDesktopSession(session);
    hilog.info(DOMAIN, '[SESS]',
      'diagnostic launch registered sid=%{public}s pid=%{public}d tl=%{public}d',
      session.sessionId, session.pid, session.toplevelId);
    return session;
  }

  private pendingLaunches: Array<PendingLaunchRequest> = [];
  private pendingLaunchListenerInstalled: boolean = false;
  private drainingPendingLaunches: boolean = false;

  private ensurePendingLaunchListener(): void {
    if (this.pendingLaunchListenerInstalled) return;
    this.pendingLaunchListenerInstalled = true;
    let sawPreparing = false;
    WineEngineService.getInstance().subscribe((state: EngineState): void => {
      if (state === EngineState.PREPARING || state === EngineState.SWITCHING) sawPreparing = true;

      if (state === EngineState.READY) {
        this.drainPendingLaunches();
      } else if (sawPreparing && (state === EngineState.ERROR || state === EngineState.STOPPED)) {
        // 只有引擎真实经过启动后失败/停止才拒绝挂起请求；初始 STOPPED 快照
        // 只是引擎即将启动，不能把请求直接放弃。
        const pending = this.pendingLaunches.splice(0);
        pending.forEach((req: PendingLaunchRequest) => req.resolve(null));
      }
    });
  }

  private deferLaunchUntilReady(app: AppDescriptor, settings: GlobalSettings,
    mode: DisplayMode, presentationMode: WinePresentationMode,
    homeDirectory: string): Promise<RuntimeSession | null> {
    return new Promise<RuntimeSession | null>((resolve: (result: RuntimeSession | null) => void): void => {
      this.pendingLaunches.push({ app, settings, mode, presentationMode, homeDirectory, resolve });

      this.ensurePendingLaunchListener();
      // 触发引擎启动（不 await）；引擎 READY 事件会驱动队列真正执行。
      WineEngineService.getInstance().ensureReady(mode, presentationMode, homeDirectory).catch(() => {});
      // 竞态兜底：若已经就绪立即排水。
      if (WineEngineService.getInstance().isReady()) this.drainPendingLaunches();
    });
  }

  private drainPendingLaunches(): void {
    if (this.drainingPendingLaunches) return;
    this.drainingPendingLaunches = true;
    const pending = this.pendingLaunches.splice(0);

    for (const req of pending) {
      const engine = WineEngineService.getInstance();
      if (!engine.isReady() || engine.getPresentationMode() !== req.presentationMode) {
        // 状态又切走（重启/切换模式中）：请求重新入队，等下一次 READY 事件
        // 再排水，而不是静默判失败（否则表现为"点了没反应"）。
        const state = engine.getState();
        if (state === EngineState.PREPARING || state === EngineState.SWITCHING) {
          this.pendingLaunches.push(req);
        } else {
          req.resolve(null);
        }
        continue;
      }
      this.launchReady(req.app, req.settings, req.mode, req.presentationMode, req.homeDirectory)
        .then((session: RuntimeSession | null): void => req.resolve(session))
        .catch(() => req.resolve(null));
    }
    this.drainingPendingLaunches = false;
  }

  async switchModeAndLaunch(app: AppDescriptor, settings: GlobalSettings, homeDirectory: string): Promise<RuntimeSession | null> {
    this.sessions.clear();
    this.pendingToplevels = [];
    await WineEngineService.getInstance().switchDisplaySession();
    return this.launch(app, settings, homeDirectory);
  }

  async resume(session: RuntimeSession): Promise<void> {
    if (!this.context) return;
    try { testNapi.activateWineSession(session.sessionId); } catch (_) {}
    if (session.toplevelId <= 0) {
      // ArkTS 侧关联可能因 created 事件缺 sessionId 而丢失，从 native 会话补回。
      try {
        const nativeSession = testNapi.getWineSession(session.sessionId) as NativeWineSessionInfo | null;
        if (nativeSession && nativeSession.toplevelId > 0) {
          session.toplevelId = nativeSession.toplevelId;
          hilog.info(DOMAIN, '[SESS]', 'resume: native toplevel fallback sid=%{public}s tl=%{public}d',
            session.sessionId, session.toplevelId);
        }
      } catch (_) {
      }
    }
    if (session.toplevelId > 0) {
      try { testNapi.raiseToplevel(session.toplevelId); } catch (_) {}
    }
    if (session.presentationMode === WinePresentationMode.DESKTOP) {
      if (session.toplevelId <= 0) return;
      // 浮窗模式: 桌面由 Index 承载 (DesktopLayer floating 切换), 不启动 DesktopAbility。
      // 激活/提升程序窗口到桌面 root 合成, 并通知 Index 进全屏显示 (auto-promote
      // 依赖本方法, 否则窗口不出现且 schedulePromoteRetry 无限重试)。
      if (AppSettingsStore.getInstance().getGlobalSettings().desktopWindowMode === DesktopWindowMode.FLOATING) {
        try { testNapi.activateWineSession(session.sessionId); } catch (_) {}
        try { testNapi.raiseToplevel(session.toplevelId); } catch (_) {}
        AppStorage.setOrCreate('desktopEnterFullscreen', true);
        return;
      }
      const want: Want = {
        bundleName: 'com.vintage.pomelopro',
        abilityName: 'DesktopAbility',
        parameters: { toplevelId: session.toplevelId }
      };
      await this.context.startAbility(want);
    } else if (session.presentationMode === WinePresentationMode.MANAGED_WINDOWS) {
      if (session.toplevelId <= 0) return;
      const managedWindow = WineWindowManager.getInstance().getWindow(session.toplevelId);
      if (managedWindow) {
        await managedWindow.showWindow();
        return;
      }
      const want: Want = {
        bundleName: 'com.vintage.pomelopro',
        abilityName: 'WineWindowAbility',
        parameters: { toplevelId: session.toplevelId }
      };
      await this.context.startAbility(want);
    }
  }

  stop(session: RuntimeSession): void {
    if (session.pid > 0 && !testNapi.stopWineSession(session.sessionId)) testNapi.killProcess(session.pid);
    session.state = SessionState.STOPPED;
    this.sessions.delete(session.sessionId);
  }

  /** Stop a card even when its ArkTS session was lost but native Wine still
   * reports the process. This is also the fallback used by the running filter. */
  stopApp(app: AppDescriptor): boolean {
    const session = this.findByAppId(app.id);
    if (session) {
      this.stop(session);
      return true;
    }
    const expectedPath = normalizeCatalogLaunchIdentity(app.launchTarget.executable);
    if (expectedPath.length === 0) return false;
    let stopped = false;
    this.getNativeProcesses().forEach((process: NativeProcessInfo): void => {
      if (process.state !== 'running' ||
        normalizeCatalogLaunchIdentity(process.path) !== expectedPath) return;
      let processStopped = false;
      try {
        if (process.sessionId && process.sessionId.length > 0) {
          processStopped = testNapi.stopWineSession(process.sessionId);
        }
      } catch (_) {
        processStopped = false;
      }
      if (!processStopped) {
        try { processStopped = testNapi.killProcess(process.pid); } catch (_) { processStopped = false; }
      }
      stopped = processStopped || stopped;
    });
    return stopped;
  }

  async stopAll(): Promise<void> {
    this.sessions.clear();
    this.pendingToplevels = [];
    await InputOverlayWindowCoordinator.getInstance().setVisible(false);
    await WineEngineService.getInstance().stopAll();
  }

  async restartEngine(mode: DisplayMode, homeDirectory: string): Promise<boolean> {
    this.sessions.clear();
    this.pendingToplevels = [];
    await InputOverlayWindowCoordinator.getInstance().setVisible(false);
    const presentationMode = resolveWinePresentationMode(LaunchKind.WINE_EXE, mode,
      AppSettingsStore.getInstance().usesManagedWineWindows());
    return WineEngineService.getInstance().restart(mode, presentationMode, homeDirectory);
  }

  async resetPrefix(): Promise<void> {
    this.sessions.clear();
    this.pendingToplevels = [];
    await InputOverlayWindowCoordinator.getInstance().setVisible(false);
    const preserve = AppSettingsStore.getInstance().getGlobalSettings().preserveUserDataOnReset;
    const home = AppCatalogService.getInstance().getWineHomeDirectory();
    await WineEngineService.getInstance().resetPrefix(preserve, home);
  }

  getSessions(): RuntimeSession[] {
    this.reconcile();
    const result: RuntimeSession[] = [];
    this.sessions.forEach((session: RuntimeSession) => result.push(session));
    return result;
  }

  getSession(sessionId: string): RuntimeSession | null {
    this.reconcile();
    return this.sessions.get(sessionId) ?? null;
  }

  getRunningPaths(): Set<string> {
    const result: Set<string> = new Set<string>();
    this.reconcile();
    this.sessions.forEach((session: RuntimeSession) => {
      if (session.state !== SessionState.STOPPED && session.state !== SessionState.ERROR) {
        result.add(normalizeLaunchPath(session.executable));
      }
    });
    this.getNativeProcesses().forEach((process: NativeProcessInfo) => {
      if (process.state === 'running' && !isWineInfrastructurePath(process.path)) {
        result.add(normalizeLaunchPath(process.path));
      }
    });
    return result;
  }

  getRunningAppIds(): Set<string> {
    const result: Set<string> = new Set<string>();
    this.reconcile();
    this.sessions.forEach((session: RuntimeSession) => {
      if (session.state !== SessionState.STOPPED && session.state !== SessionState.ERROR) {
        result.add(session.appId);
      }
    });
    return result;
  }

  findByAppId(appId: string): RuntimeSession | null {
    this.reconcile();
    let found: RuntimeSession | null = null;
    this.sessions.forEach((session: RuntimeSession) => {
      if (session.appId === appId && session.state !== SessionState.STOPPED) found = session;
    });
    return found;
  }

  findByExecutable(path: string): RuntimeSession | null {
    const normalized = normalizeLaunchPath(path);
    this.reconcile();
    let found: RuntimeSession | null = null;
    this.sessions.forEach((session: RuntimeSession) => {
      if (normalizeLaunchPath(session.executable) === normalized && session.state !== SessionState.STOPPED) found = session;
    });
    return found;
  }

  associateToplevel(sessionId: string, clientPid: number, toplevelId: number): void {
    if (!sessionId && clientPid <= 0) {
      hilog.warn(DOMAIN, '[SESS]', 'associateToplevel with empty sessionId/clientPid tl=%{public}d (created event lacks identity)',
        toplevelId);
    }
    let matched: RuntimeSession | null = sessionId ? this.sessions.get(sessionId) ?? null : null;
    if (!matched && clientPid > 0) {
      this.sessions.forEach((session: RuntimeSession) => {
        if (session.pid === clientPid) matched = session;
      });
    }
    if (!matched && sessionId === DESKTOP_SESSION_ID &&
      WineEngineService.getInstance().getPresentationMode() === WinePresentationMode.DESKTOP) {
      matched = this.upsertDesktopSession(toplevelId, clientPid);
    }
    if (matched) {
      const wasUnmapped = matched.toplevelId <= 0;
      if (matched.pid <= 0 && clientPid > 0) matched.pid = clientPid;
      if (sessionId === DESKTOP_SESSION_ID || matched.toplevelId === 0) matched.toplevelId = toplevelId;
      matched.state = SessionState.RUNNING;
      if (sessionId === DESKTOP_SESSION_ID && matched.toplevelId > 0) {
        this.settleDesktopRootWaiters(matched);
      }
      // 程序窗口首次映射：非阻塞地把桌面窗口（DWA）切到该窗口，恢复
      // "点卡片 → 自动跳到桌面/程序视图"。created 事件晚于 runWineExe，
      // 此时 fire-and-forget startAbility 不会阻塞启动链（冷启动卡死
      // 只在 launch 路径 await startAbility 时发生）。
      if (sessionId !== DESKTOP_SESSION_ID && wasUnmapped && matched.toplevelId > 0)
        this.promoteDesktopSession(matched);
    } else if (toplevelId > 0) {
      this.pendingToplevels.push({ sessionId: sessionId, clientPid: clientPid, toplevelId: toplevelId });
      if (this.pendingToplevels.length > 16) this.pendingToplevels.shift();
    }
  }

  private resolvePendingToplevel(session: RuntimeSession): void {
    let matchIndex = -1;
    for (let index = 0; index < this.pendingToplevels.length; index++) {
      const pending = this.pendingToplevels[index];
      if ((pending.sessionId && pending.sessionId === session.sessionId) ||
        (pending.clientPid > 0 && pending.clientPid === session.pid)) {
        session.toplevelId = pending.toplevelId;
        matchIndex = index;
        break;
      }
    }
    if (matchIndex >= 0) this.pendingToplevels.splice(matchIndex, 1);
  }

  private promoteDesktopSession(session: RuntimeSession): void {
    if (session.presentationMode !== WinePresentationMode.DESKTOP ||
      !WineEngineService.getInstance().isReady() || session.toplevelId <= 0) return;
    hilog.info(DOMAIN, '[SESS]', 'auto-promote session=%{public}s tl=%{public}d',
      session.sessionId, session.toplevelId);
    const tl = session.toplevelId;
    const sid = session.sessionId;
    // DWA 已在前台显示桌面时，程序窗口会直接出现在桌面合成里，无需再
    // startAbility（引擎就绪瞬间重复 startAbility 偶发 201 被拒），只做轻量激活。
    const rootId = WineWindowManager.getInstance().getDesktopRootId();
    if (rootId > 0 && WineWindowManager.getInstance().isDesktopAbilityForeground(rootId)) {
      try { testNapi.activateWineSession(sid); } catch (_) {}
      try { testNapi.raiseToplevel(tl); } catch (_) {}
      return;
    }
    this.resume(session).catch((err: Error) => {
      hilog.warn(DOMAIN, '[SESS]',
        'auto-promote resume failed sid=%{public}s name=%{public}s msg=%{public}s',
        sid, err.name, err.message);
    });
    this.schedulePromoteRetry(sid, tl, 0);
  }

  private isVenusExclusiveSmoke(appId: string): boolean {
    return VENUS_EXCLUSIVE_APP_IDS.indexOf(appId) >= 0;
  }

  private isVenusExclusiveExecutable(path: string): boolean {
    const normalized = normalizeLaunchPath(path).toLowerCase();
    return normalized.indexOf('smoke') >= 0 &&
      (normalized.indexOf('winehua_d3d_switch_cube.exe') >= 0 ||
        normalized.indexOf('gears.exe') >= 0 ||
        normalized.indexOf('triangle.exe') >= 0);
  }

  private stopVenusExclusivePeers(app: AppDescriptor): void {
    if (!this.isVenusExclusiveSmoke(app.id)) return;
    const selfPath = normalizeLaunchPath(app.launchTarget.executable);
    const snapshot: RuntimeSession[] = [];
    this.sessions.forEach((session: RuntimeSession) => snapshot.push(session));
    snapshot.forEach((session: RuntimeSession) => {
      if (session.appId === app.id || session.state === SessionState.STOPPED) return;
      if (!this.isVenusExclusiveSmoke(session.appId)) return;
      hilog.info(DOMAIN, '[SESS]',
        'launch: stop Venus peer app=%{public}s before app=%{public}s',
        session.appId, app.id);
      this.stop(session);
    });
    this.getNativeProcesses().forEach((process: NativeProcessInfo): void => {
      if (process.state !== 'running') return;
      if (normalizeLaunchPath(process.path) === selfPath) return;
      if (!this.isVenusExclusiveExecutable(process.path)) return;
      try {
        if (process.sessionId && process.sessionId.length > 0) {
          testNapi.stopWineSession(process.sessionId);
        } else {
          testNapi.killProcess(process.pid);
        }
      } catch (_) {}
    });
  }

  private getNativeProcesses(): NativeProcessInfo[] {
    try {
      return testNapi.getProcessList() as NativeProcessInfo[];
    } catch (_) {
      return [];
    }
  }

  private async ensureDesktopSession(): Promise<RuntimeSession | null> {
    // 桌面根注册事件或引擎失败信号是唯二的终止条件；首次 prefix 初始化慢
    // 只会让等待更久，不会把“还在正常初始化”误判成失败。
    return this.waitForDesktopRoot();
  }

  /** 事件驱动地等待桌面根注册（toplevelId > 0），不做 resume/startAbility。
   *  root 注册（created/desktop_root 事件或状态同步发现）即成功放行，引擎
   *  ERROR/STOPPED 即明确失败；不依赖固定秒数轮询。 */
  private async waitForDesktopRootRegistered(): Promise<RuntimeSession | null> {
    const existing = this.sessions.get(DESKTOP_SESSION_ID) ?? null;
    if (existing && existing.toplevelId > 0) return existing;
    let session = this.syncDesktopSessionFromNative();
    if (session && session.toplevelId > 0) return session;
    return new Promise<RuntimeSession | null>((resolve: (result: RuntimeSession | null) => void): void => {
      const finish = (result: RuntimeSession | null): void => {
        const index = this.desktopRootWaiters.indexOf(finish);
        if (index >= 0) this.desktopRootWaiters.splice(index, 1);
        resolve(result);
      };
      this.desktopRootWaiters.push(finish);
      // 订阅后再查一次，避免 root 在“检查”与“订阅”之间注册而漏掉事件。
      const recheck = this.syncDesktopSessionFromNative();
      if (recheck && recheck.toplevelId > 0) {
        finish(recheck);
        return;
      }
      const engineState = WineEngineService.getInstance().getState();
      if (engineState !== EngineState.READY &&
        engineState !== EngineState.PREPARING &&
        engineState !== EngineState.SWITCHING) {
        finish(null);
      }
    });
  }

  /** 等待 explorer surface 关联为 desktop root；不创建或重置任何 Wine 资源。
   *  在注册等待之上补一次前台激活（仅当 DesktopAbility 未在前台时）。 */
  private async waitForDesktopRoot(): Promise<RuntimeSession | null> {
    const session = await this.waitForDesktopRootRegistered();
    if (!session || session.toplevelId <= 0) return null;
    if (!WineWindowManager.getInstance().isDesktopAbilityForeground(session.toplevelId)) {
      await this.resume(session);
    }
    return session;
  }

  private syncDesktopSessionFromNative(): RuntimeSession | null {
    let nativeSession: NativeWineSessionInfo | null = null;
    try {
      nativeSession = testNapi.getWineSession(DESKTOP_SESSION_ID) as NativeWineSessionInfo | null;
    } catch (_) {
      nativeSession = null;
    }
    if (!nativeSession || nativeSession.pid <= 0) {
      const existing = this.sessions.get(DESKTOP_SESSION_ID) ?? null;
      if (existing && existing.toplevelId > 0 &&
        WineEngineService.getInstance().getState() === EngineState.READY) {
        return existing;
      }
      this.sessions.delete(DESKTOP_SESSION_ID);
      return null;
    }
    const session = this.upsertDesktopSession(nativeSession.toplevelId, nativeSession.pid);
    if (session.toplevelId > 0) this.settleDesktopRootWaiters(session);
    return session;
  }

  private settleDesktopRootWaiters(session: RuntimeSession | null): void {
    const waiters = this.desktopRootWaiters;
    this.desktopRootWaiters = [];
    waiters.forEach((resolve: (result: RuntimeSession | null) => void): void => resolve(session));
  }

  /** 自动拉起兜底：startAbility 在引擎就绪瞬间偶发被 AMS 拒绝，1.2s 后若
   *  DWA 仍未切到目标窗口则重试，最多 2 次；会话/窗口已变化则放弃。 */
  private schedulePromoteRetry(sessionId: string, tl: number, attempt: number): void {
    if (attempt >= 2) return;
    setTimeout((): void => {
      const session = this.sessions.get(sessionId);
      if (!session || session.toplevelId !== tl) return;
      if (WineWindowManager.getInstance().isDesktopAbilityForeground(tl)) return;
      hilog.info(DOMAIN, '[SESS]', 'auto-promote retry #%{public}d sid=%{public}s tl=%{public}d',
        attempt + 1, sessionId, tl);
      this.resume(session).catch((err: Error) => {
        hilog.warn(DOMAIN, '[SESS]', 'auto-promote retry failed sid=%{public}s err=%{public}s',
          sessionId, JSON.stringify(err));
      });
      this.schedulePromoteRetry(sessionId, tl, attempt + 1);
    }, 1200);
  }

  private upsertDesktopSession(toplevelId: number, pid: number): RuntimeSession {
    const existing = this.sessions.get(DESKTOP_SESSION_ID);
    if (existing) {
      if (pid > 0) existing.pid = pid;
      if (toplevelId > 0) existing.toplevelId = toplevelId;
      existing.state = SessionState.RUNNING;
      return existing;
    }
    const session: RuntimeSession = {
      sessionId: DESKTOP_SESSION_ID,
      appId: DESKTOP_APP_ID,
      executable: DESKTOP_EXECUTABLE,
      pid: pid > 0 ? pid : 0,
      displayMode: DisplayMode.DESKTOP,
      presentationMode: WinePresentationMode.DESKTOP,
      state: SessionState.RUNNING,
      toplevelId: toplevelId > 0 ? toplevelId : 0,
      startedAt: Date.now()
    };
    this.sessions.set(DESKTOP_SESSION_ID, session);
    this.resolvePendingToplevel(session);
    return session;
  }

  private reconcile(): void {
    if (WineEngineService.getInstance().getState() === EngineState.READY &&
      WineEngineService.getInstance().getPresentationMode() === WinePresentationMode.DESKTOP) {
      this.syncDesktopSessionFromNative();
    }
    const processes = this.getNativeProcesses();
    const activePids: Set<number> = new Set<number>();
    processes.forEach((process: NativeProcessInfo) => activePids.add(process.pid));
    const removed: string[] = [];
    this.sessions.forEach((session: RuntimeSession, id: string) => {
      if (session.pid > 0 && !activePids.has(session.pid)) removed.push(id);
      if (session.pid === 0 && WineEngineService.getInstance().getState() !== EngineState.READY) removed.push(id);
    });
    removed.forEach((id: string) => {
      const stale = this.sessions.get(id);
      hilog.info(DOMAIN, '[SESS]', 'reconcile removes session=%{public}s pid=%{public}d state=%{public}s',
        id, stale ? stale.pid : -1, stale ? stale.state : 'gone');
      this.sessions.delete(id);
    });
  }
}
