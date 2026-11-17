import { skinColor } from '../service/SkinColors';
import promptAction from '@ohos.promptAction';
import { HostWindowTask, HostWindowTasks } from '../service/HostWindowTasks';
import { ThemeSettingsService } from '../service/ThemeSettingsService';
import { GameModeSettings, normalizeGameModeSettings } from '../model/GameModeSettings';
import { GamePresentationSettings } from './GamePresentationSettings';

@Component
export struct GameQuickPanel {
  onResume: () => void = (): void => {};
  @State private tasks: HostWindowTask[] = [];
  @State private settings: GameModeSettings = normalizeGameModeSettings();
  @State private hud: boolean = false;
  private listener: ((tasks: HostWindowTask[]) => void) | null = null;

  aboutToAppear(): void {
    this.settings = normalizeGameModeSettings(ThemeSettingsService.getInstance().getSettings().gameMode);
    this.hud = ThemeSettingsService.getInstance().getSettings().showFpsHud;
    this.listener = (_tasks: HostWindowTask[]): void => { this.tasks = HostWindowTasks.getInstance().controllableGames(); };
    HostWindowTasks.getInstance().subscribe(this.listener);
  }
  aboutToDisappear(): void { if (this.listener) HostWindowTasks.getInstance().unsubscribe(this.listener); }
  private async save(value: GameModeSettings): Promise<void> {
    const settings = ThemeSettingsService.getInstance().getSettings();
    settings.gameMode = value;
    await ThemeSettingsService.getInstance().update(settings);
    this.settings = value;
  }
  private async toggleHud(): Promise<void> {
    const settings = ThemeSettingsService.getInstance().getSettings();
    settings.showFpsHud = !settings.showFpsHud;
    await ThemeSettingsService.getInstance().update(settings);
    this.hud = settings.showFpsHud;
  }
  private async close(task: HostWindowTask): Promise<void> {
    try {
      const accepted = await HostWindowTasks.getInstance().controlGame(task.key, false);
      promptAction.showToast({ message: accepted ? '已请求正常退出，请在游戏中确认保存' : '游戏已退出或暂时无法关闭' });
      if (accepted) this.onResume();
    } catch (_) { promptAction.showToast({ message: '关闭请求未完成，请从游戏菜单退出' }); }
  }
  build() {
    Stack({ alignContent: Alignment.Center }) {
      Column().width('100%').height('100%').backgroundColor('#B3000000')
      Scroll() {
        Column({ space: 16 }) {
          Text('Game 快捷面板').fontSize(22).fontColor(skinColor('text_primary')).width('100%')
          Button('返回游戏').width('100%').onClick(this.onResume).id('game-panel-resume')
          Button('返回游戏库 · 保留会话').width('100%').onClick((): void => {
            this.onResume(); void HostWindowTasks.getInstance().showLauncher();
          }).id('game-panel-library')
          Button(this.hud ? '关闭性能浮层' : '显示性能浮层').width('100%')
            .onClick((): void => { void this.toggleHud(); }).id('game-panel-hud')
          GamePresentationSettings({ value: this.settings, onChange: (value: GameModeSettings): void => { void this.save(value); } })
          ForEach(this.tasks, (task: HostWindowTask) => {
            Button('正常退出 · ' + task.title).width('100%').onClick((): void => { void this.close(task); }).id('game-panel-close-' + task.key)
          }, (task: HostWindowTask): string => task.key)
        }.width('100%').padding(24).backgroundColor(skinColor('page_background')).borderRadius(20)
      }.width('92%').constraintSize({ maxWidth: 560, maxHeight: '90%' }).scrollBar(BarState.Auto)
    }.width('100%').height('100%').hitTestBehavior(HitTestMode.Block).id('game-quick-panel')
  }
}
