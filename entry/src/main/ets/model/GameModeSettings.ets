export type GameModeOverride = 'follow' | 'on' | 'off';
export type GameScaling = 'linear' | 'nearest' | 'integer' | 'fsr';
export type GameFrameGeneration = 'off' | 'x2';

export interface GameModeSettings {
  scaling: GameScaling;
  frameLimit: number;
  sharpness?: number;
  frameGeneration?: GameFrameGeneration;
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
    frameLimit: value?.frameLimit === 30 || value?.frameLimit === 60 ? value.frameLimit : 0,
    frameGeneration: value?.frameGeneration === 'x2' ? 'x2' : 'off'
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
      (a.settings.frameGeneration ?? 'off') === (b.settings.frameGeneration ?? 'off') &&
      (a.settings.scaling !== 'fsr' || a.settings.sharpness === b.settings.sharpness)));
}

export function frameGenerationStateText(state: number): string {
  if (state === 1) return '插帧预热中';
  if (state === 2) return '2× 插帧运行中';
  if (state === 3) return '源帧率接近屏幕上限，使用真实帧';
  if (state === 4) return '当前 Vulkan 能力不支持插帧';
  if (state === 5) return 'GPU 忙，使用真实帧';
  if (state === 6) return '插帧暂不可用，使用真实帧';
  if (state === 7) return '等待可插帧的 GPU 游戏画面';
  if (state === 8) return '等待屏幕刷新节拍';
  if (state === 9) return 'GPU 预算不足，使用真实帧';
  return '插帧关闭';
}
