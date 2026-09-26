import { AppDescriptor, AppSource, ConsoleCardLayout } from '../model/AppModels';
import { builtinCoverResource } from '../model/BuiltinArtwork';
import { skinColor } from '../service/SkinColors';

@Component
export struct ConsoleGameCard {
  @Prop @Watch('onArtworkChanged') app: AppDescriptor;
  @Prop layout: ConsoleCardLayout = ConsoleCardLayout.GALLERY;
  @Prop @Watch('onArtworkChanged') backgroundPath: string = '';
  @Prop focused: boolean = false;
  @Prop running: boolean = false;
  @Prop favorite: boolean = false;
  @Prop cardWidth: number = 220;
  @Prop cardHeight: number = 270;
  @Prop lastPlayed: string = '';
  @Prop inRing: boolean = false;
  @Prop ringPosition: number = 0;
  @Prop ringShiftX: number = 0;
  @State private artFailed: boolean = false;
  private touchStartX: number = 0;
  private touchStartY: number = 0;
  private touchMoved: boolean = false;
  onSelect: (app: AppDescriptor) => void = (_app: AppDescriptor): void => {};
  onLaunch: (app: AppDescriptor) => void = (_app: AppDescriptor): void => {};
  onDetails: (app: AppDescriptor) => void = (_app: AppDescriptor): void => {};
  onVisible: (app: AppDescriptor) => void = (_app: AppDescriptor): void => {};

  private artPath(): string {
    if (this.layout === ConsoleCardLayout.XBOX_TILES && this.backgroundPath) return this.backgroundPath;
    if (this.app.coverPath) return this.app.coverPath;
    return this.backgroundPath;
  }

  private artSource(): string {
    const path = this.artPath();
    return path.startsWith('file://') ? path : 'file://' + path;
  }

  private onArtworkChanged(): void { this.artFailed = false; }

  private ringDelta(): number {
    return this.inRing ? this.ringPosition : 0;
  }

  private ringScale(): number {
    if (!this.inRing) return 1;
    return Math.max(0.60, 1 - Math.min(3, Math.abs(this.ringDelta())) * 0.16);
  }

  private ringOpacity(): number {
    if (!this.inRing) return 1;
    return Math.max(0.42, 1 - Math.min(2.6, Math.abs(this.ringDelta())) * 0.22);
  }

  private activate(): void {
    if (this.touchMoved) return;
    if (this.focused) this.onLaunch(this.app);
    else this.onSelect(this.app);
  }

  private trackTouch(event: TouchEvent): void {
    if (event.type === TouchType.Cancel) {
      this.touchMoved = true;
      return;
    }
    if (event.touches.length === 0) return;
    const point = event.touches[0];
    if (event.type === TouchType.Down) {
      this.touchStartX = point.x;
      this.touchStartY = point.y;
      this.touchMoved = false;
    } else if (Math.abs(point.x - this.touchStartX) > 12 ||
      Math.abs(point.y - this.touchStartY) > 12) {
      this.touchMoved = true;
    }
  }

  build() {
    Stack({ alignContent: Alignment.Bottom }) {
      if (this.artPath().length > 0 && !this.artFailed) {
        Image(this.artSource())
          .width('100%').height('100%').objectFit(ImageFit.Cover)
          .blur(this.layout === ConsoleCardLayout.XBOX_TILES && !this.backgroundPath ? 14 : 0)
          .onError((): void => { this.artFailed = true; })
      } else if (this.app.source === AppSource.BUILTIN) {
        Image(builtinCoverResource(this.app.id))
          .width('100%').height('100%').objectFit(ImageFit.Cover)
      } else {
        Column() {
          Text(this.app.title.length > 0 ? this.app.title.substring(0, 1).toUpperCase() : '?')
            .fontSize(this.layout === ConsoleCardLayout.XBOX_TILES ? 50 : 74)
            .fontWeight(FontWeight.Bold).fontColor(skinColor('accent'))
        }
        .width('100%').height('100%').justifyContent(FlexAlign.Center)
        .backgroundColor(skinColor('selected_background'))
      }
      Column({ space: 5 }) {
        Row({ space: 6 }) {
          Text(this.app.title).fontSize(this.layout === ConsoleCardLayout.FOCUS_3D ? 18 : 14)
            .fontWeight(FontWeight.Bold).fontColor('#FFFFFFFF').maxLines(2)
            .layoutWeight(1)
          if (this.favorite) {
            SymbolGlyph($r('sys.symbol.star_fill')).fontSize(15).fontColor(['#FFFFD166'])
          }
        }.width('100%')
        if (this.layout !== ConsoleCardLayout.XBOX_TILES) {
          Text(this.running ? '运行中' : (this.lastPlayed ? this.lastPlayed : '查看详情'))
            .fontSize(11).fontColor('#CFE2E2EA').maxLines(1).width('100%')
        }
      }
      .width('100%')
      .padding({ left: 13, right: 13, top: 16, bottom: 12 })
      .backgroundColor('#B3000000')
      Row() {
        Button({ type: ButtonType.Circle }) {
          SymbolGlyph($r('sys.symbol.info_circle')).fontSize(17).fontColor(['#FFFFFFFF'])
        }
        .width(34).height(34).backgroundColor('#88000000')
        .onTouch((event: TouchEvent): void => event.stopPropagation())
        .onClick((): void => this.onDetails(this.app))
      }
      .width('100%').height('100%')
      .justifyContent(FlexAlign.End).alignItems(VerticalAlign.Top)
      .padding({ top: 10, right: 10 })
      .hitTestBehavior(HitTestMode.Transparent)
    }
    .width(this.cardWidth).height(this.cardHeight)
    .borderRadius(18).clip(true)
    .border({ width: this.focused ? 2 : 1,
      color: this.focused ? skinColor('accent') : '#42FFFFFF' })
    .shadow({ radius: this.focused ? 20 : 8, color: '#63000000', offsetX: 0, offsetY: 6 })
    .opacity(this.app.available ? ((this.artPath().length > 0 && !this.artFailed) ||
      this.app.source === AppSource.BUILTIN ? 1 :
      this.ringOpacity()) : this.ringOpacity() * 0.65)
    .scale({ x: this.ringScale(), y: this.ringScale() })
    .rotate({ x: 0, y: 1, z: 0,
      angle: !this.inRing ? 0 : -Math.max(-2.6, Math.min(2.6, this.ringDelta())) * 24 })
    .translate({ x: this.ringShiftX,
      y: !this.inRing ? 0 : Math.min(2.6, Math.abs(this.ringDelta())) * 11 })
    .zIndex(this.focused ? 10 : 1)
    .onTouch((event: TouchEvent): void => this.trackTouch(event))
    .onClick((): void => this.activate())
    .onVisibleAreaChange([0.1], (visible: boolean): void => {
      if (visible) this.onVisible(this.app);
    })
    .focusable(true)
    .accessibilityText(this.app.title)
  }
}
