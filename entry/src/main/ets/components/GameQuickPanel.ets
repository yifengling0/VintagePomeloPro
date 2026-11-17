import { GamepadManager, GamepadNavigationListener } from '../service/GamepadManager';
import { GameActionButton } from './GameActionButton';
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
  private navigation: GamepadNavigationListener | null = null;
  private focusIndex: number = 0;
  private listener: ((tasks: HostWindowTask[]) => void) | null = null;

  aboutToAppear(): void {
    GamepadManager.getInstance().setUiCapture(true);
    AppStorage.setOrCreate<boolean>('vpp.game.panel', true);
    this.navigation = (code: number): boolean => this.navigate(code);
    GamepadManager.getInstance().subscribeNavigation(this.navigation);
    setTimeout((): void => { try { focusControl.requestFocus('game-panel-resume'); } catch (_) {} }, 0);
    this.settings = normalizeGameModeSettings(ThemeSettingsService.getInstance().getSettings().gameMode);
    this.hud = ThemeSettingsService.getInstance().getSettings().showFpsHud;
    this.listener = (_tasks: HostWindowTask[]): void => { this.tasks = HostWindowTasks.getInstance().controllableGames(); };
    HostWindowTasks.getInstance().subscribe(this.listener);
  }
  aboutToDisappear(): void {
    if (this.listener) HostWindowTasks.getInstance().unsubscribe(this.listener);
    if (this.navigation) GamepadManager.getInstance().unsubscribeNavigation(this.navigation);
    GamepadManager.getInstance().setUiCapture(false);
    AppStorage.setOrCreate<boolean>('vpp.game.panel', false);
  }
  private navigate(code: number): boolean {
    const ids = ['game-panel-resume', 'game-panel-library', 'game-panel-hud',
      'game-scaling-linear', 'game-scaling-nearest', 'game-scaling-integer', 'game-limit-0', 'game-limit-30', 'game-limit-60']
      .concat(this.tasks.map((task: HostWindowTask): string => 'game-panel-close-' + task.key));
    if (code === 2302 || code === 2070) { this.onResume(); return true; }
    const delta = code === 2012 || code === 2014 ? -1 : code === 2013 || code === 2015 ? 1 : 0;
    if (delta) {
      this.focusIndex = Math.max(0, Math.min(ids.length - 1, this.focusIndex + delta));
      try { focusControl.requestFocus(ids[this.focusIndex]); } catch (_) {}
    } else if ([2301, 2050, 2054, 2016].includes(code)) {
      if (this.focusIndex === 0) this.onResume();
      else if (this.focusIndex === 1) { this.onResume(); void HostWindowTasks.getInstance().showLauncher(); }
      else if (this.focusIndex === 2) void this.toggleHud();
      else if (this.focusIndex < 6) void this.save({ scaling: this.focusIndex === 3 ? 'linear' : this.focusIndex === 4 ? 'nearest' : 'integer', frameLimit: this.settings.frameLimit });
      else if (this.focusIndex < 9) void this.save({ scaling: this.settings.scaling, frameLimit: [0, 30, 60][this.focusIndex - 6] });
      else { const task = this.tasks[this.focusIndex - 9]; if (task) void this.close(task); }
    }
    return true;
  }
  private async save(value: GameModeSettings): Promise<void> {
    try {
      const settings = ThemeSettingsService.getInstance().getSettings();
      settings.gameMode = value;
      await ThemeSettingsService.getInstance().update(settings);
      this.settings = value;
    } catch (_) { promptAction.showToast({ message: '画面设置保存失败，请重试' }); }
  }
  private async toggleHud(): Promise<void> {
    try {
      const settings = ThemeSettingsService.getInstance().getSettings();
      settings.showFpsHud = !settings.showFpsHud;
      await ThemeSettingsService.getInstance().update(settings);
      this.hud = settings.showFpsHud;
    } catch (_) { promptAction.showToast({ message: '性能浮层设置未保存，请重试' }); }
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
          GameActionButton({ label: '返回游戏', fullWidth: true, controlId: 'game-panel-resume', onAction: this.onResume })
          GameActionButton({ label: '返回游戏库 · 保留会话', fullWidth: true, controlId: 'game-panel-library',
            onAction: (): void => { this.onResume(); void HostWindowTasks.getInstance().showLauncher(); } })
          GameActionButton({ label: this.hud ? '关闭性能浮层' : '显示性能浮层', fullWidth: true, controlId: 'game-panel-hud',
            onAction: (): void => { void this.toggleHud(); } })
          GamePresentationSettings({ value: this.settings, onChange: (value: GameModeSettings): void => { void this.save(value); } })
          ForEach(this.tasks, (task: HostWindowTask) => {
            GameActionButton({ label: '正常退出 · ' + task.title, fullWidth: true, controlId: 'game-panel-close-' + task.key,
              onAction: (): void => { void this.close(task); } })
          }, (task: HostWindowTask): string => task.key)
        }.width('100%').padding(24).backgroundColor(skinColor('page_background')).borderRadius(20)
      }.width('92%').constraintSize({ maxWidth: 560, maxHeight: '90%' }).scrollBar(BarState.Auto)
    }.width('100%').height('100%').hitTestBehavior(HitTestMode.Default).id('game-quick-panel')
      .onTouch((event: TouchEvent): void => event.stopPropagation())
      .onMouse((event: MouseEvent): void => event.stopPropagation())
  }
}
