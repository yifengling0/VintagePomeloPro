import testNapi from 'libentry.so';
import { AppDescriptor, GlobalSettings, LaunchKind } from '../model/AppModels';
import { GameModeSettings, GamePresentation, normalizeGameModeSettings, resolveGamePresentation, sameGamePresentation } from '../model/GameModeSettings';
import { AppSettingsStore } from './AppSettingsStore';

// A Wine host has one final presenter. Resolve its policy before startup, and
// retain it for Steam's children. Browsing Game never creates a Wine process.
export class GameModeService {
  private static owner: GamePresentation | null = null;
  private static pending: boolean = false;
  private static previous: GamePresentation | null = null;
  private static appId: string = '';
  private static previousAppId: string = '';
  private static serial: number = 0;

  static reset(): void {
    if (GameModeService.pending) return;
    GameModeService.owner = null;
    GameModeService.appId = '';
    ++GameModeService.serial;
    GameModeService.publish(null);
  }

  static begin(app: AppDescriptor, settings: GlobalSettings, engineRunning: boolean): void {
    if (app.launchTarget.kind !== LaunchKind.WINE_EXE && app.launchTarget.kind !== LaunchKind.WINE_SYSTEM &&
      app.launchTarget.kind !== LaunchKind.WINE_DESKTOP && app.launchTarget.kind !== LaunchKind.STEAM_CLIENT &&
      app.launchTarget.kind !== LaunchKind.STEAM_GAME) return;
    const value = AppSettingsStore.getInstance().getAppSettings(app.id);
    const requested = resolveGamePresentation(AppStorage.get<boolean>('vpp.game.browsing') === true,
      value.gameModeOverride, settings.gameMode, value.gameModeSettings);
    const current = GameModeService.owner ?? resolveGamePresentation(false, 'off', undefined);
    if (engineRunning && !sameGamePresentation(current, requested)) {
      throw new Error('当前 Windows 会话使用另一组 Game 画面设置。请正常退出游戏和 Steam，并结束 Windows 会话后再启动。');
    }
    GameModeService.previous = GameModeService.owner;
    GameModeService.previousAppId = GameModeService.appId;
    GameModeService.pending = true;
    // Native applies before any XComponent, producer queue or Wine child exists.
    try { GameModeService.publish(requested); }
    catch (error) {
      GameModeService.pending = false; GameModeService.previous = null;
      throw new Error((error as Error).message);
    }
    GameModeService.owner = requested;
    GameModeService.appId = app.id;
    ++GameModeService.serial;
  }

  static finish(success: boolean): void {
    if (!GameModeService.pending) return;
    GameModeService.pending = false;
    if (!success) {
      GameModeService.owner = GameModeService.previous;
      GameModeService.appId = GameModeService.previousAppId;
      GameModeService.publish(GameModeService.owner);
    }
    GameModeService.previous = null;
    GameModeService.previousAppId = '';
  }

  static present(toplevelId: number): boolean {
    if (!GameModeService.owner?.enabled || toplevelId <= 0) return false;
    return testNapi.presentGameToplevel(toplevelId);
  }

  static presentDesktop(): boolean {
    if (!GameModeService.owner?.enabled) return false;
    return testNapi.presentGameToplevel(0);
  }

  static enabled(): boolean { return GameModeService.owner?.enabled === true; }

  static activeAppId(): string { return GameModeService.appId; }
  static sessionSerial(): number { return GameModeService.serial; }
  static currentSettings(): GameModeSettings { return normalizeGameModeSettings(GameModeService.owner?.settings); }

  // The presenter/graphics backend remains owned by this Wine session. Its
  // filter, sharpening and pacing are safe to change atomically between frames.
  static applyLive(value: GameModeSettings, serial: number): void {
    if (!GameModeService.owner?.enabled || GameModeService.pending || serial !== GameModeService.serial) {
      throw new Error('游戏会话已变化，请重新打开画面侧栏');
    }
    const next: GamePresentation = { enabled: true, settings: normalizeGameModeSettings(value) };
    GameModeService.publish(next);
    GameModeService.owner = next;
  }

  static noteActiveApp(appId: string): void {
    if (!GameModeService.owner?.enabled || GameModeService.appId === appId) return;
    GameModeService.appId = appId;
    ++GameModeService.serial;
  }

  private static publish(value: GamePresentation | null): void {
    const enabled = value?.enabled === true;
    testNapi.configureGamePresentation(enabled, value?.settings.scaling ?? 'linear', value?.settings.frameLimit ?? 0, value?.settings.sharpness ?? 10);
    AppStorage.setOrCreate<boolean>('vpp.game.session', enabled);
  }
}
