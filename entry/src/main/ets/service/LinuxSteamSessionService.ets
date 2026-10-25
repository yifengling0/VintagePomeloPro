import { LinuxGnuProtonComponent } from './LinuxGnuProtonComponent';
import { applyLinuxSteamDownloadPolicy } from './LinuxSteamDownloadPolicy';
import { linuxSteamDownloadHome } from './LinuxSteamDiagnosticData';
import { common } from '@kit.AbilityKit';
import { promptAction } from '@kit.ArkUI';
import { hilog } from './LogService';
import util from '@ohos.util';
import smokeNapi from 'libsmoke_napi.so';
import { DisplayMode, WinePresentationMode } from '../model/AppModels';
import { LinuxSteamAction, LinuxSteamCommandAck, LinuxSteamSnapshot, validLinuxSteamAppId } from '../model/LinuxSteamModels';
import { ConfigurationFiles } from './ConfigurationFiles';
import { LinuxSteamInstallService } from './LinuxSteamInstallService';
import { LinuxSteamLibraryService } from './LinuxSteamLibraryService';
import { LinuxSteamGameLaunch } from './LinuxSteamGameLaunch';
import { LinuxSteamWindowHost } from './LinuxSteamWindowHost';
import { LinuxSteamPageRecovery } from './LinuxSteamPageRecovery';
import { WineEngineService } from './WineEngineService';
import { ManagedLinuxSteamBridgeService } from './ManagedLinuxSteamBridgeService';
import { HostWindowTasks, HostWindowTask } from './HostWindowTasks';
import { GamepadManager } from './GamepadManager';
import { ensureSmokeDirectory, readSmokeText } from './ManagedSmokePayloadService';

const BASE = '/data/storage/el2/base/files/steamohos-native';
const TMP = `${BASE}/client-tmp`;
interface SupervisorState {
  version: number; generation: string; ready: boolean; closed: boolean; mode: number; updatedAt: number;
}
interface NativeSessionResult { status: string; }
export type LinuxSteamSessionListener = (value: LinuxSteamSnapshot) => void;

