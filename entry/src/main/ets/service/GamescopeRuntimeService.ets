import testNapi, { GamescopeRuntimeStatus } from 'libentry.so';
import { hilog } from './LogService';

// Control-plane capability only. A loadable core does not qualify any game
// graphics backend, and must never silently switch an existing Wine session.
export class GamescopeRuntimeService {
  private static status: GamescopeRuntimeStatus | null = null;
  private static pending: Promise<GamescopeRuntimeStatus> | null = null;

  static initialize(): void {
    // Ability.onCreate precedes window/content creation. Phones prepare the
    // existing clean fork server here, before ArkUI initializes GPU state.
    const prepared = testNapi.prepareGamescopeProcessBackend();
    hilog.info(0, 'Gamescope', 'process preparation=%{public}d', prepared);
    if (prepared === 0) {
      GamescopeRuntimeService.query().catch(() => {
        hilog.warn(0, 'Gamescope', 'runtime capability query unavailable');
      });
    }
  }

  static query(): Promise<GamescopeRuntimeStatus> {
    if (!GamescopeRuntimeService.pending) {
      GamescopeRuntimeService.pending = testNapi.probeGamescopeRuntime().then((value: GamescopeRuntimeStatus) => {
        GamescopeRuntimeService.status = value;
        hilog.info(0, 'Gamescope', 'loader loaded=%{public}s stage=%{public}s route=%{public}s detail=%{public}s',
          String(value.loaded), value.stage, value.processBackend, value.detail);
        return value;
      });
    }
    return GamescopeRuntimeService.pending;
  }

  static snapshot(): GamescopeRuntimeStatus | null {
    return GamescopeRuntimeService.status;
  }
}
