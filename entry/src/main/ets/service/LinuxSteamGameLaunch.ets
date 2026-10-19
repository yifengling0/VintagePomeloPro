import smokeNapi, { LinuxSteamLaunchRequest } from 'libsmoke_napi.so';
import testNapi from 'libentry.so';
import { fileIo as fs } from '@kit.CoreFileKit';
import { readSmokeText } from './ManagedSmokePayloadService';
import { promptAction } from '@kit.ArkUI';
import { DisplayMode, NativeProcessInfo, WinePresentationMode } from '../model/AppModels';
import { AppSessionService } from './AppSessionService';
import { HostWindowTasks } from './HostWindowTasks';
import { WineEngineService } from './WineEngineService';
import { writeSmokeText } from './ManagedSmokePayloadService';

interface GameSession { id: number; appId: string; sessionId: string; pid: number; }
interface LibraryGame { appId: string; name: string; state: string; installedDirectory: string; }
interface LibraryState { folders: string; games: LibraryGame[]; }

interface LaunchEvidence {
  source: string; appId: string; status: string; steamApiAuthenticationVerified: boolean;
  exitCode?: number | null; exitCodeSource?: string;
}

// Research-only adapter; Wine owns graphics, prefix, launch policy and processes.
export class LinuxSteamGameLaunch {
  private timer: number = -1;
  private sessions: GameSession[] = [];
  private results = '';
  private zRoot = '';
  private library = '';
  private ticks = 0;
  private windowsPath(path: string): string {
    return `Z:${path.substring(this.zRoot.length).replace(/\//g, '\\')}`;
  }
  start(library: string, results: string): void {
    if (!smokeNapi.startLinuxSteamLaunchBroker || !smokeNapi.startLinuxSteamLaunchBroker(library)) {
      throw new Error('Cannot create Linux Steam game launch service');
    }
    this.results = results;
    this.library = library;
    this.zRoot = library.substring(0, library.length - '/SteamLinuxARM64/library'.length);
    this.timer = setInterval(() => {
      try { this.tick(); } catch (_) {
        this.record('0', 'host_launch_service_failed');
        this.stop();
        promptAction.showToast({ message: 'Linux Steam 游戏启动服务异常，请重启 Steam', duration: 5000 });
      }
    }, 200);
  }
  stop(): void {
    if (this.timer >= 0) clearInterval(this.timer);
    this.timer = -1;
    for (const session of this.sessions) testNapi.stopWineSession(session.sessionId);
    this.sessions = [];
    if (smokeNapi.stopLinuxSteamLaunchBroker) smokeNapi.stopLinuxSteamLaunchBroker();
  }
  private record(appId: string, status: string, exitCode?: number | null, exitCodeSource?: string): void {
    const evidence: LaunchEvidence = {
      source: 'native_linux_steam_compatibility_tool', appId, status,
      steamApiAuthenticationVerified: false, exitCode, exitCodeSource
    };
    writeSmokeText(`${this.results}/game-launch-${appId}.json`, JSON.stringify(evidence));
  }
  private reject(request: LinuxSteamLaunchRequest, reason: string): void {
    if (smokeNapi.replyLinuxSteamLaunchBroker) smokeNapi.replyLinuxSteamLaunchBroker(request.id, 3, 1);
    this.record(request.appId, reason);
    promptAction.showToast({ message: `Linux Steam 启动失败：${reason}`, duration: 5000 });
  }
  private windowsSteamRunning(): boolean {
    const processes = testNapi.getProcessList() as NativeProcessInfo[];
    return processes.some((p: NativeProcessInfo) => p.state === 'running' &&
      ['steam.exe', 'steamwebhelper.exe', 'steamservice.exe'].includes(p.name.toLowerCase()));
  }
  private auditLibrary(): void {
    const games: LibraryGame[] = [];
    for (const file of fs.listFileSync(`${this.library}/steamapps`)) {
      const match = /^appmanifest_([0-9]+)\.acf$/.exec(file);
      if (!match || games.length >= 100) continue;
      const path = `${this.library}/steamapps/${file}`;
      if (fs.statSync(path).size > 262144) continue;
      const text = readSmokeText(path);
      const name = /"name"\s+"([^"]*)"/.exec(text);
      const state = /"StateFlags"\s+"([0-9]+)"/.exec(text);
      const install = /"installdir"\s+"([^"]*)"/.exec(text);
      games.push({ appId: match[1], name: name ? name[1] : '', state: state ? state[1] : '',
        installedDirectory: install ? install[1] : '' });
    }
    const root = this.library.substring(0, this.library.length - '/library'.length);
    // Bounded private evidence from the official native client. This research
    // adapter cannot infer launch/Cloud success from local manifests alone.
    const logs: Map<string, string> = new Map<string, string>();
    for (const name of ['content_log.txt', 'console_log.txt', 'cloud_log.txt']) {
      const path = `${root}/client/.steam/steam/logs/${name}`;
      try {
        if (fs.statSync(path).size <= 1048576) logs.set(name, readSmokeText(path).slice(-65536));
      } catch (_) {}
    }
    const logText: string[] = [];
    logs.forEach((value: string, key: string): void => { logText.push(JSON.stringify(key) + ':' + JSON.stringify(value)); });
    writeSmokeText(`${this.results}/client-logs.private.json`, '{' + logText.join(',') + '}');
    const data: LibraryState = { folders: readSmokeText(`${this.library}/steamapps/libraryfolders.vdf`) ||
      readSmokeText(`${root}/client/.steam/steam/steamapps/libraryfolders.vdf`), games };
    writeSmokeText(`${this.results}/library-state.private.json`, JSON.stringify(data));
  }
  private tick(): void {
    if (++this.ticks % 25 === 0) this.auditLibrary();
    if (!smokeNapi.drainLinuxSteamLaunchBroker || !smokeNapi.pollLinuxSteamLaunchSession) return;
    for (const request of smokeNapi.drainLinuxSteamLaunchBroker()) {
      if (request.cancel) {
        for (const session of this.sessions) {
          if (session.id === request.id) testNapi.stopWineSession(session.sessionId);
        }
        this.sessions = this.sessions.filter((s: GameSession) => s.id !== request.id);
        continue;
      }
      if (this.windowsSteamRunning()) { this.reject(request, '检测到 Windows Steam，请先退出后重试'); continue; }
      if (this.sessions.length > 0) { this.reject(request, '已有游戏正在运行'); continue; }
      const argv = request.argv.slice(1);
      const environment = request.environment.slice();
      // Exact opt-in in the private research directory; no arbitrary environment
      // overrides cross the authenticated GNU launch broker.
      if (readSmokeText('/data/storage/el2/base/files/steamohos-native/game-launch-diagnostics.conf').trim() === 'wineLoader=1') {
        environment.push('WINEHUA_WINEDEBUG=-all,err+all,warn+all,+loaddll,+seh,+debugstr');
      }
      const launch = WineEngineService.getInstance().launchExecutable(this.windowsPath(request.argv[0]), argv, this.windowsPath(request.cwd), environment);
      if (launch.pid <= 0 || launch.sessionId.length === 0 || launch.reused) {
        this.reject(request, 'Wine 未创建新的游戏会话'); continue;
      }
      const engine = WineEngineService.getInstance();
      AppSessionService.getInstance().registerExternalLaunch(`steam-linux-game-${request.appId}`,
        this.windowsPath(request.argv[0]), launch, engine.getMode() ?? DisplayMode.DESKTOP,
        engine.getPresentationMode() ?? WinePresentationMode.MANAGED_WINDOWS);
      HostWindowTasks.getInstance().registerSteamGame(launch.sessionId);
      const session: GameSession = { id: request.id, appId: request.appId, sessionId: launch.sessionId, pid: launch.pid };
      this.sessions.push(session);
      if (smokeNapi.replyLinuxSteamLaunchBroker) smokeNapi.replyLinuxSteamLaunchBroker(request.id, 1, 0);
      this.record(request.appId, 'wine_process_started_authentication_unverified');
    }
    const remaining: GameSession[] = [];
    for (const session of this.sessions) {
      if (this.windowsSteamRunning()) {
        testNapi.stopWineSession(session.sessionId);
        if (smokeNapi.replyLinuxSteamLaunchBroker) smokeNapi.replyLinuxSteamLaunchBroker(session.id, 3, 1);
        this.record(session.appId, 'failed_windows_steam_started');
        promptAction.showToast({ message: '游戏尝试调用 Windows Steam，本次隔离验证未通过', duration: 5000 });
        continue;
      }
      const state = smokeNapi.pollLinuxSteamLaunchSession(session.id, session.sessionId);
      if (state === 1) remaining.push(session);
      else {
        const process = testNapi.queryWineProcess(session.pid);
        const known = state === 0 && process.found && process.status === 'exited' &&
          (process.exitCodeSource === 'waitpid' || process.exitCodeSource === 'signal');
        this.record(session.appId, known ? 'wine_process_ended' : 'wine_process_state_unavailable',
          known ? process.exitCode : null, known ? process.exitCodeSource : 'unknown');
        if (known && process.exitCode !== null && process.exitCode !== 0) {
          promptAction.showToast({ message: `游戏已退出（退出码 ${process.exitCode}）`, duration: 5000 });
        }
      }
    }
    this.sessions = remaining;
  }
}
