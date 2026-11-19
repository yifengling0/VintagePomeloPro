import testNapi, { GamescopeSessionStatus, HarmonyFrameGenerationModeCapability } from 'libentry.so';
import { router } from '@kit.ArkUI';
import { GamescopeRuntimeService } from '../service/GamescopeRuntimeService';
import { hilog } from '../service/LogService';
import { hiAppEvent } from '@kit.PerformanceAnalysisKit';
import { common } from '@kit.AbilityKit';
import { ConfigurationFiles } from '../service/ConfigurationFiles';

class GamescopeDiagnosticSurface extends XComponentController {
  harmonyRun: string = '';
  surfaceId: string = '0';
  width: number = 800;
  height: number = 600;
  onSurfaceCreated(surfaceId: string): void { this.surfaceId = surfaceId; }
  onSurfaceChanged(surfaceId: string, rect: SurfaceRect): void {
    this.surfaceId = surfaceId;
    this.width = Math.round(rect.surfaceWidth); this.height = Math.round(rect.surfaceHeight);
    if (testNapi.getGamescopeSessionStatus().active) {
      testNapi.setGamescopeOutputState(this.width, this.height, true);
    }
  }
  onSurfaceDestroyed(surfaceId: string): void {
    if (this.surfaceId === surfaceId) this.surfaceId = '0';
    if (this.harmonyRun !== '') testNapi.stopHostVulkanProbe();
    testNapi.stopGamescopeSession();
  }
}

// Debug-signed package entry only. This reports each actual native gate and
// does not qualify a game/backend merely because a swapchain can be created.
@Entry
@Component
struct GamescopeDiagnostics {
  private surface: GamescopeDiagnosticSurface = new GamescopeDiagnosticSurface();
  @State private message: string = '尚未启动呈现检查';
  @State private busy: boolean = false;
  @State private active: boolean = false;
  @State private displayReady: boolean = false;
  private timer: number = -1;
  private deadline: number = 0;
  private lastStage: string = '';
  private leaving: boolean = false;
  @State private outputPercent: number = 100;
  @State private frameGenerationMessage: string = '鸿蒙超帧能力尚未查询';
  @State private harmonyGpuSupported: boolean = false;
  @State private harmonyRunning: boolean = false;
  @State private harmonyResult: string = '';
  private harmonyTimer: number = -1;

  private faultWatcher: hiAppEvent.Watcher = {
    name: 'vpp_gamescope_diagnostics',
    appEventFilters: [{ domain: hiAppEvent.domain.OS, names: [hiAppEvent.event.APP_CRASH] }],
    onReceive: (_domain: string, groups: hiAppEvent.AppEventGroup[]): void => {
      try {
        const context = getContext(this) as common.UIAbilityContext;
        ConfigurationFiles.json(context.filesDir + '/gamescope-crash-events.json',
          JSON.stringify(groups).slice(0, 1024 * 1024));
      } catch (_) { hilog.warn(0, 'Gamescope', 'Crash event snapshot unavailable'); }
    }
  };

  aboutToAppear(): void {
    void this.queryFrameGeneration();
    try {
      // Own-application crash events only, no protected system-file access.
      hiAppEvent.addWatcher(this.faultWatcher);
    } catch (_) { hilog.warn(0, 'Gamescope', 'Crash event service unavailable'); }
  }

  private async queryFrameGeneration(): Promise<void> {
    try {
      const capability = await testNapi.probeHarmonyFrameGeneration();
      if (this.leaving) return;
      this.harmonyGpuSupported = capability.gpuVulkan.vulkanEntryPointsAvailable;
      this.frameGenerationMessage = '鸿蒙 GPU 超帧 · ' + this.formatFrameGenerationMode(capability.gpuVulkan) +
        '\n鸿蒙 AI 超帧 · ' + this.formatFrameGenerationMode(capability.aiVulkan);
      hilog.info(0, 'GamescopeFG', 'capability=%{public}s', JSON.stringify(capability));
      const context = getContext(this) as common.UIAbilityContext;
      ConfigurationFiles.json(context.filesDir + '/gamescope-frame-generation-capability.json', JSON.stringify(capability));
    } catch (_) {
      if (!this.leaving) this.frameGenerationMessage = '鸿蒙超帧能力查询失败';
    }
  }

