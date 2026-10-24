import { common } from '@kit.AbilityKit';
import { FaultLogger } from '@kit.PerformanceAnalysisKit';
import { prepareLinuxSteamClientData, prepareLinuxSteamExternalStorage, linuxSteamDownloadHome } from './LinuxSteamDiagnosticData';
import { LinuxSteamWindowHost } from './LinuxSteamWindowHost';
import { auditLinuxSteamStorage } from './LinuxSteamStorageAudit';
import { isolateLinuxSteamForCleanDownload, restoreLinuxSteamAfterCleanDownload } from './LinuxSteamCleanInstallDiagnostic';
import testNapi from 'libentry.so';
import smokeNapi from 'libsmoke_napi.so';
import { hilog } from './LogService';
import {
  ensureSmokeDirectory, readSmokeText, smokeDriveC, smokePathExists,
  syncManagedSmoke, writeSmokeText
} from './ManagedSmokePayloadService';
import { WineEngineService } from './WineEngineService';
import { AppSettingsStore } from './AppSettingsStore';
import { WinePresentationMode } from '../model/AppModels';
import { LinuxSteamGameLaunch } from './LinuxSteamGameLaunch';
import { SmokeRequest } from '../common/SmokeTypes';

const DOMAIN = 0x0000;
const TAG = 'SMOKE';

// suites.json (assemble.sh 随载荷生成的编排定义) 的运行时视图
interface SmokeSuiteTest {
  testId: string;
  exe: string;
  env?: Record<string, string>;
  // argvMode: 默认 'std' = runner 生成 smoke 统一协议 argv
  // (--automation --run-id --test-id --result + mode/seconds);
  // 'raw' = 用 argv 字段原样 (d3d12 等自有参数的程序), <run-id>/<test-id> 占位符替换
  argvMode?: string;
  argv?: string[];
  // 'present' (默认, 带 --seconds N) / 'offscreen' (wine-vulkan offscreen 系)
  mode?: string;
  // 追加参数 (如 d3d9-cube 的 --d3d9)
  extraArgs?: string[];
  // 回归固定后端 (旧 SmokeRunner per-test 语义): 缺省 = 当前档位 (UI 手动入口用)
  d3dBackend?: string;
  dxvkBackend?: string;
  seconds?: number;
  timeoutMs?: number;
}

interface SmokeSuite {
  tests?: SmokeSuiteTest[];
}

interface SuitesDoc {
  suiteVersion?: string;
  suites?: Record<string, SmokeSuite>;
}

interface SmokeTestResult {
  schemaVersion: number;
  runId: string;
  testId: string;
  status: string;
  stage: string;
  message?: string;
  pid?: number;
  metrics?: Record<string, number>;
}

interface SmokeSummary {
  schemaVersion: number;
  runId: string;
  suite: string;
  status: string;
  tests?: SmokeTestResult[];
}

interface WineRecoveryEvidence {
  interrupted: boolean;
  firstReady: boolean;
  unfinished: boolean;
  recovered: boolean;
  preserved: boolean;
  initialized: boolean;
  native: string;
}

interface WineStartupEvidence {
  native: string;
  initialized: boolean;
  initStatus: string;
  dispatchStatus: string;
  isolated: boolean;
}

interface LinuxSteamFaultSummary {
  pid: number;
  timestamp: number;
  type: number;
  signal: string;
  frames: string[];
}

interface LinuxSteamGnuControl {
  exitCode: number;
  signal: number;
  childReaped: boolean;
  timedOut: boolean;
}

interface LinuxSteamGnuResult {
  status: string;
  gnuExecutionVerified: boolean;
  steamClientStarted: boolean;
  gnuWaylandVerified?: boolean;
  x11Verified?: boolean;
  ncpExitVerified: boolean;
  ncpExitSignal: number;
  exitControl?: LinuxSteamGnuControl;
  timeoutControl?: LinuxSteamGnuControl;
}

// 结果文件 / runWineProgram 返回的窄化视图 (ArkTS 禁止对象字面量类型声明)
interface SmokeResultFile {
  status?: string;
  stage?: string;
}

interface RunProgramResult {
  pid: number;
}

// 结果 JSON 的终态集合 (与 smoke 程序协议一致: PASS/FAIL/SKIP/UNSUPPORTED)
const FINAL_STATUSES: string[] = ['PASS', 'FAIL', 'SKIP', 'UNSUPPORTED'];
const POLL_INTERVAL_MS: number = 500;

