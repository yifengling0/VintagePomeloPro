import { AppDescriptor, AppSource, DisplayMode, ViewMode } from '../model/AppModels';
import { IconExtractor } from '../service/IconExtractor';

@Component
export struct AppCard {
  @Prop app: AppDescriptor;
  @Prop viewMode: ViewMode = ViewMode.GRID;
  @Prop running: boolean = false;
  @Prop favorite: boolean = false;
  /** 封面墙瓦片高度 (竖版), 由页面按断点算好传进来, 卡片不猜屏幕宽度。 */
  @Prop coverWallHeight: number = 288;
  @Prop useGlassEffect: boolean = true;
  @Prop narrowPhone: boolean = false;
  @State private pressed: boolean = false;
  /** exe 内嵌图标 (PE .rsrc 提取, 落盘缓存) — 无自定义封面时的卡面主视觉。 */
  @State private iconPath: string = '';
  /** 封面文件被删除或解码失败: 退回图标/字母占位, 不留空白卡面。 */
  @State private coverFailed: boolean = false;
  /** 每张卡片只发起一次提取; 失败不重试, 避免每轮重绘反复解析 PE。 */
  private iconRequested: boolean = false;
  onLaunch: (app: AppDescriptor) => void = (_app: AppDescriptor): void => {};
  onSettings: (app: AppDescriptor) => void = (_app: AppDescriptor): void => {};
  onLongPress: (app: AppDescriptor) => void = (_app: AppDescriptor): void => {};

  aboutToAppear(): void {
    this.requestIcon(false);
  }

  /** 内建程序用清单封面, 不走 PE 解析; 未关联/不可用程序的 exe 打不开, 直接跳过。 */
  private requestIcon(coverUnusable: boolean): void {
    if (this.iconRequested) return;
    if (this.app.source !== AppSource.DOWNLOAD) return;
    if (!coverUnusable && this.app.coverPath.length > 0) return;
    if (!this.app.available) return;
    const executable = this.app.launchTarget.executable;
    if (executable.length === 0) return;
    this.iconRequested = true;
    void this.loadIcon(executable);
  }

  private async loadIcon(executable: string): Promise<void> {
    try {
      const path = await IconExtractor.getInstance().extract(executable);
      if (path.length > 0) this.iconPath = path;
    } catch (_) {
      // 图标缺失不是错误: 保留字母占位。
    }
  }

  private hasCover(): boolean {
    return this.app.coverPath.length > 0 && !this.coverFailed;
  }

  private coverSource(): string {
    return this.app.coverPath.startsWith('file://') ? this.app.coverPath : 'file://' + this.app.coverPath;
  }

  private plateSize(): number {
    if (this.viewMode === ViewMode.COMPACT) return 54;
    if (this.viewMode === ViewMode.COVER) return this.narrowPhone ? 104 : 124;
    return this.narrowPhone ? 58 : 92;
  }

  private plateRadius(): number {
    if (this.viewMode === ViewMode.COMPACT) return 16;
    if (this.viewMode === ViewMode.COVER) return this.narrowPhone ? 22 : 30;
    return this.narrowPhone ? 18 : 26;
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
    if (this.app.id === 'builtin-desktop') return $r('app.media.builtin_desktop');
    if (this.app.id === 'builtin-explorer') return $r('app.media.builtin_explorer');
    if (this.app.id === 'builtin-winecfg') return $r('app.media.builtin_winecfg');
    if (this.app.id === 'builtin-iexplore') return $r('app.media.builtin_iexplore');
    if (this.app.id === 'builtin-control') return $r('app.media.builtin_control');
    if (this.app.id === 'builtin-taskmgr') return $r('app.media.builtin_taskmgr');
    if (this.app.id === 'builtin-regedit') return $r('app.media.builtin_regedit');
    if (this.app.id === 'builtin-cmd') return $r('app.media.builtin_cmd');
    if (this.app.id === 'builtin-notepad') return $r('app.media.builtin_notepad');
    if (this.app.id === 'builtin-graphics-smoke') return $r('app.media.builtin_graphics');
    if (this.app.id === 'builtin-dxvk-cube') return $r('app.media.builtin_graphics');
    if (this.app.id === 'builtin-vkd3d-triangle') return $r('app.media.builtin_vkd3d_triangle');
    if (this.app.id === 'builtin-vkd3d-gears') return $r('app.media.builtin_vkd3d_gears');
    if (this.app.id === 'builtin-audio-smoke') return $r('app.media.builtin_audio');
    return $r('app.media.builtin_desktop');
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
      .padding({ left: 10, right: 10, top: 4, bottom: 4 })
      .backgroundColor(color)
      .border({ width: 1, color: '#30FFFFFF' })
      .borderRadius(14)
      .shadow({ radius: 4, color: '#40000000', offsetX: 0, offsetY: 2 })
  }

