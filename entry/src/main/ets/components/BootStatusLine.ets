import { skinColor } from '../service/SkinColors';

/** The current real startup step; replaced in place without a history queue. */
@Component
export struct BootStatusLine {
  @Prop @Watch('messageChanged') message: string = '';
  @Prop elapsedSec: number = 0;
  @StorageLink('hmosdeck.motion') private motion: boolean = true;
  @State private visibleLine: string = '';
  private mounted: boolean = false;

  aboutToAppear(): void {
    this.visibleLine = this.message;
    this.mounted = true;
  }

  aboutToDisappear(): void { this.mounted = false; }

  private messageChanged(): void {
    if (this.visibleLine === this.message) return;
    if (!this.mounted || !this.motion) {
      this.visibleLine = this.message;
      return;
    }
    this.getUIContext().animateTo({ duration: 220, curve: Curve.EaseOut }, (): void => {
      this.visibleLine = this.message;
    });
  }

  build() {
    Row({ space: 12 }) {
      LoadingProgress().width(18).height(18).color(skinColor('accent'))
      Stack({ alignContent: Alignment.Start }) {
        ForEach([this.visibleLine], (line: string) => {
          Text(line).fontSize(16).fontColor(skinColor('text_primary'))
            .width('100%').maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
            .transition(this.motion ? TransitionEffect.asymmetric(
              TransitionEffect.OPACITY.combine(TransitionEffect.translate({ y: 9 })),
              TransitionEffect.OPACITY.combine(TransitionEffect.translate({ y: -9 }))) : TransitionEffect.IDENTITY)
            .id('wine-startup-status')
        }, (line: string) => line)
      }.height(36).layoutWeight(1).clip(true)
      if (this.elapsedSec >= 20) {
        Text(this.elapsedSec + 's').fontSize(12).fontColor(skinColor('text_secondary'))
          .width(42).textAlign(TextAlign.End).accessibilityText('已等待 ' + this.elapsedSec + ' 秒')
      }
    }.width('100%').height(40)
  }
}