function protocolToken(value: string, fallback: string): string {
  return /^[A-Za-z0-9._-]{1,96}$/.test(value) ? value : fallback;
}

// 瘦编排解释器: 读 suites.json → 逐测试 runWineProgram → 轮询结果 JSON → 汇总
// suite-summary.json。只搬运 env 不懂 env; 产品语义 env (DXVK 稳定化/后端档位) 由
// native 管线收口, 本类只注入结果协议键 RUN_ID/TEST_ID 与 suites 声明的诊断键。
export class SmokeRunner {
  private static instance: SmokeRunner | null = null;

  static getInstance(): SmokeRunner {
    if (!SmokeRunner.instance) {
      SmokeRunner.instance = new SmokeRunner();
    }
    return SmokeRunner.instance;
  }

  private context: common.Context | null = null;

  initialize(context: common.Context): void {
    this.context = context;
  }

  private listeners: (() => void)[] = [];
  private busy: boolean = false;
  private currentStatus: string = '';
  private currentResult: string = '';
  private currentRunId: string = '';

  subscribe(listener: () => void): void {
    this.listeners.push(listener);
  }

  private notify(): void {
    for (const l of this.listeners) {
      l();
    }
  }

  isRunning(): boolean {
    return this.busy;
  }

  statusText(): string {
    return this.currentStatus;
  }

  resultText(): string {
    return this.currentResult;
  }

  runId(): string {
    return this.currentRunId;
  }

  // 单入口: want/ui 双通道共用。忙时直接拒绝 (调用方已做 enabled 拦截)。
  start(request: SmokeRequest): void {
    if (this.busy) {
      hilog.warn(DOMAIN, TAG, 'start blocked (busy) suite=%{public}s', request.suite);
      return;
    }
    const normalized: SmokeRequest = {
      suite: protocolToken(request.suite, 'core'),
      runId: protocolToken(request.runId, `manual-${Date.now()}`),
      source: request.source,
      prefix: request.prefix === 'clean' ? 'clean' : 'reuse',
      longSeconds: request.longSeconds
    };
    this.busy = true;
    this.currentRunId = normalized.runId;
    this.currentStatus = `Smoke ${normalized.suite} 启动 (${normalized.runId})`;
    this.currentResult = '';
    this.notify();
    void this.run(normalized);
  }