export class LinuxSteamSessionService {
  private static instance: LinuxSteamSessionService | null = null;
  private context: common.UIAbilityContext | null = null;
  private listeners: Set<LinuxSteamSessionListener> = new Set<LinuxSteamSessionListener>();
  private value: LinuxSteamSnapshot = { state: 'stopped', presentation: 'desktop', message: 'Steam 尚未启动', generation: '' };
  private starting: Promise<void> | null = null;
  private worker: Promise<void> | null = null;
  private tail: Promise<void> = Promise.resolve();
  private requestId: number = 0;
  private timer: number = -1;
  private host: LinuxSteamWindowHost | null = null;
  private games: LinuxSteamGameLaunch | null = null;
  private pageRecovery: LinuxSteamPageRecovery = new LinuxSteamPageRecovery();
  private recoveryQueued: boolean = false;
  private hostRecovery: Promise<void> | null = null;
  private userAction: number = 0;
  private lastLoggedState: string = '';
  static getInstance(): LinuxSteamSessionService {
    if (!LinuxSteamSessionService.instance) LinuxSteamSessionService.instance = new LinuxSteamSessionService();
    return LinuxSteamSessionService.instance;
  }
  initialize(context: common.UIAbilityContext): void { this.context = context; }
  isActive(): boolean { return this.starting !== null || this.worker !== null ||
    (this.value.state !== 'stopped' && this.value.state !== 'failed'); }
  snapshot(): LinuxSteamSnapshot { return { state: this.value.state, presentation: this.value.presentation,
    message: this.value.message, generation: this.value.generation }; }
  subscribe(listener: LinuxSteamSessionListener): void { this.listeners.add(listener); listener(this.snapshot()); }
  unsubscribe(listener: LinuxSteamSessionListener): void { this.listeners.delete(listener); }
  private publish(): void {
    const logged = `${this.value.generation}:${this.value.state}:${this.value.presentation}:${this.value.message}`;
    if (logged !== this.lastLoggedState) {
      this.lastLoggedState = logged;
      hilog.info(0, 'DeckLaunch', 'state=%{public}s presentation=%{public}s message=%{public}s',
        this.value.state, this.value.presentation, this.value.message);
    }
    try { ensureSmokeDirectory(`${BASE}/session`); ConfigurationFiles.json(`${BASE}/session/state.json`, JSON.stringify(this.snapshot())); } catch (_) {}
    this.listeners.forEach((listener: LinuxSteamSessionListener): void => listener(this.snapshot()));
  }
  private async delay(): Promise<void> { await new Promise<void>((resolve: () => void): void => { setTimeout(resolve, 150); }); }
  private elapsed(stage: string, started: number): void {
    hilog.info(0, 'DeckLaunch', 'stage=%{public}s elapsedMs=%{public}d', stage, Date.now() - started);
  }
  private poll(): void {
    try {
      const raw = ConfigurationFiles.text(ConfigurationFiles.read(`${TMP}/deck-session-state.json`, 1024));
      const state = JSON.parse(raw) as SupervisorState;
      if (state.version !== 1 || state.generation !== this.value.generation ||
        !Number.isSafeInteger(state.updatedAt) || Date.now() - state.updatedAt > 10000 || state.updatedAt > Date.now() + 2000) return;
      if (state.closed) return;
      // ready describes the current Steam surface. Games and presentation
      // changes can withdraw it while the native client/control channel stays
      // alive. Only worker completion ends an established session; otherwise
      // navigation/shutdown would be rejected as a second cold startup.
      const next = state.ready || this.value.state === 'running' ? 'running' : 'starting';
      const presentation = state.ready ? (state.mode === 2 ? 'bigPicture' : 'desktop') : this.value.presentation;
      if (this.value.state === 'stopping') return;
      if (presentation === 'bigPicture' && this.value.presentation !== 'bigPicture') this.pageRecovery.beginBigPicture();
      if (next !== this.value.state || presentation !== this.value.presentation) {
        this.value.state = next; this.value.presentation = presentation;
        this.value.message = next === 'running' ? 'Steam 已打开，登录和同步状态请在客户端查看' : '正在打开 Steam';
        this.publish();
      }
      this.pageRecovery.poll();
      this.recoverPage();
    } catch (_) {}
  }
  private recoverPage(): void {
    if (this.recoveryQueued || this.value.state !== 'running' || this.value.presentation !== 'desktop' ||
      !this.pageRecovery.needed() || !this.host?.isForeground()) return;
    const generation = this.value.generation; const userAction = this.userAction;
    this.recoveryQueued = true;
    const operation = this.tail.then(async (): Promise<void> => {
      // A newer user request (especially shutdown, launch or Big Picture) wins.
      // Recover in-place without activating Wine or stealing a game's focus.
      if (generation !== this.value.generation || userAction !== this.userAction ||
        !this.worker || this.value.state !== 'running' || this.value.presentation !== 'desktop' ||
        !this.host?.isForeground()) return;
      const attempt = this.pageRecovery.take(); if (!attempt) return;
      ConfigurationFiles.json(`${BASE}/session/page-recovery.json`, JSON.stringify({
        generation, reason: 'cef_invalid_initiator_origin', attempt, status: 'requesting_library' }));
      await this.dispatch('library', '0');
      ConfigurationFiles.json(`${BASE}/session/page-recovery.json`, JSON.stringify({
        generation, reason: 'cef_invalid_initiator_origin', attempt, status: 'navigation_dispatched' }));
      hilog.info(0, 'DeckLaunch', 'CEF page recovery navigation dispatched attempt=%{public}d', attempt);
    }).catch((error: Error): void => {
      hilog.warn(0, 'DeckLaunch', 'CEF page recovery failed: %{public}s', error.message.substring(0, 160));
      promptAction.showToast({ message: 'Steam 页面恢复未完成，请在 Steam 选项中重新打开游戏库', duration: 5000 });
    }).finally((): void => { this.recoveryQueued = false; });
    this.tail = operation;
  }
  private async start(foregroundCurrent: () => boolean, action: LinuxSteamAction, appId: string): Promise<boolean> {
    if (this.starting) { await this.starting; return false; }
    if (this.worker) {
      if (this.value.state === 'running') return false;
      throw new Error('Steam 会话正在启动或退出，请稍后重试');
    }
    const operation = this.startInternal(foregroundCurrent, action, appId); this.starting = operation;
    try { await operation; return true; } finally { if (this.starting === operation) this.starting = null; }
  }
  private async startInternal(foregroundCurrent: () => boolean, action: LinuxSteamAction, appId: string): Promise<void> {
    if (!this.context) throw new Error('Steam 服务尚未初始化');
    const nativeStart = smokeNapi.startLinuxSteamSession;
    if (!nativeStart) throw new Error('当前应用不包含 Linux Steam 支持，请更新应用');
    this.value = { state: 'preparing', presentation: 'desktop', message: '正在校验 Steam 运行组件', generation: util.generateRandomUUID() };
    this.requestId = 0; this.publish();
    try {
      let stageAt = Date.now();
      const protonProgress = (message: string): void => { this.value.message = message; this.publish(); };
      LinuxGnuProtonComponent.subscribe(protonProgress);
      try { await LinuxSteamInstallService.getInstance().ensureReady(this.context); }
      finally { LinuxGnuProtonComponent.unsubscribe(protonProgress); }
      this.elapsed('components', stageAt); stageAt = Date.now();
      const root = LinuxSteamLibraryService.getInstance().externalRoot();
      try { applyLinuxSteamDownloadPolicy(root); }
      catch (error) {
        hilog.warn(0, 'DeckLaunch', 'Download connection preference was not applied: %{public}s', (error as Error).message.substring(0, 160));
        promptAction.showToast({ message: '下载连接设置未能应用，Steam 将使用原配置启动', duration: 5000 });
      }
      const zRoot = await linuxSteamDownloadHome(this.context);
      ConfigurationFiles.json(`${BASE}/wine-z-root.path`, zRoot + '\n');
      const engine = WineEngineService.getInstance();
      this.value.message = '正在准备游戏桌面'; this.publish();
      const ready = await engine.ensureReady(DisplayMode.DESKTOP, WinePresentationMode.DESKTOP,
        zRoot, foregroundCurrent);
      this.elapsed('wineDesktop', stageAt); stageAt = Date.now();
      if (!ready) throw new Error(engine.getMessage() || '无法准备 Wine 桌面');
      await ManagedLinuxSteamBridgeService.synchronizeCompatibility(this.context, `${root}/client/.steam/steam`, '/data/storage/el2/base/files/.wine');
      ensureSmokeDirectory(TMP); ensureSmokeDirectory(`${BASE}/session`);
      ConfigurationFiles.json(`${BASE}/session-generation.txt`, this.value.generation + '\n');
      ConfigurationFiles.json(`${BASE}/client-launch.conf`, 'arg=-cef-disable-gpu\narg=-cef-disable-gpu-compositing\nsoftwareOpenGL=1\nsocketMetadata=1\nwineWindowHost=1\nsteamClientBridge=experimental\n' +
        `startupAction=${action} ${action === 'launch' ? appId : '0'}\n`);
      const driveC = '/data/storage/el2/base/files/.wine/drive_c';
      const results = `${driveC}/hmosdeck/session`;
      if (!ensureSmokeDirectory(results)) throw new Error('无法准备 Steam 会话目录');
      this.value.message = '正在创建 Steam 窗口'; this.publish();
      this.host = new LinuxSteamWindowHost();
      await this.host.start(this.context, driveC, results);
      this.elapsed('windowHost', stageAt); stageAt = Date.now();
      this.games = new LinuxSteamGameLaunch(); this.games.start(`${root}/library`, results, zRoot);
      this.pageRecovery.start(`${root}/client/.steam/steam/logs/cef_log.txt`);
      this.value.state = 'starting'; this.value.message = '正在打开官方 Linux ARM64 Steam'; this.publish();
      this.timer = setInterval((): void => this.poll(), 400);
      LinuxGnuProtonComponent.setSessionActive(true);
      this.worker = nativeStart().then((raw: string): void => {
        const result = JSON.parse(raw) as NativeSessionResult;
        ConfigurationFiles.json(`${BASE}/session/native-result.private.json`, raw);
        if (this.value.state !== 'stopping') throw new Error(`Steam 已退出（${result.status}），可重新打开`);
        this.value.state = 'stopped'; this.value.message = 'Steam 已退出';
      }).catch((error: Error): void => {
        this.value.state = 'failed'; this.value.message = error.message.substring(0, 240);
      }).finally((): void => {
        if (this.timer >= 0) clearInterval(this.timer); this.timer = -1;
        LinuxGnuProtonComponent.setSessionActive(false);
        this.pageRecovery.stop();
        this.games?.stop(); this.host?.stop(); this.games = null; this.host = null; this.worker = null; this.publish();
      });
      const deadline = Date.now() + 90000;
      while (this.value.state === 'starting' && Date.now() < deadline) { this.poll(); await this.delay(); }
      this.elapsed('nativeClientReady', stageAt);
      if (this.snapshot().state !== 'running') throw new Error(this.snapshot().state === 'starting' ?
        'Steam 启动较慢，后台仍在准备，请稍后点击打开；不会重复启动' : this.value.message);
    } catch (error) {
      if (!this.worker) {
        if (this.timer >= 0) clearInterval(this.timer); this.timer = -1;
        LinuxGnuProtonComponent.setSessionActive(false);
        this.games?.stop(); this.host?.stop(); this.games = null; this.host = null;
        this.value.state = 'failed'; this.value.message = (error as Error).message; this.publish(); }
      if (foregroundCurrent()) await HostWindowTasks.getInstance().showLauncher();
      promptAction.showToast({ message: (error as Error).message.substring(0, 180), duration: 5000 });
      throw new Error((error as Error).message);
    }
  }
  async open(action: LinuxSteamAction = 'library', appId: string = '0'): Promise<void> {
    if (action === 'launch' && !validLinuxSteamAppId(appId)) throw new Error('Steam AppID 无效');
    const userAction = ++this.userAction;
    const tasks = HostWindowTasks.getInstance();
    const foregroundRevision = tasks.foregroundRevision();
    const foregroundCurrent = (): boolean => userAction === this.userAction &&
      foregroundRevision === tasks.foregroundRevision();
    if (action === 'shutdown' && !this.worker) return;
    const operation = this.tail.then(async (): Promise<void> => {
      let stageAt = Date.now();
      const launchTicket = action === 'launch' && foregroundCurrent() ? tasks.expectSteamGame(appId) : 0;
      try {
        const cold = await this.start(foregroundCurrent, action, appId);
        // A cold request is already queued in the official client's argv.
        // Never replay it just because its login window became visible.
        if (!cold) {
          if (action !== 'shutdown') {
            if ((!this.host || this.host.hasExited()) && !foregroundCurrent()) {
              if (launchTicket) tasks.cancelExpectedSteamGame(launchTicket);
              return; // A superseded request cannot create or foreground a replacement.
            }
            await this.ensureWindowHost();
          }
          await this.dispatch(action, appId);
        }
      }
      catch (error) { if (launchTicket) tasks.cancelExpectedSteamGame(launchTicket); throw new Error((error as Error).message); }
      this.elapsed('actionDispatch', stageAt); stageAt = Date.now();
      // A launch ACK only means the URI was forwarded. The game may already
      // have mapped a window; restoring Steam here would steal its focus.
      if (action !== 'shutdown' && action !== 'launch' && foregroundCurrent()) {
        await this.restore(); this.elapsed('windowRestore', stageAt);
      }
    });
    this.tail = operation.catch((): void => {}); return operation;
  }
  private async dispatch(action: LinuxSteamAction, appId: string): Promise<void> {
    if (this.value.state !== 'running') throw new Error('Steam 尚未就绪');
    const generation = this.value.generation;
    // Maintenance/diagnostics can also send fixed commands to this private
    // supervisor. Continue its acknowledged sequence instead of replaying an
    // older number and silently waiting for an ACK that can never arrive.
    try {
      const previous = JSON.parse(ConfigurationFiles.text(ConfigurationFiles.read(`${TMP}/deck-command-ack.json`, 1024))) as LinuxSteamCommandAck;
      if (previous.version === 1 && previous.generation === generation && Number.isSafeInteger(previous.requestId) &&
        previous.requestId > this.requestId && previous.requestId < 2147483647) this.requestId = previous.requestId;
    } catch (_) {}
    const requestId = ++this.requestId;
    ConfigurationFiles.json(`${TMP}/deck-command.txt`, `1 ${generation} ${requestId} ${action} ${action === 'launch' ? appId : '0'}\n`);
    // The native client may exit before its dispatch ACK is observed. Record
    // the accepted local shutdown request before yielding to the worker.
    if (action === 'shutdown') {
      this.value.state = 'stopping'; this.value.message = '正在退出 Steam'; this.publish();
    }
    const deadline = Date.now() + 15000;
    try {
      while (Date.now() < deadline && generation === this.value.generation && this.worker) {
        try {
          const ack = JSON.parse(ConfigurationFiles.text(ConfigurationFiles.read(`${TMP}/deck-command-ack.json`, 1024))) as LinuxSteamCommandAck;
          if (ack.version === 1 && ack.generation === generation && ack.requestId === requestId) {
            if (ack.status !== 'dispatched' || ack.exitCode !== 0) throw new Error(`Steam 操作未完成（${ack.status}）`);
            return;
          }
        } catch (error) { if ((error as Error).message.startsWith('Steam 操作')) throw new Error((error as Error).message); }
        await this.delay();
      }
      if (action === 'shutdown' && generation === this.value.generation && this.snapshot().state === 'stopped') return;
      throw new Error('Steam 没有确认本次操作，请重试');
    } catch (error) {
      // A rejected or unacknowledged command must not leave a live client in
      // stopping forever. Keep it available for navigation and another exit.
      if (action === 'shutdown' && generation === this.value.generation && this.worker && this.snapshot().state === 'stopping') {
        this.value.state = 'running'; this.value.message = 'Steam 未确认退出，可重试'; this.publish();
      }
      throw new Error((error as Error).message);
    }
  }
  private async ensureWindowHost(): Promise<void> {
    if (this.hostRecovery) { await this.hostRecovery; return; }
    if (this.host && !this.host.hasExited()) return;
    if (!this.context || !this.worker || this.value.state !== 'running')
      throw new Error('Steam 会话尚未就绪，无法恢复窗口');
    const context = this.context;
    const generation = this.value.generation;
    const previous = this.host;
    previous?.stop(); this.host = null;
    const replacement = new LinuxSteamWindowHost(); this.host = replacement;
    this.value.message = '正在恢复 Steam 承载窗口'; this.publish();
    const operation = (async (): Promise<void> => {
      try {
        await replacement.start(context, '/data/storage/el2/base/files/.wine/drive_c',
          '/data/storage/el2/base/files/.wine/drive_c/hmosdeck/session', true);
        if (generation !== this.value.generation || !this.worker || this.value.state !== 'running' || this.host !== replacement)
          throw new Error('Steam 窗口恢复已取消');
        this.value.message = 'Steam 已打开，登录和同步状态请在客户端查看'; this.publish();
      } catch (error) {
        replacement.stop(); if (this.host === replacement) this.host = null;
        throw new Error(`Steam 窗口恢复失败：${(error as Error).message}`);
      }
    })();
    this.hostRecovery = operation;
    try { await operation; } finally { if (this.hostRecovery === operation) this.hostRecovery = null; }
  }

  async restore(): Promise<void> {
    GamepadManager.getInstance().releaseAll();
    if (this.host && await this.host.restore()) return;
    const tasks = HostWindowTasks.getInstance();
    const steam = tasks.snapshot().find((task: HostWindowTask): boolean => task.source === 'steam-linux');
    if (steam && await tasks.activate(steam.key)) return;
    throw new Error('Steam 窗口正在切换，请稍后重试');
  }
  async returnHome(): Promise<void> {
    this.userAction++;
    GamepadManager.getInstance().releaseAll();
    await HostWindowTasks.getInstance().showLauncher();
  }
}
