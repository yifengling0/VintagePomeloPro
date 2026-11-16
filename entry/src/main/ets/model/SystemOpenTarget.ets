export interface SystemOpenTarget {
  uri: string;
  path: string;
}
// Parsing is separate from permissions and Ability dispatch.
export function parseSystemOpenTarget(target: string, prefix: string): SystemOpenTarget | null {
  if (!target || target.length > 4090 || /[\x00-\x1f\x7f]/.test(target)) return null;
  if (/^https?:\/\/[^/\s]+(?:\/|$)/i.test(target) || /^mailto:[^\s]+$/i.test(target)) {
    return { uri: target, path: '' };
  }
  if (!/^file:\/\//i.test(target)) return null;
  let path: string;
  try { path = decodeURIComponent(target.substring(7)); } catch (_) { return null; }
  if (/^localhost\//i.test(path)) path = path.substring(9);
  if (!path.startsWith('/') || /[\x00-\x1f\x7f]/.test(path) || path.includes(String.fromCharCode(92))) return null;
  const drive = /^\/([a-zA-Z]):(?:\/|$)/.exec(path);
  if (drive) path = prefix + '/dosdevices/' + drive[1].toLowerCase() + ':' + path.substring(3);
  return { uri: '', path };
}