  private async run(request: SmokeRequest): Promise<void> {
    const prefixMode: string = request.prefix === 'clean' ? 'clean' : 'reuse';
    const driveC: string = smokeDriveC(prefixMode);
    hilog.info(DOMAIN, TAG, 'run begin suite=%{public}s runId=%{public}s prefix=%{public}s engineReady=%{public}d',
      request.suite, request.runId, prefixMode, WineEngineService.getInstance().isReady());
    try {
      // Readiness-only diagnostic: use the production startup coordinator in a
      // separate empty container; never launch games or alter the product prefix.
      if (request.suite === 'wine-startup-ready') {
        const ready = WineEngineService.getInstance().isReady();
        const prefix = driveC.substring(0, driveC.length - '/drive_c'.length);
        const evidence: WineStartupEvidence = {
          native: testNapi.getWineStartupState(),
          initialized: testNapi.checkWinePrefix(prefixMode === 'clean' ? 'clean' : 'default'),
          initStatus: readSmokeText(prefix + '/.winehua-wineboot-init-status'),
          dispatchStatus: readSmokeText(prefix + '/.winehua-wineboot-dispatch-status'),
          isolated: prefixMode === 'clean'
        };
        const resultsDir = `${driveC}/smoke/results/${request.runId}`;
        ensureSmokeDirectory(resultsDir);
        writeSmokeText(resultsDir + '/startup.json', JSON.stringify(evidence));
        await this.finishSuite(request, [{ schemaVersion: 1, runId: request.runId,
          testId: 'wine-startup', status: ready && evidence.initialized ? 'PASS' : 'FAIL',
          stage: 'prefix-ready', message: prefixMode === 'clean' ? 'Isolated fresh prefix' : 'Existing prefix' }], driveC);
        return;
      }
      if (request.suite === 'wine-startup-interrupted-recovery' && prefixMode === 'reuse') {
        await this.runWineStartupRecovery(request, driveC);
        return;
      }
      // The proton RunWineProgram ABI has no prefix switch. Never report a clean-prefix
      // test while actually spawning inside the active product container.
      if (prefixMode === 'clean') {
        await this.finishSuite(request, [this.unsupported(request, 'clean prefix is not supported by this native ABI')], driveC);
        return;
      }
      if (request.suite === 'linux-steam-clean-restore') {
        if (!this.context) throw new Error('Restore context unavailable');
        const resultsDir = `${driveC}/smoke/results/${request.runId}`;
        if (!ensureSmokeDirectory(resultsDir)) throw new Error('Cannot create diagnostic result directory');
        writeSmokeText(`${resultsDir}/restoration.json`, await restoreLinuxSteamAfterCleanDownload(this.context));
        await this.finishSuite(request, [{ schemaVersion: 1, runId: request.runId, testId: 'clean-download-restore',
          status: 'PASS', stage: 'old_resources_restored', message: 'Old resources restored; downloaded trial preserved separately' }], driveC);
        return;
      }
      if (request.suite === 'linux-steam-clean-stage') {
        if (!this.context) throw new Error('Clean download context unavailable');
        const resultsDir = `${driveC}/smoke/results/${request.runId}`;
        if (!ensureSmokeDirectory(resultsDir)) throw new Error('Cannot create diagnostic result directory');
        writeSmokeText(`${resultsDir}/isolation.json`, await isolateLinuxSteamForCleanDownload(this.context));
        await this.finishSuite(request, [{ schemaVersion: 1, runId: request.runId, testId: 'clean-download-isolation',
          status: 'PASS', stage: 'old_resources_preserved', message: 'Empty component directory; private app data preserved' }], driveC);
        return;
      }
      // Explicit read-only diagnostic. The legacy query is needed for faults
      // predating this session; only bounded signal/stack metadata is exported.
      if (request.suite === 'linux-steam-faults') {
        const resultsDir = `${driveC}/smoke/results/${request.runId}`;
        if (!ensureSmokeDirectory(resultsDir)) throw new Error('Cannot create fault audit directory');
        const faults = await FaultLogger.query(FaultLogger.FaultType.NO_SPECIFIC);
        const summaries: LinuxSteamFaultSummary[] = faults.slice(0, 10).map((fault: FaultLogger.FaultLogInfo): LinuxSteamFaultSummary => {
          const signal = fault.reason.match(/SIG[A-Z0-9]+(?:\([^)]{0,80}\))?/);
          const frames: string[] = fault.fullLog.split('\n').filter((line: string): boolean => /^\s*#\d+\s+pc\b/.test(line)).slice(0, 12)
            .map((line: string): string => line.replace(/(?:\/[^\s/()]+)+\/([^\s/()]+)/g, '$1').slice(0, 320));
          return { pid: fault.pid, timestamp: fault.timestamp, type: fault.type, signal: signal ? signal[0] : 'unspecified', frames: frames };
        });
        writeSmokeText(`${resultsDir}/fault-summary.json`, JSON.stringify(summaries));
        await this.finishSuite(request, [{ schemaVersion: 1, runId: request.runId, testId: 'fault-audit',
          status: 'PASS', stage: 'metadata_collected', message: 'App-owned fault metadata only; no raw dump or account payload' }], driveC);
        return;
      }
      if (request.suite === 'linux-steam-storage') {
        if (!this.context) throw new Error('Storage diagnostic context unavailable');
        const resultsDir = `${driveC}/smoke/results/${request.runId}`;
        if (!ensureSmokeDirectory(resultsDir)) throw new Error('Cannot create storage audit directory');
        writeSmokeText(`${resultsDir}/storage-audit.json`, await auditLinuxSteamStorage(this.context));
        await this.finishSuite(request, [{ schemaVersion: 1, runId: request.runId, testId: 'storage-audit',
          status: 'PASS', stage: 'metadata_collected', message: 'Read-only bounded directory size audit' }], driveC);
        return;
      }
      if (request.suite === 'linux-steam-gnu') {
        await this.runLinuxSteamGnu(request, driveC);
        return;
      }
      if (!WineEngineService.getInstance().isReady()) {
        await this.finishSuite(request, [
          this.failure(request, 'suites', 'engine not ready', -1)
        ], driveC);
        return;
      }
      if (request.suite === 'linux-steam-client') {
        await this.runLinuxSteamClient(request, driveC);
        return;
      }
      if (request.suite === 'linux-steam-gnu-xwayland') {
        await this.runLinuxSteamGnuXwayland(request, driveC);
        return;
      }
      if (request.suite === 'linux-steam-gnu-wayland') {
        await this.runLinuxSteamGnuWayland(request, driveC);
        return;
      }
      if (request.suite === 'linux-steam-host') {
        await this.runLinuxSteamHost(request, driveC);
        return;
      }
      if (!syncManagedSmoke(prefixMode, 'diagnostic')) {
        await this.finishSuite(request, [
          this.failure(request, 'suites', `managed payload unavailable (${prefixMode})`, -1)
        ], driveC);
        return;
      }
      const suitesPath: string = `${driveC}/smoke/suites.json`;
      const raw: string = readSmokeText(suitesPath);
      if (!raw) {
        await this.finishSuite(request, [
          this.failure(request, 'suites', `missing ${suitesPath} (seed 未完成或载荷缺失)`, -1)
        ], driveC);
        return;
      }
      const doc: SuitesDoc = JSON.parse(raw) as SuitesDoc;
      const suite: SmokeSuite | undefined = doc.suites ? doc.suites[request.suite] : undefined;
      const tests: SmokeSuiteTest[] | undefined = suite ? suite.tests : undefined;
      if (!tests || tests.length === 0) {
        await this.finishSuite(request, [
          this.failure(request, 'suites', `suite "${request.suite}" 无定义 (suites.json)`, -1)
        ], driveC);
        return;
      }
      const resultsDir: string = `${driveC}/smoke/results/${request.runId}`;
      ensureSmokeDirectory(resultsDir);
      const results: SmokeTestResult[] = [];
      for (let i = 0; i < tests.length; i++) {
        const test: SmokeSuiteTest = tests[i];
        this.currentStatus = `Smoke ${request.suite}: ${i + 1}/${tests.length} ${test.testId}`;
        this.notify();
        const r = await this.runTest(request, test, resultsDir);
        results.push(r);
        hilog.info(DOMAIN, TAG, 'test done suite=%{public}s test=%{public}s status=%{public}s stage=%{public}s',
          request.suite, test.testId, r.status, r.stage);
      }
      const passed = results.filter(r => r.status === 'PASS').length;
      const summary: SmokeSummary = {
        schemaVersion: 1,
        runId: request.runId,
        suite: request.suite,
        status: passed === results.length && results.length > 0 ? 'PASS' : 'FAIL',
        tests: results
      };
      writeSmokeText(`${resultsDir}/suite-summary.json`, JSON.stringify(summary));
      this.currentResult = `${summary.status} ${passed}/${results.length}`;
      this.currentStatus = `Smoke ${request.suite} ${summary.status} (${summary.status === 'PASS' ?
        '全部通过' : '详见 suite-summary.json'})`;
      hilog.info(DOMAIN, TAG, 'run done suite=%{public}s status=%{public}s %{public}d/%{public}d',
        request.suite, summary.status, passed, results.length);
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'run ERROR: %{public}s %{public}s', (e as Error).message, JSON.stringify(e));
      this.currentResult = 'ERROR';
      this.currentStatus = `Smoke ${request.suite} ERROR`;
    } finally {
      this.busy = false;
      this.notify();
    }
  }

  /** Interrupt only the disposable container, then retry without resetting it. */
  private async runWineStartupRecovery(request: SmokeRequest, resultsDrive: string): Promise<void> {
    const engine = WineEngineService.getInstance();
    const settings = AppSettingsStore.getInstance().getGlobalSettings();
    const home = '/storage/Users/currentUser/Download/com.vintage.pomelopro';
    const prefix = '/data/storage/el2/base/files/containers/clean/prefix';
    let firstFinished = false;
    const first = engine.ensureSmokeReady(settings.defaultDisplayMode, WinePresentationMode.DESKTOP, home, 'clean')
      .then((ready: boolean): boolean => { firstFinished = true; return ready; });
    const deadline = Date.now() + 45000;
    while (!firstFinished && engine.getPhase() !== 'wineboot' && Date.now() < deadline) {
      await new Promise<void>((resolve: () => void) => setTimeout(resolve, 100));
    }
    // Stop during registration, after Wine wrote its early update timestamp.
    while (!firstFinished && !smokePathExists(prefix + '/.update-timestamp') && Date.now() < deadline) {
      await new Promise<void>((resolve: () => void) => setTimeout(resolve, 100));
    }
    const interrupted = !firstFinished && smokePathExists(prefix + '/.winehua-init-in-progress') &&
      smokePathExists(prefix + '/.update-timestamp');
    if (!interrupted) {
      await engine.stopSession();
      await first;
      throw new Error('Diagnostic did not reach an incomplete isolated registration');
    }
    ensureSmokeDirectory(prefix + '/drive_c');
    const sentinel = prefix + '/drive_c/startup-recovery-sentinel.txt';
    writeSmokeText(sentinel, request.runId);
    await engine.stopSession();
    const firstReady = await first;
    const unfinished = smokePathExists(prefix + '/.winehua-init-in-progress');
    // No resetWinePrefix here: keep the interrupted cache, registry and sentinel.
    const recovered = await engine.startSession(settings.defaultDisplayMode,
      WinePresentationMode.DESKTOP, home, 'clean');
    const preserved = readSmokeText(sentinel) === request.runId;
    const initialized = testNapi.checkWinePrefix('clean') &&
      !smokePathExists(prefix + '/.winehua-init-in-progress');
    const resultsDir = `${resultsDrive}/smoke/results/${request.runId}`;
    ensureSmokeDirectory(resultsDir);
    writeSmokeText(resultsDir + '/recovery.json', JSON.stringify({
      interrupted, firstReady, unfinished, recovered, preserved, initialized,
      native: testNapi.getWineStartupState()
    } as WineRecoveryEvidence));
    await this.finishSuite(request, [{ schemaVersion: 1, runId: request.runId,
      testId: 'wine-startup-interrupted-recovery',
      status: interrupted && !firstReady && unfinished && recovered && preserved && initialized ? 'PASS' : 'FAIL',
      stage: 'recovered-without-reset', message: 'Disposable prefix only; original product data preserved' }], resultsDrive);
  }

  private async runLinuxSteamGnu(request: SmokeRequest, driveC: string): Promise<void> {
    const probe = smokeNapi.probeLinuxSteamGnu;
    if (!probe) {
      await this.finishSuite(request, [this.unsupported(request, 'Native GNU diagnostics are disabled')], driveC);
      return;
    }
    const resultsDir = `${driveC}/smoke/results/${request.runId}`;
    if (!ensureSmokeDirectory(resultsDir)) throw new Error('Cannot create native GNU result directory');
    if (!this.context) throw new Error('Steam diagnostic context is unavailable');
    const externalRoot = await prepareLinuxSteamExternalStorage(this.context);
    writeSmokeText(`${resultsDir}/external-storage.json`, JSON.stringify({ root: externalRoot, drive: 'Z:' }));
    const raw = await probe();
    writeSmokeText(`${resultsDir}/linux-steam-gnu.json`, raw);
    const result = JSON.parse(raw) as LinuxSteamGnuResult;
    const failure = result.exitControl;
    const timeout = result.timeoutControl;
    const passed = result.gnuExecutionVerified && !result.steamClientStarted && result.ncpExitVerified &&
      result.ncpExitSignal === 0 && failure !== undefined && failure.exitCode === 37 && failure.childReaped &&
      timeout !== undefined && timeout.timedOut && timeout.signal === 9 && timeout.childReaped;
    const test: SmokeTestResult = {
      schemaVersion: 1, runId: request.runId, testId: 'linux-steam-gnu',
      status: passed ? 'PASS' : 'FAIL', stage: result.status,
      message: 'Native GNU ARM64 loader and self-built fixture; Steam and graphics are not tested'
    };
    await this.finishSuite(request, [test], driveC);
  }

  private async runLinuxSteamClient(request: SmokeRequest, driveC: string): Promise<void> {
    const probe = smokeNapi.probeLinuxSteamGnuClient;
    if (!probe) {
      await this.finishSuite(request, [this.unsupported(request, 'Native GNU diagnostics are disabled')], driveC);
      return;
    }
    const resultsDir = `${driveC}/smoke/results/${request.runId}`;
    if (!ensureSmokeDirectory(resultsDir)) throw new Error('Cannot create Steam result directory');
    if (!this.context) throw new Error('Steam diagnostic context is unavailable');
    this.currentStatus = '准备原版 Steam 资源';
    this.notify();
    const preparingAt = Date.now();
    await prepareLinuxSteamClientData(this.context);
    writeSmokeText(`${resultsDir}/startup-timing.json`, JSON.stringify({
      resourcePreparationMs: Date.now() - preparingAt, probeStartedAt: Date.now()
    }));
    const external = readSmokeText('/data/storage/el2/base/files/steamohos-native/external-root.path').trim();
    const launchService = new LinuxSteamGameLaunch();
    const windowHost = new LinuxSteamWindowHost();
    const useWineWindow = readSmokeText('/data/storage/el2/base/files/steamohos-native/client-launch.conf')
      .split('\n').some((line: string): boolean => line.trim() === 'wineWindowHost=1');
    if (useWineWindow) await windowHost.start(this.context, driveC, resultsDir);
    const zRoot = await linuxSteamDownloadHome(this.context);
    writeSmokeText('/data/storage/el2/base/files/steamohos-native/wine-z-root.path', zRoot + '\n');
    launchService.start(`${external}/library`, resultsDir, zRoot);
    let raw: string;
    try { raw = await probe(); } finally { launchService.stop(); windowHost.stop(); }
    writeSmokeText(`${resultsDir}/linux-steam-client.json`, raw);
    const result = JSON.parse(raw) as LinuxSteamGnuResult;
    const test: SmokeTestResult = {
      schemaVersion: 1, runId: request.runId, testId: 'linux-steam-client',
      status: result.steamClientStarted ? 'PASS' : 'FAIL', stage: result.status,
      message: 'Original ARM64 Steam execution attempt; PASS requires actual Steam client startup'
    };
    await this.finishSuite(request, [test], driveC);
  }

  private async runLinuxSteamGnuXwayland(request: SmokeRequest, driveC: string): Promise<void> {
    const probe = smokeNapi.probeLinuxSteamGnuXwayland;
    if (!probe) {
      await this.finishSuite(request, [this.unsupported(request, 'Native GNU diagnostics are disabled')], driveC);
      return;
    }
    const resultsDir = `${driveC}/smoke/results/${request.runId}`;
    if (!ensureSmokeDirectory(resultsDir)) throw new Error('Cannot create GNU Xwayland result directory');
    const raw = await probe();
    writeSmokeText(`${resultsDir}/linux-steam-gnu-xwayland.json`, raw);
    const result = JSON.parse(raw) as LinuxSteamGnuResult;
    const passed = result.gnuExecutionVerified && result.x11Verified === true &&
      !result.steamClientStarted && result.ncpExitVerified && result.ncpExitSignal === 0;
    const test: SmokeTestResult = {
      schemaVersion: 1, runId: request.runId, testId: 'linux-steam-gnu-xwayland',
      status: passed ? 'PASS' : 'FAIL', stage: result.status,
      message: 'Native Xwayland and real X11 drawing/readback; input, GPU and Steam are separate gates'
    };
    await this.finishSuite(request, [test], driveC);
  }

  private async runLinuxSteamGnuWayland(request: SmokeRequest, driveC: string): Promise<void> {
    const probe = smokeNapi.probeLinuxSteamGnuWayland;
    if (!probe) {
      await this.finishSuite(request, [this.unsupported(request, 'Native GNU diagnostics are disabled')], driveC);
      return;
    }
    const resultsDir = `${driveC}/smoke/results/${request.runId}`;
    if (!ensureSmokeDirectory(resultsDir)) throw new Error('Cannot create GNU Wayland result directory');
    const raw = await probe();
    writeSmokeText(`${resultsDir}/linux-steam-gnu-wayland.json`, raw);
    const result = JSON.parse(raw) as LinuxSteamGnuResult;
    const passed = result.gnuExecutionVerified && result.gnuWaylandVerified === true &&
      !result.steamClientStarted && result.ncpExitVerified && result.ncpExitSignal === 0;
    const test: SmokeTestResult = {
      schemaVersion: 1, runId: request.runId, testId: 'linux-steam-gnu-wayland',
      status: passed ? 'PASS' : 'FAIL', stage: result.status,
      message: 'Native GNU libwayland SHM and bidirectional FD transfer; input events, scanout and Steam are not verified'
    };
    await this.finishSuite(request, [test], driveC);
  }

  private async runLinuxSteamHost(request: SmokeRequest, driveC: string): Promise<void> {
    const probe = smokeNapi.probeLinuxSteamHost;
    const inject = smokeNapi.injectLinuxSteamHostKeys;
    if (!probe || !inject) {
      await this.finishSuite(request, [this.unsupported(request,
        'Linux Steam host diagnostics are disabled in this build')], driveC);
      return;
    }
    const resultsDir = `${driveC}/smoke/results/${request.runId}`;
    if (!ensureSmokeDirectory(resultsDir)) throw new Error('Cannot create Linux Steam host result directory');
    const timer = setInterval((): void => { inject(); }, 100);
    try {
      const result = await probe();
      writeSmokeText(`${resultsDir}/linux-steam-host.json`, JSON.stringify(result));
      const passed = result.status === 'host_protocol_verified' && result.inputInjected &&
        result.inputVerified && !result.gnuExecutionVerified && !result.steamClientStarted;
      const metrics: Record<string, number> = {
        'framesSubmitted': result.framesSubmitted, 'bufferReleases': result.bufferReleases,
        'frameCallbacks': result.frameCallbacks, 'keyEvents': result.keyEvents
      };
      const test: SmokeTestResult = {
        schemaVersion: 1, runId: request.runId, testId: 'linux-steam-host',
        status: passed ? 'PASS' : 'FAIL', stage: result.stage,
        message: 'OHOS host SHM and synthetic keyboard probe; GNU/Steam and visible presentation are separate checks',
        metrics: metrics
      };
      await this.finishSuite(request, [test], driveC);
    } finally {
      clearInterval(timer);
    }
  }

  private async runTest(request: SmokeRequest, test: SmokeSuiteTest, resultsDir: string): Promise<SmokeTestResult> {
    // 结果协议键由 runner 控制; suites 声明的诊断键随后合并 (测试专属, 非产品语义)
    const environment: Record<string, string> = {
      'WINEHUA_SMOKE_RUN_ID': request.runId,
      'WINEHUA_SMOKE_TEST_ID': test.testId
    };
    const declared: Record<string, string> = test.env || {};
    for (const key of Object.keys(declared)) {
      if (declared[key] !== undefined) {
        environment[key] = declared[key];
      }
    }
    // seconds/timeoutMs: 缺失用默认; -1 = 取请求的 longSeconds (dxvk-long 系列)
    const unresolved: number = test.seconds === undefined ? 0 : test.seconds;
    const seconds: number = unresolved < 0 ? (request.longSeconds || 3600) : unresolved;
    const timeoutMs: number = test.timeoutMs === undefined ? 120000 :
      (test.timeoutMs < 0 ? seconds * 1000 + 90000 : test.timeoutMs);
    // arg 展开: 'std' (默认) = smoke 统一协议 argv, runner 生成
    // (--automation → 结果协议由 argv 承载, 见 winehua_smoke_protocol.h);
    // 'raw' = 原样 argv (d3d12 的 --frames/--result/--checkpoint), 占位符替换
    const resultPath: string = `${resultsDir}/${test.testId}.json`;
    const argv: string[] = [];
    if (test.argvMode === 'raw') {
      const declaredArgv: string[] = test.argv || [];
      for (const arg of declaredArgv) {
        argv.push(arg.split('<run-id>').join(request.runId)
          .split('<test-id>').join(test.testId));
      }
    } else {
      // 协议键必须 Windows 盘符形式 (C:\smoke\results\...): native 路径
      // 转反斜杠是无盘符形式 (\data\...), wine 解析为当前盘根下相对路径,
      // PE 结果落错目录 — runner 轮询 native resultPath 永远超时 (实测
      // 渲染 613 帧完成但结果文件未出现在轮询目录)。raw 模式由 suites
      // 自带盘符形式 argv, 不经过里。
      const std = ['--automation', '--run-id', request.runId,
        '--test-id', test.testId,
        '--result', `C:\\smoke\\results\\${request.runId}\\${test.testId}.json`];
      if (test.mode === 'offscreen') {
        argv.push(...std, '--offscreen');
      } else {
        argv.push(...std, '--present', '--seconds', `${seconds}`);
      }
      const extra: string[] = test.extraArgs || [];
      for (const arg of extra) {
        argv.push(arg);
      }
    }
    // d3d 后端: suites 显式声明 (回归跑固定后端), 缺省 = 当前档位 (UI 手动 core)
    const d3dBackend: string = test.d3dBackend ? test.d3dBackend :
      WineEngineService.getInstance().getD3dBackend();
    if (!['dxvk_legacy', 'dxvk_modern_2_6', 'wined3d'].includes(d3dBackend)) {
      return this.unsupported(request, 'native RunWineProgram does not support backend ' + d3dBackend, test.testId);
    }
    const derivedDxvk: string = d3dBackend === 'dxvk_modern_2_6' ? 'dxvk_modern_2_6' : 'dxvk_legacy';
    if (test.dxvkBackend && test.dxvkBackend !== derivedDxvk) {
      return this.unsupported(request, 'native RunWineProgram derives dxvkBackend from d3dBackend', test.testId);
    }
    const winPath: string = 'C:\\smoke\\' + test.exe.replace(/\//g, '\\');
    hilog.info(DOMAIN, TAG, 'spawn test=%{public}s exe=%{public}s seconds=%{public}d timeoutMs=%{public}d back=%{public}s',
      test.testId, winPath, seconds, timeoutMs, d3dBackend);
    const process = testNapi.runWineProgram({
      windowsExePath: winPath,
      argv: argv,
      environment: environment,
      workingDirectory: 'C:\\smoke',
      d3dBackend: d3dBackend
      // Presentation is derived by native from d3dBackend; offscreen/automation
      // are already carried in the smoke executable's supported argv protocol.
    }) as RunProgramResult;
    if (!process || process.pid <= 0) {
      return this.failure(request, 'startup', `runWineProgram failed: ${winPath}`, -1, test.testId);
    }
    // 轮询结果 JSON: 文件出现且 status 为终态即采; 超时记 FAIL(timeout)
    const deadline = Date.now() + timeoutMs;
    let lastText: string = '';
    while (Date.now() < deadline) {
      await new Promise<void>((r) => setTimeout(r, POLL_INTERVAL_MS));
      lastText = readSmokeText(resultPath);
      if (lastText) {
        try {
          const obj = JSON.parse(lastText) as SmokeResultFile;
          const status = obj.status ? `${obj.status}` : '';
          if (status && FINAL_STATUSES.includes(status)) {
            return {
              schemaVersion: 1,
              runId: request.runId,
              testId: test.testId,
              status: status,
              stage: obj.stage ? `${obj.stage}` : 'complete',
              pid: process.pid
            };
          }
        } catch (_) {
          // 半写 JSON: 继续轮询
        }
      }
    }
    hilog.error(DOMAIN, TAG, 'timeout test=%{public}s lastText=%{public}s',
      test.testId, lastText.substring(0, 200));
    return this.failure(request, 'timeout', `轮询超时 ${timeoutMs}ms (结果文件: ${resultPath})`, process.pid, test.testId);
  }

  private unsupported(request: SmokeRequest, message: string, testId?: string): SmokeTestResult {
    return { schemaVersion: 1, runId: request.runId, testId: testId || request.suite,
      status: 'UNSUPPORTED', stage: 'native-contract', message: message };
  }

  private failure(request: SmokeRequest, stage: string, message: string, pid: number, testId?: string): SmokeTestResult {
    return {
      schemaVersion: 1,
      runId: request.runId,
      testId: testId || request.suite,
      status: 'FAIL',
      stage: stage,
      message: message,
      pid: pid
    };
  }

  private async finishSuite(request: SmokeRequest, results: SmokeTestResult[], driveC: string): Promise<void> {
    const resultsDir: string = `${driveC}/smoke/results/${request.runId}`;
    ensureSmokeDirectory(resultsDir);
    const summary: SmokeSummary = {
      schemaVersion: 1,
      runId: request.runId,
      suite: request.suite,
      status: results.length > 0 && results.every((result: SmokeTestResult): boolean => result.status === 'PASS') ?
        'PASS' : (results.length > 0 && results.every((result: SmokeTestResult): boolean => result.status === 'UNSUPPORTED') ?
        'UNSUPPORTED' : 'FAIL'),
      tests: results
    };
    writeSmokeText(`${resultsDir}/suite-summary.json`, JSON.stringify(summary));
    this.currentResult = summary.status;
    this.currentStatus = `Smoke ${request.suite} ${summary.status} (${results[0] ? results[0].message : 'unknown'})`;
    if (summary.status === 'PASS') {
      hilog.info(DOMAIN, TAG, 'suite completed suite=%{public}s status=PASS', request.suite);
    } else {
      hilog.error(DOMAIN, TAG, 'suite failed early suite=%{public}s cause=%{public}s',
        request.suite, results[0] ? results[0].message : '');
    }
  }
}
