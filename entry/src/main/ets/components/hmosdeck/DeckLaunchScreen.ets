import { DeckMotion } from './DeckMotion';

/** One opaque launch surface; no library badge, fallback title or dialog chrome. */
@Component
export struct DeckLaunchScreen {
  @Prop title: string = '';
  @Prop @Watch('coverChanged') cover: string = '';
  @Prop builtin: Resource | undefined = undefined;
  @Prop message: string = '';
  @StorageLink('hmosdeck.motion') private motion: boolean = true;
  @State private entered: boolean = false;
  @State private artworkFailed: boolean = false;
  private entryTimer: number = -1;

  private coverChanged(): void { this.artworkFailed = false; }

  aboutToAppear(): void {
    this.entered = !this.motion;
    if (this.motion) this.entryTimer = setTimeout((): void => {
      this.entryTimer = -1;
      this.getUIContext().animateTo({ duration: 220, curve: DeckMotion.ease }, (): void => { this.entered = true; });
    }, 16);
  }

  aboutToDisappear(): void {
    if (this.entryTimer >= 0) clearTimeout(this.entryTimer);
    this.entryTimer = -1;
  }

  build() {
    Stack({ alignContent: Alignment.Center }) {
      Column({ space: 22 }) {
        if (this.cover && !this.artworkFailed) {
          Image(this.cover).width(96).height(144).objectFit(ImageFit.Contain).draggable(false)
            .borderRadius(10).onError((): void => { this.artworkFailed = true; })
        } else if (this.builtin && !this.artworkFailed) {
          Image(this.builtin).width(96).height(144).objectFit(ImageFit.Contain).draggable(false)
            .borderRadius(10).onError((): void => { this.artworkFailed = true; })
        } else {
          Stack({ alignContent: Alignment.Center }) {
            SymbolGlyph($r('sys.symbol.gamecontroller')).fontSize(52).fontColor(['#F2F4F7'])
          }.width(104).height(104).borderRadius(30).backgroundColor('#172738')
        }
        Text(this.title).fontSize(28).fontWeight(FontWeight.Bold).fontColor('#F2F4F7')
          .textAlign(TextAlign.Center).maxLines(2).width('100%').id('deck-launch-title')
        LoadingProgress().width(26).height(26).color('#1A9FFF')
        Text(this.message).fontSize(14).fontColor('#9AA3AF').textAlign(TextAlign.Center)
          .maxLines(3).width('100%').id('deck-launch-status')
      }.width('84%').constraintSize({ maxWidth: 440 }).padding({ top: 24, bottom: 24 })
        .opacity(this.entered ? 1 : 0).translate({ y: this.entered ? 0 : 8 })
        .scale({ x: this.entered ? 1 : 0.97, y: this.entered ? 1 : 0.97 })
    }.width('100%').height('100%').backgroundColor('#0A0B0D')
      .linearGradient({ angle: 145, colors: [['#132333', 0], ['#0A0B0D', 0.65], ['#0A0B0D', 1]] })
      .hitTestBehavior(HitTestMode.Block).onClick((): void => {})
      .accessibilityText('正在启动 ' + this.title).id('deck-launch-feedback')
  }
}
