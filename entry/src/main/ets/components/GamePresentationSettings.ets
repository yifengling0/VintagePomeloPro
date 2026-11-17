import { GameActionButton } from './GameActionButton';
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
          GameActionButton({ label: mode === 'linear' ? '线性' : mode === 'nearest' ? '最近邻' : '整数缩放',
            selected: this.value.scaling === mode, controlId: 'game-scaling-' + mode,
            onAction: (): void => this.change(mode, this.value.frameLimit) }).margin({ right: 8, bottom: 8 })
        })
      }.width('100%')
      Text('帧率上限').fontSize(14).fontColor(skinColor('text_primary')).width('100%')
      Flex({ wrap: FlexWrap.Wrap }) {
        ForEach([0, 30, 60], (fps: number) => {
          GameActionButton({ label: fps === 0 ? '默认' : fps + ' FPS', selected: this.value.frameLimit === fps,
            controlId: 'game-limit-' + fps, onAction: (): void => this.change(this.value.scaling, fps) })
            .margin({ right: 8, bottom: 8 })
        })
      }.width('100%')
      Text('设置在下次 Windows 会话启动时生效。整数缩放在无法完整容纳画面时改为等比缩小。保留游戏分辨率和 Wine DPI。')
        .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
    }.width('100%').alignItems(HorizontalAlign.Start)
  }
}
