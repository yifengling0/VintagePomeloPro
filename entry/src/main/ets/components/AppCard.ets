import { AppDescriptor, AppSource, DisplayMode, ViewMode } from '../model/AppModels';
import { builtinCoverResource } from '../model/BuiltinArtwork';

@Component
export struct AppCard {
  @Prop app: AppDescriptor;
  @Prop viewMode: ViewMode = ViewMode.GRID;
  @Prop running: boolean = false;
  @Prop useGlassEffect: boolean = true;
  @Prop narrowPhone: boolean = false;
  @Prop artworkPath: string = '';
  @State private pressed: boolean = false;
  onLaunch: (app: AppDescriptor) => void = (_app: AppDescriptor): void => {};
  onSettings: (app: AppDescriptor) => void = (_app: AppDescriptor): void => {};
  onLongPress: (app: AppDescriptor) => void = (_app: AppDescriptor): void => {};
  onVisible: (app: AppDescriptor) => void = (_app: AppDescriptor): void => {};

  private coverSource(path: string): string {
    return path.startsWith('file://') ? path : 'file://' + path;
  }

  private coverHeight(): number {
    if (this.viewMode === ViewMode.COMPACT) return 104;
    // VintagePomelo's large-cover mode is a full-width showcase card, not a
    // two-column grid with a few extra pixels. Keep the phone cover visibly
    // taller now that LARGE is rendered as a single-column list.
    if (this.narrowPhone) return this.viewMode === ViewMode.LARGE ? 196 : 120;
    return this.viewMode === ViewMode.LARGE ? 246 : 220;
  }

  private fallbackMonogram(): string {
    const title = this.app.title.trim();
    if (title.length === 0) return 'EXE';
    const first = title.charAt(0).toUpperCase();
    return first.length > 0 ? first : 'EXE';
  }

  private fallbackPalette(): number {
    const value = this.app.id + this.app.title;
    let hash = 0;
    for (let index = 0; index < value.length; index++) {
      hash = (hash * 31 + value.charCodeAt(index)) >>> 0;
    }
    return hash % 4;
  }

  private fallbackStartColor(): string {
    const palette = this.fallbackPalette();
    if (palette === 1) return '#355F69';
    if (palette === 2) return '#565180';
    if (palette === 3) return '#40566F';
    return '#40577A';
  }

  private fallbackEndColor(): string {
    const palette = this.fallbackPalette();
    if (palette === 1) return '#4C7180';
    if (palette === 2) return '#6D648E';
    if (palette === 3) return '#4D756E';
    return '#6A6284';
  }

  /** Deterministic cover slices supplied with the built-in application manifest. */
  private builtinCover(): Resource {
    return builtinCoverResource(this.app.id);
  }

  private executableText(): string {
    // Explorer supplies both the desktop shell and the file manager.  Label the
    // shell role explicitly while keeping explorer.exe as the actual launch target.
    if (this.app.id === 'builtin-desktop') return 'desktop (explorer)';
    if (this.app.id === 'builtin-dxvk-cube') return 'DXVK Legacy · D3D11';
    return this.app.launchTarget.executable;
  }

  private handleCardTouch(event: TouchEvent): void {
    if (event.type === TouchType.Down) this.pressed = true;
    if (event.type === TouchType.Up || event.type === TouchType.Cancel) this.pressed = false;
  }

  @Builder
  private Badge(text: string, color: string) {
    Text(text)
      .fontSize(10.5)
      .fontWeight(FontWeight.Bold)
      .fontColor('#FFFFFFFF')
      .padding({ left: 9, right: 9, top: 4, bottom: 4 })
      .backgroundColor(color)
      .borderRadius(12)
  }

  @Builder
  private CompactBadge(text: string, color: string) {
    Text(text)
      .fontSize(8.5)
      .fontWeight(FontWeight.Bold)
      .fontColor('#FFF7FAFF')
      .maxLines(1)
      .padding({ left: 6, right: 6, top: 3, bottom: 3 })
      .backgroundColor(color)
      .border({ width: 1, color: '#24FFFFFF' })
      .borderRadius(9)
  }

