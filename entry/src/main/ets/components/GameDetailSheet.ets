import { AppDescriptor, AppSource } from '../model/AppModels';

export interface GameDetailSheetCallbacks {
  onLaunch: (app: AppDescriptor) => void;
  onClose: () => void;
  onToggleFavorite: (app: AppDescriptor) => void;
  onOpenSettings: (app: AppDescriptor) => void;
  /** 从 SteamGridDB 抓取竖版封面 (展示侧零改动: 抓到的图写入游戏目录 cover.png)。 */
  onScrapeCover: (app: AppDescriptor) => void;
}

@Component
export struct GameDetailSheet {
  @Prop app: AppDescriptor;
  @Prop isRunning: boolean = false;
  @Prop isFavorite: boolean = false;
  @Prop lastPlayedText: string = '';
  @Prop playTimeText: string = '';
  private callbacks: GameDetailSheetCallbacks = {
    onLaunch: (_app: AppDescriptor): void => {},
    onClose: (): void => {},
    onToggleFavorite: (_app: AppDescriptor): void => {},
    onOpenSettings: (_app: AppDescriptor): void => {},
    onScrapeCover: (_app: AppDescriptor): void => {}
  };

  build() {
    Stack() {
      Column() {
        Row() {
          Button({ type: ButtonType.Circle }) {
            SymbolGlyph($r('sys.symbol.xmark')).fontSize(18)
              .fontColor(['#FFFFFFFF'])
          }
          .width(40).height(40)
          .backgroundColor('#66000000')
          .border({ width: 1, color: '#30FFFFFF' })
          .borderRadius(20)
          .onClick(() => this.callbacks.onClose())
        }
        .width('100%')
        .justifyContent(FlexAlign.End)
        .padding({ right: 16, top: 16 })
      }
      .width('100%')
      .height('100%')
      .alignItems(HorizontalAlign.End)

      Column() {
        Row({ space: 28 }) {
          if (this.app.coverPath.length > 0) {
            // coverPath 是裸文件路径, Image 需要 file:// 前缀 (AppCard.coverSource 同款)。
            Image(this.app.coverPath.startsWith('file://')
              ? this.app.coverPath : 'file://' + this.app.coverPath)
              .width(168).height(224)
              .borderRadius(16)
              .objectFit(ImageFit.Cover)
              .border({ width: 1, color: '#40FFFFFF' })
              .shadow({ radius: 24, color: '#80000000', offsetX: 0, offsetY: 10 })
          } else {
            Stack() {
              Column() {
                SymbolGlyph($r('sys.symbol.house')).fontSize(56)
                  .fontColor([$r('app.color.accent')])
              }
            }
            .width(168).height(224)
            .borderRadius(16)
            .backgroundColor($r('app.color.selected_background'))
            .border({ width: 1, color: '#40FFFFFF' })
            .shadow({ radius: 24, color: '#80000000', offsetX: 0, offsetY: 10 })
          }

          Column({ space: 10 }) {
            Text(this.app.title)
              .fontSize(30)
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
        .padding({ left: 32, right: 32, top: 24, bottom: 20 })

        Row({ space: 16 }) {
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
          .padding({ left: 32, right: 32 })
          .borderRadius(27)
          .backgroundColor(this.isRunning ? '#D94848' : $r('app.color.accent'))
          .onClick(() => this.callbacks.onLaunch(this.app))

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
        }
        .width('100%')
        .justifyContent(FlexAlign.Center)
        .padding({ top: 10, bottom: 28 })
      }
      .width('100%')
      .justifyContent(FlexAlign.Center)
    }
    .width('100%')
    .height('100%')
  }
}