  private formatFrameGenerationMode(mode: HarmonyFrameGenerationModeCapability): string {
    if (mode.stage === 'candidate-available') return '设备支持';
    if (mode.stage === 'runtime-check-required') return '可运行场景检查';
    if (mode.stage === 'device-unsupported') return '设备未报告支持';
    if (mode.stage === 'vulkan-entry-point-unavailable') return '缺少入口 · ' + mode.detail;
    if (mode.stage === 'support-query-unavailable') return '系统缺少能力查询接口';
    return '系统超帧库不可用';
  }

  aboutToDisappear(): void {
    this.leaving = true;
    if (this.timer !== -1) clearTimeout(this.timer);
    this.timer = -1;
    if (this.harmonyTimer !== -1) clearTimeout(this.harmonyTimer);
    this.harmonyTimer = -1;
    if (this.surface.harmonyRun !== '') testNapi.stopHostVulkanProbe();
    try { hiAppEvent.removeWatcher(this.faultWatcher); } catch (_) {}
    testNapi.stopGamescopeSession();
  }

  onPageHide(): void {
    if (this.harmonyRunning) testNapi.stopHostVulkanProbe();
  }

  private startHarmonyGpu(): void {
    if (this.surface.surfaceId === '0') {
      this.harmonyResult = '等待显示窗口创建';
      return;
    }
    const runId = 'harmony-gpu-' + Date.now();
    if (!testNapi.runHostVulkanProbe(BigInt(this.surface.surfaceId), runId, true)) {
      this.harmonyResult = '呈现资源正忙，请稍后重试';
      return;
    }
    this.surface.harmonyRun = runId;
    this.harmonyRunning = true;
    this.harmonyResult = '正在渲染测试场景';
    this.pollHarmonyGpu();
  }

  private pollHarmonyGpu(): void {
    if (this.leaving) return;
    const status = testNapi.getHarmonyGpuProbeResult();
    if (status.runId !== this.surface.harmonyRun) return;
    if (status.stage === 'starting' || status.stage === 'running') {
      this.harmonyTimer = setTimeout(() => this.pollHarmonyGpu(), 250);
      return;
    }
    this.harmonyTimer = -1;
    this.harmonyRunning = false;
    if (status.stage === 'passed') {
      this.harmonyResult = '送显成功 · 关闭时真实 ' + status.baselineFrames +
        ' 帧 · 开启后真实 ' + status.realFrames + ' 帧 · 生成 ' + status.generatedFrames + ' 帧';
    } else if (status.stage === 'cancelled') {
      this.harmonyResult = '测试已停止';
    } else {
      this.harmonyResult = status.detail + (status.sdkError === 0 ? '' : ' · ' + status.sdkError);
    }
  }

  private async start(): Promise<void> {
    this.busy = true;
    try {
      const runtime = await GamescopeRuntimeService.query();
      if (!runtime.standaloneOutputSupported) {
        this.message = '手机使用应用内呈现适配；独立 Gamescope 输出尚未开放';
        return;
      }
      if (!runtime.loaded) { this.message = runtime.stage + ': ' + runtime.detail; return; }
      if (this.surface.surfaceId === '0') { this.message = '输出窗口尚未创建'; return; }
      const rect = this.surface.getXComponentSurfaceRect();
      this.surface.width = Math.round(rect.surfaceWidth);
      this.surface.height = Math.round(rect.surfaceHeight);
      this.deadline = Date.now() + 15000;
      this.lastStage = '';
      const result = await testNapi.startGamescopeSession({
        surfaceId: BigInt(this.surface.surfaceId), renderWidth: 800, renderHeight: 600,
        outputWidth: this.surface.width, outputHeight: this.surface.height,
        filter: 0, fpsLimit: 0, sharpness: 2
      });
      if (result !== 0) { this.message = '子进程启动失败: ' + result.toString(); return; }
      this.active = true;
      this.poll();
    } catch (error) {
      this.message = '呈现检查无法启动';
    } finally { this.busy = false; }
  }

