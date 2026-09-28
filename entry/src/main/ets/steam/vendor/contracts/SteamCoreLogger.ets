import { SteamAuthError, SteamAuthErrorSupport } from './SteamAuthErrors';

/**
 * Platform-independent observability boundary for steam_core.
 *
 * The core never imports NGF, HarmonyOS or a platform logging API. Hosts may
 * inject a logger, while tests and standalone consumers can keep the default
 * no-op implementation.
 */
export interface ISteamCoreLogger {
  debug(scope: string, message: string): void;
  info(scope: string, message: string): void;
  warn(scope: string, message: string): void;
  error(scope: string, message: string): void;
  state(scope: string, from: string, to: string, details: string): void;
}

export class SteamCoreNoopLogger implements ISteamCoreLogger {
  debug(_scope: string, _message: string): void {
  }

  info(_scope: string, _message: string): void {
  }

  warn(_scope: string, _message: string): void {
  }

  error(_scope: string, _message: string): void {
  }

  state(_scope: string, _from: string, _to: string, _details: string): void {
  }
}

/** Converts caught values to a safe type label without retaining the message. */
export class SteamCoreLogSupport {
  static errorKind(error: Error | Object | string | number | boolean | null | undefined): string {
    if (error instanceof SteamAuthError) {
      return SteamAuthErrorSupport.diagnosticLabel(error);
    }
    if (error instanceof Error && error.name.length > 0) {
      return error.name;
    }
    return 'NonError';
  }
}
