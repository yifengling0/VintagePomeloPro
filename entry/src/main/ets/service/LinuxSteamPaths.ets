// Shared by installation, launch and Cloud path conversion. Drive boundaries
// are checked before slicing; a native path is never guessed to be a Z: path.
export const LINUX_STEAM_PRIVATE_ROOT = '/data/storage/el2/base/files/.wine/drive_c/SteamLinuxARM64';
export const LINUX_STEAM_WINE_PREFIX = '/data/storage/el2/base/files/.wine';
export type LinuxSteamStorageLocation = 'private' | 'download';

function normalizedAbsolute(path: string): string {
  if (!path.startsWith('/') || /[\x00-\x1f\\:]/.test(path)) throw new Error('Steam 路径无效');
  const parts: string[] = [];
  for (const part of path.split('/')) {
    if (!part || part === '.') continue;
    if (part === '..') { if (parts.length === 0) throw new Error('Steam 路径超出磁盘'); parts.pop(); }
    else parts.push(part);
  }
  return '/' + parts.join('/');
}
export function linuxSteamWindowsPath(path: string, zRoot: string,
  prefix: string = LINUX_STEAM_WINE_PREFIX): string {
  const absolute = normalizedAbsolute(path);
  for (const drive of ['C', 'Z']) {
    const root = drive === 'C' ? prefix + '/drive_c' : zRoot;
    if (!root || root === '/') continue;
    const base = normalizedAbsolute(root);
    if (absolute === base || absolute.startsWith(base + '/')) {
      return drive + ':\\' + absolute.substring(base.length).replace(/^\//, '').replace(/\//g, '\\');
    }
  }
  throw new Error('Steam 路径未映射到 Wine 磁盘，请检查安装位置');
}
export function linuxSteamLaunchArgument(argument: string, zRoot: string): string {
  // argv has already been split by Steam. Preserve quoting, flags, URLs and
  // Windows paths. Only standalone native paths on known drives are mapped.
  if (!argument.startsWith('/')) return argument;
  try { return linuxSteamWindowsPath(argument, zRoot); } catch (_) { return argument; }
}