  private poll(): void {
    if (this.leaving) return;
    const status: GamescopeSessionStatus = testNapi.getGamescopeSessionStatus();
    this.active = status.active;
    this.displayReady = status.displayReady;
    this.message = status.stage + ' · ' + status.detail + ' · error=' + status.error.toString();
    if (status.stage !== this.lastStage) {
      this.lastStage = status.stage;
      hilog.info(0, 'Gamescope', 'session=%{public}s pid=%{public}d output=%{public}s display=%{public}s presented=%{public}s detail=%{public}s',
        status.stage, status.pid, String(status.outputReady), String(status.displayReady), String(status.presented), status.detail);
    }
    if (Date.now() > this.deadline && !status.displayReady && status.active) {
      testNapi.stopGamescopeSession();
      this.deadline = Date.now() + 15000;
      this.message = '显示服务未就绪，正在请求正常停止';
    }
    if (this.active) this.timer = setTimeout(() => this.poll(), 250);
    else this.timer = -1;
  }

  build() {
    Column({ space: 12 }) {
      Row({ space: 12 }) {
        Button('返回').onClick(() => router.back())
        Text('Gamescope 呈现检查').fontSize(20).fontColor('#FFFFFF').layoutWeight(1)
      }.width('100%')
      Flex({ wrap: FlexWrap.Wrap }) {
        Button('切换尺寸').id('gamescope-resize').enabled(this.active)
          .margin({ right: 8, bottom: 6 })
          .onClick(() => { this.outputPercent = this.outputPercent === 100 ? 75 : this.outputPercent === 75 ? 50 : 100; })
        Button('显示测试图').id('gamescope-pixel-probe').enabled(this.active && this.displayReady)
          .margin({ right: 8, bottom: 6 })
          .onClick(() => {
            const result = testNapi.startGamescopeDiagnosticClient();
            if (result !== 0) this.message = '测试图启动失败: ' + result.toString();
          })
        Button('启动检查').id('gamescope-start').enabled(!this.busy && !this.active && !this.harmonyRunning)
          .margin({ right: 8, bottom: 6 })
          .onClick(() => { void this.start(); })
        Button('正常停止').id('gamescope-stop').enabled(this.active)
          .margin({ bottom: 6 })
          .onClick(() => { testNapi.stopGamescopeSession(); })
      }.width('100%')
      Text(this.message).id('gamescope-state').fontColor('#FFFFFF').fontSize(14).width('100%')
      Text(this.frameGenerationMessage).id('gamescope-frame-generation-state').fontColor('#B8C5D9').fontSize(14).width('100%')
      Row({ space: 8 }) {
        Button('验证鸿蒙 GPU 超帧').id('harmony-gpu-run')
          .enabled(this.harmonyGpuSupported && !this.busy && !this.active && !this.harmonyRunning)
          .onClick(() => this.startHarmonyGpu())
        if (this.harmonyRunning) {
          Button('停止').id('harmony-gpu-stop').onClick(() => { testNapi.stopHostVulkanProbe(); })
        }
      }.width('100%')
      if (this.harmonyResult !== '') {
        Text(this.harmonyResult).id('harmony-gpu-result').fontSize(13).fontColor('#B8C5D9').width('100%')
      }
      XComponent({ id: 'gamescope-output', type: XComponentType.SURFACE, controller: this.surface })
        .width(this.outputPercent.toString() + '%').layoutWeight(1).backgroundColor('#000000')
    }.width('100%').height('100%').padding(24).backgroundColor('#121722')
  }
}
