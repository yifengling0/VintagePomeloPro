import { DeckMotion } from './DeckMotion';

/** Keeps the launch status legible across the launcher and Wine windows. */
@Component
export struct DeckLaunchScreen {
  @Prop title: string = '';
  @Prop @Watch('coverChanged') cover: string = '';
  @Prop builtin: Resource | undefined = undefined;
  @Prop message: string = '';
  @Prop hint: string = '';
  @StorageLink('hmosdeck.motion') private motion: boolean = true;
  @State private entered: boolean = false;
  @State private artworkFailed: boolean = false;
  private entryTimer: number = -1;

  private coverChanged(): void { this.artworkFailed = false; }

  aboutToAppear(): void {
    this.entered = !this.motion;
    if (this.motion) this.entryTimer = setTimeout((): void => {
      this.entryTimer = -1;
      this.getUIContext().animateTo({ duration: 200, curve: DeckMotion.ease }, (): void => { this.entered = true; });
    }, 16);
  }

  aboutToDisappear(): void {
    if (this.entryTimer >= 0) clearTimeout(this.entryTimer);
    this.entryTimer = -1;
  }

  build() {
    Stack({ alignContent: Alignment.Center }) {
      Column({ space: 16 }) {
        Stack({ alignContent: Alignment.Center }) {
          if (this.cover && !this.artworkFailed) {
            Image(this.cover).width('100%').height('100%').objectFit(ImageFit.Contain).draggable(false)
              .onError((): void => { this.artworkFailed = true; })
          } else if (this.builtin && !this.artworkFailed) {
            Image(this.builtin).width('100%').height('100%').objectFit(ImageFit.Contain).draggable(false)
              .onError((): void => { this.artworkFailed = true; })
          } else {
            SymbolGlyph($r('sys.symbol.gamecontroller')).fontSize(42).fontColor(['#C9E6FF'])
          }
        }.width(84).height(98).borderRadius(18).clip(true).backgroundColor('#172D40')
          .border({ width: 1, color: '#39536A' })
          .translate({ y: this.entered || !this.motion ? 0 : 6 })
          .scale({ x: this.entered || !this.motion ? 1 : 0.96, y: this.entered || !this.motion ? 1 : 0.96 })
        Text(this.title).fontSize(28).fontWeight(FontWeight.Bold).fontColor('#F2F4F7')
          .textAlign(TextAlign.Center).maxLines(2).width('100%').id('deck-launch-title')
        Column({ space: 8 }) {
          Row({ space: 9 }) {
            LoadingProgress().width(18).height(18).color('#1A9FFF')
            Text(this.message).fontSize(14).fontWeight(FontWeight.Medium).fontColor('#F2F4F7')
              .id('deck-launch-status')
          }.justifyContent(FlexAlign.Center)
          if (this.hint) {
            Text(this.hint).fontSize(13).fontColor('#AEBBC9').textAlign(TextAlign.Center)
              .width('100%').id('deck-launch-hint')
          }
        }.width('100%').padding({ left: 16, right: 16, top: 12, bottom: 12 })
          .borderRadius(16).backgroundColor('#CC121417').border({ width: 1, color: '#293746' })
      }.width('84%').constraintSize({ maxWidth: 440 }).padding({ top: 24, bottom: 24 })
    }.width('100%').height('100%').backgroundColor('#0A0B0D')
      .linearGradient({ angle: 145, colors: [['#132333', 0], ['#0A0B0D', 0.65], ['#0A0B0D', 1]] })
      .hitTestBehavior(HitTestMode.Block).onClick((): void => {})
      .accessibilityText('正在启动 ' + this.title).id('deck-launch-feedback')
  }
}
