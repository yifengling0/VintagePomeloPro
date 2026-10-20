// Product UI state is independent of the official Steam presentation.
export type SteamPresentation = 'desktop' | 'bigPicture';
export type LinuxSteamSessionState = 'stopped' | 'preparing' | 'starting' | 'running' | 'stopping' | 'failed';
export type LinuxSteamAction = 'library' | 'downloads' | 'store' | 'account' | 'desktop' | 'bigPicture' | 'launch' | 'shutdown';
export interface LinuxSteamGame {
  appId: string;
  name: string;
  directory: string;
  cover: string;
  hero: string;
  lastPlayed: number;
  updating: boolean;
}
export interface LinuxSteamSnapshot {
  state: LinuxSteamSessionState;
  presentation: SteamPresentation;
  message: string;
  generation: string;
}
export interface LinuxSteamCommandAck {
  version: number;
  generation: string;
  requestId: number;
  status: string;
  exitCode: number;
}
export function validLinuxSteamAppId(value: string): boolean {
  return /^[1-9][0-9]{0,9}$/.test(value) && Number(value) <= 4294967295;
}
