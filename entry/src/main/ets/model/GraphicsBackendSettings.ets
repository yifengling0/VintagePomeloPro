/** Persist intent, resolve inheritance only when launching a game. */
export const GRAPHICS_BACKEND_DEFAULT = 'default';
export const GRAPHICS_BACKEND_IDS: string[] = ['dxvk_1_10', 'dxvk_2_6', 'virgl', 'dxvk9', 'zink'];
export const APP_OPENGL_SOFTPIPE = 'opengl_softpipe';
export const APP_OPENGL_VIRGL_COMPAT = 'opengl_virgl_compat';
export const APP_GRAPHICS_BACKEND_IDS: string[] =
  [GRAPHICS_BACKEND_DEFAULT].concat(GRAPHICS_BACKEND_IDS, [APP_OPENGL_VIRGL_COMPAT, APP_OPENGL_SOFTPIPE]);

export function normalizeGraphicsBackend(value: string): string {
  return GRAPHICS_BACKEND_IDS.includes(value) ? value : 'dxvk_1_10';
}

export function normalizeAppGraphicsBackend(value: string): string {
  return APP_GRAPHICS_BACKEND_IDS.includes(value) ? value : GRAPHICS_BACKEND_DEFAULT;
}

export function resolveGraphicsBackend(system: string, game: string): string {
  const selected = normalizeAppGraphicsBackend(game);
  return selected === GRAPHICS_BACKEND_DEFAULT ? normalizeGraphicsBackend(system) : selected;
}

export function graphicsBackendLabel(value: string): string {
  switch (value) {
    case 'virgl': return 'VirGL / GLES';
    case APP_OPENGL_VIRGL_COMPAT: return 'VirGL OpenGL 兼容（实验）';
    case APP_OPENGL_SOFTPIPE: return 'OpenGL 软件渲染（实验）';
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

export function resolveAppOpenGLEnvironment(backend: string): string[] {
  if (backend === APP_OPENGL_VIRGL_COMPAT) {
    // Mesa otherwise limits shader compilation to GLSL 1.40 on a 3.1 context.
    // Scope the version override to the explicitly selected game's process.
    return ['MESA_GL_VERSION_OVERRIDE=3.2FC', 'MESA_GLSL_VERSION_OVERRIDE=150'];
  }
  if (backend !== APP_OPENGL_SOFTPIPE) return [];
  return [
    'GALLIUM_DRIVER=softpipe',
    'LIBGL_ALWAYS_SOFTWARE=1',
    'MESA_LOADER_DRIVER_OVERRIDE=swrast',
    'WINEHUA_FRAME_ZERO_COPY=0'
  ];
}
