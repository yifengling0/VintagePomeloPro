import { skinColor } from '../service/SkinColors';

/** Shared library thumbnail/banner: loading and missing artwork never use the app icon. */
@Component
export struct SteamGameArtwork {
  @Prop @Watch('resetImage') appId: string = '';
  @Prop @Watch('resetImage') imageUrl: string = '';
  @Prop compact: boolean = false;
  @State private loaded: boolean = false;
  @State private failed: boolean = false;
  @State private useHeader: boolean = false;

  aboutToAppear(): void { this.resetImage(); }

  private resetImage(): void {
    this.loaded = false;
    this.useHeader = this.imageUrl.length === 0;
    this.failed = this.currentUrl().length === 0;
  }

  private headerUrl(): string {
    return /^\d+$/.test(this.appId)
      ? 'https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/' + this.appId + '/header.jpg'
      : '';
  }

  private currentUrl(): string {
    return this.useHeader ? this.headerUrl() : this.imageUrl;
  }

  private imageError(source: string): void {
    if (source !== this.currentUrl()) return;
    this.loaded = false;
    const header = this.headerUrl();
    if (!this.useHeader && header.length > 0 && header !== source) {
      // One known app-ID fallback; do not repeatedly retry a missing image.
      this.useHeader = true;
    } else {
      this.failed = true;
    }
  }

  @Builder
  private RemoteImage(source: string) {
    Image(source).width('100%').height('100%').objectFit(ImageFit.Cover).draggable(false)
      .opacity(this.loaded ? 1 : 0)
      .onComplete((): void => {
        if (source === this.currentUrl()) this.loaded = true;
      })
      .onError((): void => { this.imageError(source); })
  }

  build() {
    Stack() {
      if (!this.loaded) {
        Column({ space: this.compact ? 2 : 8 }) {
          if (!this.failed) {
            LoadingProgress().width(this.compact ? 16 : 32).height(this.compact ? 16 : 32)
              .color(skinColor('accent'))
          }
          Text(this.failed ? '暂无图片' : '加载中')
            .fontSize(this.compact ? 10 : 13).fontColor(skinColor('text_secondary'))
        }
        .width('100%').height('100%').justifyContent(FlexAlign.Center)
        .id('steam-artwork-status-' + this.appId)
      }
      if (!this.failed) {
        this.RemoteImage(this.currentUrl())
      }
    }
    .width('100%').height('100%').backgroundColor(skinColor('selected_background'))
  }
}
