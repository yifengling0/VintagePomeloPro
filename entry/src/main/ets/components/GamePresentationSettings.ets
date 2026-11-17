import { GameModeSettings, GameScaling, normalizeGameModeSettings } from '../model/GameModeSettings';
import { skinColor } from '../service/SkinColors';

@Component
export struct GamePresentationSettings {
  @Prop value: GameModeSettings = normalizeGameModeSettings();
  onChange: (value: GameModeSettings) => void = (_value: GameModeSettings): void => {};

  private change(scaling: GameScaling, frameLimit: number): void {
    this.onChange(normalizeGameModeSettings({ scaling: scaling, frameLimit: frameLimit }));
  }

  build() {
    Column({ space: 12 }) {
      Text('画面缩放').fontSize(14).fontColor(skinColor('text_primary')).width('100%')
      Flex({ wrap: FlexWrap.Wrap }) {
        ForEach(['linear', 'nearest', 'integer'], (mode: GameScaling) => {
          Button(mode === 'linear' ? '线性' : mode === 'nearest' ? '最近邻' : '整数缩放')
            .fontSize(13).height(36).margin({ right: 8, bottom: 8 })
            .fontColor(this.value.scaling === mode ? Color.White : skinColor('text_primary'))
            .backgroundColor(this.value.scaling === mode ? '#2874BC' : skinColor('search_background'))
            .onClick((): void => this.change(mode, this.value.frameLimit)).id('game-scaling-' + mode)
        })
      }.width('100%')
      Text('帧率上限').fontSize(14).fontColor(skinColor('text_primary')).width('100%')
      Flex({ wrap: FlexWrap.Wrap }) {
        ForEach([0, 30, 60], (fps: number) => {
          Button(fps === 0 ? '默认' : fps + ' FPS').fontSize(13).height(36).margin({ right: 8, bottom: 8 })
            .fontColor(this.value.frameLimit === fps ? Color.White : skinColor('text_primary'))
            .backgroundColor(this.value.frameLimit === fps ? '#2874BC' : skinColor('search_background'))
            .onClick((): void => this.change(this.value.scaling, fps)).id('game-limit-' + fps)
        })
      }.width('100%')
      Text('设置在下次 Windows 会话启动时生效。整数缩放在无法完整容纳画面时改为等比缩小。保留游戏分辨率和 Wine DPI。')
        .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
    }.width('100%').alignItems(HorizontalAlign.Start)
  }
}
