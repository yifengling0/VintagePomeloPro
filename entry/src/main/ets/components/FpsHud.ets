import testNapi from 'libentry.so';
import { common } from '@kit.AbilityKit';
import { GlobalSettings } from '../model/AppModels';
import { PerformanceHudSettings, hasPerformanceHudContent, normalizePerformanceHud } from '../model/PerformanceHudSettings';
import { PerformanceListener, PerformanceMonitorService, PerformanceReading } from '../service/PerformanceMonitorService';
import { ThemeSettingsListener, ThemeSettingsService } from '../service/ThemeSettingsService';

@Component
export struct FpsHud {
  /** 0 selects the desktop root; other values address a managed Wine window. */
  @Prop toplevelId: number = 0;
  @Prop @Watch('onHideChanged') hide: boolean = false;
  @State private hudEnabled: boolean = false;
  @State private metrics: PerformanceHudSettings = normalizePerformanceHud();
  @State private text: string = '';
  private appeared: boolean = false;
  private settingsListener: ThemeSettingsListener | null = null;
  private performanceListener: PerformanceListener | null = null;

  aboutToAppear(): void {
    this.appeared = true;
    this.applySettings(ThemeSettingsService.getInstance().getSettings());
    this.settingsListener = (settings: GlobalSettings): void => { this.applySettings(settings); };
    ThemeSettingsService.getInstance().subscribe(this.settingsListener);
  }

  aboutToDisappear(): void {
    this.appeared = false;
    this.stopMonitoring();
    if (this.settingsListener) {
      ThemeSettingsService.getInstance().unsubscribe(this.settingsListener);
      this.settingsListener = null;
    }
  }

  private applySettings(settings: GlobalSettings): void {
    this.hudEnabled = settings.showFpsHud;
    this.metrics = normalizePerformanceHud(settings.performanceHud);
    this.onHideChanged();
  }

  onHideChanged(): void {
    this.stopMonitoring();
    if (!this.appeared || !this.active()) return;
    this.performanceListener = (reading: PerformanceReading, foreground: boolean): void => {
      if (!foreground) { this.text = ''; return; }
      const lines: string[] = [];
      if (this.metrics.fps) lines.push(this.readFps());
      if (this.metrics.appCpu && reading.appCpu !== '不可用') lines.push('应用CPU ' + this.shortValue(reading.appCpu));
      if (this.metrics.systemCpu && reading.systemCpu !== '不可用') lines.push('系统CPU ' + this.shortValue(reading.systemCpu));
      if (this.metrics.batteryTemperature && reading.batteryTemperature !== '不可用') {
        lines.push('电池 ' + this.shortValue(reading.batteryTemperature));
      }
      if (this.metrics.thermalLevel && reading.thermalLevel !== '不可用') {
        lines.push('热 ' + this.shortValue(reading.thermalLevel));
      }
      this.text = lines.join('  ·  ');
    };
    PerformanceMonitorService.getInstance().subscribe(
      getContext(this) as common.UIAbilityContext, this.performanceListener, this.metrics);
  }

  private stopMonitoring(): void {
    if (this.performanceListener) {
      PerformanceMonitorService.getInstance().unsubscribe(this.performanceListener);
      this.performanceListener = null;
    }
    this.text = '';
  }

  private active(): boolean {
    return this.hudEnabled && !this.hide && hasPerformanceHudContent(this.metrics);
  }

  private shortValue(value: string): string {
    return value === '采样中' ? '…' : value;
  }

  private readFps(): string {
    try {
      const fg = testNapi.getGameFrameGenerationStatus();
      if (fg.state > 0) {
        return Math.round(fg.presentedFps) + ' FPS' + (fg.generatedFps > 0 ? ' · 插帧' : '');
      }
      const id: number = this.toplevelId > 0 ? this.toplevelId : testNapi.getDesktopRootId() as number;
      const fps: number = testNapi.getDisplayFps(id) as number;
      return (Number.isFinite(fps) && fps >= 0 && fps <= 1000 ? Math.round(fps).toString() : '—') + ' FPS';
    } catch (_) { return '— FPS'; }
  }

  build() {
    if (this.active() && this.text.length > 0) {
      Text(this.text)
        .fontSize(10)
        .maxLines(1)
        .lineHeight(14)
        .fontWeight(FontWeight.Medium)
        .fontColor('#FF76FF03')
        .padding({ left: 6, right: 6, top: 2, bottom: 2 })
        .backgroundColor('#99000000')
        .borderRadius(3)
        .hitTestBehavior(HitTestMode.None)
        .id('performance-hud')
    }
  }
}
