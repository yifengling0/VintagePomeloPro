import testNapi from 'libentry.so';
import { AppDescriptor, GlobalSettings, LaunchKind, harmonyGpuLaunchBlockReason } from '../model/AppModels';
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
  private static harmonyBlockReason: string = '';
  private static previousHarmonyBlockReason: string = '';
  private static harmonyObservationSerial: number = -1;
  private static harmonyMissingSamples: number = 0;

  static reset(): void {
    if (GameModeService.pending) return;
    GameModeService.owner = null;
    GameModeService.appId = '';
    GameModeService.harmonyBlockReason = '';
    ++GameModeService.serial;
    GameModeService.publish(null);
  }

  static begin(app: AppDescriptor, settings: GlobalSettings, engineRunning: boolean): void {
    if (app.launchTarget.kind !== LaunchKind.WINE_EXE && app.launchTarget.kind !== LaunchKind.WINE_SYSTEM &&
      app.launchTarget.kind !== LaunchKind.WINE_DESKTOP && app.launchTarget.kind !== LaunchKind.STEAM_CLIENT &&
      app.launchTarget.kind !== LaunchKind.STEAM_GAME) return;
    const store = AppSettingsStore.getInstance();
    const value = store.getAppSettings(app.id);
    const requested = resolveGamePresentation(AppStorage.get<boolean>('vpp.game.browsing') === true,
      value.gameModeOverride, settings.gameMode, value.gameModeSettings);
    const harmonyBlockReason = harmonyGpuLaunchBlockReason(app, value, store.getRenderMode());
    if (harmonyBlockReason.length > 0 && requested.settings.frameGeneration === 'harmonyGpu') {
      requested.settings.frameGeneration = 'off';
    }
    const current = GameModeService.owner ?? resolveGamePresentation(false, 'off', undefined);
    if (engineRunning && !sameGamePresentation(current, requested)) {
      throw new Error('当前 Windows 会话使用另一组 Game 画面设置。请正常退出游戏和 Steam，并结束 Windows 会话后再启动。');
    }
    GameModeService.previous = GameModeService.owner;
    GameModeService.previousAppId = GameModeService.appId;
    GameModeService.previousHarmonyBlockReason = GameModeService.harmonyBlockReason;
    GameModeService.pending = true;
    // Native applies before any XComponent, producer queue or Wine child exists.
    try { GameModeService.publish(requested); }
    catch (error) {
      GameModeService.pending = false; GameModeService.previous = null;
      throw new Error((error as Error).message);
    }
    GameModeService.owner = requested;
    GameModeService.appId = app.id;
    GameModeService.harmonyBlockReason = harmonyBlockReason;
    ++GameModeService.serial;
  }

  static finish(success: boolean): void {
    if (!GameModeService.pending) return;
    GameModeService.pending = false;
    if (!success) {
      GameModeService.owner = GameModeService.previous;
      GameModeService.appId = GameModeService.previousAppId;
      GameModeService.harmonyBlockReason = GameModeService.previousHarmonyBlockReason;
      GameModeService.publish(GameModeService.owner);
    }
    GameModeService.previous = null;
    GameModeService.previousAppId = '';
    GameModeService.previousHarmonyBlockReason = '';
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

  static currentHarmonyBlockReason(): string {
    if (GameModeService.harmonyBlockReason.length > 0) return GameModeService.harmonyBlockReason;
    if (GameModeService.harmonyObservationSerial === GameModeService.serial &&
        GameModeService.harmonyMissingSamples >= 4) {
      return '当前游戏画面缺少匹配的深度或相机数据';
    }
    return '';
  }

  static observeHarmonyScene(serial: number, state: number, generated: number): void {
    if (serial !== GameModeService.serial || GameModeService.owner?.settings.frameGeneration !== 'harmonyGpu') return;
    if (GameModeService.harmonyObservationSerial !== serial) {
      GameModeService.harmonyObservationSerial = serial;
      GameModeService.harmonyMissingSamples = 0;
    }
    // A single loading frame is inconclusive. Keep observing the current
    // session so a valid scene can make the option available again.
    if (state === 10 && generated === 0) {
      GameModeService.harmonyMissingSamples = Math.min(4, GameModeService.harmonyMissingSamples + 1);
    } else {
      GameModeService.harmonyMissingSamples = 0;
    }
  }

  static applyLive(value: GameModeSettings, serial: number): void {
    if (!GameModeService.owner?.enabled || GameModeService.pending || serial !== GameModeService.serial) {
      throw new Error('游戏会话已变化，请重新打开画面侧栏');
    }
    if (value.frameGeneration === 'harmonyGpu' && GameModeService.owner.settings.frameGeneration !== 'harmonyGpu') {
      const reason = GameModeService.currentHarmonyBlockReason();
      if (reason.length > 0) throw new Error(reason);
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
    testNapi.configureGamePresentation(enabled, value?.settings.scaling ?? 'linear', value?.settings.frameLimit ?? 0, value?.settings.sharpness ?? 10,
      value?.settings.frameGeneration ?? 'off');
    AppStorage.setOrCreate<boolean>('vpp.game.session', enabled);
  }
}
