import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import testNapi from 'libentry.so';
import smokeNapi from 'libsmoke_napi.so';
import { DisplayMode, NativeWineSessionInfo, WinePresentationMode } from '../model/AppModels';
import { AppSessionService } from './AppSessionService';
import { HostWindowTask, HostWindowTasks } from './HostWindowTasks';
import { WineWindowManager } from './WineWindowManager';
import { WineEngineService } from './WineEngineService';
import { AppSettingsStore } from './AppSettingsStore';
import { GamepadManager } from './GamepadManager';
import { InputProfileService } from './InputProfileService';
import { InputOverlayWindowCoordinator } from './InputOverlayWindowCoordinator';
import { ensureSmokeDirectory, readSmokeText, writeSmokeText } from './ManagedSmokePayloadService';
import { ConfigurationFiles } from './ConfigurationFiles';
import { parseLinuxSteamWindowPreferences } from '../model/LinuxSteamWindowPreferences';

interface WindowState {
  version: number; x: number; y: number; width: number; height: number;
  minimized: number; activation: number; closed: number; mode: number; controlSerial: number; geometryReady?: number;
}
interface SteamWindowState {
  version: number; window: number; mode: number; minimized: number; activation: number; closed: number;
  moveRequest: number;
  normalWidth?: number;
  normalHeight?: number;
}
const BRIDGE = '/data/storage/el2/base/files/steamohos-native/client-tmp';

// Wine owns a real HWND/taskbar entry; only its client content is foreign.
// Poll geometry/state, never pixels. All rendering and hit-testing stay native.
export class LinuxSteamWindowHost {
  private sessionId: string = '';
  private statePath: string = '';
  private timer: number = -1;
  private source: number = 0;
  private host: number = 0;
  private lastGeometry: string = '';
  private activation: number = -1;
  private evidence: string = '';
  private started: number = 0;
  private navigation: boolean = false;
  private nativeState: string = '';
  private controlSerial: number = 0;
  private previousMinimized: number = 0;
  private previousActivation: number = -1;
  private sourceKey: string = '';
  private hostKey: string = '';
  private lastStatus: string = '';
  private lastCandidates: string = '';
  private moveRequest: number = 0;
  private presentationSerial: number = 0;

