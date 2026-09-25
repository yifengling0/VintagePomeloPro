import { skinColor } from '../service/SkinColors';
import { AppDescriptor, AppSource } from '../model/AppModels';
import { builtinCoverResource } from '../model/BuiltinArtwork';

export interface GameDetailSheetCallbacks {
  onLaunch: (app: AppDescriptor) => void;
  onClose: () => void;
  onToggleFavorite: (app: AppDescriptor) => void;
  onOpenSettings: (app: AppDescriptor) => void;
  /** Fetch a cover from Steam's public source, then optional SteamGridDB. */
  onScrapeCover: (app: AppDescriptor) => void;
}

@Component
export struct GameDetailSheet {
  @Prop app: AppDescriptor;
  @Prop backgroundPath: string = '';
  @Prop isRunning: boolean = false;
  @Prop isFavorite: boolean = false;
  @Prop lastPlayedText: string = '';
  @Prop playTimeText: string = '';
  @Prop compact: boolean = false;
  @Prop topInset: number = 0;
  callbacks: GameDetailSheetCallbacks = {
    onLaunch: (_app: AppDescriptor): void => {},
    onClose: (): void => {},
    onToggleFavorite: (_app: AppDescriptor): void => {},
    onOpenSettings: (_app: AppDescriptor): void => {},
    onScrapeCover: (_app: AppDescriptor): void => {}
  };

  private artworkPath(): string {
    return this.app.coverPath || this.backgroundPath;
  }

  private artworkSource(): string {
    const path = this.artworkPath();
    return path.startsWith('file://') ? path : 'file://' + path;
  }