  @Builder
  private CompactBadge(text: string, color: string) {
    Text(text)
      .fontSize(8.5)
      .fontWeight(FontWeight.Bold)
      .fontColor('#FFF7FAFF')
      .maxLines(1)
      .padding({ left: 7, right: 7, top: 3, bottom: 3 })
      .backgroundColor(color)
      .border({ width: 1, color: '#30FFFFFF' })
      .borderRadius(10)
  }

  /**
   * 卡面主视觉的统一载体: 拿到 exe 图标就放图标, 否则放首字母占位。
   * 尺寸随视图模式变化, 三种模式观感一致。
   */
  @Builder
  private ArtPlate(size: number, radius: number) {
    Stack({ alignContent: Alignment.Center }) {
      if (this.iconPath.length > 0) {
        Image(`file://${this.iconPath}`)
          .width(size * 0.74)
          .height(size * 0.74)
          .objectFit(ImageFit.Contain)
          // exe 图标常见 32~256px, 放大到卡面时必须用高质量插值, 否则发糊。
          .interpolation(ImageInterpolation.High)
          .draggable(false)
      } else {
        Text(this.fallbackMonogram())
          .fontSize(size * 0.58)
          .fontWeight(FontWeight.Bold)
          .fontColor('#F2FFFFFF')
      }
    }
    .width(size)
    .height(size)
    .backgroundColor(this.iconPath.length > 0 ? '#18FFFFFF' : '#28FFFFFF')
    .border({ width: 1.5, color: '#42FFFFFF' })
    .borderRadius(radius)
    .shadow({ radius: 14, color: '#38000000', offsetX: 0, offsetY: 6 })
  }