  async start(context: common.Context, driveC: string, results: string): Promise<void> {
    if (!WineWindowManager.getInstance().getDesktopMode()) {
      throw new Error('Linux Steam 的 Wine 窗口需要现有虚拟桌面模式');
    }
    const directory = `${driveC}/hmosdeck`;
    if (!ensureSmokeDirectory(directory)) throw new Error('Cannot prepare Wine Steam host');
    const payload = await context.resourceManager.getRawFileContent('hmosdeck/hmosdeck-steam.exe');
    if (payload.byteLength < 1024 || payload.byteLength > 524288 || payload[0] !== 77 || payload[1] !== 90) {
      throw new Error('Invalid Wine Steam host executable');
    }
    const executable = `${directory}/hmosdeck-steam.exe`;
    const file = fs.openSync(executable, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
    try { fs.writeSync(file.fd, payload.buffer); } finally { fs.closeSync(file); }
    this.statePath = `${results}/wine-window-state.json`;
    this.evidence = `${results}/wine-window-binding.json`;
    // Commands belong to one HWND lifetime. A previous serial must never resize
    // the next host before its first native window has even been created.
    for (const path of [this.statePath, `${this.statePath}.control`]) {
      if (fs.accessSync(path)) fs.unlinkSync(path);
    }
    for (const name of ['wine-window-native.json', 'wine-window-command.txt', 'wine-window-geometry.txt']) {
      if (fs.accessSync(`${BRIDGE}/${name}`)) fs.unlinkSync(`${BRIDGE}/${name}`);
    }
    const windowsState = `C:${this.statePath.substring(driveC.length).replace(/\//g, '\\')}`;
    const environment = [`HMOSDECK_WINDOW_STATE=${windowsState}`];
    try {
      const saved = parseLinuxSteamWindowPreferences(ConfigurationFiles.text(
        ConfigurationFiles.read(`${BRIDGE}/wine-window-preferences.conf`, 96)));
      if (saved) environment.push(`HMOSDECK_WINDOW_SIZE=${saved.width}x${saved.height}`);
    } catch (_) {}
    const launch = WineEngineService.getInstance().launchExecutable('C:\\hmosdeck\\hmosdeck-steam.exe',
      [], 'C:\\hmosdeck', environment);
    if (launch.pid <= 0 || !launch.sessionId || launch.reused) throw new Error('Wine 未创建 Steam 宿主进程');
    this.sessionId = launch.sessionId;
    this.started = Date.now();
    AppSessionService.getInstance().registerExternalLaunch('steam-linux-client',
      'C:\\hmosdeck\\hmosdeck-steam.exe', launch, DisplayMode.DESKTOP, WinePresentationMode.DESKTOP, false);
    this.timer = setInterval((): void => {
      try { this.tick(); } catch (error) {
        this.record(`binding_error:${(error as Error).message.substring(0, 160)}`);
      }
    }, 150);
    this.record('wine_window_started_waiting_for_xwayland');
    // A cold Wine worker can take longer than five seconds even after the
    // desktop is ready. Bound the wait, stop on exit/cancellation, and accept
    // only this HWND lifetime's settled geometry.
    const deadline = Date.now() + 20000;
    while (Date.now() < deadline) {
      if (this.sessionId !== launch.sessionId) throw new Error('Steam 窗口启动已取消');
      const session = testNapi.getWineSession(launch.sessionId) as NativeWineSessionInfo | null;
      if (session?.state === 'stopped') {
        this.stop(); throw new Error('Steam 宿主窗口进程已退出，请重试');
      }
      const initial = readSmokeText(this.statePath);
      if (initial && initial.length <= 1024) {
        const state = JSON.parse(initial) as WindowState;
        if (state.version === 1 && state.closed === 1) {
          this.stop(); throw new Error('Steam 宿主窗口已关闭');
        }
        if (state.version === 1 && state.closed === 0 && state.geometryReady === 1 && Number.isSafeInteger(state.width) &&
          Number.isSafeInteger(state.height) && state.width >= 320 && state.width <= 4096 &&
          state.height >= 200 && state.height <= 4096 && state.width * state.height <= 8388608) {
          writeSmokeText(`${BRIDGE}/wine-window-geometry.txt`, `${state.width}x${state.height}\n`);
          return;
        }
      }
      await new Promise<void>((resolve: () => void): void => { setTimeout(resolve, 100); });
    }
    if (this.sessionId === launch.sessionId) this.stop();
    throw new Error('Steam 宿主窗口创建超时（20 秒），请重试');
  }

  async restore(): Promise<boolean> {
    return this.hostKey ? HostWindowTasks.getInstance().activate(this.hostKey) : false;
  }

  isForeground(): boolean {
    return this.host > 0 && !HostWindowTasks.getInstance().isLauncherForeground() &&
      testNapi.getKeyboardFocusedToplevel() === this.host;
  }

  private record(status: string): void {
    if (this.lastStatus === status) return;
    this.lastStatus = status;
    // Evidence is optional, including when called from the interval's catch.
    // A full diagnostic directory must not turn binding/cleanup into an
    // uncaught timer exception or stop an otherwise healthy native session.
    try {
      writeSmokeText(this.evidence, JSON.stringify({ status, source: this.source, host: this.host,
        sessionId: this.sessionId, elapsedMs: Date.now() - this.started,
        nativeSteam: true, pixelPolling: false }));
    } catch (_) {}
  }

  private tick(): void {
    const text = readSmokeText(this.statePath);
    if (!text || text.length > 1024) return;
    const state = JSON.parse(text) as WindowState;
    if (state.version !== 1) return;
    if (state.closed === 1) {
      if (this.source > 0) testNapi.sendToplevelClose(this.source);
      this.record('wine_window_closed');
      this.stop(); return;
    }
    if (!this.syncPresentation(state) || state.geometryReady === 0) return;
    const session = testNapi.getWineSession(this.sessionId) as NativeWineSessionInfo | null;
    if (!session || session.toplevelId <= 0) {
      this.record('waiting_for_wine_window'); return;
    }
    const ownsProcess = smokeNapi.isLinuxSteamProcess;
    if (!ownsProcess) return;
    const taskService = HostWindowTasks.getInstance();
    if (this.source <= 0) taskService.refreshLinuxSteamOwnership(ownsProcess);
    const tasks = taskService.snapshot();
    const host = tasks.find((item: HostWindowTask): boolean => item.toplevelId === session.toplevelId);
    const source = tasks.find((item: HostWindowTask): boolean => item.source === 'steam-linux' &&
      ownsProcess(item.clientPid));
    if (!host || !source || host.toplevelId === source.toplevelId) {
      const candidates = JSON.stringify(tasks.map((task: HostWindowTask): string =>
        `${task.toplevelId}:${task.clientPid}:${task.source}:${ownsProcess(task.clientPid) ? 1 : 0}`));
      if (candidates !== this.lastCandidates) {
        this.lastCandidates = candidates;
        try { writeSmokeText(`${this.evidence}.candidates`, candidates); } catch (_) {}
      }
      this.record(!host ? 'waiting_for_wine_host_task' : 'waiting_for_native_steam_surface'); return;
    }
    if ((this.sourceKey && this.sourceKey !== source.key) || (this.hostKey && this.hostKey !== host.key)) {
      this.record('window_identity_changed'); this.stop(); return;
    }
    this.sourceKey = source.key; this.hostKey = host.key;
    this.host = host.toplevelId; this.source = source.toplevelId;
    this.setNavigation(testNapi.getKeyboardFocusedToplevel() === this.host);
    if (state.minimized !== 0 || state.width < 1 || state.height < 1) return;
    const geometry = `${this.source}:${this.host}:${state.x}:${state.y}:${state.width}:${state.height}`;
    if (geometry !== this.lastGeometry) {
      if (!testNapi.bindExternalToplevel(this.source, this.host, source.clientPid, host.clientPid,
        state.x, state.y, state.width, state.height)) {
        this.record('native_binding_rejected'); return;
      }
      this.lastGeometry = geometry;
      testNapi.notifyToplevelResize(this.source, state.width, state.height);
      this.record('bound_to_wine_client_area');
    }
    if (state.activation !== this.activation) {
      this.activation = state.activation;
      const tasks = HostWindowTasks.getInstance();
      if (!tasks.isLauncherForeground() && !tasks.isSteamGameForeground()) testNapi.activateToplevel(this.host);
    }
  }

  private syncPresentation(state: WindowState): boolean {
    const raw = readSmokeText(`${BRIDGE}/wine-window-native.json`);
    if (!raw || raw.length > 1024) return true;
    const native = JSON.parse(raw) as SteamWindowState;
    if (native.version !== 1 || ![0, 1, 2].includes(native.mode) ||
      ![0, 1].includes(native.minimized)) return true;
    if (native.closed === 1) {
      this.record('native_steam_exited'); this.stop(); return false;
    }
    // Window withdrawal is a transition, not a request to leave fullscreen.
    // Keep the last presentation until Steam has selected the replacement.
    if (native.window === 0) return false;
    if (Number.isSafeInteger(native.moveRequest) && native.moveRequest > this.moveRequest) {
      this.moveRequest = native.moveRequest;
      if (this.source > 0 && this.host > 0 && native.mode === 0 && native.minimized === 0)
        testNapi.moveExternalHost(this.source, this.host);
    }
    // Wine taskbar restore is a request to remap the same native client.
    if ((this.previousMinimized === 1 && state.minimized === 0) ||
      (this.previousActivation >= 0 && this.previousActivation !== state.activation && state.minimized === 0)) {
      writeSmokeText(`${BRIDGE}/wine-window-command.txt`, `${++this.controlSerial} 1\n`);
    }
    this.previousMinimized = state.minimized;
    this.previousActivation = state.activation;
    const normalWidth = native.normalWidth ?? 0; const normalHeight = native.normalHeight ?? 0;
    const dimensions = Number.isSafeInteger(normalWidth) && Number.isSafeInteger(normalHeight) &&
      normalWidth >= 320 && normalWidth <= 4096 && normalHeight >= 200 && normalHeight <= 4096 &&
      normalWidth * normalHeight <= 8388608 ? ` ${normalWidth} ${normalHeight}` : '';
    const key = `${native.window}:${native.mode}:${native.minimized}:${native.activation}${dimensions}`;
    if (key !== this.nativeState) {
      this.nativeState = key;
      this.presentationSerial = ++this.controlSerial;
      // Steam may publish mode/activation changes while a Wine game owns
      // focus. Mirror geometry without issuing SetForegroundWindow then.
      const tasks = HostWindowTasks.getInstance();
      const activate = tasks.isLauncherForeground() || tasks.isSteamGameForeground() ? 0 : 1;
      writeSmokeText(`${this.statePath}.control`,
        `${this.presentationSerial} ${native.mode} ${native.minimized} ${activate}${dimensions}\n`);
    }
    // Ignore intermediate WM_MOVE/WM_SIZE publications from the previous mode.
    // Resize Xwayland only after the Wine host applied this exact request.
    return state.mode === native.mode && state.controlSerial >= this.presentationSerial;
  }

  stop(): void {
    if (this.timer >= 0) clearInterval(this.timer);
    this.timer = -1;
    this.setNavigation(false);
    if (this.host > 0) testNapi.unbindExternalToplevel(this.host);
    if (this.sessionId) testNapi.stopWineSession(this.sessionId);
    this.sessionId = '';
  }

  private setNavigation(enabled: boolean): void {
    if (this.navigation === enabled) return;
    this.navigation = enabled;
    const settings = AppSettingsStore.getInstance().getGlobalSettings();
    const profile = InputProfileService.getInstance().getProfile(enabled ? 'builtin-steam-navigation' :
      (settings.desktopInputProfileId || 'default'));
    const manager = GamepadManager.getInstance();
    manager.releaseAll(); manager.setHostNavigation(false);
    manager.setOutputMode(enabled ? 'dinput' : (settings.controllerOutputMode ?? 'dinput'));
    manager.setButtonMappings(profile.gamepadMappings);
    const coordinator = InputOverlayWindowCoordinator.getInstance();
    const snapshot = coordinator.getSnapshot();
    coordinator.update(snapshot.session, profile.id, snapshot.controlsVisible, snapshot.opacity);
  }
}
