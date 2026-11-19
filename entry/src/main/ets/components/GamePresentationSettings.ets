import { GameActionButton } from './GameActionButton';
import { GameFrameGeneration, GameModeSettings, GameScaling, normalizeGameModeSettings, frameGenerationStateText } from '../model/GameModeSettings';
import testNapi from 'libentry.so';
import { skinColor } from '../service/SkinColors';

@Component
export struct GamePresentationSettings {
  @Prop value: GameModeSettings = normalizeGameModeSettings();
  @Prop inGame: boolean = false;
  @State private fgStatus: string = '';
  @State private fgRates: string = '';
  @State private fgGpuTime: string = '';
  private fgTimer: number = -1;
  onChange: (value: GameModeSettings) => void = (_value: GameModeSettings): void => {};

  private change(scaling: GameScaling, frameLimit: number, sharpness: number = this.value.sharpness ?? 10,
    frameGeneration: GameFrameGeneration = this.value.frameGeneration ?? 'off'): void {
    this.onChange(normalizeGameModeSettings({ scaling: scaling, frameLimit: frameLimit, sharpness: sharpness, frameGeneration: frameGeneration }));
  }
  aboutToAppear(): void {
    if (this.inGame) {
      this.readFgStatus();
      this.fgTimer = setInterval((): void => this.readFgStatus(), 500);
    }
  }
  aboutToDisappear(): void { if (this.fgTimer >= 0) clearInterval(this.fgTimer); this.fgTimer = -1; }
  private readFgStatus(): void {
    try {
      const fg = testNapi.getGameFrameGenerationStatus();
      this.fgStatus = frameGenerationStateText(fg.state);
      this.fgRates = fg.state > 0 ? '真实 ' + Math.round(fg.realFps) + ' · 生成 ' +
        Math.round(fg.generatedFps) + ' · 呈现 ' + Math.round(fg.presentedFps) + ' FPS' : '';
      this.fgGpuTime = fg.generatedFps > 0 && fg.gpuMs > 0 ? '插帧 GPU 耗时 ' + fg.gpuMs.toFixed(1) + ' ms' : '';
    } catch (_) {
      this.fgStatus = '插帧状态暂不可用';
      this.fgRates = '';
      this.fgGpuTime = '';
    }
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
      Text('图像插帧 · 免费').fontSize(14).fontWeight(FontWeight.Medium)
        .fontColor(this.inGame ? '#E8EDF5' : skinColor('text_primary')).width('100%')
      if (!this.inGame || this.value.frameGeneration !== 'harmonyGpu') {
        Flex({ wrap: FlexWrap.Wrap }) {
          GameActionButton({ label: '关闭', selected: this.value.frameGeneration === 'off', controlId: 'game-framegen-off', darkPanel: this.inGame,
            onAction: (): void => this.change(this.value.scaling, this.value.frameLimit, this.value.sharpness ?? 10, 'off') })
            .margin({ right: 8, bottom: 8 })
          GameActionButton({ label: 'Win-FG 2×', selected: this.value.frameGeneration === 'x2', controlId: 'game-framegen-x2', darkPanel: this.inGame,
            onAction: (): void => this.change(this.value.scaling, this.value.frameLimit, this.value.sharpness ?? 10, 'x2') })
            .margin({ right: 8, bottom: 8 })
          if (!this.inGame) {
            GameActionButton({ label: '鸿蒙 GPU 超帧（试验）', selected: this.value.frameGeneration === 'harmonyGpu',
              controlId: 'game-framegen-harmony',
              onAction: (): void => this.change(this.value.scaling, this.value.frameLimit, this.value.sharpness ?? 10, 'harmonyGpu') })
              .margin({ bottom: 8 })
          }
        }.width('100%')
      }
      if (this.value.frameGeneration === 'harmonyGpu') {
        Text(this.inGame ? '已选择鸿蒙 GPU 超帧，切换方式需重新启动游戏。' :
          '试验支持能提供深度和相机数据的 DXVK9 固定管线程序，下次启动生效。')
          .fontSize(12).fontColor(this.inGame ? '#9AA9BE' : skinColor('text_secondary')).width('100%')
      }
      if (this.inGame && this.value.frameGeneration === 'x2') {
        Column({ space: 4 }) {
          Text(this.fgStatus).fontSize(12).fontColor('#9AA9BE').width('100%').id('game-framegen-status')
          if (this.fgRates.length > 0) {
            Text(this.fgRates).fontSize(12).fontColor('#E8EDF5').width('100%').id('game-framegen-rates')
          }
          if (this.fgGpuTime.length > 0) {
            Text(this.fgGpuTime).fontSize(12).fontColor('#9AA9BE').width('100%').id('game-framegen-gpu-time')
          }
        }.width('100%')
      }
      Text('改善画面流畅度，实际效果取决于设备和游戏；不改变游戏逻辑及输入帧率。')
        .fontSize(12).fontColor(this.inGame ? '#9AA9BE' : skinColor('text_secondary')).width('100%')
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
