/** Persist intent, resolve inheritance only when launching a game. */
export const GRAPHICS_BACKEND_DEFAULT = 'default';
export const APP_OPENGL_MOBILEGL = 'opengl_mobilegl';
export const APP_OPENGL_MOBILEGL_VENUS = 'opengl_mobilegl_venus';
export const OPENGL_BACKEND_AUTO = 'auto';
export const OPENGL_BACKEND_MOBILEGL = 'mobilegl';
export const OPENGL_BACKEND_VIRGL = 'virgl';
export const OPENGL_BACKEND_CHOICES: string[] = [OPENGL_BACKEND_AUTO, OPENGL_BACKEND_MOBILEGL, OPENGL_BACKEND_VIRGL];
export const APP_OPENGL_BACKEND_CHOICES: string[] = [GRAPHICS_BACKEND_DEFAULT].concat(OPENGL_BACKEND_CHOICES);
export const VKD3D_VENUS = 'vkd3d_limited_500k';
export const VKD3D_DIRECT = 'vkd3d_limited_500k_direct';
export const GRAPHICS_BACKEND_IDS: string[] = ['dxvk_1_10', 'dxvk_2_6', 'virgl', 'dxvk9', 'zink',
  VKD3D_VENUS, VKD3D_DIRECT, APP_OPENGL_MOBILEGL, APP_OPENGL_MOBILEGL_VENUS];
export const APP_OPENGL_SOFTPIPE = 'opengl_softpipe';
export const APP_OPENGL_VIRGL_COMPAT = 'opengl_virgl_compat';
export const APP_OPENGL_VIRGL_33 = 'opengl_virgl_33';
export const GRAPHICS_PROFILE_IDS: string[] = ['dxvk_1_10', 'dxvk_2_6', 'virgl', 'zink', VKD3D_DIRECT];
export const APP_GRAPHICS_BACKEND_IDS: string[] = [GRAPHICS_BACKEND_DEFAULT].concat(
  GRAPHICS_PROFILE_IDS, [APP_OPENGL_VIRGL_COMPAT]);

/** Keep saved backend IDs readable while presenting the reduced set of choices. */
export function graphicsProfileButton(backend: string): string {
  if (backend === 'dxvk9') return 'dxvk_1_10';
  if (isMobileGLBackend(backend)) return backend === APP_OPENGL_MOBILEGL_VENUS ? 'dxvk_2_6' : 'dxvk_1_10';
  return isVirglOpenGLCompatibility(backend) ? APP_OPENGL_VIRGL_COMPAT : backend;
}

export function showVirglOpenGLCompatibility(gpu: string, selected: string, mobileGLAvailable: boolean = true): boolean {
  const model = /Maleoon\s*(\d+)/i.exec(gpu);
  return !mobileGLAvailable || isVirglOpenGLCompatibility(selected) ||
    (model !== null && parseInt(model[1], 10) <= 910);
}

export function normalizeGraphicsBackend(value: string): string {
  return GRAPHICS_BACKEND_IDS.includes(value) ? value : 'dxvk_1_10';
}

export function normalizeAppGraphicsBackend(value: string): string {
  return value === GRAPHICS_BACKEND_DEFAULT || value === APP_OPENGL_SOFTPIPE ||
    isVirglOpenGLCompatibility(value) || GRAPHICS_BACKEND_IDS.includes(value)
    ? value : GRAPHICS_BACKEND_DEFAULT;
}

export function normalizeOpenGLBackend(value: string | undefined): string {
  return value && OPENGL_BACKEND_CHOICES.includes(value) ? value : OPENGL_BACKEND_AUTO;
}

export function normalizeAppOpenGLBackend(value: string | undefined): string {
  return value && APP_OPENGL_BACKEND_CHOICES.includes(value) ? value : GRAPHICS_BACKEND_DEFAULT;
}

