import { DeckMotion } from './DeckMotion';
import { DeckCoverBounds } from '../../model/DeckLaunchArtwork';

/** Keeps the launch status legible across the launcher and Wine windows. */
@Component
export struct DeckLaunchScreen {
  @Prop title: string = '';
  @Prop @Watch('coverChanged') cover: string = '';
  @Prop builtin: Resource | undefined = undefined;
  @Prop message: string = '';
  @Prop hint: string = '';
  @Prop animate: boolean = false;
  @Prop origin: DeckCoverBounds | undefined = undefined;
  @StorageLink('hmosdeck.motion') @Watch('motionChanged') private motion: boolean = true;
  @State private entered: boolean = false;
  @State private artworkFailed: boolean = false;
  @State private target: DeckCoverBounds | undefined = undefined;
  private entryTimer: number = -1;

  private coverChanged(): void { this.artworkFailed = false; }

  aboutToAppear(): void {
    this.entered = !this.motion || !this.animate;
    if (!this.entered) this.scheduleEntry(this.origin ? 64 : 16);
  }

  private scheduleEntry(delay: number): void {
    this.cancelEntry();
    this.entryTimer = setTimeout((): void => {
      this.entryTimer = -1;
      this.getUIContext().animateTo({ duration: this.target ? 260 : 180, curve: DeckMotion.ease },
        (): void => { this.entered = true; });
    }, delay);
  }

  private cancelEntry(): void {
    if (this.entryTimer >= 0) clearTimeout(this.entryTimer);
    this.entryTimer = -1;
  }

  aboutToDisappear(): void { this.cancelEntry(); }

  private motionChanged(): void {
    if (this.motion) return;
    this.cancelEntry();
    this.getUIContext().animateTo({ duration: 0 }, (): void => { this.entered = true; });
  }

  private coverLaidOut(area: Area): void {
    if (this.entered || !this.origin || this.target) return;
    const width = Number(area.width);
    const height = Number(area.height);
    if (width <= 0 || height <= 0) return;
    this.target = { x: Number(area.globalPosition.x), y: Number(area.globalPosition.y), width, height };
    // Render at the card's position for one frame before moving the cover.
    this.scheduleEntry(16);
  }

  private coverOffset(horizontal: boolean): number {
    if (this.entered) return 0;
    if (!this.origin || !this.target) return horizontal ? 0 : 6;
    if (horizontal) return this.origin.x + this.origin.width / 2 - this.target.x - this.target.width / 2;
    return this.origin.y + this.origin.height / 2 - this.target.y - this.target.height / 2;
  }

  private coverScale(): number {
    if (this.entered) return 1;
    if (!this.origin || !this.target) return 0.96;
    return Math.min(this.origin.width / this.target.width, this.origin.height / this.target.height);
  }

  build() {
    Stack({ alignContent: Alignment.Center }) {
      Column()
        .width('100%').height('100%').backgroundColor('#0A0B0D')
        .linearGradient({ angle: 145, colors: [['#132333', 0], ['#0A0B0D', 0.65], ['#0A0B0D', 1]] })
        .opacity(this.entered || !this.origin ? 1 : 0.76)
      Column({ space: 16 }) {
        Stack({ alignContent: Alignment.Center }) {
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
          }.width('100%').height('100%').borderRadius(14).clip(true).backgroundColor('#172D40')
            .border({ width: 1, color: '#39536A' })
            .opacity(this.entered || !this.origin || this.target ? 1 : 0)
            .scale({ x: this.coverScale(), y: this.coverScale() })
            .translate({ x: this.coverOffset(true), y: this.coverOffset(false) })
            .id('deck-launch-artwork')
        }.width(this.cover || this.builtin ? 80 : 84).height(this.cover || this.builtin ? 120 : 98)
          .onAreaChange((_old: Area, area: Area): void => this.coverLaidOut(area))
        Text(this.title).fontSize(28).fontWeight(FontWeight.Bold).fontColor('#F2F4F7')
          .textAlign(TextAlign.Center).maxLines(2).width('100%').zIndex(1).id('deck-launch-title')
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
          .borderRadius(16).backgroundColor('#CC121417').border({ width: 1, color: '#293746' }).zIndex(1)
      }.width('84%').constraintSize({ maxWidth: 440 }).padding({ top: 24, bottom: 24 })
    }.width('100%').height('100%')
      .hitTestBehavior(HitTestMode.Block).onClick((): void => {})
      .accessibilityText('正在启动 ' + this.title).id('deck-launch-feedback')
  }
}
