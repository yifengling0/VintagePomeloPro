/** The same bundled covers are used by the classic card, console card, and detail page. */
export function builtinCoverResource(appId: string): Resource {
  if (appId === 'builtin-desktop') return $r('app.media.builtin_desktop');
  if (appId === 'builtin-explorer') return $r('app.media.builtin_explorer');
  if (appId === 'builtin-winecfg') return $r('app.media.builtin_winecfg');
  if (appId === 'builtin-iexplore') return $r('app.media.builtin_iexplore');
  if (appId === 'builtin-control') return $r('app.media.builtin_control');
  if (appId === 'builtin-taskmgr') return $r('app.media.builtin_taskmgr');
  if (appId === 'builtin-regedit') return $r('app.media.builtin_regedit');
  if (appId === 'builtin-cmd') return $r('app.media.builtin_cmd');
  if (appId === 'builtin-notepad') return $r('app.media.builtin_notepad');
  if (appId === 'builtin-graphics-smoke' || appId === 'builtin-dxvk-cube')
    return $r('app.media.builtin_graphics');
  if (appId === 'builtin-vkd3d-triangle') return $r('app.media.builtin_vkd3d_triangle');
  if (appId === 'builtin-vkd3d-gears') return $r('app.media.builtin_vkd3d_gears');
  if (appId === 'builtin-audio-smoke') return $r('app.media.builtin_audio');
  return $r('app.media.builtin_desktop');
}
