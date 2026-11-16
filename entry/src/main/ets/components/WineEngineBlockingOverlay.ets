import { HostWindowTasks } from '../service/HostWindowTasks';
import { skinColor } from '../service/SkinColors';
import { EngineState } from '../model/AppModels';
import { EngineStateListener, WineEngineService } from '../service/WineEngineService';
import { InputOverlayWindowCoordinator } from '../service/InputOverlayWindowCoordinator';
import { DeckLaunchFeedback, DeckLaunchFeedbackListener, DeckLaunchFeedbackService } from '../service/DeckLaunchFeedbackService';
import { DeckLaunchScreen } from './hmosdeck/DeckLaunchScreen';
import { BootStatusLine } from './BootStatusLine';

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
  @State private launch: DeckLaunchFeedback | null = DeckLaunchFeedbackService.getInstance().snapshot();
  private launchListener: DeckLaunchFeedbackListener | null = null;
  private listener: EngineStateListener | null = null;
  private phaseTicker: number = -1;

  aboutToAppear(): void {
    this.launchListener = (feedback: DeckLaunchFeedback | null): void => { this.launch = feedback; };
    DeckLaunchFeedbackService.getInstance().subscribe(this.launchListener);
    this.listener = (state: EngineState, message: string): void => {
      this.state = state;
      this.message = message;
      this.phaseText = WineEngineService.getInstance().getPhaseText();
      this.elapsedSec = Math.floor(WineEngineService.getInstance().getPreparationElapsedMs() / 1000);
      // The input page owns a separate child window above the native Wine
      // surface. Keep that window alive and switch its hit-test state while
      // the engine is preparing, so XComponent frames cannot bypass this UI.
      InputOverlayWindowCoordinator.getInstance().setEngineBusy(this.isBusy() || this.state === EngineState.ERROR).catch(() => {});
      this.syncPhaseTicker();
    };
    WineEngineService.getInstance().subscribe(this.listener);
    InputOverlayWindowCoordinator.getInstance().setEngineBusy(this.isBusy() || this.state === EngineState.ERROR).catch(() => {});
    this.syncPhaseTicker();
  }

  aboutToDisappear(): void {
    if (this.launchListener) DeckLaunchFeedbackService.getInstance().unsubscribe(this.launchListener);
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

  private statusLine(): string {
    if (this.state === EngineState.ERROR) return this.message;
    if (this.isBusy()) {
      if (this.phaseText) return this.phaseText;
      return this.state === EngineState.SWITCHING ? '正在重建显示会话' : '正在准备运行组件';
    }
    return this.launch ? this.launch.message : this.message;
  }

  build() {
    if (this.launch && !this.launch.classic && this.state !== EngineState.ERROR) {
      DeckLaunchScreen({ title: this.launch.title, cover: this.launch.cover,
        builtin: this.launch.builtin, message: this.statusLine(), elapsedSec: this.elapsedSec })
        .width('100%').height('100%').zIndex(1000)
    } else if (this.state !== EngineState.ERROR && (this.launch || this.isBusy())) {
      Stack({ alignContent: Alignment.Center }) {
        BootStatusLine({ message: this.statusLine(), elapsedSec: this.elapsedSec })
          .width('84%').constraintSize({ maxWidth: 440 })
          .id('wine-engine-progress-panel')
      }.width('100%').height('100%').backgroundColor(skinColor('start_window_background'))
        .zIndex(1000).hitTestBehavior(HitTestMode.Block).id('wine-engine-blocking-overlay')
    } else if (this.state === EngineState.ERROR) {
      Stack() {
        Column()
          .width('100%').height('100%')
          .backgroundColor('#E80B1220')
          .hitTestBehavior(HitTestMode.Block)
        Column({ space: 16 }) {
          Text('Wine 启动未完成')
            .fontSize(22).fontWeight(FontWeight.Bold).fontColor(skinColor('text_primary'))
            .textAlign(TextAlign.Center)
            .id('wine-engine-blocking-title')
          Text(this.message)
            .fontSize(15).fontColor(skinColor('text_secondary')).textAlign(TextAlign.Center)
            .constraintSize({ maxWidth: 420 })
          Text('可重新启动，或返回主页检查 Wine 设置')
            .fontSize(13).fontColor(skinColor('text_secondary')).textAlign(TextAlign.Center)
            .constraintSize({ maxWidth: 420 })
          if (this.state === EngineState.ERROR) {
            Row({ space: 12 }) {
              Button('返回主页').onClick(() => {
                HostWindowTasks.getInstance().showLauncher().catch(() => {});
              }).id('wine-engine-return-home')
              Button('重新启动').onClick(() => {
                WineEngineService.getInstance().ensureEngineReady().catch(() => {});
              }).id('wine-engine-retry')
            }
          }
        }
        .width('86%').constraintSize({ maxWidth: 480 })
        .padding({ left: 24, right: 24, top: 28, bottom: 28 })
        .backgroundColor(skinColor('card_background'))
        .border({ width: 1, color: skinColor('selected_background') })
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