  @Builder
  private DownloadFallback(height: number) {
    Stack({ alignContent: Alignment.Center }) {
      Circle()
        .width(this.viewMode === ViewMode.COMPACT ? 96 : (this.narrowPhone ? 104 : 190))
        .height(this.viewMode === ViewMode.COMPACT ? 96 : (this.narrowPhone ? 104 : 190))
        .fill('#0FFFFFFF')
        .position({ x: this.viewMode === ViewMode.COMPACT ? 58 : '62%',
          y: this.viewMode === ViewMode.COMPACT ? -34 : (this.narrowPhone ? -26 : -62) })
      Circle()
        .width(this.viewMode === ViewMode.COMPACT ? 72 : (this.narrowPhone ? 78 : 150))
        .height(this.viewMode === ViewMode.COMPACT ? 72 : (this.narrowPhone ? 78 : 150))
        .fill('#0AFFFFFF')
        .position({ x: this.viewMode === ViewMode.COMPACT ? -24 : -38, y: this.viewMode === ViewMode.COMPACT ? 60 : '58%' })
      Column({ space: this.viewMode === ViewMode.COMPACT ? 4 : 8 }) {
        Stack({ alignContent: Alignment.Center }) {
          Text(this.fallbackMonogram())
            .fontSize(this.viewMode === ViewMode.COMPACT ? 31 : (this.narrowPhone ? 34 : 54))
            .fontWeight(FontWeight.Bold)
            .fontColor('#F2FFFFFF')
        }
        .width(this.viewMode === ViewMode.COMPACT ? 54 : (this.narrowPhone ? 58 : 92))
        .height(this.viewMode === ViewMode.COMPACT ? 54 : (this.narrowPhone ? 58 : 92))
        .backgroundColor('#24FFFFFF')
        .border({ width: 1.5, color: '#38FFFFFF' })
        .borderRadius(this.viewMode === ViewMode.COMPACT ? 16 : (this.narrowPhone ? 18 : 26))
        .shadow({ radius: 12, color: '#33000000', offsetX: 0, offsetY: 5 })
        if (this.viewMode !== ViewMode.COMPACT) {
          Text('EXE')
            .fontSize(10)
            .fontWeight(FontWeight.Bold)
            .fontColor('#CFF7FAFF')
            .letterSpacing(1.2)
        }
      }
    }
    .width('100%')
    .height(height)
    .linearGradient({
      angle: 142,
      colors: [[this.fallbackStartColor(), 0], [this.fallbackEndColor(), 1]]
    })
  }

  @Builder
  private Cover(height: number) {
    Stack({ alignContent: Alignment.TopStart }) {
      if (this.app.coverPath.length > 0) {
        Image(this.coverSource(this.app.coverPath))
          .width('100%')
          .height(height)
          .objectFit(ImageFit.Cover)
          .draggable(false)
      } else if (this.app.source === AppSource.BUILTIN) {
        Image(this.builtinCover())
          .width('100%')
          .height(height)
          .objectFit(ImageFit.Cover)
          .draggable(false)
      } else if (this.artworkPath.length > 0) {
        Image(this.coverSource(this.artworkPath))
          .width('100%')
          .height(height)
          .objectFit(ImageFit.Cover)
          .draggable(false)
      } else {
        this.DownloadFallback(height)
      }

      if (this.viewMode === ViewMode.COMPACT) {
        Row({ space: 4 }) {
          if (this.app.source === AppSource.BUILTIN) {
            this.CompactBadge('内建', '#E02979B8')
          }
        }
        .padding({ left: 8, right: 8, top: 8 })
        if (this.running) {
          Circle().width(10).height(10).fill('#FF35B779')
            .stroke('#E6FFFFFF').strokeWidth(1.5)
            .shadow({ radius: 5, color: '#6635B779' })
            .position({ x: 100, y: 10 })
        }
      } else {
        Row({ space: 5 }) {
          if (this.app.source === AppSource.BUILTIN) {
            this.Badge('内建', '#DE2979B8')
          }
          if (this.running) {
            this.Badge('运行中', '#CC22A06B')
          }
          if (!this.app.available) {
            this.Badge('不可用', '#CCD64C4C')
          }
        }
        .padding(10)
      }
    }
    .width(this.viewMode === ViewMode.COMPACT ? 120 : '100%')
    .height(height)
    .clip(true)
  }

