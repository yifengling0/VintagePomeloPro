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

interface WindowState {
  version: number; x: number; y: number; width: number; height: number;
  minimized: number; activation: number; closed: number;
}
interface SteamWindowState {
  version: number; window: number; mode: number; minimized: number; activation: number; closed: number;
  moveRequest: number;
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
  private moveRequest: number = 0;

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
    if (fs.accessSync(this.statePath)) fs.unlinkSync(this.statePath);
    for (const name of ['wine-window-native.json', 'wine-window-command.txt']) {
      if (fs.accessSync(`${BRIDGE}/${name}`)) fs.unlinkSync(`${BRIDGE}/${name}`);
    }
    const windowsState = `C:${this.statePath.substring(driveC.length).replace(/\//g, '\\')}`;
    const launch = WineEngineService.getInstance().launchExecutable('C:\\hmosdeck\\hmosdeck-steam.exe',
      [], 'C:\\hmosdeck', [`HMOSDECK_WINDOW_STATE=${windowsState}`]);
    if (launch.pid <= 0 || !launch.sessionId || launch.reused) throw new Error('Wine 未创建 Steam 宿主进程');
    this.sessionId = launch.sessionId;
    this.started = Date.now();
    AppSessionService.getInstance().registerExternalLaunch('steam-linux-client',
      'C:\\hmosdeck\\hmosdeck-steam.exe', launch, DisplayMode.DESKTOP, WinePresentationMode.DESKTOP);
    this.timer = setInterval((): void => {
      try { this.tick(); } catch (error) {
        this.record(`binding_error:${(error as Error).message.substring(0, 160)}`);
      }
    }, 150);
    this.record('wine_window_started_waiting_for_xwayland');
  }

  private record(status: string): void {
    if (this.lastStatus === status) return;
    this.lastStatus = status;
    writeSmokeText(this.evidence, JSON.stringify({ status, source: this.source, host: this.host,
      sessionId: this.sessionId, elapsedMs: Date.now() - this.started,
      nativeSteam: true, pixelPolling: false }));
  }

  private tick(): void {
    const text = readSmokeText(this.statePath);
    if (!text || text.length > 1024) return;
    const state = JSON.parse(text) as WindowState;
    if (state.version !== 1) return;
    if (!this.syncPresentation(state)) return;
    if (state.closed === 1) {
      if (this.source > 0) testNapi.sendToplevelClose(this.source);
      this.record('wine_window_closed');
      this.stop(); return;
    }
    const session = testNapi.getWineSession(this.sessionId) as NativeWineSessionInfo | null;
    if (!session || session.toplevelId <= 0) {
      this.record('waiting_for_wine_window'); return;
    }
    const tasks = HostWindowTasks.getInstance().snapshot();
    const ownsProcess = smokeNapi.isLinuxSteamProcess;
    if (!ownsProcess) return;
    const host = tasks.find((item: HostWindowTask): boolean => item.toplevelId === session.toplevelId);
    const source = tasks.find((item: HostWindowTask): boolean => item.source === 'steam-linux' &&
      ownsProcess(item.clientPid));
    if (!host || !source || host.toplevelId === source.toplevelId) {
      this.record('waiting_for_native_steam_surface'); return;
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
      testNapi.activateToplevel(this.host);
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
    const key = `${native.window}:${native.mode}:${native.minimized}:${native.activation}`;
    if (key !== this.nativeState) {
      this.nativeState = key;
      writeSmokeText(`${this.statePath}.control`,
        `${++this.controlSerial} ${native.mode} ${native.minimized} 1\n`);
    }
    return true;
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
