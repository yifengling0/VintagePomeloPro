import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { zlib } from '@kit.BasicServicesKit';
import { hilog } from './LogService';
import testNapi from 'libentry.so';
import {
  D3DRendererPreference,
  DisplayMode,
  EngineState,
  NativeLaunchResult,
  NativeProcessInfo,
  ScreenOrientationSetting,
  WineLanguage,
  WinePresentationMode,
  canTransitionEngineState,
  LaunchKind,
  resolveWinePresentationMode,
  box64PresetEnvString
} from '../model/AppModels';
import { AppSettingsStore } from './AppSettingsStore';
import { syncManagedSmoke } from './ManagedSmokePayloadService';
import { WineWindowManager } from './WineWindowManager';

const DOMAIN = 0x0000;
const TAG = 'WineEngine';
const FILES_BASE = '/data/storage/el2/base/files';
const WINE_ROOT = FILES_BASE + '/wine';
const WINE_BIN = WINE_ROOT + '/bin';
const WINE_LIB = WINE_BIN + '/x86_64-unix';
const BOX64 = WINE_BIN + '/box64';
const SOCK = FILES_BASE + '/.wine/wine-wayland';
const PREFIX_MARKER = WINE_ROOT + '/share/wine/wine.inf';
const PREVIOUS_PREFIX_MARKER = FILES_BASE + '/.winehua-wine.inf.previous';
const RUNTIME_VERSION_RAWFILE = 'wine_runtime.version';
const RUNTIME_VERSION_MARKER = WINE_ROOT + '/.winehua-runtime-version';
// Runtime extraction and prefix configuration are versioned independently.
// The user's prefix and Download games must remain intact across upgrades.
const PREFIX_RUNTIME_VERSION_MARKER = FILES_BASE + '/.wine/.winehua-prefix-runtime-version';
const PREFIX_SYSTEM_REG = FILES_BASE + '/.wine/system.reg';
// ArkTS 不再用固定阶段死线: native 协调者会发 phase:/fail:/wine-ready 事件,
// 等待只由事件结束。下面的安全网仅在"长时间完全无事件"时兜底, 正常不会触发。
const ENGINE_NO_EVENT_SAFETY_MS = 30 * 60 * 1000;
// 对齐 native drain 上限 (约 30s) + tsfn 余量。NCP 退出靠 appspawn 回调,
// 回调丢失时提前放行会让新会话与旧 drain 抢跑 (winehua d256317e)。
const NATIVE_SHUTDOWN_TIMEOUT_MS = 35000;
const NATIVE_SHUTDOWN_POLL_MS = 100;
const PREFIX_DIR = FILES_BASE + '/.wine';
const RESET_KEEP_DIR = '/data/storage/el2/base/cache/wine-reset-keep';
const PREFIX_USER_KEEP_FOLDERS: string[] = [
  'Documents', 'Desktop', 'Pictures', 'Music', 'Videos', 'Saved Games'
];
function executableUsesVkd3dGraphics(executable: string): boolean {
  const normalized = executable.toLowerCase();
  return normalized.indexOf('smoke') >= 0 &&
    (normalized.indexOf('gears.exe') >= 0 || normalized.indexOf('triangle.exe') >= 0);
}

interface RuntimePathRequirement {
  path: string;
  label: string;
  directory?: boolean;
}

interface FontSubstitute {
  name: string;
  replacement: string;
  // Wine's font installer canonicalizes a small set of legacy aliases after
  // startup. Keep them in wine.inf/direct migration, but do not make those
  // non-persistent aliases a perpetual runtime health failure.
  persistent?: boolean;
}

// Keep this list aligned with the OHOS FontSubstitutes block assembled into
// wine.inf. Fresh prefixes do not reliably retain every sans-serif mapping,
// while older prefixes predate the block entirely. Update only this registry
// section directly before Wine starts instead of running the slow wineboot
// --update.
// Device fonts register localized family names. HarmonyOS_Sans_SC.ttf is
// "鸿蒙黑体", not "HarmonyOS Sans SC". Noto CJK *.ttc also exposes JP first.
// Point SC replacements at the registered SC TTF so GBK games (宋体) do not
// pick the Japanese face. Keep the English alias for leftover lookups.
const MASTER_FONT_SC = '鸿蒙黑体';
const MASTER_FONT_SC_GB2312 = MASTER_FONT_SC + ',134';

function isAsciiHexDigit(code: number): boolean {
  return (code >= 0x30 && code <= 0x39) ||
    (code >= 0x41 && code <= 0x46) ||
    (code >= 0x61 && code <= 0x66);
}

// wineserver dumps non-ASCII as \xHH / \xHHHH; parse_strW also accepts UTF-8.
// Match the dumped form so prefixFontSubstitutesCurrent() stays true after Wine
// flushes system.reg.
function wineRegistryEscape(text: string): string {
  let out = '';
  const len = text.length;
  for (let i = 0; i < len; i++) {
    const code = text.charCodeAt(i);
    if (code > 127) {
      let hex = code.toString(16);
      const next = i + 1 < len ? text.charCodeAt(i + 1) : 0;
      if (next > 0 && next <= 127 && isAsciiHexDigit(next)) {
        while (hex.length < 4) {
          hex = '0' + hex;
        }
      }
      out += '\\x' + hex;
    } else {
      out += text.charAt(i);
    }
  }
  return out;
}

function escapeRegExp(text: string): string {
  return text.replace(/[\\^$.*+?()[\]{}|]/g, '\\$&');
}

function fontSubstituteRegistryLine(item: FontSubstitute): string {
  return '"' + wineRegistryEscape(item.name) + '"="' + wineRegistryEscape(item.replacement) + '"';
}

function fontSubstituteRegistryKeys(item: FontSubstitute): string[] {
  const escaped = wineRegistryEscape(item.name);
  if (escaped === item.name) {
    return [item.name];
  }
  return [escaped, item.name];
}

function fontSubstituteLinePresent(registry: string, item: FontSubstitute): boolean {
  const escapedLine = fontSubstituteRegistryLine(item);
  if (registry.includes(escapedLine)) {
    return true;
  }
  const utf8Line = '"' + item.name + '"="' + item.replacement + '"';
  return utf8Line !== escapedLine && registry.includes(utf8Line);
}

