import { GameActionButton } from './GameActionButton';
import { GameModeSettings, GameScaling, normalizeGameModeSettings } from '../model/GameModeSettings';
import { skinColor } from '../service/SkinColors';

@Component
export struct GamePresentationSettings {
  @Prop value: GameModeSettings = normalizeGameModeSettings();
  @Prop inGame: boolean = false;
  onChange: (value: GameModeSettings) => void = (_value: GameModeSettings): void => {};

  private change(scaling: GameScaling, frameLimit: number, sharpness: number = this.value.sharpness ?? 10): void {
    this.onChange(normalizeGameModeSettings({ scaling: scaling, frameLimit: frameLimit, sharpness: sharpness }));
  }

  build() {
    Column({ space: 12 }) {
      Text('画面缩放').fontSize(14).fontWeight(FontWeight.Medium)
        .fontColor(this.inGame ? '#E8EDF5' : skinColor('text_primary')).width('100%')
      Flex({ wrap: FlexWrap.Wrap }) {
        ForEach(['linear', 'nearest', 'integer', 'fsr'], (mode: GameScaling) => {
          GameActionButton({ label: mode === 'linear' ? '线性' : mode === 'nearest' ? '最近邻' : mode === 'integer' ? '整数缩放' : 'FSR 1.0',
            selected: this.value.scaling === mode, controlId: 'game-scaling-' + mode, darkPanel: this.inGame,
            onAction: (): void => this.change(mode, this.value.frameLimit) }).margin({ right: 8, bottom: 8 })
        })
      }.width('100%')
      if (this.value.scaling === 'fsr') {
        Text('FSR 锐化 · ' + (20 - (this.value.sharpness ?? 10))).fontSize(14)
          .fontColor(this.inGame ? '#E8EDF5' : skinColor('text_primary'))
        Slider({ value: 20 - (this.value.sharpness ?? 10), min: 0, max: 20, step: 1 })
          .id('game-fsr-sharpness')
          .onChange((value: number): void => this.change(this.value.scaling, this.value.frameLimit, 20 - Math.round(value)))
      }
      Text('帧率上限').fontSize(14).fontWeight(FontWeight.Medium)
        .fontColor(this.inGame ? '#E8EDF5' : skinColor('text_primary')).width('100%')
      Flex({ wrap: FlexWrap.Wrap }) {
        ForEach([0, 30, 60], (fps: number) => {
          GameActionButton({ label: fps === 0 ? '跟随屏幕' : fps + ' FPS', selected: this.value.frameLimit === fps,
            controlId: 'game-limit-' + fps, darkPanel: this.inGame, onAction: (): void => this.change(this.value.scaling, fps) })
            .margin({ right: 8, bottom: 8 })
        })
      }.width('100%')
      Text(this.inGame ? '调整实时生效，自动记住当前游戏的画面设置。' :
        '默认设置在下次启动时使用。游戏运行时可在画面侧栏实时调整缩放、FSR 和限帧。')
        .fontSize(12).fontColor(this.inGame ? '#9AA9BE' : skinColor('text_secondary')).width('100%')
    }.width('100%').alignItems(HorizontalAlign.Start)
  }
}