  build() {
    Stack({ alignContent: Alignment.TopEnd }) {
      Scroll() {
        Column() {
        Row({ space: this.compact ? 14 : 28 }) {
          if (this.artworkPath().length > 0) {
            Image(this.artworkSource())
              .width(this.compact ? 100 : 168).height(this.compact ? 148 : 224)
              .borderRadius(16)
              .objectFit(ImageFit.Cover)
              .border({ width: 1, color: '#40FFFFFF' })
              .shadow({ radius: 24, color: '#80000000', offsetX: 0, offsetY: 10 })
          } else if (this.app.source === AppSource.BUILTIN) {
            Image(builtinCoverResource(this.app.id))
              .width(this.compact ? 100 : 168).height(this.compact ? 148 : 224)
              .borderRadius(16)
              .objectFit(ImageFit.Cover)
              .border({ width: 1, color: '#40FFFFFF' })
              .shadow({ radius: 24, color: '#80000000', offsetX: 0, offsetY: 10 })
          } else {
            Stack() {
              Column() {
                SymbolGlyph($r('sys.symbol.house')).fontSize(56)
                  .fontColor([skinColor('accent')])
              }
            }
            .width(this.compact ? 100 : 168).height(this.compact ? 148 : 224)
            .borderRadius(16)
            .backgroundColor(skinColor('selected_background'))
            .border({ width: 1, color: '#40FFFFFF' })
            .shadow({ radius: 24, color: '#80000000', offsetX: 0, offsetY: 10 })
          }

          Column({ space: 10 }) {
            Text(this.app.title)
              .fontSize(this.compact ? 20 : 30)
              .fontWeight(FontWeight.Bold)
              .fontColor('#FFFFFFFF')
              .maxLines(2)
              .textOverflow({ overflow: TextOverflow.Ellipsis })

            Text(this.app.source === AppSource.BUILTIN ? '内建应用' : 'Windows 程序')
              .fontSize(15)
              .fontColor('#B0FFFFFF')

            // 信息行带图标, 与标题形成清晰层级。
            if (this.lastPlayedText.length > 0) {
              Row({ space: 7 }) {
                SymbolGlyph($r('sys.symbol.clock')).fontSize(14).fontColor(['#90FFFFFF'])
                Text(this.lastPlayedText).fontSize(15).fontColor('#B8FFFFFF')
              }
            }

            if (this.playTimeText.length > 0) {
              Row({ space: 7 }) {
                SymbolGlyph($r('sys.symbol.timer')).fontSize(14).fontColor(['#90FFFFFF'])
                Text(this.playTimeText).fontSize(15).fontColor('#B8FFFFFF')
              }
            }

            if (this.isRunning) {
              Row({ space: 7 }) {
                Circle().width(9).height(9).fill('#4CAF50')
                Text('运行中').fontSize(15).fontColor('#4CAF50')
              }
            }
          }
          .alignItems(HorizontalAlign.Start)
          .layoutWeight(1)
        }
        .width('100%')
        .padding({ left: this.compact ? 14 : 32, right: this.compact ? 14 : 32,
          top: this.compact ? 52 : 24, bottom: 20 })

        Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.Center }) {
          Button({ type: ButtonType.Normal }) {
            Row({ space: 10 }) {
              SymbolGlyph($r('sys.symbol.play_fill')).fontSize(20)
                .fontColor(['#FFFFFFFF'])
              Text(this.isRunning ? '关闭程序' : '启动游戏').fontSize(17)
                .fontColor('#FFFFFFFF')
                .fontWeight(FontWeight.Bold)
            }
          }
          .height(54)
          .width(this.compact ? '100%' : 170)
          .padding({ left: 32, right: 32 })
          .borderRadius(27)
          .backgroundColor(this.isRunning ? '#D94848' : skinColor('accent'))
          .onClick(() => this.callbacks.onLaunch(this.app))
          .margin({ right: 8, bottom: 8 })

          Button({ type: ButtonType.Circle }) {
            SymbolGlyph(this.isFavorite ? $r('sys.symbol.star_fill') : $r('sys.symbol.star'))
              .fontSize(22)
              .fontColor([this.isFavorite ? '#FFD700' : '#FFFFFFFF'])
          }
          .width(54).height(54)
          .backgroundColor('#66000000')
          .border({ width: 1, color: '#30FFFFFF' })
          .borderRadius(27)
          .onClick(() => this.callbacks.onToggleFavorite(this.app))
          .margin({ right: 8, bottom: 8 })

          Button({ type: ButtonType.Circle }) {
            SymbolGlyph($r('sys.symbol.cloud_and_arrow_down')).fontSize(22)
              .fontColor(['#FFFFFFFF'])
          }
          .width(54).height(54)
          .backgroundColor('#66000000')
          .border({ width: 1, color: '#30FFFFFF' })
          .borderRadius(27)
          .onClick(() => this.callbacks.onScrapeCover(this.app))
          .id('detail-scrape-cover')
          .accessibilityText('自动获取封面，公开图源无需 Key')
          .margin({ right: 8, bottom: 8 })

          Button({ type: ButtonType.Circle }) {
            SymbolGlyph($r('sys.symbol.gearshape')).fontSize(22)
              .fontColor(['#FFFFFFFF'])
          }
          .width(54).height(54)
          .backgroundColor('#66000000')
          .border({ width: 1, color: '#30FFFFFF' })
          .borderRadius(27)
          .onClick(() => this.callbacks.onOpenSettings(this.app))
          .id('detail-open-settings')
          .margin({ right: 8, bottom: 8 })
        }
        .width('100%')
        .padding({ top: 10, bottom: 28 })
        if (this.app.source === AppSource.DOWNLOAD) {
          Text('云朵按钮：自动获取封面。优先使用无需 Key 的 Steam 公共图源。')
            .fontSize(11).fontColor('#B8FFFFFF').textAlign(TextAlign.Center)
            .width('100%').padding({ bottom: 20 })
        }
        }
        .width('100%')
      }
      .width('100%').height('100%')
      .scrollBar(BarState.Off)
      // 内容容器是全屏的, 空白区必须透明穿透 —— ArkUI 默认命中测试会阻塞
      // 下层兄弟节点, 否则它会吞掉右上角关闭按钮的点击 (用户实测点不动)。
      .hitTestBehavior(HitTestMode.Transparent)

      Button({ type: ButtonType.Circle }) {
        SymbolGlyph($r('sys.symbol.xmark')).fontSize(18)
          .fontColor(['#FFFFFFFF'])
      }
      .width(40).height(40)
      .backgroundColor('#66000000')
      .border({ width: 1, color: '#30FFFFFF' })
      .borderRadius(20)
      .margin({ right: 16, top: Math.max(16, this.topInset + 12) })
      .onClick(() => this.callbacks.onClose())
      .id('detail-close-button')
    }
    .width('100%')
    .height('100%')
  }
}