const MASTER_FONT_SUBSTITUTES: FontSubstitute[] = [
  { name: 'HarmonyOS Sans SC', replacement: MASTER_FONT_SC },
  { name: '宋体', replacement: MASTER_FONT_SC },
  { name: '新宋体', replacement: MASTER_FONT_SC },
  { name: '黑体', replacement: MASTER_FONT_SC },
  { name: '楷体', replacement: MASTER_FONT_SC },
  { name: '楷体_GB2312', replacement: MASTER_FONT_SC },
  { name: '仿宋', replacement: MASTER_FONT_SC },
  { name: '仿宋_GB2312', replacement: MASTER_FONT_SC },
  { name: '微软雅黑', replacement: MASTER_FONT_SC },
  { name: '萝莉体', replacement: MASTER_FONT_SC },
  { name: 'Lolita', replacement: MASTER_FONT_SC },
  { name: 'Noto Sans CJK KR', replacement: MASTER_FONT_SC },
  { name: 'Noto Serif CJK JP', replacement: MASTER_FONT_SC },
  { name: 'Noto Serif CJK KR', replacement: MASTER_FONT_SC },
  { name: '宋体,134', replacement: MASTER_FONT_SC_GB2312 },
  { name: '新宋体,134', replacement: MASTER_FONT_SC_GB2312 },
  { name: '黑体,134', replacement: MASTER_FONT_SC_GB2312 },
  { name: '微软雅黑,134', replacement: MASTER_FONT_SC_GB2312 },
  { name: '萝莉体,134', replacement: MASTER_FONT_SC_GB2312 },
  { name: 'Lolita,134', replacement: MASTER_FONT_SC_GB2312 },
  { name: 'System', replacement: MASTER_FONT_SC },
  { name: 'MS Sans Serif', replacement: MASTER_FONT_SC, persistent: false },
  { name: 'MS Shell Dlg', replacement: MASTER_FONT_SC },
  { name: 'MS Shell Dlg 2', replacement: MASTER_FONT_SC },
  { name: 'Arial', replacement: MASTER_FONT_SC },
  { name: 'Arial Black', replacement: MASTER_FONT_SC },
  { name: 'Calibri', replacement: MASTER_FONT_SC },
  { name: 'Cambria', replacement: 'Noto Serif' },
  { name: 'Candara', replacement: MASTER_FONT_SC },
  { name: 'Comic Sans MS', replacement: MASTER_FONT_SC },
  { name: 'Constantia', replacement: 'Noto Serif' },
  { name: 'Corbel', replacement: MASTER_FONT_SC },
  { name: 'Impact', replacement: MASTER_FONT_SC },
  { name: 'Palatino Linotype', replacement: 'Noto Serif' },
  { name: 'Segoe UI', replacement: MASTER_FONT_SC },
  { name: 'Tahoma', replacement: MASTER_FONT_SC, persistent: false },
  { name: 'Trebuchet MS', replacement: MASTER_FONT_SC },
  { name: 'Verdana', replacement: MASTER_FONT_SC },
  { name: 'Georgia', replacement: 'Noto Serif' },
  { name: 'Times New Roman', replacement: 'Noto Serif' },
  { name: 'Microsoft JhengHei', replacement: 'HarmonyOS Sans TC', persistent: false },
  { name: 'Microsoft JhengHei UI', replacement: 'HarmonyOS Sans TC', persistent: false },
  { name: 'MS Gothic', replacement: MASTER_FONT_SC },
  { name: 'MS PGothic', replacement: MASTER_FONT_SC },
  { name: 'MS UI Gothic', replacement: MASTER_FONT_SC },
  { name: 'Noto Sans CJK JP', replacement: MASTER_FONT_SC },
  { name: 'Microsoft YaHei', replacement: MASTER_FONT_SC },
  { name: 'Microsoft YaHei UI', replacement: MASTER_FONT_SC },
  { name: 'SimSun', replacement: MASTER_FONT_SC },
  { name: 'NSimSun', replacement: MASTER_FONT_SC },
  { name: 'SimSun,134', replacement: MASTER_FONT_SC_GB2312 },
  { name: 'NSimSun,134', replacement: MASTER_FONT_SC_GB2312 },
  { name: 'Microsoft YaHei,134', replacement: MASTER_FONT_SC_GB2312 },
  { name: 'SimHei', replacement: MASTER_FONT_SC },
  { name: 'FangSong', replacement: MASTER_FONT_SC },
  { name: 'KaiTi', replacement: MASTER_FONT_SC },
  { name: 'YouYuan', replacement: MASTER_FONT_SC },
  { name: 'LiSu', replacement: MASTER_FONT_SC },
  { name: 'DengXian', replacement: MASTER_FONT_SC },
  { name: 'STSong', replacement: MASTER_FONT_SC },
  { name: 'STKaiti', replacement: MASTER_FONT_SC },
  { name: 'STFangsong', replacement: MASTER_FONT_SC },
  { name: 'STHeiti', replacement: MASTER_FONT_SC },
  { name: 'STXihei', replacement: MASTER_FONT_SC },
  { name: 'STLiti', replacement: MASTER_FONT_SC },
  { name: 'STXingkai', replacement: MASTER_FONT_SC },
  { name: 'STXinwei', replacement: MASTER_FONT_SC },
  { name: 'STHupo', replacement: MASTER_FONT_SC },
  { name: 'STCaiyun', replacement: MASTER_FONT_SC },
  { name: 'STZhongSong', replacement: MASTER_FONT_SC },
  { name: 'STBaoli', replacement: MASTER_FONT_SC },
  { name: 'FZShuTi', replacement: MASTER_FONT_SC },
  { name: 'FZYaoti', replacement: MASTER_FONT_SC },
  { name: '汉仪魏碑', replacement: '汉仪魏碑简' },
  { name: '汉仪魏碑,134', replacement: '汉仪魏碑简,134' },
  { name: 'HYWeiBei', replacement: '汉仪魏碑简' },
  { name: 'hyh1gjm', replacement: '汉仪魏碑简' },
  { name: '汉仪中黑', replacement: '汉仪中黑 简' },
  { name: '汉仪中黑简', replacement: '汉仪中黑 简' },
  { name: '汉仪中黑,134', replacement: '汉仪中黑 简,134' },
  { name: 'hyb1gjm', replacement: '汉仪中黑 简' },
  { name: '汉仪柏青体', replacement: '汉仪柏青体简' },
  { name: '汉仪柏青简', replacement: '汉仪柏青体简' },
  { name: '华康少女体', replacement: '华康少女文字W5' },
  { name: 'hksnt', replacement: '华康少女文字W5' },
  { name: 'MingLiU', replacement: 'HarmonyOS Sans TC', persistent: false },
  { name: 'PMingLiU', replacement: 'HarmonyOS Sans TC', persistent: false },
  { name: 'DFKai-SB', replacement: 'Noto Serif CJK TC' },
  { name: 'Consolas', replacement: 'Noto Sans Mono' },
  { name: 'Courier', replacement: 'Noto Sans Mono', persistent: false },
  { name: 'Courier New', replacement: 'Noto Sans Mono' },
  { name: 'Fixedsys', replacement: 'Noto Sans Mono' },
  { name: 'Lucida Console', replacement: 'Noto Sans Mono' }
];

// Game TTFs registered via AddFontResourceEx must keep their own family names.
// Older prefixes mapped these to 鸿蒙黑体, which then served Hangul/kana at GBK
// code points. Strip leftover keys even when the master list is otherwise current.
// Real game TTF families must not be redirected to 鸿蒙黑体. Alias keys for
// slightly-wrong CreateFont names live in MASTER_FONT_SUBSTITUTES instead.
const OBSOLETE_GAME_FONT_SUBSTITUTES: string[] = [
  '汉仪魏碑简', 'HYWeiBeiJ',
  '汉仪中黑 简', 'HYZhongHeiJ', 'HYZhongHei',
  '汉仪柏青体简', 'HYBaiQingJ', 'HYBaiQing',
  '华康少女文字W5',
  '方正胖头鱼简体'
];

function fontSubstituteRegistryKey(line: string): string {
  const trimmed = line.trim();
  if (!trimmed.startsWith('"')) {
    return '';
  }
  const end = trimmed.indexOf('"=', 1);
  if (end < 0) {
    return '';
  }
  return trimmed.substring(1, end);
}

function unescapeWineRegistryKey(key: string): string {
  let out = '';
  for (let i = 0; i < key.length; i++) {
    if (key.charAt(i) === '\\' && i + 1 < key.length && key.charAt(i + 1) === 'x') {
      let hex = '';
      let j = i + 2;
      while (j < key.length && isAsciiHexDigit(key.charCodeAt(j)) && hex.length < 4) {
        hex += key.charAt(j);
        j++;
      }
      if (hex.length > 0) {
        out += String.fromCharCode(parseInt(hex, 16));
        i = j - 1;
        continue;
      }
    }
    out += key.charAt(i);
  }
  return out;
}

function isObsoleteGameFontSubstituteKey(key: string): boolean {
  const decoded = unescapeWineRegistryKey(key);
  const base = decoded.split(',')[0];
  return OBSOLETE_GAME_FONT_SUBSTITUTES.indexOf(base) >= 0 ||
    OBSOLETE_GAME_FONT_SUBSTITUTES.indexOf(decoded) >= 0;
}

function registryHasObsoleteGameFontSubstitutes(registry: string): boolean {
  const sectionHeader = '[Software\\\\Microsoft\\\\Windows NT\\\\CurrentVersion\\\\FontSubstitutes]';
  const sectionStart = registry.indexOf(sectionHeader);
  if (sectionStart < 0) return false;
  let sectionEnd = registry.indexOf('\n[', sectionStart + sectionHeader.length);
  if (sectionEnd < 0) sectionEnd = registry.length;
  const lines = registry.substring(sectionStart, sectionEnd).split('\n');
  for (let i = 0; i < lines.length; i++) {
    if (isObsoleteGameFontSubstituteKey(fontSubstituteRegistryKey(lines[i]))) {
      return true;
    }
  }
  return false;
}

const REQUIRED_RUNTIME_PATHS: RuntimePathRequirement[] = [
  { path: PREFIX_MARKER, label: 'share/wine/wine.inf' },
  { path: WINE_BIN + '/explorer.exe', label: 'bin/explorer.exe' },
  { path: WINE_BIN + '/Alarm01.wav', label: 'bin/Alarm01.wav' },
  { path: WINE_BIN + '/guest_gfx/winehua-guest-gfx.env', label: 'bin/guest_gfx/winehua-guest-gfx.env' },
  { path: WINE_BIN + '/guest_gfx/lib/libEGL.so', label: 'bin/guest_gfx/lib/libEGL.so' },
  { path: WINE_BIN + '/guest_gfx/lib/dri/virtio_gpu_dri.so', label: 'bin/guest_gfx/lib/dri/virtio_gpu_dri.so' },
  { path: WINE_BIN + '/guest_vulkan/lib/libvulkan.so.1', label: 'bin/guest_vulkan/lib/libvulkan.so.1' },
  { path: WINE_BIN + '/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json',
    label: 'bin/guest_vulkan/share/vulkan/icd.d/venus_icd.x86_64.json' },
  { path: WINE_ROOT + '/dxvk/manifest.json', label: 'dxvk/manifest.json' },
  { path: WINE_ROOT + '/dxvk/legacy/x64/d3d9.dll', label: 'dxvk/legacy/x64/d3d9.dll' },
  { path: WINE_ROOT + '/dxvk/legacy/x64/d3d10core.dll', label: 'dxvk/legacy/x64/d3d10core.dll' },
  { path: WINE_ROOT + '/dxvk/legacy/x64/d3d10.dll', label: 'dxvk/legacy/x64/d3d10.dll' },
  { path: WINE_ROOT + '/dxvk/legacy/x64/d3d10_1.dll', label: 'dxvk/legacy/x64/d3d10_1.dll' },
  { path: WINE_ROOT + '/dxvk/legacy/x64/d3d11.dll', label: 'dxvk/legacy/x64/d3d11.dll' },
  { path: WINE_ROOT + '/dxvk/legacy/x64/dxgi.dll', label: 'dxvk/legacy/x64/dxgi.dll' },
  { path: WINE_ROOT + '/dxvk/legacy/x86/d3d9.dll', label: 'dxvk/legacy/x86/d3d9.dll' },
  { path: WINE_ROOT + '/dxvk/legacy/x86/d3d10core.dll', label: 'dxvk/legacy/x86/d3d10core.dll' },
  { path: WINE_ROOT + '/dxvk/legacy/x86/d3d10.dll', label: 'dxvk/legacy/x86/d3d10.dll' },
  { path: WINE_ROOT + '/dxvk/legacy/x86/d3d10_1.dll', label: 'dxvk/legacy/x86/d3d10_1.dll' },
  { path: WINE_ROOT + '/dxvk/legacy/x86/d3d11.dll', label: 'dxvk/legacy/x86/d3d11.dll' },
  { path: WINE_ROOT + '/dxvk/legacy/x86/dxgi.dll', label: 'dxvk/legacy/x86/dxgi.dll' },
  { path: WINE_ROOT + '/smoke/manifest.json', label: 'smoke/manifest.json' },
  { path: WINE_ROOT + '/smoke/x64/winehua_d3d_switch_cube.exe',
    label: 'smoke/x64/winehua_d3d_switch_cube.exe' },
  { path: WINE_ROOT + '/smoke/x86/winehua_d3d_switch_cube.exe',
    label: 'smoke/x86/winehua_d3d_switch_cube.exe' },
  { path: WINE_BIN + '/x86_64-windows', label: 'bin/x86_64-windows', directory: true },
  { path: WINE_BIN + '/i386-windows', label: 'bin/i386-windows', directory: true }
];

