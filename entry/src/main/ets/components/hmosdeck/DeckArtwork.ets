// Painted fallback stays behind the decoded capsule, including while offline.
@Component
export struct DeckArtwork {
  @Prop name: string = '';
  @Prop @Watch('sourceChanged') source: string = '';
  @Prop appId: string = '';
  @Prop libraryLabel: string = 'STEAM LIBRARY';
  @Prop builtin: Resource | undefined = undefined;
  @Prop badge: string = '';
  @State private failed: boolean = false;
  @State private loaded: boolean = false;
  private sourceChanged(): void { this.failed = false; this.loaded = false; }
  private tone(): string {
    const colors = ['#2C5563', '#4D3B64', '#644535', '#2D5A48', '#35476C'];
    let hash = 0;
    for (let i = 0; i < this.name.length; i++) hash = (hash * 31 + this.name.charCodeAt(i)) >>> 0;
    return colors[hash % colors.length];
  }
  build() {
    Stack({ alignContent: Alignment.BottomStart }) {
      Column({ space: 14 }) {
        if (!this.badge) Text(this.libraryLabel).fontSize(9).letterSpacing(3).fontColor('#9EFFFFFF')
        Blank()
        Text(this.name).fontSize(21).fontWeight(FontWeight.Bold).fontColor('#F2F4F7').maxLines(4)
        Row().width(30).height(2).backgroundColor('#7FFFFFFF')
      }.width('100%').height('100%').alignItems(HorizontalAlign.Start).padding(20)
        .linearGradient({ angle: 145, colors: [[this.tone(), 0], ['#151C24', 0.65], ['#0D1016', 1]] })
      if (this.source && !this.failed) {
        // Let ArkUI decode with the original aspect ratio. A fixed decode width
        // AND height reshaped landscape artwork before objectFit was applied.
        Image(this.source).width('100%').height('100%').objectFit(ImageFit.Contain)
          .backgroundColor('#12171E').draggable(false)
          .onComplete((): void => { this.loaded = true; })
          .onError((): void => { this.failed = true; })
          .id('deck-art-' + this.appId).accessibilityText(this.name + (this.loaded ? ' 封面已加载' : ' 封面加载中'))
      } else if (this.builtin) {
        Image(this.builtin).width('100%').height('100%').objectFit(ImageFit.Cover)
          .backgroundColor('#12171E').draggable(false)
          .id('deck-art-' + this.appId).accessibilityText(this.name + ' 内置封面')
      }
      if (this.badge) {
        Text(this.badge).fontSize(10).fontWeight(FontWeight.Medium).fontColor('#F2F4F7')
          .padding({ left: 9, right: 9, top: 5, bottom: 5 }).backgroundColor('#D9121417').borderRadius(7)
          .position({ x: 10, y: 10 })
      }
    }.width('100%').height('100%').clip(true).borderRadius(14)
  }
}
