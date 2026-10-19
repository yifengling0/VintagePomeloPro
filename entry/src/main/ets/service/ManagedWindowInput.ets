import { common } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import testNapi from 'libentry.so';
import { DisplayMode, InputProfile } from '../model/AppModels';
import { AppSettingsStore } from './AppSettingsStore';
import { GamepadManager } from './GamepadManager';
import { HostWindowTask, HostWindowTasks } from './HostWindowTasks';
import { InputDispatcher } from './InputDispatcher';
import { InputOverlayWindowCoordinator } from './InputOverlayWindowCoordinator';
import { InputProfileService } from './InputProfileService';

// One foreground input owner across native Steam and its OHOS Wine games.
export class ManagedWindowInput {
  private static instance: ManagedWindowInput | null = null;
  private owner: number = 0;
  private generation: number = 0;
  private operations: Promise<void> = Promise.resolve();

  static getInstance(): ManagedWindowInput {
    if (!ManagedWindowInput.instance) ManagedWindowInput.instance = new ManagedWindowInput();
    return ManagedWindowInput.instance;
  }

  private task(id: number): HostWindowTask | undefined {
    return HostWindowTasks.getInstance().snapshot().find((item: HostWindowTask): boolean =>
      item.toplevelId === id && (item.source === 'steam-linux' || item.source === 'steam-game'));
  }

  foreground(id: number, stage: window.WindowStage, parent: window.Window,
    context: common.UIAbilityContext): void {
    const task = this.task(id);
    if (!task) return;
    this.owner = id;
    const serial = ++this.generation;
    HostWindowTasks.getInstance().noteInteraction(id);
    const run = this.operations.then(async (): Promise<void> => {
      if (serial !== this.generation) return;
      const settings = AppSettingsStore.getInstance().getGlobalSettings();
      const steam = task.source === 'steam-linux';
      // Native Steam consumes the same ControllerHub through an SDL3 virtual
      // gamepad. Keep physical input in the hub, avoiding duplicate key navigation.
      const manager = GamepadManager.getInstance();
      manager.releaseAll();
      manager.setHostNavigation(false);
      manager.setOutputMode(steam ? 'dinput' : (settings.controllerOutputMode ?? 'dinput'));
      const profiles = InputProfileService.getInstance();
      await profiles.initialize(context);
      if (serial !== this.generation) return;
      const profile: InputProfile = profiles.getProfile(steam ? 'builtin-steam-navigation' :
        (settings.desktopInputProfileId || 'default'));
      manager.setButtonMappings(profile.gamepadMappings);
      InputDispatcher.getInstance().setSession({ sessionId: task.sessionId || task.key,
        displayMode: DisplayMode.DESKTOP, toplevelId: id, active: true });
      const focusTarget = HostWindowTasks.getInstance().focusTarget(id);
      testNapi.activateToplevel(focusTarget);
      await InputOverlayWindowCoordinator.getInstance().attach(stage, parent,
        task.sessionId || task.key, DisplayMode.DESKTOP, id,
        profile.id, settings.showVirtualControls, settings.controlOpacity,
        settings.virtualInputEnabled, true);
      if (serial !== this.generation) return;
    });
    this.operations = run.catch((): void => {});
  }

  background(id: number): void {
    if (this.owner !== id) return;
    this.owner = 0;
    ++this.generation;
    GamepadManager.getInstance().releaseAll();
    GamepadManager.getInstance().setHostNavigation(false);
    HostWindowTasks.getInstance().noteBackground(id);
    InputDispatcher.getInstance().releaseAll('managed-window-background');
    const settings = AppSettingsStore.getInstance().getGlobalSettings();
    GamepadManager.getInstance().setOutputMode(settings.controllerOutputMode ?? 'dinput');
    this.operations = this.operations.then(async (): Promise<void> => {
      if (this.owner === 0) await InputOverlayWindowCoordinator.getInstance().setVisible(false);
    }).catch((): void => {});
  }

  syncBounds(id: number): void {
    if (this.owner === id) void InputOverlayWindowCoordinator.getInstance().syncBounds().catch((): void => {});
  }
}
