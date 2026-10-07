import { EngineState } from '../model/AppModels';
import { EngineStateListener, WineEngineService } from '../service/WineEngineService';
import { InputOverlayWindowCoordinator } from '../service/InputOverlayWindowCoordinator';

/**
 * Blocks the actual Wine host window while the native runtime is preparing.
 * The application-library overlay cannot cover DesktopAbility because that
 * ability owns a separate window, so the blocker must live in both Wine pages.
 */
@Component
export struct WineEngineBlockingOverlay {
  @State private state: EngineState = WineEngineService.getInstance().getState();
  @State private message: string = WineEngineService.getInstance().getMessage();
  @State private phaseText: string = WineEngineService.getInstance().getPhaseText();
  @State private elapsedSec: number = 0;
  private listener: EngineStateListener | null = null;
  private phaseTicker: number = -1;

  aboutToAppear(): void {
    this.listener = (state: EngineState, message: string): void => {
      this.state = state;
      this.message = message;
      this.phaseText = WineEngineService.getInstance().getPhaseText();
      this.elapsedSec = Math.floor(WineEngineService.getInstance().getPreparationElapsedMs() / 1000);
      // The input page owns a separate child window above the native Wine
      // surface. Keep that window alive and switch its hit-test state while
      // the engine is preparing, so XComponent frames cannot bypass this UI.
      InputOverlayWindowCoordinator.getInstance().setEngineBusy(this.isBusy()).catch(() => {});
      this.syncPhaseTicker();
    };
    WineEngineService.getInstance().subscribe(this.listener);
    InputOverlayWindowCoordinator.getInstance().setEngineBusy(this.isBusy()).catch(() => {});
    this.syncPhaseTicker();
  }

  aboutToDisappear(): void {
    if (this.phaseTicker >= 0) {
      clearInterval(this.phaseTicker);
      this.phaseTicker = -1;
    }
    if (this.listener) WineEngineService.getInstance().unsubscribe(this.listener);
  }

  private isBusy(): boolean {
    return this.state === EngineState.PREPARING || this.state === EngineState.SWITCHING;
  }

  private syncPhaseTicker(): void {
    if (this.isBusy() && this.phaseTicker < 0) {
      this.phaseTicker = setInterval(() => {
        this.elapsedSec = Math.floor(WineEngineService.getInstance().getPreparationElapsedMs() / 1000);
      }, 1000);
    } else if (!this.isBusy() && this.phaseTicker >= 0) {
      clearInterval(this.phaseTicker);
      this.phaseTicker = -1;
    }
  }

  private title(): string {
    return this.state === EngineState.SWITCHING ? '正在重建 Wine 桌面' : '正在初始化 Wine';
  }

  private detail(): string {
    const elapsed = '（已 ' + this.elapsedSec + ' 秒）';
    if (this.phaseText) return '当前阶段：' + this.phaseText + elapsed;
    if (this.state === EngineState.SWITCHING) return '正在关闭旧会话并重新建立 Wayland 显示' + elapsed;
    return '首次启动会解压运行环境并初始化 Windows，可能需要几分钟' + elapsed;
  }

  build() {
    if (this.isBusy()) {
      Stack() {
        Column()
          .width('100%').height('100%')
          .backgroundColor('#E80B1220')
          .hitTestBehavior(HitTestMode.Block)
        Column({ space: 16 }) {
          LoadingProgress().width(56).height(56).color('#2F6BFF')
          Text(this.title())
            .fontSize(22).fontWeight(FontWeight.Bold).fontColor('#101828')
            .textAlign(TextAlign.Center)
            .id('wine-engine-blocking-title')
          Text(this.message)
            .fontSize(15).fontColor('#344054').textAlign(TextAlign.Center)
            .constraintSize({ maxWidth: 420 })
          Text(this.detail())
            .fontSize(13).fontColor('#667085').textAlign(TextAlign.Center)
            .constraintSize({ maxWidth: 420 })
        }
        .width('86%').constraintSize({ maxWidth: 480 })
        .padding({ left: 24, right: 24, top: 28, bottom: 28 })
        .backgroundColor('#FFFFFFFF')
        .border({ width: 2, color: '#FFB8C7E8' })
        .borderRadius(20)
        .shadow({ radius: 24, color: '#66000000', offsetX: 0, offsetY: 10 })
        .hitTestBehavior(HitTestMode.Block)
        .id('wine-engine-progress-panel')
      }
      .width('100%').height('100%')
      .alignContent(Alignment.Center)
      .zIndex(1000)
      .hitTestBehavior(HitTestMode.Block)
      .id('wine-engine-blocking-overlay')
    }
  }
}
