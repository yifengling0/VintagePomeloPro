export const smokeMapPoint: (guestX: number, guestY: number) => PointMap | null;

export interface LinuxSteamHostResult {
  schemaVersion: number;
  status: string;
  stage: string;
  discoveryStatus: number;
  surfaceStatus: number;
  globalsSeen: number;
  versions: number[];
  syncReceived: boolean;
  windowConfigured: boolean;
  framesSubmitted: number;
  bufferReleases: number;
  frameCallbacks: number;
  displayError: number;
  keyboardKeymap: boolean;
  keyboardEntered: boolean;
  keyEvents: number;
  inputVerified: boolean;
  inputInjected: boolean;
  framePresented: boolean;
  gnuExecutionVerified: boolean;
  steamClientStarted: boolean;
}

// Absent in normal builds; enabled only by WINEHUA_ENABLE_STEAMOHOS_HOST.
export const probeLinuxSteamHost: (() => Promise<LinuxSteamHostResult>) | undefined;
export const injectLinuxSteamHostKeys: (() => boolean) | undefined;
export const probeLinuxSteamGnu: (() => Promise<string>) | undefined;
export const startLinuxProtonGame: ((ticket: number, wineLanguage?: string) => Promise<string>) | undefined;
export const cancelLinuxProtonGame: ((ticket: number) => boolean) | undefined;
export const probeLinuxProton: (() => Promise<string>) | undefined;

export interface PointMap {
  px: number;
  py: number;
}

export const probeLinuxSteamGnuWayland: (() => Promise<string>) | undefined;

export const probeLinuxSteamGnuXwayland: (() => Promise<string>) | undefined;

export const probeLinuxSteamGnuClient: (() => Promise<string>) | undefined;

export interface LinuxSteamLaunchRequest {
  id: number; cancel: boolean; appId: string; cwd: string; install: string;
  role: 'game' | 'install_script' | 'driver_query';
  backend?: 'ohos-wine' | 'gnu-proton' | 'gnu-proton-gl';
  argv: string[]; environment: string[];
}
export const startLinuxSteamLaunchBroker: ((library: string) => boolean) | undefined;
export const stopLinuxSteamLaunchBroker: (() => boolean) | undefined;
export const drainLinuxSteamLaunchBroker: (() => LinuxSteamLaunchRequest[]) | undefined;
export const replyLinuxSteamLaunchBroker: ((id: number, kind: number, code: number) => boolean) | undefined;
export const pollLinuxSteamLaunchSession: ((id: number, sessionId: string) => number) | undefined;

export const isLinuxSteamProcess: ((pid: number) => boolean) | undefined;

// Persistent product session, separate from bounded diagnostic probes.
export const startLinuxSteamSession: (() => Promise<string>) | undefined;
