/** Classic cards use landscape compositions; Deck capsules use dedicated 2:3 art. */
export function builtinCoverResource(appId: string, portrait: boolean = false): Resource {
  if (appId === 'builtin-desktop') return portrait ? $r('app.media.builtin_desktop_portrait') : $r('app.media.builtin_desktop_landscape');
  if (appId === 'builtin-explorer') return portrait ? $r('app.media.builtin_explorer_portrait') : $r('app.media.builtin_explorer_landscape');
  if (appId === 'builtin-winecfg') return portrait ? $r('app.media.builtin_winecfg_portrait') : $r('app.media.builtin_winecfg_landscape');
  if (appId === 'builtin-iexplore') return portrait ? $r('app.media.builtin_iexplore_portrait') : $r('app.media.builtin_iexplore_landscape');
  if (appId === 'builtin-control') return portrait ? $r('app.media.builtin_control_portrait') : $r('app.media.builtin_control_landscape');
  if (appId === 'builtin-taskmgr') return portrait ? $r('app.media.builtin_taskmgr_portrait') : $r('app.media.builtin_taskmgr_landscape');
  if (appId === 'builtin-regedit') return portrait ? $r('app.media.builtin_regedit_portrait') : $r('app.media.builtin_regedit_landscape');
  if (appId === 'builtin-cmd') return portrait ? $r('app.media.builtin_cmd_portrait') : $r('app.media.builtin_cmd_landscape');
  if (appId === 'builtin-notepad') return portrait ? $r('app.media.builtin_notepad_portrait') : $r('app.media.builtin_notepad_landscape');
  if (appId === 'builtin-graphics-smoke' || appId === 'builtin-dxvk-cube')
    return portrait ? $r('app.media.builtin_graphics_portrait') : $r('app.media.builtin_graphics_landscape');
  if (appId === 'builtin-vkd3d-triangle') return portrait ? $r('app.media.builtin_vkd3d_triangle_portrait') : $r('app.media.builtin_vkd3d_triangle_landscape');
  if (appId === 'builtin-vkd3d-gears') return portrait ? $r('app.media.builtin_vkd3d_gears_portrait') : $r('app.media.builtin_vkd3d_gears_landscape');
  if (appId === 'builtin-audio-smoke') return portrait ? $r('app.media.builtin_audio_portrait') : $r('app.media.builtin_audio_landscape');
  return portrait ? $r('app.media.builtin_desktop_portrait') : $r('app.media.builtin_desktop_landscape');
}
