import { ConsoleCardLayout, ConsolePalette, GlobalSettings, ThemeMode, UiSkin, ViewMode } from './AppModels';
export interface SyncedPreferences {
  schemaVersion: number; themeMode: ThemeMode; uiSkin: UiSkin; consolePalette: ConsolePalette;
  consoleCardLayout: ConsoleCardLayout; viewMode: ViewMode; useGlassEffect: boolean;
  desktopInputProfileId: string;
}
export interface PreferencesEnvelope {
  schemaVersion: number; deviceClass: string; revision: number; payloadSha256: string; payload: SyncedPreferences;
}
export interface PreferencesRead { current: PreferencesEnvelope | null; }
export interface PreferencesWrite { status: string; current: PreferencesEnvelope; }
export function preferencesPayload(value: GlobalSettings): SyncedPreferences {
  return { schemaVersion: 1, themeMode: value.themeMode, uiSkin: value.uiSkin, consolePalette: value.consolePalette,
    consoleCardLayout: value.consoleCardLayout, viewMode: value.viewMode, useGlassEffect: value.useGlassEffect,
    desktopInputProfileId: value.desktopInputProfileId };
}
export function validateSyncedPreferences(value: SyncedPreferences): void {
  const keys = ['schemaVersion', 'themeMode', 'uiSkin', 'consolePalette', 'consoleCardLayout', 'viewMode', 'useGlassEffect', 'desktopInputProfileId'];
  if (!value || Object.keys(value).some((key: string): boolean => keys.indexOf(key) < 0) || value.schemaVersion !== 1 ||
    ['system','light','dark'].indexOf(value.themeMode) < 0 || ['classic','console'].indexOf(value.uiSkin) < 0 ||
    ['violet','blue','pearl'].indexOf(value.consolePalette) < 0 || ['focus3d','bento_tiles'].indexOf(value.consoleCardLayout) < 0 ||
    ['grid','compact','large','files'].indexOf(value.viewMode) < 0 || typeof value.useGlassEffect !== 'boolean' ||
    !/^[a-zA-Z0-9_-]{1,128}$/.test(value.desktopInputProfileId) || value.desktopInputProfileId.startsWith('app-')) throw new Error('INVALID_PREFERENCES');
}
export interface SyncBaseline { id: string; revision: number; hash: string; }
export interface SyncAccountState {
  userId: string; profilesEnabled: boolean; preferencesEnabled: boolean; profiles: SyncBaseline[];
  preferences: SyncBaseline | null; removedProfiles: string[]; lastSyncedAt: number;
}
export interface ProfileOwner { id: string; userId: string; }
export interface AutomaticSyncData {
  schemaVersion: number; lastOwner: string; preferencesOwner: string; profileOwners: ProfileOwner[]; accounts: SyncAccountState[];
}
export interface SyncItemStatus { id: string; name: string; state: string; lastSyncedAt: number; }

/** Reject invalid journals before they can become a CAS baseline or account owner. */
export function validateAutomaticSyncData(value: AutomaticSyncData): void {
  const user = (id: string): boolean => typeof id === 'string' && /^[a-f0-9]{64}$/.test(id);
  const id = (name: string): boolean => typeof name === 'string' && /^[a-zA-Z0-9_-]{1,128}$/.test(name);
  const baseline = (base: SyncBaseline): boolean => !!base && id(base.id) && typeof base.hash === 'string' &&
    /^[a-f0-9]{64}$/.test(base.hash) && Number.isSafeInteger(base.revision) && base.revision >= 1;
  if (!value || value.schemaVersion !== 1 || !(value.lastOwner === '' || user(value.lastOwner)) ||
    !(value.preferencesOwner === '' || user(value.preferencesOwner)) || !Array.isArray(value.accounts) || value.accounts.length > 8 ||
    !Array.isArray(value.profileOwners) || value.profileOwners.length > 2048) throw new Error('INVALID_SYNC_STATE');
  const owners = new Set<string>(), accounts = new Set<string>();
  for (const owner of value.profileOwners) {
    if (!owner || !id(owner.id) || !user(owner.userId) || owners.has(owner.id)) throw new Error('INVALID_SYNC_STATE');
    owners.add(owner.id);
  }
  for (const account of value.accounts) {
    if (!account || !user(account.userId) || accounts.has(account.userId) || typeof account.profilesEnabled !== 'boolean' ||
      typeof account.preferencesEnabled !== 'boolean' || !Number.isSafeInteger(account.lastSyncedAt) || account.lastSyncedAt < 0 ||
      !Array.isArray(account.profiles) || account.profiles.length > 256 || !Array.isArray(account.removedProfiles) || account.removedProfiles.length > 256 ||
      !(account.preferences === null || baseline(account.preferences) && account.preferences.id === 'preferences')) throw new Error('INVALID_SYNC_STATE');
    accounts.add(account.userId);
    const ids = new Set<string>(), removed = new Set<string>();
    for (const base of account.profiles) {
      if (!baseline(base) || ids.has(base.id)) throw new Error('INVALID_SYNC_STATE'); ids.add(base.id);
    }
    for (const name of account.removedProfiles) {
      if (!id(name) || removed.has(name)) throw new Error('INVALID_SYNC_STATE'); removed.add(name);
    }
  }
}