  @Builder
  private ArtFallback(height: number) {
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
        this.ArtPlate(this.plateSize(), this.plateRadius())
        if (this.iconPath.length === 0 && this.viewMode !== ViewMode.COMPACT) {
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
      if (this.hasCover()) {
        Image(this.coverSource())
          .width('100%')
          .height(height)
          .objectFit(ImageFit.Cover)
          .draggable(false)
          .onError((): void => {
            // 封面被删除或损坏时不要留白卡面: 改用 exe 图标或字母占位。
            this.coverFailed = true;
            this.requestIcon(true);
          })
      } else if (this.app.source === AppSource.BUILTIN) {
        Image(this.builtinCover())
          .width('100%')
          .height(height)
          .objectFit(ImageFit.Cover)
          .draggable(false)
      } else {
        this.ArtFallback(height)
      }

      if (this.viewMode === ViewMode.COMPACT) {
        Row({ space: 4 }) {
          if (this.favorite) {
            this.CompactBadge('★', '#E0D9A21B')
          }
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
          if (this.favorite) {
            this.Badge('收藏', '#CCD9A21B')
          }
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
      Column({ space: 2 }) {
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
        SymbolGlyph($r('sys.symbol.gearshape')).fontSize(15).fontColor([$r('app.color.text_secondary')])
      }
        .width(40)
        .height(40)
        .backgroundColor('#18FFFFFF')
        .border({ width: 1, color: '#18FFFFFF' })
        .borderRadius(12)
        .id('wine-app-settings-' + this.app.id)
        .onClick(() => {
          this.onSettings(this.app);
        })
    }
    .layoutWeight(this.viewMode === ViewMode.COMPACT ? 1 : 0)
    .width(this.viewMode === ViewMode.COMPACT ? 0 : '100%')
    .padding({ left: 14, right: 10, top: 10, bottom: 12 })
  }

  /**
   * 竖版封面墙瓦片 (游戏启动器观感): 美术铺满整格, 标题压在底部渐隐层上。
   * 盒装封面是竖版, 横版瓦片会把封面裁成一条 — 这里按竖版呈现让封面完整可看。
   */
  @Builder
  private CoverWall() {
    Stack({ alignContent: Alignment.TopStart }) {
      this.Cover(this.coverWallHeight)

      // 满高容器把信息条压到底部; 渐隐层保证浅色封面上的白字也读得清。
      Column() {
        Column({ space: 3 }) {
          Text(this.app.title)
            .fontSize(17)
            .fontWeight(FontWeight.Bold)
            .fontColor('#FFFFFFFF')
            .maxLines(2)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
          Text(this.executableText())
            .fontSize(11)
            .fontColor('#C0FFFFFF')
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })
        }
        .width('100%')
        .alignItems(HorizontalAlign.Start)
        .padding({ left: 14, right: 14, top: 40, bottom: 14 })
        .linearGradient({
          angle: 180,
          colors: [['#00000000', 0], ['#66000000', 0.35], ['#E6000000', 1]]
        })
      }
      .width('100%')
      .height('100%')
      .justifyContent(FlexAlign.End)

      Row() {
        Button({ type: ButtonType.Circle }) {
          SymbolGlyph($r('sys.symbol.gearshape')).fontSize(14).fontColor(['#FFFFFFFF'])
        }
          .width(36)
          .height(36)
          .backgroundColor('#66000000')
          .border({ width: 1, color: '#30FFFFFF' })
          .borderRadius(10)
          .id('wine-app-settings-' + this.app.id)
          .onClick(() => {
            this.onSettings(this.app);
          })
      }
      .width('100%')
      .justifyContent(FlexAlign.End)
      .padding({ right: 10, top: 10 })
    }
    .width('100%')
    .height(this.coverWallHeight)
    .borderRadius(22)
    .border({ width: 1, color: '#20FFFFFF' })
    .clip(true)
    .shadow({ radius: 20, color: '#30000000', offsetX: 0, offsetY: 8 })
    .opacity(this.app.available ? 1 : 0.62)
    .scale({ x: this.pressed ? 0.97 : 1, y: this.pressed ? 0.97 : 1 })
    .animation({ duration: 150, curve: Curve.EaseOut })
    .onTouch((event: TouchEvent) => this.handleCardTouch(event))
    .onClick(() => this.onLaunch(this.app))
    .gesture(LongPressGesture().onAction(() => this.onLongPress(this.app)))
    .id('wine-app-card-' + this.app.id)
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
      .borderRadius(20)
      .border({ width: 1, color: '#20FFFFFF' })
      .clip(true)
      .shadow({ radius: 12, color: '#20000000', offsetX: 0, offsetY: 5 })
      .opacity(this.app.available ? 1 : 0.62)
      .scale({ x: this.pressed ? 0.97 : 1, y: this.pressed ? 0.97 : 1 })
      .animation({ duration: 150, curve: Curve.EaseOut })
      .onTouch((event: TouchEvent) => this.handleCardTouch(event))
      .onClick(() => this.onLaunch(this.app))
      .gesture(LongPressGesture().onAction(() => this.onLongPress(this.app)))
      .id('wine-app-card-' + this.app.id)
    } else if (this.viewMode === ViewMode.COVER) {
      this.CoverWall()
    } else {
      Column() {
        this.Cover(this.coverHeight())
        this.TitleRow()
      }
      .width('100%')
      .backgroundColor(this.useGlassEffect ? $r('app.color.card_glass_background') : $r('app.color.card_background'))
      .backgroundBlurStyle(this.useGlassEffect ? BlurStyle.COMPONENT_THICK : BlurStyle.NONE)
      .borderRadius(22)
      .border({ width: 1, color: '#20FFFFFF' })
      .clip(true)
      .shadow({ radius: 20, color: '#30000000', offsetX: 0, offsetY: 8 })
      .opacity(this.app.available ? 1 : 0.62)
      .scale({ x: this.pressed ? 0.97 : 1, y: this.pressed ? 0.97 : 1 })
      .animation({ duration: 150, curve: Curve.EaseOut })
      .onTouch((event: TouchEvent) => this.handleCardTouch(event))
      .onClick(() => this.onLaunch(this.app))
      .gesture(LongPressGesture().onAction(() => this.onLongPress(this.app)))
      .id('wine-app-card-' + this.app.id)
    }
  }
}
