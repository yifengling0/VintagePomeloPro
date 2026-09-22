import { AppDescriptor, AppSource } from '../model/AppModels';

export interface GameDetailSheetCallbacks {
  onLaunch: (app: AppDescriptor) => void;
  onClose: () => void;
  onToggleFavorite: (app: AppDescriptor) => void;
  onOpenSettings: (app: AppDescriptor) => void;
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
    onOpenSettings: (_app: AppDescriptor): void => {}
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
        Row({ space: 20 }) {
          if (this.app.coverPath.length > 0) {
            // coverPath 是裸文件路径, Image 需要 file:// 前缀 (AppCard.coverSource 同款)。
            Image(this.app.coverPath.startsWith('file://')
              ? this.app.coverPath : 'file://' + this.app.coverPath)
              .width(120).height(160)
              .borderRadius(12)
              .objectFit(ImageFit.Cover)
              .border({ width: 1, color: '#40FFFFFF' })
          } else {
            Stack() {
              Column() {
                SymbolGlyph($r('sys.symbol.house')).fontSize(40)
                  .fontColor([$r('app.color.accent')])
              }
            }
            .width(120).height(160)
            .borderRadius(12)
            .backgroundColor($r('app.color.selected_background'))
            .border({ width: 1, color: '#40FFFFFF' })
          }

          Column({ space: 8 }) {
            Text(this.app.title)
              .fontSize(24)
              .fontWeight(FontWeight.Bold)
              .fontColor('#FFFFFFFF')
              .maxLines(2)
              .textOverflow({ overflow: TextOverflow.Ellipsis })

            Text(this.app.source === AppSource.BUILTIN ? '内建应用' : 'Windows 程序')
              .fontSize(14)
              .fontColor('#C0FFFFFF')

            if (this.lastPlayedText.length > 0) {
              Text(this.lastPlayedText)
                .fontSize(13)
                .fontColor('#A0FFFFFF')
            }

            if (this.playTimeText.length > 0) {
              Text(this.playTimeText)
                .fontSize(13)
                .fontColor('#A0FFFFFF')
            }

            if (this.isRunning) {
              Row({ space: 6 }) {
                Circle().width(8).height(8).fill('#4CAF50')
                Text('运行中').fontSize(13).fontColor('#4CAF50')
              }
            }
          }
          .alignItems(HorizontalAlign.Start)
          .layoutWeight(1)
        }
        .width('100%')
        .padding({ left: 24, right: 24, top: 24, bottom: 16 })

        Row({ space: 16 }) {
          Button({ type: ButtonType.Normal }) {
            Row({ space: 8 }) {
              SymbolGlyph($r('sys.symbol.play_fill')).fontSize(18)
                .fontColor(['#FFFFFFFF'])
              Text(this.isRunning ? '关闭程序' : '启动游戏').fontSize(16)
                .fontColor('#FFFFFFFF')
                .fontWeight(FontWeight.Bold)
            }
          }
          .height(48)
          .padding({ left: 24, right: 24 })
          .borderRadius(24)
          .backgroundColor(this.isRunning ? '#D94848' : $r('app.color.accent'))
          .onClick(() => this.callbacks.onLaunch(this.app))

          Button({ type: ButtonType.Circle }) {
            SymbolGlyph(this.isFavorite ? $r('sys.symbol.star_fill') : $r('sys.symbol.star'))
              .fontSize(20)
              .fontColor([this.isFavorite ? '#FFD700' : '#FFFFFFFF'])
          }
          .width(48).height(48)
          .backgroundColor('#66000000')
          .border({ width: 1, color: '#30FFFFFF' })
          .borderRadius(24)
          .onClick(() => this.callbacks.onToggleFavorite(this.app))

          Button({ type: ButtonType.Circle }) {
            SymbolGlyph($r('sys.symbol.gearshape')).fontSize(20)
              .fontColor(['#FFFFFFFF'])
          }
          .width(48).height(48)
          .backgroundColor('#66000000')
          .border({ width: 1, color: '#30FFFFFF' })
          .borderRadius(24)
          .onClick(() => this.callbacks.onOpenSettings(this.app))
        }
        .width('100%')
        .justifyContent(FlexAlign.Center)
        .padding({ top: 8, bottom: 24 })
      }
      .width('100%')
      .justifyContent(FlexAlign.Center)
    }
    .width('100%')
    .height('100%')
  }
}