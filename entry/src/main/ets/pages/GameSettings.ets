import router from '@ohos.router';
import { GamePresentationSettings } from '../components/GamePresentationSettings';
import { GameModeSettings, normalizeGameModeSettings } from '../model/GameModeSettings';
import { ThemeSettingsService } from '../service/ThemeSettingsService';
import { skinColor } from '../service/SkinColors';

@Entry
@Component
struct GameSettings {
  @State private value: GameModeSettings = normalizeGameModeSettings();
  aboutToAppear(): void { this.value = normalizeGameModeSettings(ThemeSettingsService.getInstance().getSettings().gameMode); }
  private async save(value: GameModeSettings): Promise<void> {
    const settings = ThemeSettingsService.getInstance().getSettings();
    settings.gameMode = normalizeGameModeSettings(value);
    await ThemeSettingsService.getInstance().update(settings);
    this.value = normalizeGameModeSettings(value);
  }
  build() {
    Column({ space: 20 }) {
      Row({ space: 12 }) {
        Button('返回').onClick((): void => router.back()).id('game-settings-back')
        Text('Game 画面设置').fontSize(22).fontColor(skinColor('text_primary')).layoutWeight(1)
      }.width('100%')
      Scroll() {
        Column({ space: 24 }) {
          GamePresentationSettings({ value: this.value, onChange: (value: GameModeSettings): void => { void this.save(value); } })
          Button('系统设置').onClick((): void => { void router.pushUrl({ url: 'pages/SystemSettings' }); }).id('game-system-settings')
        }.width('100%')
      }.layoutWeight(1).width('100%')
    }.width('100%').height('100%').padding({ top: 48, bottom: 28, left: 24, right: 24 })
      .backgroundColor(skinColor('page_background')).id('game-settings-page')
  }
}
