import { fileIo as fs } from '@kit.CoreFileKit';
import util from '@ohos.util';
import { SteamVdfNode, parseSteamVdf } from '../steam/SteamLocalRules';

const BASE = '/data/storage/el2/base/files/steamohos-native';
const FILES = '/data/storage/el2/base/files';
const TAIL_BYTES = 65536;
const LOG_TAIL_BYTES = 262144;
const REPORT_BYTES = 8 * 1024 * 1024;

// Export fixed diagnostic files only. Login/configuration files, cookies,
// saved passwords, game files and arbitrary directory contents are excluded.
export function redactSteamDiagnostic(text: string): string {
  return text.replace(/-----BEGIN [^-]*PRIVATE KEY-----[\s\S]*?-----END [^-]*PRIVATE KEY-----/g, '<private key>')
    // Steam CDN URLs may carry short-lived credentials under opaque keys
    // such as t/k. Keep the host/path for routing diagnosis, remove the query.
    .replace(/(https?:\/\/[^\s?"'<>]+)\?[^\s"'<>]*/gi, '$1?<redacted>')
    .replace(/((?:access[_-]?token|refresh[_-]?token|auth[_-]?token|password|passwd|authorization|cookie|login[_-]?key|steamloginsecure|sessionid|ticket|machineauth|hmosdeck_steam_bridge|steamohos_launch_nonce)[\t ]*["']?[\t ]*[:=][\t ]*)[^\r\n]*/gi, '$1<redacted>')
    .replace(/(["'](?:access[_-]?token|refresh[_-]?token|password|passwd|login[_-]?key|sessionid|ticket)["'][\t ]+["'])[^"'\r\n]*/gi, '$1<redacted>')
    .replace(/((?:--?)(?:password|passwd|access-token|auth-token|ticket)[\t ]+)(?:"[^"]*"|'[^']*'|[^\s]+)/gi, '$1<redacted>')
    .replace(/((?:accountname|username|loginname)[\t ]*[:=][\t ]*)[^\s]+/gi, '$1<account>')
    .replace(/\b7656119\d{10}\b/g, '<SteamID>')
    .replace(/\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,}\b/gi, '<email>');
}

export function readDiagnosticTail(path: string, limit: number, whole: boolean = false): string {
  if (!fs.lstatSync(path).isFile() || fs.lstatSync(path).isSymbolicLink()) throw new Error('unsafe diagnostic');
  const file = fs.openSync(path, fs.OpenMode.READ_ONLY | fs.OpenMode.NOFOLLOW);
  try {
    const stat = fs.statSync(file.fd);
    if (!stat.isFile() || stat.size < 0) throw new Error('invalid diagnostic');
    if (whole && stat.size > limit) throw new Error('diagnostic input too large');
    const length = Math.min(stat.size, limit);
    const offset = Math.max(0, stat.size - length);
    const bytes = new ArrayBuffer(length);
    let count = 0;
    while (count < length) {
      const part = new ArrayBuffer(Math.min(length - count, TAIL_BYTES));
      const n = fs.readSync(file.fd, part, { offset: offset + count, length: part.byteLength });
      if (n <= 0) break;
      new Uint8Array(bytes).set(new Uint8Array(part, 0, n), count); count += n;
    }
    let text = util.TextDecoder.create('utf-8', { fatal: false }).decodeWithStream(new Uint8Array(bytes, 0, count), { stream: false });
    // A bounded tail can start inside an account/token field. Drop the partial
    // first line before redaction rather than exporting its unidentified suffix.
    if (offset > 0) { const newline = text.indexOf('\n'); text = newline >= 0 ? text.substring(newline + 1) : ''; }
    if (whole) { if (count !== length) throw new Error('incomplete diagnostic input'); return text; }
    return `bytes=${stat.size} offset=${offset} read=${count} truncated=${offset > 0} mtime=${stat.mtime}\n` + redactSteamDiagnostic(text);
  } finally { fs.closeSync(file); }
}

// Read configuration for routing evidence, but export only numeric AppIDs and
// tool names/priorities. The complete config can contain account credentials.
export function summarizeSteamToolSelection(text: string): string {
  let node: SteamVdfNode | undefined = parseSteamVdf(text);
  for (const key of ['installconfigstore', 'software', 'valve', 'steam', 'compattoolmapping']) {
    node = node?.child(key);
  }
  const lines: string[] = [];
  let count = 0;
  node?.children.forEach((mapping: SteamVdfNode, id: string): void => {
    if (!/^(?:0|[1-9][0-9]{0,9})$/.test(id) || count++ >= 128) return;
    const name = mapping.text('name');
    const priority = mapping.text('priority');
    if (name.length > 128 || /[\x00-\x1f]/.test(name) || !/^[0-9]{0,10}$/.test(priority)) return;
    lines.push(JSON.stringify({ appId: id, tool: name, priority }));
  });
  return lines.length > 0 ? lines.join('\n') : 'No explicit compatibility-tool mappings';
}

// Shallow, allowlisted discovery only: never recurse into games, userdata,
// browser profiles, crash dumps, registry files or Steam account databases.
export function diagnosticDirectory(path: string): boolean {
  try { const st = fs.lstatSync(path); return st.isDirectory() && !st.isSymbolicLink(); } catch (_) { return false; }
}

export function diagnosticNames(path: string, pattern: RegExp, maximum: number): string[] {
  if (!diagnosticDirectory(path)) return [];
  try {
    return fs.listFileSync(path).filter((name: string): boolean => pattern.test(name) &&
      !/[\/\\\x00-\x1f]/.test(name)).sort((a: string, b: string): number => {
      try { return fs.lstatSync(`${path}/${b}`).mtime - fs.lstatSync(`${path}/${a}`).mtime || b.localeCompare(a); }
      catch (_) { return b.localeCompare(a); }
    }).slice(0, maximum);
  } catch (_) { return []; }
}

function validAbsoluteDirectory(path: string): boolean {
  return path.startsWith('/') && path !== '/' && !path.split('/').includes('..') &&
    !/[\\\x00-\x1f]/.test(path) && diagnosticDirectory(path);
}

export async function collectSteamStartupDiagnostics(downloadRoot: string = ''): Promise<string> {
  let text = 'Steam / Wine diagnostics v3\nCollected=' + new Date().toISOString() +
    '\nRecent log tails only; credentials redacted. Missing files do not imply a failure.\n';
  let found = false;
  let remaining = REPORT_BYTES;
  const append = async (label: string, path: string, limit: number = TAIL_BYTES): Promise<void> => {
    // Yield before every bounded source so dozens of Steam/VirGL log reads
    // cannot monopolize ArkUI and trip the system's input/IPC freeze watchdog.
    await new Promise<void>((resolve): void => { setTimeout(resolve, 0); });
    if (remaining < 4096) { text += `\n=== ${label}: omitted (export size limit) ===\n`; return; }
    try {
      const tail = readDiagnosticTail(path, Math.min(limit, Math.floor(remaining / 4)));
      const bytes = new util.TextEncoder().encodeInto(tail).byteLength;
      remaining -= bytes;
      text += `\n=== ${redactSteamDiagnostic(label)} ===\n${tail}\n`; found = true;
    }
    catch (_) { text += `\n=== ${label}: unavailable ===\n`; }
  };
  await append('session', `${BASE}/session/state.json`, 4096);
  await append('native startup', `${BASE}/session/native-startup.json`, 4096);
  await append('window binding', '/data/storage/el2/base/files/.wine/drive_c/hmosdeck/session/binding.json', 4096);
  await append('Wine window', '/data/storage/el2/base/files/.wine/drive_c/hmosdeck/session/wine-window-state.json', 4096);
  const launch = '/data/storage/el2/base/files/.wine/drive_c/hmosdeck/session';
  const wineTemp = '/data/storage/el2/base/files/.wine/drive_c/windows/temp';
  await append('Steam API bridge client', `${wineTemp}/hmosdeck-steam-api-state.json`, 4096);
  await append('Steam API bridge server', `${wineTemp}/hmosdeck-steam-bridge-state.json`, 4096);
  await append('last game launch', `${launch}/last-game-launch.json`, 4096);
  await append('last client helper', `${launch}/last-client-helper.json`, 4096);
  await append('library state', `${launch}/library-state.private.json`, 32768);
  await append('Steam supervisor', `${BASE}/client-tmp/deck-session-state.json`, 4096);
  await append('native output', `${BASE}/client-native.private.log`);
  // Persisted capture survives app restarts. Also include older releases'
  // temporary capture, including yesterday's crash, rather than just today.
  for (const directory of [`${FILES}/wine-logs`, '/data/storage/el2/base/temp']) {
    const logs = diagnosticNames(directory, /^wine_stderr_\d{8}\.log$/, 4);
    if (logs.length === 0) text += `\nWine stderr (${directory}): unavailable\n`;
    for (const name of logs) await append(`Wine stderr ${directory}/${name}`, `${directory}/${name}`, LOG_TAIL_BYTES);
  }
  await append('Wine CEF utility', '/data/storage/el2/base/temp/cef-utility-diag.log');
  const prefixes = [`${FILES}/.wine`];
  for (const id of diagnosticNames(`${FILES}/containers`, /^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$/, 8)) {
    if (diagnosticDirectory(`${FILES}/containers/${id}`)) prefixes.push(`${FILES}/containers/${id}/prefix`);
  }
  let dxvkFiles = 0;
  for (const prefix of prefixes) {
    if (!diagnosticDirectory(prefix) || !diagnosticDirectory(`${prefix}/drive_c`) ||
      !diagnosticDirectory(`${prefix}/drive_c/windows`)) continue;
    const temp = `${prefix}/drive_c/windows/temp`;
    for (const name of diagnosticNames(temp, /^[^\/\\\x00-\x1f]{1,180}_(?:d3d9|d3d11|dxgi)\.log$/i, 8)) {
      if (dxvkFiles++ >= 16) { text += '\nOlder DXVK logs omitted (maximum 16).\n'; break; }
      await append(`DXVK ${prefix}/${name}`, `${temp}/${name}`);
    }
    if (prefix !== prefixes[0]) {
      await append(`Steam API client ${prefix}`, `${temp}/hmosdeck-steam-api-state.json`, 4096);
      await append(`Steam API server ${prefix}`, `${temp}/hmosdeck-steam-bridge-state.json`, 4096);
    }
  }
  const steamLogs = ['console_log.txt', 'gameprocess_log.txt', 'content_log.txt', 'connection_log.txt',
    'cloud_log.txt', 'compat_log.txt', 'installscript_log.txt', 'bootstrap_log.txt',
    'steamwebhelper.log', 'cef_log.txt', 'webhelper.txt', 'shader_log.txt', 'controller.txt'];
  const appendSteam = async (label: string, root: string, limit: number = TAIL_BYTES): Promise<void> => {
    if (!diagnosticDirectory(`${root}/logs`)) { text += `\n${label} logs: unavailable\n`; return; }
    for (const name of steamLogs) await append(`${label} ${name}`, `${root}/logs/${name}`, limit);
  };
  // Managed import, old C: installs and shallow legacy Z:\\games imports.
  // Only a directory with a real steam.exe is accepted as a Windows client.
  const windowsRoots: string[] = [];
  const windows = (path: string): void => {
    if (windowsRoots.includes(path) || !diagnosticDirectory(path)) return;
    try {
      const exe = fs.lstatSync(`${path}/steam.exe`);
      if (exe.isFile() && !exe.isSymbolicLink()) windowsRoots.push(path);
    } catch (_) {}
  };
  if (downloadRoot && validAbsoluteDirectory(downloadRoot)) {
    windows(`${downloadRoot}/Steam`);
    for (const parent of [`${downloadRoot}/games`, `${downloadRoot}/games/games`]) {
      if (!diagnosticDirectory(parent)) continue;
      for (const name of diagnosticNames(parent, /^[^\/\\\x00-\x1f]{1,180}$/, 64)) {
        const path = `${parent}/${name}`;
        if (!diagnosticDirectory(path)) continue;
        await new Promise<void>((resolve): void => { setTimeout(resolve, 0); });
        windows(path); windows(`${path}/Steam`);
      }
    }
  }
  for (const prefix of prefixes) {
    if (!diagnosticDirectory(prefix) || !diagnosticDirectory(`${prefix}/drive_c`)) continue;
    for (const folder of ['Program Files (x86)', 'Program Files']) {
      if (diagnosticDirectory(`${prefix}/drive_c/${folder}`)) windows(`${prefix}/drive_c/${folder}/Steam`);
    }
  }
  windowsRoots.sort((a: string, b: string): number => {
    try { return fs.lstatSync(`${b}/logs`).mtime - fs.lstatSync(`${a}/logs`).mtime; } catch (_) { return 0; }
  });
  for (const root of windowsRoots.slice(0, 4)) await appendSteam(`Windows Steam ${root}`, root);
  if (windowsRoots.length > 4) text += '\nOlder Windows Steam clients omitted (maximum 4).\n';
  try {
    // Read the complete, short root marker; never interpret a truncated path.
    const root = readDiagnosticTail(`${BASE}/external-root.path`, 4096, true).trim();
    if (!root.startsWith('/') || !root.endsWith('/SteamLinuxARM64') || root.split('/').includes('..') ||
      /[\r\n\u0000]/.test(root)) throw new Error('invalid root');
    await appendSteam('Linux ARM64 Steam', `${root}/client/.steam/steam`, 131072);
    try {
      const config = readDiagnosticTail(`${root}/client/.steam/steam/config/config.vdf`, 1048576, true);
      text += '\n=== compatibility tool selection ===\n' +
        redactSteamDiagnostic(summarizeSteamToolSelection(config)) + '\n';
      found = true;
    } catch (_) { text += '\n=== compatibility tool selection: unavailable ===\n'; }
  } catch (_) { text += '\nSteam log root unavailable\n'; }
  text += `\nDiagnostic budget: ${REPORT_BYTES - remaining}/${REPORT_BYTES} bytes; max 4 Wine files per location, 8 DXVK files per prefix, 4 Windows clients.\n`;
  return found ? text : '';
}
