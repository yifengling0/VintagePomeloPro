/** Persist intent, resolve inheritance only when launching a game. */
export const GRAPHICS_BACKEND_DEFAULT = 'default';
export const GRAPHICS_BACKEND_IDS: string[] = ['dxvk_1_10', 'dxvk_2_6', 'virgl', 'dxvk9', 'zink'];
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
    case 'dxvk9': return 'DXVK9（32位实验）';
    case 'zink': return 'Zink（实验）';
    case 'dxvk_1_10': return 'DXVK 1.10';
    case 'dxvk_2_6': return 'DXVK 2.6（实验）';
    default: return '跟随系统';
  }
}

/** Explicitly reset for every launch, including a game inheriting another renderer. */
export function resolveZinkPresentationEnvironment(backend: string): string[] {
  return ['WINEHUA_ZINK_PRESENT=' + (backend === 'wined3d_zink' ? '1' : '0')];
}
