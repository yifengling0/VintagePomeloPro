export type GameModeOverride = 'follow' | 'on' | 'off';
export type GameScaling = 'linear' | 'nearest' | 'integer' | 'fsr';

export interface GameModeSettings {
  scaling: GameScaling;
  frameLimit: number;
  sharpness?: number;
}

export interface GamePresentation {
  enabled: boolean;
  settings: GameModeSettings;
}

export function normalizeGameModeOverride(value?: string): GameModeOverride {
  return value === 'on' || value === 'off' ? value : 'follow';
}

export function normalizeGameModeSettings(value?: GameModeSettings): GameModeSettings {
  return {
    scaling: value?.scaling === 'nearest' || value?.scaling === 'integer' || value?.scaling === 'fsr' ? value.scaling : 'linear',
    sharpness: Number.isInteger(value?.sharpness) && (value?.sharpness ?? -1) >= 0 && (value?.sharpness ?? 21) <= 20 ? value?.sharpness : 10,
    frameLimit: value?.frameLimit === 30 || value?.frameLimit === 60 ? value.frameLimit : 0
  };
}

export function resolveGamePresentation(inGameMode: boolean, override: string | undefined,
  global: GameModeSettings | undefined, perGame?: GameModeSettings): GamePresentation {
  const mode = normalizeGameModeOverride(override);
  return { enabled: mode === 'on' || (mode === 'follow' && inGameMode),
    settings: normalizeGameModeSettings(perGame ?? global) };
}

export function sameGamePresentation(a: GamePresentation, b: GamePresentation): boolean {
  return a.enabled === b.enabled && (!a.enabled ||
    (a.settings.scaling === b.settings.scaling && a.settings.frameLimit === b.settings.frameLimit &&
      (a.settings.scaling !== 'fsr' || a.settings.sharpness === b.settings.sharpness)));
}