export type EngineStateListener = (state: EngineState, message: string) => void;

function pathExists(path: string): boolean {
  try {
    return !!fs.accessSync(path);
  } catch (_) {
    return false;
  }
}

function pathMatchesRequirement(item: RuntimePathRequirement): boolean {
  if (!pathExists(item.path)) return false;
  if (!item.directory) return true;
  try {
    return fs.statSync(item.path).isDirectory();
  } catch (_) {
    return false;
  }
}

function isCommandScript(executable: string): boolean {
  const lower = executable.trim().toLowerCase();
  return lower.endsWith('.bat') || lower.endsWith('.cmd');
}

function isWineDesktopProgram(executable: string): boolean {
  const name = executable.trim().replace(/\\/g, '/').toLowerCase();
  const base = name.substring(name.lastIndexOf('/') + 1);
  return base === 'explorer' || base === 'explorer.exe';
}

function ensureDirectory(path: string): void {
  if (!pathExists(path)) {
    fs.mkdirSync(path, true);
  }
}

// Wine opens C:\windows\fonts in lowercase. OHOS paths are case-sensitive, so a
// user-imported C:\windows\Fonts directory is invisible to GDI and games that
// CreateFont() a packed face (萝莉体) get a random /system/fonts fallback.
const WINE_WINDOWS_DIR = PREFIX_DIR + '/drive_c/windows';
const WINE_FONTS_DIR = WINE_WINDOWS_DIR + '/fonts';
const WINE_FONTS_DIR_LEGACY = WINE_WINDOWS_DIR + '/Fonts';

function ensureWineFontsDirectory(): void {
  if (!pathExists(WINE_WINDOWS_DIR)) return;
  const hasLower = pathExists(WINE_FONTS_DIR);
  const hasUpper = pathExists(WINE_FONTS_DIR_LEGACY);
  if (hasUpper && !hasLower) {
    fs.renameSync(WINE_FONTS_DIR_LEGACY, WINE_FONTS_DIR);
    hilog.info(DOMAIN, TAG, 'renamed C:\\windows\\Fonts to fonts for case-sensitive Wine lookup');
    return;
  }
  if (!hasUpper || !hasLower) return;
  const names = fs.listFileSync(WINE_FONTS_DIR_LEGACY);
  for (let i = 0; i < names.length; i++) {
    const src = WINE_FONTS_DIR_LEGACY + '/' + names[i];
    const dst = WINE_FONTS_DIR + '/' + names[i];
    if (!pathExists(dst)) {
      fs.renameSync(src, dst);
    } else {
      try { fs.unlinkSync(src); } catch (_) {}
    }
  }
  try { fs.rmdirSync(WINE_FONTS_DIR_LEGACY); } catch (_) {}
  hilog.info(DOMAIN, TAG, 'merged legacy C:\\windows\\Fonts into fonts');
}

const WINE_SHARE_FONTS = WINE_ROOT + '/share/wine/fonts';

function importedFontShouldPublish(name: string): boolean {
  const lower = name.toLowerCase();
  if (lower === '1.ttf' || lower === '1.otf' || lower === '1.ttc') return true;
  if (lower.indexOf('simsun') >= 0 || lower.indexOf('simhei') >= 0 ||
    lower.indexOf('simkai') >= 0 || lower.indexOf('msyh') >= 0) return true;
  if (lower.indexOf('lolita') >= 0) return true;
  if (name.indexOf('萝莉') >= 0 || name.indexOf('安装字体') >= 0) return true;
  return false;
}

// Wine's unix NT path through $WINEPREFIX cannot see ".wine" (dotfiles hidden),
// and C:\\windows\\fonts directory enumeration does not surface imported TTFs.
// Copy the CJK faces this title needs into share/wine/fonts, which Wine already
// loads via \\??\\unix\\...\\wine\\share\\wine\\fonts (see marlett.ttf).
function copyImportedFontsIntoWineShare(): void {
  if (!pathExists(WINE_FONTS_DIR) || !pathExists(WINE_SHARE_FONTS)) return;
  const names = fs.listFileSync(WINE_FONTS_DIR);
  let copied = 0;
  for (let i = 0; i < names.length; i++) {
    const name = names[i];
    if (!importedFontShouldPublish(name)) continue;
    const src = WINE_FONTS_DIR + '/' + name;
    const dst = WINE_SHARE_FONTS + '/' + name;
    try {
      if (pathExists(dst) && fs.statSync(src).size === fs.statSync(dst).size) continue;
      copyRecursiveOrThrow(src, dst);
      copied++;
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'failed to publish imported font %{public}s: %{public}s',
        name, JSON.stringify(error));
    }
  }
  if (copied > 0) {
    hilog.info(DOMAIN, TAG, 'published %{public}d imported fonts into wine share/fonts', copied);
  }
}

function wineUnixNtRegPath(unixPath: string): string {
  return '\\\\??\\\\unix' + unixPath.split('/').join('\\\\');
}

interface ImportedFontFace {
  face: string;
  file: string;
}

const IMPORTED_FONT_FACES: ImportedFontFace[] = [
  { face: 'Lolita', file: '1.ttf' },
  { face: '萝莉体', file: '1.ttf' },
  { face: 'Microsoft YaHei', file: 'msyh.ttc' },
  { face: 'Microsoft YaHei UI', file: 'msyh.ttc' }
];

const FONTS_REGISTRY_SECTIONS: string[] = [
  '[Software\\\\Microsoft\\\\Windows NT\\\\CurrentVersion\\\\Fonts]',
  '[Software\\\\Wow6432Node\\\\Microsoft\\\\Windows NT\\\\CurrentVersion\\\\Fonts]',
  '[Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Fonts]'
];

function importedFontRegistryLine(face: ImportedFontFace): string {
  return '"' + wineRegistryEscape(face.face) + ' (TrueType)"="' +
    wineUnixNtRegPath(WINE_SHARE_FONTS + '/' + face.file) + '"';
}

function upsertRegistrySectionLine(registry: string, sectionHeader: string, line: string): string {
  const sectionStart = registry.indexOf(sectionHeader);
  if (sectionStart < 0) return registry;
  let sectionEnd = registry.indexOf('\n[', sectionStart + sectionHeader.length);
  if (sectionEnd < 0) sectionEnd = registry.length;
  const section = registry.substring(sectionStart, sectionEnd);
  if (section.indexOf(line) >= 0) return registry;
  const eq = line.indexOf('=');
  if (eq < 0) return registry;
  const keyRe = new RegExp('^' + escapeRegExp(line.substring(0, eq + 1)) + '.*$', 'm');
  const nextSection = keyRe.test(section)
    ? section.replace(keyRe, line)
    : section + (section.endsWith('\n') ? '' : '\n') + line + '\n';
  return registry.substring(0, sectionStart) + nextSection + registry.substring(sectionEnd);
}