export function openGLBackendLabel(value: string): string {
  switch (value) {
    case OPENGL_BACKEND_MOBILEGL: return 'MobileGL';
    case OPENGL_BACKEND_VIRGL: return 'VirGL';
    case OPENGL_BACKEND_AUTO: return '自动';
    default: return '跟随系统';
  }
}

export function resolveOpenGLBackend(system: string, app: string | undefined, graphicsMode: string): string {
  if (graphicsMode === 'zink' || graphicsMode === APP_OPENGL_SOFTPIPE || isVirglOpenGLCompatibility(graphicsMode)) {
    return GRAPHICS_BACKEND_DEFAULT;
  }
  const selected = normalizeAppOpenGLBackend(app);
  return selected === GRAPHICS_BACKEND_DEFAULT ? normalizeOpenGLBackend(system) : selected;
}

export function supportsMobileGLVulkan(apiVersion: number): boolean {
  const variant = apiVersion >>> 29;
  const major = (apiVersion >>> 22) & 0x7f;
  const minor = (apiVersion >>> 12) & 0x3ff;
  return variant === 0 && (major > 1 || (major === 1 && minor >= 3));
}

export function isMobileGLBackend(backend: string): boolean {
  return backend === APP_OPENGL_MOBILEGL || backend === APP_OPENGL_MOBILEGL_VENUS;
}

export function isVkd3dBackend(backend: string): boolean {
  return backend === VKD3D_VENUS || backend === VKD3D_DIRECT;
}

export function isVirglOpenGLCompatibility(backend: string): boolean {
  return backend === APP_OPENGL_VIRGL_COMPAT || backend === APP_OPENGL_VIRGL_33;
}

export function resolveGraphicsBackend(system: string, game: string): string {
  const selected = normalizeAppGraphicsBackend(game);
  return selected === GRAPHICS_BACKEND_DEFAULT ? normalizeGraphicsBackend(system) : selected;
}

export function graphicsBackendLabel(value: string): string {
  switch (value) {
    case 'virgl': return 'VirGL';
    case APP_OPENGL_VIRGL_COMPAT: return 'VirGL OpenGL 兼容（实验）';
    case APP_OPENGL_VIRGL_33: return 'VirGL OpenGL 3.3（实验）';
    case APP_OPENGL_MOBILEGL: return 'MobileGL OpenGL 4.6（实验）';
    case APP_OPENGL_MOBILEGL_VENUS: return 'MobileGL OpenGL 4.6 · Venus（实验）';
    case APP_OPENGL_SOFTPIPE: return 'OpenGL 软件渲染（实验）';
    case 'dxvk9': return 'DXVK9（32位实验）';
    case 'zink': return 'Zink（实验）';
    case 'dxvk_1_10': return 'DXVK 1.10';
    case 'dxvk_2_6': return 'DXVK 2.6（实验）';
    case VKD3D_VENUS: return 'DX12 · Venus（实验）';
    case VKD3D_DIRECT: return 'DX12 自动';
    default: return '跟随系统';
  }
}

export function supportsGpuBcUpload(mode: string): boolean {
  return mode === 'virgl' || mode === 'dxvk_1_10';
}

export function gpuBcUploadEnvironment(backend: string, enabled: boolean): string {
  const supported = backend === 'wined3d' || backend === 'dxvk_legacy' || backend === 'dxvk_legacy_d3d9';
  return 'WINEHUA_GPU_BC_UPLOAD=' + (enabled && supported ? '1' : '0');
}

/** Explicitly reset for every launch, including a game inheriting another renderer. */
export function resolveZinkPresentationEnvironment(backend: string): string[] {
  return ['WINEHUA_ZINK_PRESENT=' + (backend === 'wined3d_zink' ? '1' : '0')];
}

export function resolveAppOpenGLEnvironment(backend: string): string[] {
  if (backend === APP_OPENGL_VIRGL_33) {
    return ['MESA_GL_VERSION_OVERRIDE=3.3FC', 'MESA_GLSL_VERSION_OVERRIDE=330'];
  }
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
