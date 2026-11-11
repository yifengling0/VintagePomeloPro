import { InputOverlayWindowCoordinator } from './InputOverlayWindowCoordinator';
import { InputProfileService } from './InputProfileService';
import { GamepadManager } from './GamepadManager';
import { AppSettingsStore } from './AppSettingsStore';
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';
import smokeNapi from 'libsmoke_napi.so';
import { HostWindowTasks, HostWindowTask } from './HostWindowTasks';
import { WineWindowManager } from './WineWindowManager';

// Uses the existing Wayland/NativeBuffer presenter. No Wine process, HWND,
// external-surface rebinding or frame readback is needed for this carrier.
export class LinuxSteamNativeWindowHost {
  private navigation: boolean = false;
  private active: boolean = false;
  private timer: number = -1;
  private lastWindows: string = '';
  async start(_context: common.Context, _driveC: string, _results: string,
    _recovering: boolean = false): Promise<void> {
    this.active = true;
    this.refresh();
    this.timer = setInterval((): void => this.refresh(), 150);
  }
  private refresh(): void {
    if (!this.active || !smokeNapi.isLinuxSteamProcess) return;
    const tasks = HostWindowTasks.getInstance();
    tasks.refreshLinuxSteamOwnership(smokeNapi.isLinuxSteamProcess);
    const observed = tasks.snapshot().map((task: HostWindowTask): string =>
      `${task.key}:${task.source}:${task.clientPid}:${smokeNapi.isLinuxSteamProcess?.(task.clientPid) === true}`).join(',');
    if (observed !== this.lastWindows) {
      this.lastWindows = observed;
      hilog.info(0, 'DeckLaunch', 'native carrier ownership=%{public}s', observed.substring(0, 1000));
    }
    this.setNavigation(this.isForeground());
    for (const task of tasks.snapshot()) {
      if (task.source === 'steam-linux' && smokeNapi.isLinuxSteamProcess(task.clientPid))
        WineWindowManager.getInstance().manageExternalToplevel(task.key);
    }
  }
  private setNavigation(enabled: boolean): void {
    if (this.navigation === enabled) return;
    this.navigation = enabled;
    const settings = AppSettingsStore.getInstance().getGlobalSettings();
    const profile = InputProfileService.getInstance().getProfile(enabled ? 'builtin-steam-navigation' :
      (settings.desktopInputProfileId || 'default'));
    const manager = GamepadManager.getInstance();
    manager.releaseAll(); manager.setHostNavigation(false);
    manager.setOutputMode(enabled ? 'dinput' : (settings.controllerOutputMode ?? 'dinput'));
    manager.setButtonMappings(profile.gamepadMappings);
    const coordinator = InputOverlayWindowCoordinator.getInstance();
    const snapshot = coordinator.getSnapshot();
    coordinator.update(snapshot.session, profile.id, snapshot.controlsVisible, snapshot.opacity);
  }
  hasExited(): boolean { return !this.active; }
  async restore(forLaunch: boolean = false): Promise<boolean> {
    if (!this.active) return false;
    this.refresh();
    const tasks = HostWindowTasks.getInstance();
    const task = tasks.snapshot().find((item: HostWindowTask): boolean => item.source === 'steam-linux');
    if (!task) return false;
    return forLaunch ? tasks.activateSteamLaunchPrompt(task.key, '') : tasks.activate(task.key);
  }
  isForeground(): boolean {
    const tasks = HostWindowTasks.getInstance();
    if (!this.active || tasks.isLauncherForeground() || tasks.isSteamGameForeground()) return false;
    const focused = testNapi.getKeyboardFocusedToplevel();
    return tasks.snapshot().some((task: HostWindowTask): boolean =>
      task.source === 'steam-linux' && !task.minimized && task.toplevelId === focused);
  }
  stop(): void {
    this.setNavigation(false);
    this.active = false;
    if (this.timer >= 0) clearInterval(this.timer);
    this.timer = -1;
    // SessionService owns the native child. A carrier replacement must not
    // close Steam, a shared FEX server, or another game's window.
  }
}
