/** Persist intent, resolve inheritance only when launching a game. */
export const GRAPHICS_BACKEND_DEFAULT = 'default';
export const GRAPHICS_BACKEND_IDS: string[] = ['dxvk_1_10', 'dxvk_2_6', 'virgl'];
export const APP_GRAPHICS_BACKEND_IDS: string[] = [GRAPHICS_BACKEND_DEFAULT].concat(GRAPHICS_BACKEND_IDS);

export function normalizeGraphicsBackend(value: string): string {
  return GRAPHICS_BACKEND_IDS.includes(value) ? value : 'dxvk_1_10';
}

export function normalizeAppGraphicsBackend(value: string): string {
  return GRAPHICS_BACKEND_IDS.includes(value) ? value : GRAPHICS_BACKEND_DEFAULT;
}

export function resolveGraphicsBackend(system: string, game: string): string {
  const selected = normalizeAppGraphicsBackend(game);
  return selected === GRAPHICS_BACKEND_DEFAULT ? normalizeGraphicsBackend(system) : selected;
}

export function graphicsBackendLabel(value: string): string {
  switch (value) {
    case 'virgl': return 'VirGL / GLES';
    case 'dxvk_1_10': return 'DXVK 1.10';
    case 'dxvk_2_6': return 'DXVK 2.6（实验）';
    default: return '跟随系统';
  }
}