// Directory enumeration of both C:\\windows\\fonts and share/wine/fonts misses
// newly copied TTFs. load_registry_fonts() still loads a full unix NT path, which
// is how marlett.ttf gets into GDI. Register 萝莉体 the same way.
function ensureImportedFontFacesRegistered(): void {
  if (!pathExists(PREFIX_SYSTEM_REG) || !pathExists(WINE_SHARE_FONTS)) return;
  let registry = fs.readTextSync(PREFIX_SYSTEM_REG);
  let changed = false;
  IMPORTED_FONT_FACES.forEach((face: ImportedFontFace) => {
    if (!pathExists(WINE_SHARE_FONTS + '/' + face.file)) return;
    const line = importedFontRegistryLine(face);
    FONTS_REGISTRY_SECTIONS.forEach((header: string) => {
      const updated = upsertRegistrySectionLine(registry, header, line);
      if (updated !== registry) {
        registry = updated;
        changed = true;
      }
    });
  });
  if (!changed) return;
  const temporary = PREFIX_SYSTEM_REG + '.winehua-fontfaces.tmp';
  const output = fs.openSync(temporary, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
  try {
    fs.writeSync(output.fd, registry);
  } finally {
    fs.closeSync(output);
  }
  fs.renameSync(temporary, PREFIX_SYSTEM_REG);
  hilog.info(DOMAIN, TAG, 'registered imported CJK faces in HKLM Fonts');
}

function isDirectory(path: string): boolean {
  try {
    return fs.statSync(path).isDirectory();
  } catch (_) {
    return false;
  }
}

function removeRecursive(path: string): void {
  if (!pathExists(path)) {
    return;
  }
  try {
    const stat = fs.lstatSync(path);
    if (stat.isSymbolicLink() || !stat.isDirectory()) {
      fs.unlinkSync(path);
      return;
    }
  } catch (_) {
    return;
  }
  try {
    const names = fs.listFileSync(path);
    for (let i = 0; i < names.length; i++) {
      removeRecursive(`${path}/${names[i]}`);
    }
  } catch (_) {}
  try {
    fs.rmdirSync(path);
  } catch (_) {}
}

function copyRecursiveOrThrow(source: string, target: string): void {
  const stat = fs.statSync(source);
  if (!stat.isDirectory()) {
    const slash = target.lastIndexOf('/');
    if (slash > 0) {
      ensureDirectory(target.substring(0, slash));
    }
    fs.copyFileSync(source, target);
    return;
  }
  ensureDirectory(target);
  const names = fs.listFileSync(source);
  for (let i = 0; i < names.length; i++) {
    copyRecursiveOrThrow(`${source}/${names[i]}`, `${target}/${names[i]}`);
  }
}

function backupPrefixUserData(keepDir: string): void {
  removeRecursive(keepDir);
  ensureDirectory(keepDir);
  if (!pathExists(PREFIX_DIR)) {
    return;
  }
  const regs: string[] = ['user.reg', 'system.reg', 'userdef.reg'];
  for (let i = 0; i < regs.length; i++) {
    const src = `${PREFIX_DIR}/${regs[i]}`;
    if (pathExists(src)) {
      copyRecursiveOrThrow(src, `${keepDir}/${regs[i]}`);
    }
  }
  const users = `${PREFIX_DIR}/drive_c/users`;
  if (!pathExists(users) || !isDirectory(users)) {
    return;
  }
  let names: string[] = [];
  try {
    names = fs.listFileSync(users);
  } catch (_) {
    return;
  }
  for (let u = 0; u < names.length; u++) {
    const userDir = `${users}/${names[u]}`;
    if (!isDirectory(userDir)) {
      continue;
    }
    for (let f = 0; f < PREFIX_USER_KEEP_FOLDERS.length; f++) {
      const folder = PREFIX_USER_KEEP_FOLDERS[f];
      const src = `${userDir}/${folder}`;
      if (pathExists(src)) {
        copyRecursiveOrThrow(src, `${keepDir}/users/${names[u]}/${folder}`);
      }
    }
  }
}

function restorePrefixUserData(keepDir: string): void {
  if (!pathExists(keepDir)) {
    return;
  }
  ensureDirectory(PREFIX_DIR);
  const regs: string[] = ['user.reg', 'system.reg', 'userdef.reg'];
  for (let i = 0; i < regs.length; i++) {
    const src = `${keepDir}/${regs[i]}`;
    if (pathExists(src)) {
      copyRecursiveOrThrow(src, `${PREFIX_DIR}/${regs[i]}`);
    }
  }
  const usersKeep = `${keepDir}/users`;
  if (!pathExists(usersKeep) || !isDirectory(usersKeep)) {
    return;
  }
  let names: string[] = [];
  try {
    names = fs.listFileSync(usersKeep);
  } catch (_) {
    return;
  }
  for (let u = 0; u < names.length; u++) {
    const userDir = `${usersKeep}/${names[u]}`;
    if (!isDirectory(userDir)) {
      continue;
    }
    for (let f = 0; f < PREFIX_USER_KEEP_FOLDERS.length; f++) {
      const folder = PREFIX_USER_KEEP_FOLDERS[f];
      const src = `${userDir}/${folder}`;
      if (pathExists(src)) {
        copyRecursiveOrThrow(src, `${PREFIX_DIR}/drive_c/users/${names[u]}/${folder}`);
      }
    }
  }
}

export class WineEngineService {
  private static instance: WineEngineService | null = null;
  private context: common.UIAbilityContext | null = null;
  private state: EngineState = EngineState.STOPPED;
  private message: string = 'Wine 引擎未启动';
  private mode: DisplayMode | null = null;
  private presentationMode: WinePresentationMode | null = null;
  private homeDirectory: string = '';
  private listeners: Set<EngineStateListener> = new Set<EngineStateListener>();
  private waiters: Array<(ready: boolean) => void> = [];
  private callbackInstalled: boolean = false;
  private restartPromise: Promise<boolean> | null = null;
  private prefixRefreshRequired: boolean = false;
  private pendingPrefixVersion: Uint8Array = new Uint8Array(0);
  private d3dBackend: string = 'dxvk_legacy';
  // Broker/Spawner prefix is session-scoped; smoke clean must switch the whole
  // engine session rather than only overriding one child process environment.
  private activePrefixMode: string = 'reuse';
  private activeWineLanguage: WineLanguage = WineLanguage.CHINESE;
  private hostGraphicsBackend: string = 'dxvk_legacy';
  // Product sessions derive Host and Guest from the same D3D backend. Only
  // the qualified VKD3D smoke path owns an explicit one-session override.
  private hostGraphicsBackendOverride: string = '';
  private appliedHostGraphicsBackend: string = 'dxvk_legacy';
  private hostGraphicsLabExperiment: string = '';
  private appliedHostGraphicsLabExperiment: string = '';
  private enginePhase: string = '';
  private phaseStartedAt: number = 0;
  private readySafetyTimer: number = -1;
  // Want 自动化 (winehua.autoStart / winehua.program): 挂在本服务, 不引入 WineEnvService
  autoStartPending: boolean = false;
  private autoProgram: string = '';

  static getInstance(): WineEngineService {
    if (!WineEngineService.instance) WineEngineService.instance = new WineEngineService();
    return WineEngineService.instance;
  }

  initialize(context: common.UIAbilityContext): void {
    this.context = context;
    if (this.callbackInstalled) return;
    this.callbackInstalled = true;
    testNapi.setStateCallback((raw: string) => this.handleNativeState(raw));
  }

  setAutoProgram(v: string): void {
    this.autoProgram = v;
  }

  getAutoProgram(): string {
    return this.autoProgram;
  }

  consumeAutoProgram(): string {
    const p = this.autoProgram;
    this.autoProgram = '';
    return p;
  }

  consumeAutoStart(): boolean {
    if (!this.autoStartPending) return false;
    this.autoStartPending = false;
    return true;
  }

  /**
   * Want 自动化: 按当前 applyModePolicy 拉起引擎 (autoStart / program 共用)。
   */
  async ensureEngineReady(): Promise<boolean> {
    if (!this.context) return false;
    const settings = AppSettingsStore.getInstance().getGlobalSettings();
    const home = this.homeDirectory.length > 0 ? this.homeDirectory :
      '/storage/Users/currentUser/Download/com.vintage.pomelopro';
    WineWindowManager.getInstance().applyModePolicy();
    return this.ensureReady(settings.defaultDisplayMode,
      WineWindowManager.getInstance().presentationMode, home);
  }

  getState(): EngineState {
    return this.state;
  }

  getMessage(): string {
    return this.message;
  }

  getPhase(): string {
    return this.enginePhase;
  }

  getPhaseText(): string {
    return this.enginePhase ? this.phaseText(this.enginePhase) : '';
  }

  getPhaseElapsedMs(): number {
    return this.enginePhase ? Date.now() - this.phaseStartedAt : 0;
  }

  private phaseText(phase: string): string {
    switch (phase) {
      case 'graphics': return '准备图形栈';
      case 'wineserver': return '启动 Wine 服务';
      case 'wineboot': return '初始化 Wine 环境';
      case 'explorer': return '启动桌面';
      case 'ready': return '就绪';
      default: return phase;
    }
  }

  private failReasonText(reason: string): string {
    switch (reason) {
      case 'graphics-unavailable': return '图形栈未就绪';
      case 'wineserver-spawn': return 'Wine 服务启动失败';
      case 'wineserver-died': return 'Wine 服务退出';
      case 'wineboot-spawn': return 'Wine 环境初始化启动失败';
      case 'wineboot-no-progress': return 'Wine 环境初始化无进展';
      case 'wineboot-cap': return 'Wine 环境初始化超时';
      case 'wineboot-failed': return 'Wine 环境初始化失败';
      case 'prefix-prepare': return '运行环境准备失败';
      case 'explorer-spawn': return '桌面启动失败';
      case 'explorer-died': return '桌面进程退出';
      case 'explorer-root-timeout': return '桌面就绪超时';
      default: return reason;
    }
  }

  getMode(): DisplayMode | null {
    return this.mode;
  }

  getPresentationMode(): WinePresentationMode | null {
    return this.presentationMode;
  }

  isReady(): boolean {
    return this.state === EngineState.READY;
  }

  /** Native runWineProgram compatibility: expose the applied product backend
   * without duplicating the backend-selection policy in smoke/UI callers. */
  getD3dBackend(): string {
    return this.d3dBackend;
  }

  subscribe(listener: EngineStateListener): void {
    this.listeners.add(listener);
    listener(this.state, this.message);
  }

  unsubscribe(listener: EngineStateListener): void {
    this.listeners.delete(listener);
  }

  /**
   * 会话生命周期三原语（对齐 WineHua d256317e 命名；行为保持本仓既有路径）。
   * startSession = 幂等拉起引擎。首启/二启仍由 native prefix 状态决定，
   * 不上游 WineEnvService、不引入前台 overlay / opBusy。
   * 产品入口 ensureReady 转发到本方法，AppSessionService 无需改调用方。
   */
  async startSession(mode: DisplayMode, presentationMode: WinePresentationMode,
    homeDirectory: string, prefixMode: string = 'reuse'): Promise<boolean> {
    if (!this.context) return false;
    const requestedPrefixMode = prefixMode === 'clean' ? 'clean' : 'reuse';
    const requestedWineLanguage = AppSettingsStore.getInstance().getGlobalSettings().wineLanguage;
    const requestedD3DBackend = this.selectedD3DBackend();
    this.hostGraphicsBackend = this.hostGraphicsBackendOverride.length > 0 ?
      this.hostGraphicsBackendOverride : requestedD3DBackend;
    if (this.state === EngineState.READY && this.mode === mode &&
      this.presentationMode === presentationMode && this.d3dBackend === requestedD3DBackend &&
      this.activePrefixMode === requestedPrefixMode &&
      this.activeWineLanguage === requestedWineLanguage &&
      this.appliedHostGraphicsBackend === this.hostGraphicsBackend &&
      this.appliedHostGraphicsLabExperiment === this.hostGraphicsLabExperiment) {
      this.syncManagedSmokePayload('ready-reuse');
      return true;
    }
    if (this.state === EngineState.PREPARING && this.mode === mode &&
      this.presentationMode === presentationMode &&
      this.d3dBackend === requestedD3DBackend &&
      this.activePrefixMode === requestedPrefixMode &&
      this.activeWineLanguage === requestedWineLanguage &&
      this.appliedHostGraphicsBackend === this.hostGraphicsBackend &&
      this.appliedHostGraphicsLabExperiment === this.hostGraphicsLabExperiment) return this.waitForReady();

    if (this.state === EngineState.ERROR) {
      // native 进程仍存活说明协调者可能还在推进 (例如之前 ArkTS 侧瞬态失败):
      // 不重启 server, 只重新进入等待, 避免双服务竞态。
      let processes: NativeProcessInfo[] = [];
      try { processes = testNapi.getProcessList() as NativeProcessInfo[]; } catch (_) {}
      if (processes.length > 0) {
        this.setState(EngineState.PREPARING, '正在等待引擎恢复');
        return this.waitForReady();
      }
    }

    if (this.state !== EngineState.STOPPED &&
      (this.mode !== mode || this.presentationMode !== presentationMode ||
       this.d3dBackend !== requestedD3DBackend ||
       this.activePrefixMode !== requestedPrefixMode ||
       this.activeWineLanguage !== requestedWineLanguage ||
       this.appliedHostGraphicsBackend !== this.hostGraphicsBackend ||
       this.appliedHostGraphicsLabExperiment !== this.hostGraphicsLabExperiment)) {
      const switchingHost = (this.appliedHostGraphicsBackend !== this.hostGraphicsBackend ||
        this.appliedHostGraphicsLabExperiment !== this.hostGraphicsLabExperiment) &&
        this.mode === mode && this.presentationMode === presentationMode &&
        this.d3dBackend === requestedD3DBackend;
      this.setState(EngineState.SWITCHING,
        switchingHost ? '正在切换图形主机配置' :
        (this.d3dBackend !== requestedD3DBackend ? '正在切换 Direct3D 渲染策略' : '正在切换显示模式'));
      await this.stopSession();
    }

    this.mode = mode;
    this.presentationMode = presentationMode;
    this.d3dBackend = requestedD3DBackend;
    this.activePrefixMode = requestedPrefixMode;
    this.activeWineLanguage = requestedWineLanguage;
    this.homeDirectory = homeDirectory;
    this.setState(EngineState.PREPARING, '正在准备 Wine 引擎');

    try {
      const settings = AppSettingsStore.getInstance().getGlobalSettings();
      const bundledRuntimeVersion = this.getBundledRuntimeVersion();
      const prefixRefreshRequired = await this.ensureRuntimeInstalled(bundledRuntimeVersion);
      this.prefixRefreshRequired = prefixRefreshRequired;
      this.pendingPrefixVersion = prefixRefreshRequired ? bundledRuntimeVersion : new Uint8Array(0);
      // 桌面模式按最新设置统一判定 (phone 恒虚拟; tablet 默虚拟可切多窗口; PC 按设置)。
      // 切换后重启引擎即生效 — applyModePolicy 内部已同步 native setDesktopMode。
      WineWindowManager.getInstance().applyModePolicy();
      this.presentationMode = WineWindowManager.getInstance().presentationMode;
      if (this.presentationMode !== WinePresentationMode.MANAGED_WINDOWS) {
        const orientation = settings.desktopOrientation ?? ScreenOrientationSetting.SENSOR_LANDSCAPE;
        // This must happen before startServer/launchClient. A later ArkUI
        // window resize can correct the surface, but cannot prevent the first
        // Wayland desktop frame from being created at the old orientation.
        WineWindowManager.getInstance().prepareOutputForOrientation(orientation);
      }
      // Resolve the backend intent into one Native-owned Host/Guest route
      // before VirGL starts. ArkTS no longer duplicates shadow/upload flags.
      try {
        const policyApplied = this.hostGraphicsLabExperiment.length > 0 ?
          (testNapi.setHostGraphicsExperimentForLab(
            this.hostGraphicsLabExperiment, this.hostGraphicsBackend) as boolean) :
          (testNapi.setHostGraphicsBackend(this.hostGraphicsBackend) as boolean);
        if (!policyApplied) {
          this.setState(EngineState.ERROR, '图形配置无效');
          this.finishWaiters(false);
          return false;
        }
        this.appliedHostGraphicsBackend = this.hostGraphicsBackend;
        this.appliedHostGraphicsLabExperiment = this.hostGraphicsLabExperiment;
      } catch (error) {
        hilog.error(DOMAIN, TAG,
          'graphics policy resolve failed backend=%{public}s labExperiment=%{public}s error=%{public}s',
          this.hostGraphicsBackend, this.hostGraphicsLabExperiment, JSON.stringify(error));
        this.setState(EngineState.ERROR, '图形配置解析失败');
        this.finishWaiters(false);
        return false;
      }
      const serverReady = testNapi.startServer(SOCK) as boolean;
      if (!serverReady) {
        this.setState(EngineState.ERROR, 'Wayland 服务启动失败');
        this.finishWaiters(false);
        return false;
      }
      // Native runs wineboot --init only for a new prefix. Existing prefixes
      // are migrated above before any wineserver process starts.
      testNapi.launchClient(BOX64, [], SOCK, WINE_BIN + ':' + WINE_LIB,
        homeDirectory, requestedPrefixMode === 'clean', requestedPrefixMode, this.d3dBackend,
        box64PresetEnvString(settings.box64Preset), this.activeWineLanguage);
      return this.waitForReady();
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'ensureReady failed: %{public}s', JSON.stringify(error));
      this.setState(EngineState.ERROR, 'Wine 引擎准备失败');
      this.finishWaiters(false);
      return false;
    }
  }

  async ensureReady(mode: DisplayMode, presentationMode: WinePresentationMode,
    homeDirectory: string): Promise<boolean> {
    return this.startSession(mode, presentationMode, homeDirectory, 'reuse');
  }

  /**
   * Prepare the smoke session through the same engine owner. Reuse stays on
   * the product prefix. Clean first completes any product runtime migration,
   * then stops the broker and boots an isolated .wine-smoke session.
   */
  async ensureSmokeReady(mode: DisplayMode, presentationMode: WinePresentationMode,
    homeDirectory: string, prefixMode: string): Promise<boolean> {
    const productReady = await this.ensureReady(mode, presentationMode, homeDirectory);
    if (!productReady || prefixMode !== 'clean') return productReady;
    await this.stopSession();
    let reset = false;
    try { reset = testNapi.resetWinePrefix('clean') as boolean; } catch (_) {}
    if (!reset) {
      this.setState(EngineState.ERROR, '隔离 smoke 环境清理失败');
      return false;
    }
    return this.startSession(mode, presentationMode, homeDirectory, 'clean');
  }

  /** Restore the normal product prefix after an isolated clean smoke run. */
  async restoreProductSession(): Promise<boolean> {
    if (this.activePrefixMode !== 'clean') return true;
    const mode = this.mode;
    const presentationMode = this.presentationMode;
    const homeDirectory = this.homeDirectory;
    if (mode === null || presentationMode === null || homeDirectory.length === 0) return false;
    await this.stopSession();
    return this.startSession(mode, presentationMode, homeDirectory, 'reuse');
  }

  launchExecutable(executable: string, argumentsValue: string[] = [],
    workingDirectory: string = '', envOverrides: string[] = []): NativeLaunchResult {
    if (this.state !== EngineState.READY) {
      hilog.warn(DOMAIN, '[ENG]', 'launchExecutable skipped exe=%{public}s state=%{public}s',
        executable, this.state);
      return { pid: -1, sessionId: '', reused: false };
    }
    if (executable.trim().toLowerCase().startsWith('c:\\smoke\\') &&
      !this.syncManagedSmokePayload('managed-smoke-launch')) {
      return { pid: -1, sessionId: '', reused: false };
    }
    // Batch files need Wine's command interpreter. This still uses the exact
    // same runWineExe -> process broker -> BuildWineEnv path as explorer.exe;
    // only Wine's program argument changes, so GL/VirGL and runtime variables
    // cannot diverge from a normal Explorer-launched Windows application.
    const program = isCommandScript(executable) ? 'cmd.exe' : executable;
    const launchArguments = isCommandScript(executable) ?
      ['/c', executable].concat(argumentsValue) : argumentsValue;
    // Carry the already-applied LAB id into the child launch. Native resolves
    // the Guest half from the same policy authority that configured Host;
    // product sessions add no experiment override.
    const launchEnvironment = envOverrides.slice();
    // 旧 Windows 游戏（梦幻群侠传/XYQ 系）用 GDI 把文字画进位图后再二值化成字形
    // 遮罩：启动器.exe 0x4a5b40 只把 > 0x7ffe 的像素当笔画。灰度抗锯齿会把 12px
    // 汉字笔画变成灰色，于是被当成透明，游戏内文字只剩零散笔画碎片，看起来像乱码。
    // 游戏进程关掉 GDI 文字抗锯齿，桌面 shell 保持默认外观。
    const fontAntialiasOverridden = launchEnvironment.some((entry: string) =>
      entry.startsWith('WINEHUA_FONT_AA='));
    if (!isWineDesktopProgram(program) && !fontAntialiasOverridden) {
      launchEnvironment.push('WINEHUA_FONT_AA=bitmap');
    }
    if (this.appliedHostGraphicsLabExperiment.length > 0) {
      launchEnvironment.push(
        `WINEHUA_GRAPHICS_PROFILE=${this.appliedHostGraphicsLabExperiment}`);
    }
    return testNapi.runWineExe(
      WINE_BIN,
      SOCK,
      WINE_BIN + ':' + WINE_LIB,
      program,
      this.homeDirectory,
      launchArguments,
      workingDirectory,
      this.d3dBackend,
      launchEnvironment,
      this.activeWineLanguage
    ) as NativeLaunchResult;
  }

  private selectedD3DBackend(): string {
    const stored = AppSettingsStore.getInstance().getRenderMode();
    const mode = stored.length > 0 ? stored :
      (AppStorage.get<string>('winehua.render.mode') ?? 'dxvk_1_10');
    let backend = 'dxvk_legacy';
    if (mode === 'virgl') {
      backend = 'wined3d';
    } else if (mode === 'dxvk_2_6' && this.gpuSupportsDxvk26()) {
      backend = 'dxvk_modern_2_6';
    }
    AppStorage.setOrCreate<string>('winehua.render.resolved', backend);
    try {
      const gpu = (testNapi.getHostGpuName() as string) ?? '';
      hilog.info(DOMAIN, TAG, 'render adapt mode=%{public}s gpu=%{public}s backend=%{public}s',
        mode, gpu, backend);
    } catch (_) {
      hilog.info(DOMAIN, TAG, 'render adapt mode=%{public}s backend=%{public}s', mode, backend);
    }
    return backend;
  }

  /** GPU 能力判定: 马良 (Mali-Gxxx) 920 以上支持 DXVK 2.6。 */
  private gpuSupportsDxvk26(): boolean {
    try {
      const name = (testNapi.getHostGpuName() as string) ?? '';
      // 马良 GPU: "Maleoon 910" / "Mali-G920" 等。920 以下不支持 DXVK2.6。
      const m = /(?:Maleoon|Mali-?G)\s*(\d+)/i.exec(name);
      if (m) return parseInt(m[1], 10) >= 920;
      return true;
    } catch (_) {
      return true;
    }
  }

  selectHostGraphicsBackendForExecutable(executable: string): string {
    // A normal product launch owns the next engine policy. LAB overrides are
    // scoped to the explicit automation session that selected them.
    this.hostGraphicsLabExperiment = '';
    this.hostGraphicsBackendOverride = executableUsesVkd3dGraphics(executable) ?
      'vkd3d_limited_500k' : '';
    this.hostGraphicsBackend = this.hostGraphicsBackendOverride.length > 0 ?
      this.hostGraphicsBackendOverride : this.selectedD3DBackend();
    return this.hostGraphicsBackend;
  }

  selectHostGraphicsExperimentForLab(experimentId: string): boolean {
    const selected = experimentId.trim();
    if (selected.length === 0) return false;
    try {
      // Validate through the same Native resolver used immediately before
      // Host startup. This also fails closed before an automation run waits
      // for a profile that can never be applied.
      const valid = testNapi.setHostGraphicsExperimentForLab(
        selected, this.hostGraphicsBackend) as boolean;
      if (!valid) return false;
      this.hostGraphicsLabExperiment = selected;
      return true;
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'LAB graphics experiment rejected id=%{public}s error=%{public}s',
        selected, JSON.stringify(error));
      return false;
    }
  }

  needsHostGraphicsRestart(): boolean {
    return this.appliedHostGraphicsBackend !== this.hostGraphicsBackend ||
      this.appliedHostGraphicsLabExperiment !== this.hostGraphicsLabExperiment;
  }

  /**
   * Desktop root 被用户关闭后，只重启 Wine explorer shell。
   * 不调用 stopSession，因而不会中断桌面模式中仍在运行的其它 Windows 程序。
   */
  restartDesktopShell(): NativeLaunchResult {
    if (this.state !== EngineState.READY ||
      this.presentationMode !== WinePresentationMode.DESKTOP) {
      return { pid: -1, sessionId: '', reused: false };
    }
    // Initial desktop startup uses Wine's `explorer` program name.  Reusing
    // that same form is essential after users close the shell; `explorer.exe`
    // is not guaranteed to resolve from a reconstructed Wayland session.
    return this.launchExecutable('explorer', [
      WineWindowManager.getInstance().getDesktopShellArgument()
    ]);
  }

  /**
   * 停止全部登记进程并等待 native drain。无论当前是否 READY 都执行：
   * 无会话时主 wineserver 仍可能常驻，跳过停止会在活引擎脚下 wipe。
   * 产品入口 stopAll 转发到本方法。NAPI 仍是 testNapi.stopAll。
   */
  async stopSession(): Promise<void> {
    try {
      testNapi.stopAll();
    } catch (_) {
    }
    this.finishWaiters(false);
    await this.waitForNativeShutdown();
    this.mode = null;
    this.presentationMode = null;
    this.activePrefixMode = 'reuse';
    this.setState(EngineState.STOPPED, 'Wine 引擎已停止');
    // Leave a small socket-settle window after the process list becomes empty.
    // Restarting the Wayland server immediately can otherwise race the old
    // wine-wayland socket teardown on slower ARM tablets.
    await new Promise<void>((resolve: () => void) => setTimeout(resolve, 300));
  }

  async stopAll(): Promise<void> {
    return this.stopSession();
  }

  /**
   * 重启 = stopSession + startSession。不删 prefix。
   * Concurrent UI requests share one operation so two taps cannot start competing servers.
   */
  async restart(mode: DisplayMode, presentationMode: WinePresentationMode,
    homeDirectory: string): Promise<boolean> {
    if (this.restartPromise) return this.restartPromise;
    const operation = this.performRestart(mode, presentationMode, homeDirectory);
    this.restartPromise = operation;
    const ready = await operation;
    if (this.restartPromise === operation) this.restartPromise = null;
    return ready;
  }

  private async performRestart(mode: DisplayMode, presentationMode: WinePresentationMode,
    homeDirectory: string): Promise<boolean> {
    if (this.state !== EngineState.STOPPED) {
      this.setState(EngineState.SWITCHING, '正在重新启动 Wine 引擎');
    }
    await this.stopSession();
    return this.startSession(mode, presentationMode, homeDirectory);
  }

  private async waitForNativeShutdown(): Promise<void> {
    const deadline = Date.now() + NATIVE_SHUTDOWN_TIMEOUT_MS;
    while (Date.now() < deadline) {
      let processes: NativeProcessInfo[] = [];
      try { processes = testNapi.getProcessList() as NativeProcessInfo[]; } catch (_) {}
      if (processes.length === 0) return;
      await new Promise<void>((resolve: () => void) => setTimeout(resolve, NATIVE_SHUTDOWN_POLL_MS));
    }
    hilog.warn(DOMAIN, TAG, 'native process list did not settle before restart timeout');
  }

  async switchDisplaySession(): Promise<void> {
    if (this.state === EngineState.READY || this.state === EngineState.PREPARING) {
      this.setState(EngineState.SWITCHING, '正在切换显示模式');
    }
    await this.stopSession();
  }

  /**
   * 清空 Wine prefix 并重装 bundled runtime。调用前必须已经 stopSession，
   * 否则活着的 wineserver 可能把内存 registry 回写进新 prefix。
   * 本仓不清 WINE_ROOT 整树（forceRefreshRuntime 解绑版本标记后按缺失路径重装）。
   */
  async wipeEnvironment(): Promise<void> {
    testNapi.resetWinePrefix();
    await this.forceRefreshRuntime();
  }

  /**
   * 重置 = stopSession + wipeEnvironment + startSession。
   * 文档/注册表备份还原是本仓产品逻辑，包在 wipe 前后，不改变三原语语义。
   */
  async resetPrefix(preserveUserData?: boolean, homeDirectory?: string): Promise<void> {
    const preserve = preserveUserData ??
      AppSettingsStore.getInstance().getGlobalSettings().preserveUserDataOnReset;
    const home = homeDirectory ?? '';
    await this.stopSession();
    if (preserve) {
      try {
        backupPrefixUserData(RESET_KEEP_DIR);
      } catch (error) {
        hilog.error(DOMAIN, TAG, 'prefix user-data backup failed: %{public}s', JSON.stringify(error));
        this.setState(EngineState.ERROR, '备份文档或注册表失败，已中止重置');
        throw new Error('备份文档或注册表失败: ' + JSON.stringify(error));
      }
    }
    await this.wipeEnvironment();
    if (preserve) {
      try {
        restorePrefixUserData(RESET_KEEP_DIR);
        removeRecursive(RESET_KEEP_DIR);
      } catch (error) {
        hilog.error(DOMAIN, TAG, 'prefix user-data restore failed: %{public}s', JSON.stringify(error));
        this.setState(EngineState.ERROR, '还原文档或注册表失败，备份仍在 cache/wine-reset-keep');
        throw new Error('还原文档或注册表失败: ' + JSON.stringify(error));
      }
    } else {
      removeRecursive(RESET_KEEP_DIR);
    }
    if (home.length > 0) {
      const settings = AppSettingsStore.getInstance().getGlobalSettings();
      const presentation = resolveWinePresentationMode(
        LaunchKind.WINE_EXE, settings.defaultDisplayMode,
        AppSettingsStore.getInstance().usesManagedWineWindows());
      await this.startSession(settings.defaultDisplayMode, presentation, home);
      return;
    }
    this.setState(EngineState.STOPPED, 'Wine 环境已重置');
  }

  private async forceRefreshRuntime(): Promise<void> {
    try { fs.unlinkSync(RUNTIME_VERSION_MARKER); } catch (_) {}
    try { fs.unlinkSync(PREFIX_RUNTIME_VERSION_MARKER); } catch (_) {}
    const bundled = this.getBundledRuntimeVersion();
    await this.ensureRuntimeInstalled(bundled);
  }

  private waitForReady(): Promise<boolean> {
    if (this.state === EngineState.READY) return Promise.resolve(true);
    if (this.state === EngineState.ERROR) return Promise.resolve(false);
    this.resetReadySafetyNet();
    return new Promise<boolean>((resolve: (ready: boolean) => void) => {
      let completed = false;
      const finish = (ready: boolean): void => {
        if (completed) return;
        completed = true;
        resolve(ready);
      };
      this.waiters.push(finish);
    });
  }

  private resetReadySafetyNet(): void {
    if (this.readySafetyTimer >= 0) {
      clearTimeout(this.readySafetyTimer);
      this.readySafetyTimer = -1;
    }
    if (this.state === EngineState.PREPARING || this.state === EngineState.SWITCHING) {
      this.readySafetyTimer = setTimeout(() => {
        this.readySafetyTimer = -1;
        if (this.state === EngineState.PREPARING || this.state === EngineState.SWITCHING) {
          this.setState(EngineState.ERROR, '引擎长时间无响应，请重试');
          this.finishWaiters(false);
        }
      }, ENGINE_NO_EVENT_SAFETY_MS);
    }
  }

  private finishWaiters(ready: boolean): void {
    const pending = this.waiters;
    this.waiters = [];
    pending.forEach((resolve: (ready: boolean) => void) => resolve(ready));
  }

  private handleNativeState(raw: string): void {
    if (raw.startsWith('phase:')) {
      this.handleEnginePhase(raw.substring('phase:'.length));
      return;
    }
    if (raw.startsWith('fail:')) {
      const reason = raw.substring('fail:'.length);
      hilog.error(DOMAIN, TAG, 'engine failed: %{public}s', reason);
      this.setState(EngineState.ERROR, 'Wine 启动失败：' + this.failReasonText(reason));
      this.finishWaiters(false);
      return;
    }
    const separator = raw.indexOf(':');
    const processScoped = separator > 0;
    const value = separator >= 0 ? raw.substring(separator + 1) : raw;
    if (value === 'wine-ready' || value === 'wine-running') {
      const becomingReady = this.state !== EngineState.READY;
      if (value === 'wine-ready' && this.prefixRefreshRequired) {
        // Do not claim a refreshed prefix until native wineboot completed and
        // the marker is durable. A failed write deliberately leaves the
        // marker stale so the next launch retries the refresh.
        if (!this.writeVersionMarker(PREFIX_RUNTIME_VERSION_MARKER, this.pendingPrefixVersion)) {
          this.setState(EngineState.ERROR, 'Wine 环境配置结果保存失败');
          this.finishWaiters(false);
          return;
        }
        this.prefixRefreshRequired = false;
        this.pendingPrefixVersion = new Uint8Array(0);
      }
      if (becomingReady) this.syncManagedSmokePayload('engine-ready');
      this.setState(EngineState.READY, value === 'wine-running' ? 'Windows 程序运行中' : 'Wine 引擎已就绪');
      this.finishWaiters(true);
    } else if (value === 'wineserver-starting' || value === 'wineboot-starting') {
      this.setState(EngineState.PREPARING, value === 'wineboot-starting' ? '正在初始化 Wine 环境' : '正在启动 Wine 服务');
    } else if (value === 'wine-failed' && processScoped && this.state === EngineState.READY) {
      // runWineExe reports a process-scoped failure as "-1:wine-failed".
      // The engine and Wayland server are still usable; AppSessionService
      // already rejects the failed NativeLaunchResult for that one card.
      hilog.warn(DOMAIN, TAG, 'Windows program launch failed while engine remains ready: %{public}s', raw);
    } else if (value === 'wine-failed' || value === 'wineboot-failed' || value === 'wineserver-failed') {
      this.setState(EngineState.ERROR, 'Wine 启动失败');
      this.finishWaiters(false);
    } else if (value === 'exited' && (testNapi.getProcessList() as NativeProcessInfo[]).length === 0) {
      if (this.state === EngineState.PREPARING || this.state === EngineState.SWITCHING) {
        // appspawn wrapper processes may exit while wineboot/services continue
        // in broker-spawned children. The native launch coordinator emits an
        // explicit ready/failure result; the no-event safety net remains the
        // bounded fallback when that result never arrives.
        hilog.info(DOMAIN, TAG, 'transient zero-process exit while %{public}s; waiting for launch result', this.state);
      } else if (this.state !== EngineState.STOPPED) {
        this.setState(EngineState.STOPPED, 'Wine 引擎已停止');
      }
    }
  }

  private handleEnginePhase(phase: string): void {
    this.enginePhase = phase;
    this.phaseStartedAt = Date.now();
    if (phase === 'ready') {
      // phase:ready is informational. Only the following wine-ready terminal
      // event may persist the prefix runtime marker and release callers; doing
      // it here races past that durable finalization on upgraded prefixes.
      return;
    }
    const phaseMessage = '正在初始化 Wine：' + this.phaseText(phase);
    if (this.state === EngineState.PREPARING || this.state === EngineState.SWITCHING) {
      this.setState(this.state, phaseMessage);
    } else if (this.state !== EngineState.READY) {
      this.setState(EngineState.PREPARING, phaseMessage);
    }
    this.resetReadySafetyNet();
  }

  private syncManagedSmokePayload(trigger: string): boolean {
    const ready = syncManagedSmoke(this.activePrefixMode);
    if (ready) {
      hilog.info(DOMAIN, TAG, 'managed C:\\smoke ready trigger=%{public}s', trigger);
    } else {
      hilog.error(DOMAIN, TAG, 'managed C:\\smoke sync failed trigger=%{public}s', trigger);
    }
    return ready;
  }

  private setState(state: EngineState, message: string): void {
    if (!canTransitionEngineState(this.state, state)) {
      hilog.warn(DOMAIN, TAG, 'unexpected engine transition %{public}s -> %{public}s', this.state, state);
    }
    hilog.info(DOMAIN, TAG, 'engine state %{public}s -> %{public}s: %{public}s', this.state, state, message);
    this.state = state;
    this.message = message;
    if (state === EngineState.READY || state === EngineState.STOPPED || state === EngineState.ERROR) {
      if (this.readySafetyTimer >= 0) {
        clearTimeout(this.readySafetyTimer);
        this.readySafetyTimer = -1;
      }
    }
    this.listeners.forEach((listener: EngineStateListener) => listener(state, message));
  }

  private getMissingRuntimePaths(): string[] {
    const missing: string[] = [];
    REQUIRED_RUNTIME_PATHS.forEach((item: RuntimePathRequirement) => {
      if (!pathMatchesRequirement(item)) missing.push(item.label);
    });
    return missing;
  }

  private getBundledRuntimeVersion(): Uint8Array {
    if (!this.context) return new Uint8Array(0);
    return this.context.resourceManager.getRawFileContentSync(RUNTIME_VERSION_RAWFILE);
  }

  private versionMarkerMatches(path: string, expected: Uint8Array): boolean {
    if (expected.byteLength === 0 || !pathExists(path)) return false;
    let input: fs.File | null = null;
    try {
      const stat = fs.statSync(path);
      if (stat.size !== expected.byteLength) return false;
      input = fs.openSync(path, fs.OpenMode.READ_ONLY);
      const buffer = new ArrayBuffer(expected.byteLength);
      const length = fs.readSync(input.fd, buffer, { length: expected.byteLength });
      if (length !== expected.byteLength) return false;
      const actual = new Uint8Array(buffer);
      for (let index = 0; index < expected.byteLength; index++) {
        if (actual[index] !== expected[index]) return false;
      }
      return true;
    } catch (_) {
      return false;
    } finally {
      if (input) {
        try { fs.closeSync(input); } catch (_) {}
      }
    }
  }

  private runtimeVersionMatches(expected: Uint8Array): boolean {
    return this.versionMarkerMatches(RUNTIME_VERSION_MARKER, expected);
  }

  private prefixVersionMatches(expected: Uint8Array): boolean {
    return this.versionMarkerMatches(PREFIX_RUNTIME_VERSION_MARKER, expected);
  }

  private prefixFontSubstitutesCurrent(): boolean {
    if (!pathExists(PREFIX_SYSTEM_REG)) return false;
    try {
      const registry = fs.readTextSync(PREFIX_SYSTEM_REG);
      if (registryHasObsoleteGameFontSubstitutes(registry)) {
        return false;
      }
      return MASTER_FONT_SUBSTITUTES.every((item: FontSubstitute) =>
        item.persistent === false || fontSubstituteLinePresent(registry, item));
    } catch (_) {
      return false;
    }
  }

  private updatePrefixFontSubstitutes(): boolean {
    if (!pathExists(PREFIX_SYSTEM_REG)) return false;
    const temporary = PREFIX_SYSTEM_REG + '.winehua-fonts.tmp';
    let output: fs.File | null = null;
    try {
      const registry = fs.readTextSync(PREFIX_SYSTEM_REG);
      const sectionHeader = '[Software\\\\Microsoft\\\\Windows NT\\\\CurrentVersion\\\\FontSubstitutes]';
      const sectionStart = registry.indexOf(sectionHeader);
      if (sectionStart < 0) throw new Error('FontSubstitutes registry section missing');
      let sectionEnd = registry.indexOf('\n[', sectionStart + sectionHeader.length);
      if (sectionEnd < 0) sectionEnd = registry.length;
      const replacements: Map<string, FontSubstitute> = new Map<string, FontSubstitute>();
      MASTER_FONT_SUBSTITUTES.forEach((item: FontSubstitute) => {
        fontSubstituteRegistryKeys(item).forEach((key: string) => {
          replacements.set(key, item);
        });
      });
      const seen: Set<string> = new Set<string>();
      const lines = registry.substring(sectionStart, sectionEnd).split('\n');
      const updatedLines: string[] = [];
      lines.forEach((line: string) => {
        const key = fontSubstituteRegistryKey(line);
        if (isObsoleteGameFontSubstituteKey(key)) return;
        const item = replacements.get(key);
        if (!item) {
          updatedLines.push(line);
        } else if (!seen.has(item.name)) {
          updatedLines.push(fontSubstituteRegistryLine(item));
          seen.add(item.name);
        }
      });
      MASTER_FONT_SUBSTITUTES.forEach((item: FontSubstitute) => {
        if (!seen.has(item.name)) updatedLines.push(fontSubstituteRegistryLine(item));
      });
      const updatedSection = updatedLines.join('\n') + '\n';
      const updatedRegistry = registry.substring(0, sectionStart) + updatedSection +
        registry.substring(sectionEnd);
      output = fs.openSync(temporary,
        fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
      fs.writeSync(output.fd, updatedRegistry);
      fs.closeSync(output);
      output = null;
      // rename is atomic on the prefix filesystem and replaces the old file.
      fs.renameSync(temporary, PREFIX_SYSTEM_REG);
      if (!this.prefixFontSubstitutesCurrent()) {
        throw new Error('FontSubstitutes verification failed after registry update');
      }
      hilog.info(DOMAIN, TAG, 'updated existing prefix to master FontSubstitutes without wineboot');
      return true;
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'FontSubstitutes registry update failed: %{public}s', JSON.stringify(error));
      try { fs.unlinkSync(temporary); } catch (_) {}
      return false;
    } finally {
      if (output) {
        try { fs.closeSync(output); } catch (_) {}
      }
    }
  }

  private writeVersionMarker(path: string, expected: Uint8Array): boolean {
    let output: fs.File | null = null;
    try {
      output = fs.openSync(path,
      fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
      const buffer = new ArrayBuffer(expected.byteLength);
      new Uint8Array(buffer).set(expected);
      fs.writeSync(output.fd, buffer, { length: expected.byteLength });
      return true;
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'write version marker failed path=%{public}s error=%{public}s',
        path, JSON.stringify(error));
      return false;
    } finally {
      if (output) {
        try { fs.closeSync(output); } catch (_) {}
      }
    }
  }

  private textFilesMatch(left: string, right: string): boolean {
    try {
      const leftStat = fs.statSync(left);
      const rightStat = fs.statSync(right);
      return leftStat.size === rightStat.size &&
        fs.readTextSync(left) === fs.readTextSync(right);
    } catch (_) {
      return false;
    }
  }

  private reconcilePreservedWineInf(): void {
    if (!pathExists(PREVIOUS_PREFIX_MARKER)) return;
    try {
      if (!pathExists(PREFIX_MARKER)) {
        fs.renameSync(PREVIOUS_PREFIX_MARKER, PREFIX_MARKER);
        hilog.warn(DOMAIN, TAG, 'restored wine.inf left by an interrupted runtime refresh');
      } else if (this.textFilesMatch(PREVIOUS_PREFIX_MARKER, PREFIX_MARKER)) {
        // Restore the old inode, including its mtime. Wine compares wine.inf's
        // mtime with .update-timestamp; replacing identical content would make
        // every graphics-only/runtime-only build reinstall the whole prefix.
        fs.unlinkSync(PREFIX_MARKER);
        fs.renameSync(PREVIOUS_PREFIX_MARKER, PREFIX_MARKER);
        hilog.info(DOMAIN, TAG, 'preserved unchanged wine.inf timestamp; prefix reinstall not required');
      } else {
        fs.unlinkSync(PREVIOUS_PREFIX_MARKER);
        hilog.info(DOMAIN, TAG, 'wine.inf content changed; prefix refresh remains required');
      }
    } catch (error) {
      hilog.error(DOMAIN, TAG, 'wine.inf refresh reconciliation failed: %{public}s', JSON.stringify(error));
      throw new Error('wine.inf refresh reconciliation failed: ' + JSON.stringify(error));
    }
  }

  private async ensureRuntimeInstalled(bundledVersion: Uint8Array): Promise<boolean> {
    // Recover or reconcile a marker stashed by a previous interrupted extract
    // before deciding whether this runtime is complete.
    this.reconcilePreservedWineInf();
    try {
      ensureWineFontsDirectory();
      copyImportedFontsIntoWineShare();
      ensureImportedFontFacesRegistered();
    } catch (error) {
      hilog.warn(DOMAIN, TAG, 'Wine fonts directory migration failed: %{public}s', JSON.stringify(error));
    }
    const missingBefore = this.getMissingRuntimePaths();
    const versionMatches = this.runtimeVersionMatches(bundledVersion);
    const prefixMatches = this.prefixVersionMatches(bundledVersion);
    const fontSubstitutesCurrent = this.prefixFontSubstitutesCurrent();
    if (missingBefore.length === 0 && versionMatches) {
      if (prefixMatches && fontSubstitutesCurrent) return false;
      // Upgrade path for versions released before the prefix marker existed:
      // when the extracted runtime itself did not change and the prefix is
      // complete, migrate the targeted registry values and adopt it.
      let prefixInitialized = false;
      try { prefixInitialized = testNapi.checkWinePrefix() as boolean; } catch (_) {}
      if (prefixInitialized && !fontSubstitutesCurrent) {
        hilog.warn(DOMAIN, TAG, 'existing prefix uses stale FontSubstitutes; applying direct migration');
        if (!this.updatePrefixFontSubstitutes()) {
          throw new Error('Unable to migrate master FontSubstitutes');
        }
      }
      if (prefixInitialized) {
        hilog.info(DOMAIN, TAG, 'adopting initialized prefix for unchanged runtime version');
        if (!this.writeVersionMarker(PREFIX_RUNTIME_VERSION_MARKER, bundledVersion)) {
          throw new Error('Unable to persist Wine prefix baseline marker');
        }
        return false;
      }
      return true;
    }
    if (!this.context) throw new Error('Ability context unavailable');
    hilog.warn(DOMAIN, TAG,
      'runtime refresh required, extracting wine-data.zip, runtimeVersionMatch=%{public}s '
        + 'prefixVersionMatch=%{public}s fontSubstitutesCurrent=%{public}s missing=%{public}s',
      versionMatches ? 'true' : 'false', prefixMatches ? 'true' : 'false',
      fontSubstitutesCurrent ? 'true' : 'false', missingBefore.join(','));
    const rawFd = this.context.resourceManager.getRawFdSync('wine-data.zip');
    const temporaryZip = FILES_BASE + '/wine-data.zip';
    let wineInfPreserved = false;
    try {
      const output = fs.openSync(temporaryZip, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
      const chunkSize = 4 * 1024 * 1024;
      const buffer = new ArrayBuffer(chunkSize);
      let position = rawFd.offset;
      const end = rawFd.offset + rawFd.length;
      while (position < end) {
        const requested = Math.min(chunkSize, end - position);
        const length = fs.readSync(rawFd.fd, buffer, { offset: position, length: requested });
        if (length <= 0) break;
        fs.writeSync(output.fd, buffer, { length: length });
        position += length;
      }
      fs.closeSync(output);
      try {
        fs.mkdirSync(WINE_ROOT, true);
      } catch (_) {}
      if (pathExists(PREFIX_MARKER)) {
        try { fs.unlinkSync(PREVIOUS_PREFIX_MARKER); } catch (_) {}
        fs.renameSync(PREFIX_MARKER, PREVIOUS_PREFIX_MARKER);
        wineInfPreserved = true;
      }
      await zlib.decompressFile(temporaryZip, WINE_ROOT, {});
      if (wineInfPreserved) {
        this.reconcilePreservedWineInf();
        wineInfPreserved = false;
      }
      const missingAfter = this.getMissingRuntimePaths();
      if (missingAfter.length > 0) {
        throw new Error('Wine runtime incomplete after extract: ' + missingAfter.join(', '));
      }
      if (!this.writeVersionMarker(RUNTIME_VERSION_MARKER, bundledVersion)) {
        throw new Error('Unable to persist Wine runtime version marker');
      }
      copyImportedFontsIntoWineShare();
      ensureImportedFontFacesRegistered();
    } finally {
      this.context.resourceManager.closeRawFdSync('wine-data.zip');
      try { fs.unlinkSync(temporaryZip); } catch (_) {}
      if (wineInfPreserved) this.reconcilePreservedWineInf();
    }
    let prefixInitialized = false;
    try { prefixInitialized = testNapi.checkWinePrefix() as boolean; } catch (_) {}
    if (prefixInitialized && !this.prefixFontSubstitutesCurrent() && !this.updatePrefixFontSubstitutes()) {
      throw new Error('Unable to migrate master FontSubstitutes after runtime refresh');
    }
    return true;
  }
}
