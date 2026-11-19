import { GamepadManager, GamepadNavigationListener } from '../service/GamepadManager';
import { GameActionButton } from './GameActionButton';
import { GamePanelInput } from '../service/GamePanelInput';
import { InputDispatcher } from '../service/InputDispatcher';
import { GameModeService } from '../service/GameModeService';
import { AppSettingsStore } from '../service/AppSettingsStore';
import { cloneAppSettings } from '../model/AppModels';
import promptAction from '@ohos.promptAction';
import { HostWindowTask, HostWindowTasks } from '../service/HostWindowTasks';
import { ThemeSettingsService } from '../service/ThemeSettingsService';
import { GameModeSettings, GameScaling, normalizeGameModeSettings } from '../model/GameModeSettings';
import { GamePresentationSettings } from './GamePresentationSettings';

@Component
export struct GameQuickPanel {
  onResume: () => void = (): void => {};
  @State private tasks: HostWindowTask[] = [];
  @State private settings: GameModeSettings = normalizeGameModeSettings();
  @State private harmonyBlockReason: string = '';
  @State private hud: boolean = false;
  @State private panelWidth: number = 340;
  private sessionSerial: number = 0;
  private settingsAppId: string = '';
  private pendingSave: GameModeSettings | null = null;
  private saving: boolean = false;
  private visible: boolean = false;
  private navigation: GamepadNavigationListener | null = null;
  private focusIndex: number = 0;
  private focusedControl: string = 'game-panel-resume';
  private listener: ((tasks: HostWindowTask[]) => void) | null = null;