  @Builder
  private TitleRow() {
    Row() {
      Column({ space: 3 }) {
        Text(this.app.title)
          .fontSize(15)
          .fontWeight(FontWeight.Medium)
          .fontColor($r('app.color.text_primary'))
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
        Text(this.executableText())
          .fontSize(11)
          .fontColor($r('app.color.text_secondary'))
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
      .layoutWeight(1)
      .alignItems(HorizontalAlign.Start)

      Button({ type: ButtonType.Circle }) {
        SymbolGlyph($r('sys.symbol.gearshape')).fontSize(16).fontColor([$r('app.color.text_secondary')])
      }
        .width(48)
        .height(48)
        .backgroundColor('#00000000')
        .id('wine-app-settings-' + this.app.id)
        .onClick(() => {
          this.onSettings(this.app);
        })
    }
    // COMPACT 卡片是 Row(Cover + TitleRow): TitleRow 必须占剩余空间,
    // 否则 width('100%') 会占满整卡宽度并把右侧设定按钮挤出裁剪区。
    .layoutWeight(this.viewMode === ViewMode.COMPACT ? 1 : 0)
    .width(this.viewMode === ViewMode.COMPACT ? 0 : '100%')
    .padding({ left: 14, right: 8, top: 11, bottom: 12 })
  }

  build() {
    if (this.viewMode === ViewMode.COMPACT) {
      Row() {
        this.Cover(104)
        this.TitleRow()
      }
      .height(104)
      .width('100%')
      .backgroundColor(this.useGlassEffect ? $r('app.color.card_glass_background') : $r('app.color.card_background'))
      .backgroundBlurStyle(this.useGlassEffect ? BlurStyle.COMPONENT_THICK : BlurStyle.NONE)
      .borderRadius(18)
      .border({ width: 1, color: '#15FFFFFF' })
      .clip(true)
      .shadow({ radius: 10, color: '#18000000', offsetX: 0, offsetY: 4 })
      .opacity(this.app.available ? 1 : 0.62)
      .scale({ x: this.pressed ? 0.985 : 1, y: this.pressed ? 0.985 : 1 })
      .animation({ duration: 120, curve: Curve.EaseOut })
      .onTouch((event: TouchEvent) => this.handleCardTouch(event))
      .onClick(() => this.onLaunch(this.app))
      .onVisibleAreaChange([0.1], (visible: boolean): void => {
        if (visible) this.onVisible(this.app);
      })
      .gesture(LongPressGesture().onAction(() => this.onLongPress(this.app)))
      .id('wine-app-card-' + this.app.id)
    } else {
      Column() {
        this.Cover(this.coverHeight())
        this.TitleRow()
      }
      .width('100%')
      .backgroundColor(this.useGlassEffect ? $r('app.color.card_glass_background') : $r('app.color.card_background'))
      .backgroundBlurStyle(this.useGlassEffect ? BlurStyle.COMPONENT_THICK : BlurStyle.NONE)
      .borderRadius(20)
      .border({ width: 1, color: '#14FFFFFF' })
      .clip(true)
      .shadow({ radius: 16, color: '#22000000', offsetX: 0, offsetY: 7 })
      .opacity(this.app.available ? 1 : 0.62)
      .scale({ x: this.pressed ? 0.985 : 1, y: this.pressed ? 0.985 : 1 })
      .animation({ duration: 120, curve: Curve.EaseOut })
      .onTouch((event: TouchEvent) => this.handleCardTouch(event))
      .onClick(() => this.onLaunch(this.app))
      .onVisibleAreaChange([0.1], (visible: boolean): void => {
        if (visible) this.onVisible(this.app);
      })
      .gesture(LongPressGesture().onAction(() => this.onLongPress(this.app)))
      .id('wine-app-card-' + this.app.id)
    }
  }
}