  aboutToAppear(): void {
    this.visible = true;
    InputDispatcher.getInstance().releaseAll('game-panel-open');
    GamepadManager.getInstance().setUiCapture(true);
    AppStorage.setOrCreate<boolean>('vpp.game.panel', true);
    this.navigation = (code: number): boolean => this.navigate(code);
    GamePanelInput.attach(this.navigation);
    GamepadManager.getInstance().subscribeNavigation(this.navigation);
    setTimeout((): void => { try { focusControl.requestFocus('game-panel-resume'); } catch (_) {} }, 0);
    this.sessionSerial = GameModeService.sessionSerial();
    this.settingsAppId = GameModeService.activeAppId();
    this.settings = GameModeService.currentSettings();
    this.harmonyBlockReason = GameModeService.currentHarmonyBlockReason();
    this.hud = ThemeSettingsService.getInstance().getSettings().showFpsHud;
    this.listener = (_tasks: HostWindowTask[]): void => { this.tasks = HostWindowTasks.getInstance().controllableGames(); };
    HostWindowTasks.getInstance().subscribe(this.listener);
  }
  aboutToDisappear(): void {
    this.visible = false;
    if (this.listener) HostWindowTasks.getInstance().unsubscribe(this.listener);
    if (this.navigation) {
      GamePanelInput.detach(this.navigation);
      GamepadManager.getInstance().unsubscribeNavigation(this.navigation);
    }
    GamepadManager.getInstance().setUiCapture(false);
    AppStorage.setOrCreate<boolean>('vpp.game.panel', false);
  }
  private navigate(code: number): boolean {
    const ids = ['game-panel-resume',
      'game-scaling-linear', 'game-scaling-nearest', 'game-scaling-integer', 'game-scaling-fsr'];
    ids.push('game-framegen-off', 'game-framegen-x2');
    if (this.harmonyBlockReason.length === 0) ids.push('game-framegen-harmony');
    if (this.settings.scaling === 'fsr') ids.push('game-fsr-sharpness');
    ids.push('game-limit-0', 'game-limit-30', 'game-limit-60');
    ids.push('game-panel-hud', 'game-panel-library');
    ids.push(...this.tasks.map((task: HostWindowTask): string => 'game-panel-close-' + task.key));
    const focused = ids.indexOf(this.focusedControl);
    if (focused < 0) {
      this.focusIndex = 0;
      this.focusedControl = ids[0];
      try { focusControl.requestFocus(this.focusedControl); } catch (_) {}
      if ([2301, 2050, 2054, 2016].includes(code)) return true;
    } else {
      this.focusIndex = focused;
    }
    if (code === 2302 || code === 2070) { this.onResume(); return true; }
    if (ids[this.focusIndex] === 'game-fsr-sharpness' && (code === 2014 || code === 2015)) {
      this.save({ scaling: 'fsr', frameLimit: this.settings.frameLimit,
        sharpness: Math.max(0, Math.min(20, (this.settings.sharpness ?? 10) + (code === 2014 ? 1 : -1))), frameGeneration: this.settings.frameGeneration });
      return true;
    }
    const delta = code === 2012 || code === 2014 ? -1 : code === 2013 || code === 2015 ? 1 : 0;
    if (delta) {
      this.focusIndex = Math.max(0, Math.min(ids.length - 1, this.focusIndex + delta));
      this.focusedControl = ids[this.focusIndex];
      try { focusControl.requestFocus(this.focusedControl); } catch (_) {}
    } else if ([2301, 2050, 2054, 2016].includes(code)) {
      const id = ids[this.focusIndex];
      if (id === 'game-panel-resume') this.onResume();
      else if (id === 'game-panel-library') { this.onResume(); void HostWindowTasks.getInstance().showLauncher(); }
      else if (id === 'game-panel-hud') void this.toggleHud();
      else if (id.startsWith('game-scaling-')) this.save({ scaling: id.substring(13) as GameScaling,
        frameLimit: this.settings.frameLimit, sharpness: this.settings.sharpness, frameGeneration: this.settings.frameGeneration });
      else if (id.startsWith('game-limit-')) this.save({ scaling: this.settings.scaling,
        frameLimit: Number(id.substring(11)), sharpness: this.settings.sharpness, frameGeneration: this.settings.frameGeneration });
      else if (id.startsWith('game-framegen-')) this.save({ scaling: this.settings.scaling, frameLimit: this.settings.frameLimit,
        sharpness: this.settings.sharpness,
        frameGeneration: id === 'game-framegen-harmony' ? 'harmonyGpu' : id === 'game-framegen-x2' ? 'x2' : 'off' });
      else { const task = this.tasks.find((item: HostWindowTask): boolean => 'game-panel-close-' + item.key === id); if (task) void this.close(task); }
    }
    return true;
  }
  private save(value: GameModeSettings): void {
    try {
      GameModeService.applyLive(value, this.sessionSerial);
      this.settings = normalizeGameModeSettings(value);
      this.pendingSave = this.settings;
      void this.persist();
    } catch (error) { promptAction.showToast({ message: (error as Error).message }); }
  }
  private async persist(): Promise<void> {
    if (this.saving || !this.settingsAppId) return;
    this.saving = true;
    try {
      // One write in flight plus the latest value; dragging never queues an
      // unbounded preference/portable-file write for every slider event.
      while (this.pendingSave) {
        const value = this.pendingSave; this.pendingSave = null;
        const store = AppSettingsStore.getInstance();
        const next = cloneAppSettings(store.getAppSettings(this.settingsAppId));
        next.gameModeSettings = value;
        await store.saveAppSettings(next);
      }
    } catch (_) {
      if (this.visible) promptAction.showToast({ message: '画面设置未保存，请重试' });
    } finally { this.saving = false; }
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
    Stack({ alignContent: Alignment.End }) {
      Column().width('100%').height('100%').backgroundColor('#50000000')
        .onClick(this.onResume).id('game-panel-scrim')
      Column({ space: 18 }) {
        Row() {
          Column({ space: 4 }) {
            Text('游戏控制').fontSize(21).fontWeight(FontWeight.Bold).fontColor('#F2F5FA')
            Text('画面与性能').fontSize(12).fontColor('#9AA9BE')
          }.alignItems(HorizontalAlign.Start).layoutWeight(1)
          GameActionButton({ label: '收起', darkPanel: true, controlId: 'game-panel-resume', onAction: this.onResume })
        }.width('100%')
        Divider().color('#2E3746')
        Scroll() {
          Column({ space: 20 }) {
            GamePresentationSettings({ value: this.settings, inGame: true, harmonyBlockReason: this.harmonyBlockReason,
              onHarmonyStatus: (state: number, generated: number): void => {
                GameModeService.observeHarmonyScene(this.sessionSerial, state, generated);
                this.harmonyBlockReason = GameModeService.currentHarmonyBlockReason();
              },
              onChange: (value: GameModeSettings): void => this.save(value) })
            Divider().color('#2E3746')
            GameActionButton({ label: this.hud ? '隐藏性能浮层' : '显示性能浮层', fullWidth: true,
              darkPanel: true, controlId: 'game-panel-hud', onAction: (): void => { void this.toggleHud(); } })
            GameActionButton({ label: '返回游戏库 · 保留游戏', fullWidth: true, darkPanel: true, controlId: 'game-panel-library',
            onAction: (): void => { this.onResume(); void HostWindowTasks.getInstance().showLauncher(); } })
            ForEach(this.tasks, (task: HostWindowTask) => {
              GameActionButton({ label: '正常退出 · ' + task.title, fullWidth: true, darkPanel: true,
                controlId: 'game-panel-close-' + task.key, onAction: (): void => { void this.close(task); } })
            }, (task: HostWindowTask): string => task.key)
          }.width('100%').alignItems(HorizontalAlign.Start)
        }.layoutWeight(1).width('100%').scrollBar(BarState.Auto)
      }.width(this.panelWidth).height('100%').padding({ top: 28, bottom: 24, left: 22, right: 22 })
        .backgroundColor('#F5181E29').border({ width: { left: 1 }, color: '#394354' })
        .shadow({ radius: 24, color: '#60000000', offsetX: -6 })
        .transition(TransitionEffect.move(TransitionEdge.END).animation({ duration: 180, curve: Curve.EaseOut }))
        .id('game-side-panel')
    }.width('100%').height('100%').hitTestBehavior(HitTestMode.Default).id('game-quick-panel')
      .onAreaChange((_old: Area, area: Area): void => { this.panelWidth = Math.min(360, Math.max(0, Number(area.width) - 20)); })
      .onTouch((event: TouchEvent): void => event.stopPropagation())
      .onMouse((event: MouseEvent): void => event.stopPropagation())
      .onKeyEvent((event: KeyEvent): void => {
        if (GamePanelInput.handle(event.keyCode, event.type === KeyType.Down)) event.stopPropagation();
      })
  }
}
