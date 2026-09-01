import { bindingToEvdevName, evdevNameToBinding } from '../common/EvdevKeyNames';
import { PerformanceHudSettings, normalizePerformanceHud } from './PerformanceHudSettings';
export enum AppSource {
  BUILTIN = 'builtin',
  DOWNLOAD = 'download'
}

export enum LaunchKind {
  WINE_EXE = 'wine-exe',
  WINE_SYSTEM = 'wine-system',
  WINE_DESKTOP = 'wine-desktop',
  HARMONY_ABILITY = 'harmony-ability',
  NATIVE_TOOL = 'native-tool',
  OTHER_ENGINE = 'other-engine'
}

/** 用户可见显示模式：单应用全屏已移除，统一使用桌面模式。 */
export enum DisplayMode {
  DESKTOP = 'desktop'
}

/** Internal host topology. */
export enum WinePresentationMode {
  DESKTOP = 'desktop',
  MANAGED_WINDOWS = 'managed-windows'
}

export enum EngineState {
  STOPPED = 'stopped',
  PREPARING = 'preparing',
  READY = 'ready',
  SWITCHING = 'switching',
  ERROR = 'error'
}

export enum SessionState {
  STARTING = 'starting',
  RUNNING = 'running',
  BACKGROUND = 'background',
  STOPPED = 'stopped',
  ERROR = 'error'
}

export enum ViewMode {
  GRID = 'grid',
  COMPACT = 'compact',
  LARGE = 'large',
  FILES = 'files'
}

export enum ThemeMode {
  SYSTEM = 'system',
  LIGHT = 'light',
  DARK = 'dark'
}

/** Locale exported to Wine processes. Keep values aligned with Native policy. */
export enum WineLanguage {
  CHINESE = 'zh_CN',
  ENGLISH = 'en_US'
}

/** Orientation policy for the Wine desktop host window. */
export enum ScreenOrientationSetting {
  SENSOR_LANDSCAPE = 'sensor-landscape',
  LANDSCAPE = 'landscape',
  PORTRAIT = 'portrait'
}

/**
 * Wine desktop host window layout. Rounded-screen tablets clip the start menu
 * when the window is stretched edge-to-edge, so a safe-area mode keeps the
 * system bars and the corner cutouts out of the Wine surface.
 */
export enum DesktopWindowMode {
  /** Immersive edge-to-edge window with status/navigation bars hidden. */
  FULLSCREEN = 'fullscreen',
  /** Keep the window inside the system safe area (bars and rounded corners). */
  SAFE_AREA = 'safe_area',
  /** 画中画式小浮窗: 桌面缩成可拖动小窗, 点击放大全屏, 桌面内可缩小回小窗。 */
  FLOATING = 'floating'
}

/**
 * 自动估算设备圆角半径 (物理像素): 取显示短边的 5%, 夹在 [40, 100]。
 * 不同设备圆角不同, 固定值不合适; 短边比例估算对主流设备误差在
 * ±20px 内, 且保证避让量足够又不至于像窗口边框。
 */
export function resolveCornerSafeMarginPx(width: number, height: number): number {
  const short = Math.min(width, height);
  return Math.max(40, Math.min(100, Math.round(short * 0.05)));
}

/** Matches VintagePomelo's control-bar behavior contract. */
export enum ControlBarDirection {
  HORIZONTAL = 'horizontal',
  VERTICAL = 'vertical',
  AUTO = 'auto',
  AUTO_HIDE = 'auto_hide'
}

/** Global Direct3D translation policy used by Explorer and app-card launches. */
export enum D3DRendererPreference {
  DXVK = 'dxvk',
  VIRGL = 'virgl'
}

/** Box64 兼容性预设：默认档不注入任何覆盖（沿用系统当前已验证的配置）。 */
export const BOX64_PRESET_DEFAULT = 'default';
export const BOX64_PRESET_STABILITY = 'stability';
export const BOX64_PRESET_CONSERVATIVE = 'conservative';
export const BOX64_PRESET_INTERMEDIATE = 'intermediate';
export const BOX64_PRESET_PERFORMANCE = 'performance';
/** 按游戏启用的 Unity 专用兼容档：仅对选中的应用注入信号解释器等降速项。 */
export const BOX64_PRESET_UNITY = 'unity';

export const BOX64_PRESET_IDS: string[] = [
  BOX64_PRESET_DEFAULT,
  BOX64_PRESET_STABILITY,
  BOX64_PRESET_CONSERVATIVE,
  BOX64_PRESET_INTERMEDIATE,
  BOX64_PRESET_PERFORMANCE
];

/**
 * 单应用设置页可选的预设：全局预设 + Unity 兼容档。
 * Unity 档刻意不进全局列表，避免整体性能下降，只对选中它的游戏生效。
 */
export const BOX64_APP_PRESET_IDS: string[] = [
  ...BOX64_PRESET_IDS,
  BOX64_PRESET_UNITY
];

export function box64PresetLabel(presetId: string): string {
  switch (presetId) {
    case BOX64_PRESET_STABILITY:
      return '稳定模式';
    case BOX64_PRESET_UNITY:
      return 'Unity 兼容';
    case BOX64_PRESET_CONSERVATIVE:
      return '兼容模式';
    case BOX64_PRESET_INTERMEDIATE:
      return '均衡模式';
    case BOX64_PRESET_PERFORMANCE:
      return '性能模式';
    default:
      return '默认（当前配置）';
  }
}

export function box64PresetDescription(presetId: string): string {
  switch (presetId) {
    case BOX64_PRESET_STABILITY:
      return '优先保证兼容与稳定，适合新引擎、Unity 或频繁崩溃的程序。';
    case BOX64_PRESET_UNITY:
      return '针对 Unity 引擎的稳定档：信号处理走解释器，规避模拟层信号竞态导致的启动失败/黑屏。仅对当前游戏生效，可能略微降低帧率。';
    case BOX64_PRESET_CONSERVATIVE:
      return '兼容与性能之间偏保守，适合大部分老游戏。';
    case BOX64_PRESET_INTERMEDIATE:
      return '兼顾兼容性与速度，是多数程序的均衡选择。';
    case BOX64_PRESET_PERFORMANCE:
      return '追求最高帧率，部分程序可能因此变慢或崩溃。';
    default:
      return '不注入额外参数，使用系统已验证的默认兼容配置。';
  }
}

/**
 * 把预设展开为 KEY=VALUE 环境变量列表；默认档返回空数组，
 * 即完全不覆盖 C++ 侧的当前生产配置。
 * 弱内存屏障(WEAKBARRIER)与易失元数据(VOLATILE_METADATA)是私有保护项，
 * 不随预设下发，避免破坏已验证的渲染与文件访问路径。
 */
export function resolveBox64PresetEnv(presetId: string): string[] {
  switch (presetId) {
    case BOX64_PRESET_UNITY:
      // 稳定档参数 + 信号处理强制走解释器：Unity 频繁触发 wine unix-call
      // trap（SIGSEGV 信号回调），dynarec 信号路径偶发竞态导致调用返回
      // E_OUTOFMEMORY。仅此档注入，不影响其他游戏的性能。
      return [
        'BOX64_DYNAREC_SAFEFLAGS=2',
        'BOX64_DYNAREC_FASTNAN=0',
        'BOX64_DYNAREC_FASTROUND=0',
        'BOX64_DYNAREC_X87DOUBLE=1',
        'BOX64_DYNAREC_BIGBLOCK=0',
        'BOX64_DYNAREC_STRONGMEM=2',
        'BOX64_DYNAREC_FORWARD=128',
        'BOX64_DYNAREC_CALLRET=0',
        'BOX64_DYNAREC_WAIT=0',
        'BOX64_DYNAREC_NATIVEFLAGS=0',
        'BOX64_DYNAREC_INTERP_SIGNAL=1'
      ];
    case BOX64_PRESET_STABILITY:
      return [
        'BOX64_DYNAREC_SAFEFLAGS=2',
        'BOX64_DYNAREC_FASTNAN=0',
        'BOX64_DYNAREC_FASTROUND=0',
        'BOX64_DYNAREC_X87DOUBLE=1',
        'BOX64_DYNAREC_BIGBLOCK=0',
        'BOX64_DYNAREC_STRONGMEM=2',
        'BOX64_DYNAREC_FORWARD=128',
        'BOX64_DYNAREC_CALLRET=0',
        'BOX64_DYNAREC_WAIT=0',
        'BOX64_DYNAREC_NATIVEFLAGS=0'
      ];
    case BOX64_PRESET_CONSERVATIVE:
      return [
        'BOX64_DYNAREC_SAFEFLAGS=2',
        'BOX64_DYNAREC_FASTNAN=0',
        'BOX64_DYNAREC_FASTROUND=0',
        'BOX64_DYNAREC_X87DOUBLE=1',
        'BOX64_DYNAREC_BIGBLOCK=1',
        'BOX64_DYNAREC_STRONGMEM=1',
        'BOX64_DYNAREC_FORWARD=128',
        'BOX64_DYNAREC_CALLRET=0',
        'BOX64_DYNAREC_WAIT=1',
        'BOX64_DYNAREC_NATIVEFLAGS=0'
      ];
    case BOX64_PRESET_INTERMEDIATE:
      return [
        'BOX64_DYNAREC_SAFEFLAGS=2',
        'BOX64_DYNAREC_FASTNAN=1',
        'BOX64_DYNAREC_FASTROUND=0',
        'BOX64_DYNAREC_X87DOUBLE=1',
        'BOX64_DYNAREC_BIGBLOCK=2',
        'BOX64_DYNAREC_STRONGMEM=0',
        'BOX64_DYNAREC_FORWARD=128',
        'BOX64_DYNAREC_CALLRET=0',
        'BOX64_DYNAREC_WAIT=1',
        'BOX64_DYNAREC_NATIVEFLAGS=0'
      ];
    case BOX64_PRESET_PERFORMANCE:
      return [
        'BOX64_DYNAREC_SAFEFLAGS=1',
        'BOX64_DYNAREC_FASTNAN=1',
        'BOX64_DYNAREC_FASTROUND=1',
        'BOX64_DYNAREC_X87DOUBLE=0',
        'BOX64_DYNAREC_BIGBLOCK=3',
        'BOX64_DYNAREC_STRONGMEM=0',
        'BOX64_DYNAREC_FORWARD=512',
        'BOX64_DYNAREC_CALLRET=1',
        'BOX64_DYNAREC_WAIT=1',
        'BOX64_DYNAREC_NATIVEFLAGS=1'
      ];
    default:
      return [];
  }
}

/** 全局预设 → native launchClient compatEnvStr (分号串); 默认档为空。 */
export function box64PresetEnvString(presetId: string): string {
  return resolveBox64PresetEnv(presetId).join(';');
}

/**
 * Unity 兼容档额外追加的启动参数。参考 Winlator：Unity 引擎游戏在
 * Stability 预设之外还需要 -force-gfx-direct，让 Unity 跳过 GDI 间接
 * 渲染路径，直接绘制到窗口，减少黑屏/花屏。
 */
export const UNITY_COMPAT_LAUNCH_ARGS: string[] = ['-force-gfx-direct'];

/** 判断某预设是否启用了 Unity 兼容模式（决定是否注入启动参数）。 */
export function isUnityCompatPreset(presetId: string): boolean {
  return presetId === BOX64_PRESET_UNITY;
}

export interface VirtualMouseConfig {
  mouseSensitivity: number;
  doubleClickInterval: number;
  showCursor: boolean;
}

export const DEFAULT_VIRTUAL_MOUSE_CONFIG: VirtualMouseConfig = {
  mouseSensitivity: 100,
  doubleClickInterval: 100,
  showCursor: true
};

export function cloneVirtualMouseConfig(config?: VirtualMouseConfig): VirtualMouseConfig {
  const value = config ?? DEFAULT_VIRTUAL_MOUSE_CONFIG;
  return {
    mouseSensitivity: value.mouseSensitivity ?? DEFAULT_VIRTUAL_MOUSE_CONFIG.mouseSensitivity,
    doubleClickInterval: value.doubleClickInterval ?? DEFAULT_VIRTUAL_MOUSE_CONFIG.doubleClickInterval,
    showCursor: value.showCursor ?? DEFAULT_VIRTUAL_MOUSE_CONFIG.showCursor
  };
}

export interface LaunchTarget {
  kind: LaunchKind;
  executable: string;
  arguments: string[];
  workingDirectory: string;
}

export interface AppDescriptor {
  id: string;
  title: string;
  source: AppSource;
  directory: string;
  coverPath: string;
  available: boolean;
  launchTarget: LaunchTarget;
  displayModeOverride: DisplayMode | null;
}

export interface AppSettings {
  appId: string;
  customTitle: string;
  customCover: string;
  /** Kept separately from catalog discovery so a moved game can stay starred. */
  favorite: boolean;
  /** Box64 兼容性预设；'default' 表示跟随全局设置。 */
  box64Preset: string;
  executable: string;
  arguments: string[];
  workingDirectory: string;
  displayModeOverride: DisplayMode | null;
  inputProfileId: string;
  prefixId: string;
}

export interface GlobalSettings {
  defaultDisplayMode: DisplayMode;
  /** Windows/Wine system language. Existing installs migrate to Chinese. */
  wineLanguage: WineLanguage;
  desktopOrientation: ScreenOrientationSetting;
  /** Desktop host window layout: immersive fullscreen or safe-area inset. */
  desktopWindowMode: DesktopWindowMode;
  /** 桌面/游戏窗口打开时保持屏幕常亮，避免游玩中自动锁屏。默认开启。 */
  keepScreenOn: boolean;
  /** Box64 兼容性预设；'default' 表示使用系统当前配置。 */
  box64Preset: string;
  /** Desktop mode always uses this global profile; single-app may override it. */
  desktopInputProfileId: string;
  viewMode: ViewMode;
  themeMode: ThemeMode;
  useGlassEffect: boolean;
  usePageBackground: boolean;
  pageBackgroundUri: string;
  /** Entire ArkTS screen-input layer, including controls, mouse mode and toolbar. */
  virtualInputEnabled: boolean;
  showVirtualControls: boolean;
  autoHideVirtualControls: boolean;
  controlBarDirection: ControlBarDirection;
  /** Normalized top-left anchor inside the available overlay area. */
  controlBarPositionX: number;
  controlBarPositionY: number;
  virtualMouseConfig: VirtualMouseConfig;
  controlOpacity: number;
  gamepadDeadzone: number;
  /** Wine pad via Controller Hub (DInput/XInput + rumble) or keyboard/mouse emulation. */
  controllerOutputMode: 'dinput' | 'keyboard_legacy';
  outputScale: number;
  d3dRendererPreference: D3DRendererPreference;
  /** 重置 Wine prefix 时默认保留文档目录与注册表，只重建沙盒。 */
  preserveUserDataOnReset: boolean;
  /** Performance HUD master switch; retain the existing preference key. */
  showFpsHud: boolean;
  performanceHud: PerformanceHudSettings;
}

export interface RuntimeSession {
  sessionId: string;
  appId: string;
  executable: string;
  pid: number;
  displayMode: DisplayMode;
  presentationMode: WinePresentationMode;
  state: SessionState;
  toplevelId: number;
  startedAt: number;
}

/** ArkTS 覆盖层所使用的 Wine 会话视图，不等同于 Wine/NCP 子进程。 */
export interface WineInputSession {
  sessionId: string;
  displayMode: DisplayMode;
  toplevelId: number;
  active: boolean;
}

export interface InputDiagnostics {
  sessionId: string;
  activePointers: number;
  heldKeys: number;
  heldButtons: number;
  injectedEvents: number;
}

export interface NativeProcessInfo {
  pid: number;
  name: string;
  path: string;
  state: string;
  sessionId?: string;
}

export interface NativeLaunchResult {
  pid: number;
  sessionId: string;
  reused: boolean;
}

export interface NativeWineSessionInfo {
  pid: number;
  sessionId: string;
  path: string;
  state: string;
  toplevelId: number;
}

export const INPUT_PROFILE_SCHEMA_VERSION = 6;

/** Winlator 风格的可排布输入元素类型。 */
export enum ControlElementType {
  PANEL = 'PANEL',
  BUTTON = 'BUTTON',
  D_PAD = 'D_PAD',
  STICK = 'STICK',
  TRACKPAD = 'TRACKPAD',
  RANGE_BUTTON = 'RANGE_BUTTON'
}

export enum ControlElementShape {
  CIRCLE = 'CIRCLE',
  RECT = 'RECT',
  ROUND_RECT = 'ROUND_RECT',
  SQUARE = 'SQUARE'
}

export enum InputBindingKind {
  NONE = 'NONE',
  KEY = 'KEY',
  MOUSE = 'MOUSE'
}

export interface InputBinding {
  kind: InputBindingKind;
  code: number;
}

export const MOUSE_LEFT_BUTTON = 272;
export const MOUSE_RIGHT_BUTTON = 273;
export const MOUSE_MIDDLE_BUTTON = 274;

export function keyBinding(code: number): InputBinding {
  return { kind: InputBindingKind.KEY, code: code };
}

export function mouseBinding(code: number): InputBinding {
  return { kind: InputBindingKind.MOUSE, code: code };
}

export function cloneInputBinding(binding?: InputBinding): InputBinding {
  const value = binding ?? { kind: InputBindingKind.NONE, code: 0 };
  return { kind: value.kind ?? InputBindingKind.NONE, code: value.code ?? 0 };
}

/**
 * 元素中心使用归一化坐标（0.0-1.0），与屏幕比例无关（参考 Winlator .icp）。
 * bindings 为多键组合：BUTTON 按下时同时按下全部槽位；D_PAD/STICK 依次为
 * 上/右/下/左；TRACKPAD 不发键；RANGE_BUTTON 使用 keyList 滑动选键。
 */
export interface ControlElement {
  id: string;
  type: ControlElementType;
  shape: ControlElementShape;
  x: number;
  y: number;
  scale: number;
  /** Horizontal multiplier. 1 keeps the legacy square control size. */
  widthScale?: number;
  /** Vertical multiplier. Mainly used by non-interactive layout panels. */
  heightScale?: number;
  /** Viewport-relative size for PANEL decorations. */
  panelWidthRatio?: number;
  panelHeightRatio?: number;
  /** 键盘键专用: 按 overlay 宽/高归一化的目标尺寸, 跨设备自适应填满槽位。 */
  keyWidthRatio?: number;
  keyHeightRatio?: number;
  /** -1 继承方案透明度 overlayOpacity；0-100 为元素级覆盖。 */
  opacity: number;
  bindings: InputBinding[];
  toggleSwitch: boolean;
  text: string;
  iconId: number;
  visible: boolean;
  /** RANGE_BUTTON 可选键列表（evdev 码）。 */
  keyList: number[];
  /** RANGE_BUTTON 方向：'vertical' 纵向（默认）/ 'horizontal' 横向。 */
  orientation: string;
  /** STICK 输出模式：'keys' 方向键（默认）/ 'mouse' 鼠标视角。 */
  outputMode: string;
}

export interface InputProfile {
  id: string;
  name: string;
  schemaVersion: number;
  /** Shipped-template revision; custom profiles intentionally leave this unset. */
  builtInRevision?: number;
  /** 触摸板指针速度倍率（0.1-5.0）。 */
  cursorSpeed: number;
  /** 方案级覆盖层透明度（0-100）。 */
  overlayOpacity: number;
  gamepadMappings: GamepadButtonMapping[];
  /** 当前设备画面使用的元素集（归一化坐标，自动适配任意屏幕比例）。 */
  elements: ControlElement[];
  /** 废弃兼容字段：曾按断点分三套，加载时回退读取平板套。 */
  elementsTablet?: ControlElement[];
  elementsPhone?: ControlElement[];
  elementsDesktop?: ControlElement[];
}

/** VintagePomelo-compatible 4/8-way virtual joystick quantization. */
export function resolveVirtualJoystickDirection(dx: number, dy: number, radius: number,
  deadzonePercent: number, eightDirection: boolean): VirtualJoystickDirection {
  const none: VirtualJoystickDirection = { up: false, down: false, left: false, right: false };
  const safeRadius = Math.max(1, radius);
  const deadzone = safeRadius * Math.max(0, Math.min(100, deadzonePercent)) / 100;
  if (Math.sqrt(dx * dx + dy * dy) < deadzone) return none;
  const angle = Math.atan2(dy, dx) * 180 / Math.PI;
  if (!eightDirection) {
    if (angle >= -45 && angle < 45) return { up: false, down: false, left: false, right: true };
    if (angle >= 45 && angle < 135) return { up: false, down: true, left: false, right: false };
    if (angle >= 135 || angle < -135) return { up: false, down: false, left: true, right: false };
    return { up: true, down: false, left: false, right: false };
  }
  if (angle >= -22.5 && angle < 22.5) return { up: false, down: false, left: false, right: true };
  if (angle >= 22.5 && angle < 67.5) return { up: false, down: true, left: false, right: true };
  if (angle >= 67.5 && angle < 112.5) return { up: false, down: true, left: false, right: false };
  if (angle >= 112.5 && angle < 157.5) return { up: false, down: true, left: true, right: false };
  if (angle >= 157.5 || angle < -157.5) return { up: false, down: false, left: true, right: false };
  if (angle >= -157.5 && angle < -112.5) return { up: true, down: false, left: true, right: false };
  if (angle >= -112.5 && angle < -67.5) return { up: true, down: false, left: false, right: false };
  return { up: true, down: false, left: false, right: true };
}

export interface VirtualJoystickDirection {
  up: boolean;
  down: boolean;
  left: boolean;
  right: boolean;
}

export interface GamepadButtonMapping {
  id: string;
  label: string;
  buttonCode: number;
  evdevCode: number;
}

export const DEFAULT_GAMEPAD_MAPPINGS: GamepadButtonMapping[] = [
  { id: 'dpad-up', label: '方向键 ↑', buttonCode: 2012, evdevCode: 103 },
  { id: 'dpad-down', label: '方向键 ↓', buttonCode: 2013, evdevCode: 108 },
  { id: 'dpad-left', label: '方向键 ←', buttonCode: 2014, evdevCode: 105 },
  { id: 'dpad-right', label: '方向键 →', buttonCode: 2015, evdevCode: 106 },
  { id: 'a', label: 'A / 确认', buttonCode: 2301, evdevCode: 57 },
  { id: 'b', label: 'B / 返回', buttonCode: 2302, evdevCode: 45 },
  { id: 'x', label: 'X', buttonCode: 2304, evdevCode: 30 },
  { id: 'y', label: 'Y', buttonCode: 2305, evdevCode: 31 },
  { id: 'l1', label: 'L1', buttonCode: 2307, evdevCode: 16 },
  { id: 'r1', label: 'R1', buttonCode: 2308, evdevCode: 17 },
  { id: 'l2', label: 'L2', buttonCode: 2309, evdevCode: 42 },
  { id: 'r2', label: 'R2', buttonCode: 2310, evdevCode: 29 },
  { id: 'select', label: 'Select', buttonCode: 2311, evdevCode: 1 },
  { id: 'start', label: 'Start', buttonCode: 2312, evdevCode: 28 },
  { id: 'l3', label: 'L3', buttonCode: 2313, evdevCode: 28 },
  { id: 'r3', label: 'R3', buttonCode: 2314, evdevCode: 57 },
  { id: 'home', label: 'Home', buttonCode: 2315, evdevCode: 63 }
];

export function generateControlElementId(prefix: string = 'el'): string {
  return prefix + '-' + Date.now().toString(36) + '-' + Math.floor(Math.random() * 0x10000).toString(36);
}

function clamp01(value: number): number {
  return Math.max(0, Math.min(1, value));
}

/**
 * 创建一个新元素。shape 缺省按类型选择：触摸板/滚动键为圆角矩形，
 * 其余为圆形。
 */
export function createControlElement(
  type: ControlElementType,
  x: number,
  y: number,
  text: string,
  bindings: InputBinding[],
  shape?: ControlElementShape,
  scale?: number,
  toggleSwitch?: boolean,
  keyList?: number[],
  orientation?: string,
  outputMode?: string,
  widthScale?: number,
  heightScale?: number): ControlElement {
  const resolvedShape = shape ?? (type === ControlElementType.TRACKPAD || type === ControlElementType.RANGE_BUTTON
    ? ControlElementShape.ROUND_RECT : ControlElementShape.CIRCLE);
  return {
    id: generateControlElementId('el'),
    type: type,
    shape: resolvedShape,
    x: clamp01(x),
    y: clamp01(y),
    scale: Math.max(0.1, Math.min(5, scale ?? 1)),
    widthScale: Math.max(0.2, Math.min(12, widthScale ?? 1)),
    heightScale: Math.max(0.2, Math.min(12, heightScale ?? 1)),
    keyWidthRatio: undefined,
    keyHeightRatio: undefined,
    opacity: -1,
    bindings: (bindings ?? []).map(cloneInputBinding),
    toggleSwitch: toggleSwitch ?? false,
    text: text,
    iconId: 0,
    visible: true,
    keyList: (keyList ?? []).map((code: number): number => code),
    orientation: orientation ?? 'vertical',
    outputMode: outputMode ?? 'keys'
  };
}

export function cloneControlElement(element: ControlElement): ControlElement {
  return {
    id: element.id,
    type: element.type ?? ControlElementType.BUTTON,
    shape: element.shape ?? ControlElementShape.CIRCLE,
    x: clamp01(element.x ?? 0.5),
    y: clamp01(element.y ?? 0.5),
    scale: element.scale ?? 1,
    widthScale: Math.max(0.2, Math.min(12, element.widthScale ?? 1)),
    heightScale: Math.max(0.2, Math.min(12, element.heightScale ?? 1)),
    panelWidthRatio: element.panelWidthRatio,
    panelHeightRatio: element.panelHeightRatio,
    keyWidthRatio: element.keyWidthRatio === undefined ? undefined :
      Math.max(0.005, Math.min(0.5, element.keyWidthRatio)),
    keyHeightRatio: element.keyHeightRatio === undefined ? undefined :
      Math.max(0.005, Math.min(0.5, element.keyHeightRatio)),
    opacity: element.opacity ?? -1,
    bindings: (element.bindings ?? []).map(cloneInputBinding),
    toggleSwitch: element.toggleSwitch ?? false,
    text: element.text ?? '',
    iconId: element.iconId ?? 0,
    visible: element.visible ?? true,
    keyList: (element.keyList ?? []).map((code: number): number => code),
    orientation: element.orientation ?? 'vertical',
    outputMode: element.outputMode ?? 'keys'
  };
}

export function cloneGamepadMappings(mappings?: GamepadButtonMapping[]): GamepadButtonMapping[] {
  return (mappings ?? DEFAULT_GAMEPAD_MAPPINGS).map((mapping: GamepadButtonMapping): GamepadButtonMapping => ({
    id: mapping.id,
    label: mapping.label,
    buttonCode: mapping.buttonCode,
    evdevCode: mapping.evdevCode
  }));
}

export function cloneInputProfile(profile?: InputProfile, profileId?: string, profileName?: string): InputProfile {
  const fallback = createDefaultInputProfile();
  const value = profile ?? fallback;
  const legacyThree = (value.elementsTablet ?? []).length > 0 ? value.elementsTablet : undefined;
  const source = (value.elements ?? legacyThree ?? []).map(cloneControlElement);
  return {
    id: profileId ?? value.id,
    name: profileName ?? value.name,
    schemaVersion: INPUT_PROFILE_SCHEMA_VERSION,
    builtInRevision: value.builtInRevision,
    cursorSpeed: value.cursorSpeed ?? fallback.cursorSpeed,
    overlayOpacity: value.overlayOpacity ?? fallback.overlayOpacity,
    gamepadMappings: cloneGamepadMappings(value.gamepadMappings),
    elements: source
  };
}

export interface InputTemplate {
  id: string;
  name: string;
  description: string;
  builtInProfileId: string;
  profileName: string;
  revision: number;
}

export const INPUT_TEMPLATES: InputTemplate[] = [
  {
    id: 'default', name: '通用', description: '方向键、鼠标和常用功能键',
    builtInProfileId: 'default', profileName: '默认输入方案', revision: 4
  },
  {
    id: 'full-keyboard', name: '全键盘', description: '完整字母、数字、功能键与方向键',
    builtInProfileId: 'builtin-full-keyboard', profileName: '全键盘', revision: 14
  },
  {
    id: 'rpg', name: 'RPG', description: '移动、交互、背包、地图与技能快捷栏',
    builtInProfileId: 'builtin-rpg', profileName: 'RPG', revision: 5
  },
  {
    id: 'fps', name: 'FPS', description: '移动、视角、开火、瞄准与武器切换',
    builtInProfileId: 'builtin-fps', profileName: 'FPS', revision: 3
  },
  {
    id: 'action', name: '动作', description: '双摇杆与 ABXY 风格动作按键',
    builtInProfileId: 'builtin-action', profileName: '动作', revision: 3
  }
];

export function isBuiltInInputProfileId(profileId: string): boolean {
  return INPUT_TEMPLATES.some((template: InputTemplate): boolean =>
    template.builtInProfileId === profileId);
}

export function inputTemplateForProfileId(profileId: string): InputTemplate | undefined {
  return INPUT_TEMPLATES.find((template: InputTemplate): boolean =>
    template.builtInProfileId === profileId);
}

function dpadWASD(): InputBinding[] {
  return [keyBinding(17), keyBinding(32), keyBinding(31), keyBinding(30)];
}

function arrowBindings(): InputBinding[] {
  return [keyBinding(103), keyBinding(106), keyBinding(108), keyBinding(105)];
}

function defaultTemplateElements(): ControlElement[] {
  return [
    createControlElement(ControlElementType.D_PAD, 0.12, 0.70, '', dpadWASD()),
    createControlElement(ControlElementType.TRACKPAD, 0.68, 0.76, '视角', []),
    createControlElement(ControlElementType.BUTTON, 0.86, 0.62, '空格', [keyBinding(57)]),
    createControlElement(ControlElementType.BUTTON, 0.94, 0.62, '', [mouseBinding(MOUSE_LEFT_BUTTON)]),
    createControlElement(ControlElementType.BUTTON, 0.86, 0.84, 'E', [keyBinding(18)]),
    createControlElement(ControlElementType.BUTTON, 0.94, 0.84, '', [mouseBinding(MOUSE_RIGHT_BUTTON)]),
    createControlElement(ControlElementType.BUTTON, 0.06, 0.16, 'Esc', [keyBinding(1)], ControlElementShape.ROUND_RECT, 0.72),
    createControlElement(ControlElementType.BUTTON, 0.14, 0.16, 'Shift', [keyBinding(42)], ControlElementShape.ROUND_RECT, 0.72, true),
    createControlElement(ControlElementType.BUTTON, 0.22, 0.16, 'Ctrl', [keyBinding(29)], ControlElementShape.ROUND_RECT, 0.72, true)
  ];
}

interface KeyboardTemplateKey {
  label: string;
  code: number;
  units?: number;
  gapBefore?: number;
}

function keyboardTemplateKeyElement(key: KeyboardTemplateKey, x: number, y: number,
  scale: number, keyWidthRatio?: number, keyHeightRatio?: number): ControlElement {
  const element = createControlElement(
    ControlElementType.BUTTON,
    x,
    y,
    key.label,
    [keyBinding(key.code)],
    ControlElementShape.ROUND_RECT,
    scale,
    key.label === 'Shift' || key.label === 'Ctrl',
    [],
    'vertical',
    'keys',
    1);
  if (keyWidthRatio !== undefined) element.keyWidthRatio = keyWidthRatio;
  if (keyHeightRatio !== undefined) element.keyHeightRatio = keyHeightRatio;
  return element;
}

/**
 * Lay out a keyboard row by real key-cap units instead of spreading equal
 * buttons from edge to edge. Wider keys therefore keep the proportions of a
 * physical keyboard and the alpha rows stay visually aligned on every screen.
 */
function appendKeyboardRow(elements: ControlElement[], keys: KeyboardTemplateKey[], y: number,
  left: number = 0.10, right: number = 0.74, scale: number = 0.44,
  pitch: number = 0.075): void {
  if (keys.length === 0) return;
  const totalUnits = keys.reduce((total: number, key: KeyboardTemplateKey): number =>
    total + (key.gapBefore ?? 0) + (key.units ?? 1), 0);
  // 每个键按其真实键帽单位填满槽位: 键宽 = 槽位宽 * 94%, 键高 = 行距 * 84%,
  // 由渲染层按 overlay 宽高换算, 在平板和手机上都不会出现大间距/过小键。
  const unitWidthRatio = (right - left) / totalUnits;
  const keyHeightRatio = pitch * 0.84;
  let cursor = 0;
  keys.forEach((key: KeyboardTemplateKey): void => {
    cursor += key.gapBefore ?? 0;
    const units = key.units ?? 1;
    const center = left + (right - left) * (cursor + units / 2) / totalUnits;
    const keyWidthRatio = unitWidthRatio * units * 0.985;
    elements.push(keyboardTemplateKeyElement(key, center, y, scale, keyWidthRatio, keyHeightRatio));
    cursor += units;
  });
}

function fullKeyboardTemplateElements(): ControlElement[] {
  const elements: ControlElement[] = [];
  appendKeyboardRow(elements, [
    { label: 'Esc', code: 1 }, { label: 'F1', code: 59, gapBefore: 0.5 }, { label: 'F2', code: 60 },
    { label: 'F3', code: 61 }, { label: 'F4', code: 62 }, { label: 'F5', code: 63, gapBefore: 0.25 },
    { label: 'F6', code: 64 }, { label: 'F7', code: 65 }, { label: 'F8', code: 66 },
    { label: 'F9', code: 67, gapBefore: 0.25 }, { label: 'F10', code: 68 }, { label: 'F11', code: 87 },
    { label: 'F12', code: 88 }
  ], 0.545, 0.08, 0.93, 1.00, 0.085);
  appendKeyboardRow(elements, [
    { label: '`', code: 41 },
    { label: '1', code: 2 }, { label: '2', code: 3 }, { label: '3', code: 4 },
    { label: '4', code: 5 }, { label: '5', code: 6 }, { label: '6', code: 7 },
    { label: '7', code: 8 }, { label: '8', code: 9 }, { label: '9', code: 10 },
    { label: '0', code: 11 }, { label: '-', code: 12 }, { label: '=', code: 13 },
    { label: 'Back', code: 14, units: 0.9 }
  ], 0.625, 0.07, 0.75, 1.00, 0.082);
  appendKeyboardRow(elements, [
    { label: 'Tab', code: 15, units: 0.9 }, { label: 'Q', code: 16 }, { label: 'W', code: 17 },
    { label: 'E', code: 18 }, { label: 'R', code: 19 }, { label: 'T', code: 20 },
    { label: 'Y', code: 21 }, { label: 'U', code: 22 }, { label: 'I', code: 23 },
    { label: 'O', code: 24 }, { label: 'P', code: 25 }, { label: '[', code: 26 },
    { label: ']', code: 27 }, { label: '\\', code: 43, units: 1.4 }
  ], 0.700, 0.07, 0.75, 1.00, 0.082);
  appendKeyboardRow(elements, [
    { label: 'Caps', code: 58, units: 1.0 }, { label: 'A', code: 30 }, { label: 'S', code: 31 },
    { label: 'D', code: 32 }, { label: 'F', code: 33 }, { label: 'G', code: 34 },
    { label: 'H', code: 35 }, { label: 'J', code: 36 }, { label: 'K', code: 37 },
    { label: 'L', code: 38 }, { label: ';', code: 39 }, { label: "'", code: 40 },
    { label: 'Enter', code: 28, units: 1.0 }
  ], 0.775, 0.07, 0.75, 1.00, 0.082);
  appendKeyboardRow(elements, [
    { label: 'Shift', code: 42, units: 1.0 }, { label: 'Z', code: 44 }, { label: 'X', code: 45 },
    { label: 'C', code: 46 }, { label: 'V', code: 47 }, { label: 'B', code: 48 },
    { label: 'N', code: 49 }, { label: 'M', code: 50 }, { label: ',', code: 51 },
    { label: '.', code: 52 }, { label: '/', code: 53 }, { label: 'Shift', code: 54, units: 1.0 }
  ], 0.850, 0.07, 0.75, 1.00, 0.082);
  appendKeyboardRow(elements, [
    { label: 'Ctrl', code: 29, units: 0.9 }, { label: 'Win', code: 125, units: 0.75 },
    { label: 'Alt', code: 56, units: 0.8 }, { label: 'Space', code: 57, units: 0.8 },
    { label: 'Alt', code: 100, units: 0.8 }, { label: 'Menu', code: 127, units: 0.75 },
    { label: 'Ctrl', code: 97, units: 0.9 }
  ], 0.925, 0.07, 0.71, 1.00, 0.082);

  // Standard six-key navigation block and inverted-T arrow cluster.
  const navigationKeys: Array<[string, number, number, number]> = [
    ['Ins', 110, 0.78, 0.625], ['Home', 102, 0.835, 0.625], ['PgUp', 104, 0.89, 0.625],
    ['Del', 111, 0.78, 0.700], ['End', 107, 0.835, 0.700], ['PgDn', 109, 0.89, 0.700],
    ['↑', 103, 0.835, 0.850], ['←', 105, 0.78, 0.925],
    ['↓', 108, 0.835, 0.925], ['→', 106, 0.89, 0.925]
  ];
  navigationKeys.forEach((key: [string, number, number, number]): void => {
    // 导航/方向键: 三个一列, 列距 0.055, 行距 0.075, 尺寸按槽位填充。
    elements.push(keyboardTemplateKeyElement({ label: key[0], code: key[1], units: 0.92 },
      key[2], key[3], 1.00, 0.052, 0.075));
  });
  return elements;
}

function rpgTemplateElements(): ControlElement[] {
  return [
    createControlElement(ControlElementType.STICK, 0.13, 0.73, '', dpadWASD()),
    createControlElement(ControlElementType.TRACKPAD, 0.60, 0.58, '视角', []),
    createControlElement(ControlElementType.BUTTON, 0.82, 0.70, '攻击', [mouseBinding(MOUSE_LEFT_BUTTON)]),
    createControlElement(ControlElementType.BUTTON, 0.91, 0.59, '视角', [mouseBinding(MOUSE_RIGHT_BUTTON)]),
    createControlElement(ControlElementType.BUTTON, 0.91, 0.82, '交互', [keyBinding(18)]),
    createControlElement(ControlElementType.BUTTON, 0.73, 0.82, '跳跃', [keyBinding(57)]),
    createControlElement(ControlElementType.BUTTON, 0.05, 0.09, '菜单', [keyBinding(1)], ControlElementShape.ROUND_RECT),
    createControlElement(ControlElementType.BUTTON, 0.15, 0.09, '背包', [keyBinding(23)], ControlElementShape.ROUND_RECT),
    createControlElement(ControlElementType.BUTTON, 0.25, 0.09, '地图', [keyBinding(50)], ControlElementShape.ROUND_RECT),
    createControlElement(ControlElementType.BUTTON, 0.35, 0.09, '角色', [keyBinding(46)], ControlElementShape.ROUND_RECT),
    createControlElement(ControlElementType.BUTTON, 0.45, 0.09, '任务', [keyBinding(36)], ControlElementShape.ROUND_RECT),
    createControlElement(ControlElementType.BUTTON, 0.06, 0.44, 'Shift', [keyBinding(42)], ControlElementShape.ROUND_RECT, 0.72, true),
    createControlElement(ControlElementType.BUTTON, 0.17, 0.44, 'Ctrl', [keyBinding(29)], ControlElementShape.ROUND_RECT, 0.72, true),
    createControlElement(ControlElementType.RANGE_BUTTON, 0.972, 0.39, '技能', [],
      ControlElementShape.ROUND_RECT, 0.62, false, [2, 3, 4, 5, 6, 7], 'vertical')
  ];
}

function fpsTemplateElements(): ControlElement[] {
  return [
    createControlElement(ControlElementType.D_PAD, 0.12, 0.72, '', dpadWASD()),
    createControlElement(ControlElementType.TRACKPAD, 0.68, 0.38, '视角', []),
    createControlElement(ControlElementType.BUTTON, 0.80, 0.68, '开火', [mouseBinding(MOUSE_LEFT_BUTTON)]),
    createControlElement(ControlElementType.BUTTON, 0.89, 0.58, '瞄准', [mouseBinding(MOUSE_RIGHT_BUTTON)]),
    createControlElement(ControlElementType.BUTTON, 0.90, 0.84, 'R', [keyBinding(19)]),
    createControlElement(ControlElementType.BUTTON, 0.72, 0.84, '跳', [keyBinding(57)]),
    createControlElement(ControlElementType.BUTTON, 0.06, 0.44, 'Shift', [keyBinding(42)], ControlElementShape.ROUND_RECT, 0.72, true),
    createControlElement(ControlElementType.BUTTON, 0.17, 0.44, 'Ctrl', [keyBinding(29)], ControlElementShape.ROUND_RECT, 0.72, true),
    createControlElement(ControlElementType.BUTTON, 0.06, 0.26, 'Esc', [keyBinding(1)], ControlElementShape.ROUND_RECT, 0.72),
    createControlElement(ControlElementType.RANGE_BUTTON, 0.95, 0.46, '武器', [], ControlElementShape.ROUND_RECT, 0.9, false, [2, 3, 4, 5, 6])
  ];
}

function actionTemplateElements(): ControlElement[] {
  const elements = [
    createControlElement(ControlElementType.STICK, 0.13, 0.74, '', dpadWASD()),
    createControlElement(ControlElementType.STICK, 0.76, 0.78, '', arrowBindings(), ControlElementShape.CIRCLE, 0.9),
    createControlElement(ControlElementType.BUTTON, 0.85, 0.70, 'A', [keyBinding(57)]),
    createControlElement(ControlElementType.BUTTON, 0.92, 0.60, 'B', [keyBinding(36)]),
    createControlElement(ControlElementType.BUTTON, 0.85, 0.84, 'X', [keyBinding(18)]),
    createControlElement(ControlElementType.BUTTON, 0.92, 0.84, 'Y', [keyBinding(37)]),
    createControlElement(ControlElementType.BUTTON, 0.06, 0.26, 'L', [keyBinding(16)], ControlElementShape.ROUND_RECT, 0.72),
    createControlElement(ControlElementType.BUTTON, 0.16, 0.26, 'R', [keyBinding(17)], ControlElementShape.ROUND_RECT, 0.72),
    createControlElement(ControlElementType.BUTTON, 0.46, 0.26, 'Start', [keyBinding(28)], ControlElementShape.ROUND_RECT, 0.72),
    createControlElement(ControlElementType.BUTTON, 0.06, 0.44, 'Shift', [keyBinding(42)], ControlElementShape.ROUND_RECT, 0.72, true),
    createControlElement(ControlElementType.BUTTON, 0.17, 0.44, 'Ctrl', [keyBinding(29)], ControlElementShape.ROUND_RECT, 0.72, true)
  ];
  // 视角摇杆默认输出鼠标视角。
  if (elements.length > 1 && elements[1].type === ControlElementType.STICK) {
    elements[1].outputMode = 'mouse';
  }
  return elements;
}

function templateElements(templateId: string): ControlElement[] {
  if (templateId === 'full-keyboard') return fullKeyboardTemplateElements();
  if (templateId === 'rpg') return rpgTemplateElements();
  if (templateId === 'fps' || templateId === 'shooter') return fpsTemplateElements();
  if (templateId === 'action') return actionTemplateElements();
  return defaultTemplateElements();
}

export function createInputProfileFromTemplate(id: string, name: string, templateId: string): InputProfile {
  return {
    id: id,
    name: name,
    schemaVersion: INPUT_PROFILE_SCHEMA_VERSION,
    cursorSpeed: 1,
    overlayOpacity: 70,
    gamepadMappings: cloneGamepadMappings(DEFAULT_GAMEPAD_MAPPINGS),
    elements: templateElements(templateId)
  };
}

export function createDefaultInputProfile(): InputProfile {
  return createInputProfileFromTemplate('default', '默认输入方案', 'default');
}

export function createBuiltInInputProfiles(): InputProfile[] {
  return INPUT_TEMPLATES.map((template: InputTemplate): InputProfile => {
    const profile = createInputProfileFromTemplate(
      template.builtInProfileId,
      template.profileName,
      template.id);
    profile.builtInRevision = template.revision;
    return profile;
  });
}

export function createDefaultGlobalSettings(): GlobalSettings {
  return {
    defaultDisplayMode: DisplayMode.DESKTOP,
    wineLanguage: WineLanguage.CHINESE,
    desktopOrientation: ScreenOrientationSetting.SENSOR_LANDSCAPE,
    desktopWindowMode: DesktopWindowMode.FULLSCREEN,
    keepScreenOn: true,
    box64Preset: BOX64_PRESET_DEFAULT,
    desktopInputProfileId: 'default',
    viewMode: ViewMode.GRID,
    themeMode: ThemeMode.SYSTEM,
    useGlassEffect: true,
    usePageBackground: false,
    pageBackgroundUri: '',
    virtualInputEnabled: true,
    showVirtualControls: true,
    autoHideVirtualControls: false,
    controlBarDirection: ControlBarDirection.AUTO,
    controlBarPositionX: 1,
    controlBarPositionY: 0,
    virtualMouseConfig: cloneVirtualMouseConfig(DEFAULT_VIRTUAL_MOUSE_CONFIG),
    controlOpacity: 72,
    gamepadDeadzone: 18,
    controllerOutputMode: 'dinput',
    outputScale: 100,
    d3dRendererPreference: D3DRendererPreference.DXVK,
    preserveUserDataOnReset: true,
    showFpsHud: false,
    performanceHud: normalizePerformanceHud()
  };
}

export function resolveInputProfileId(appProfileId: string, desktopProfileId: string): string {
  const appValue = (appProfileId ?? '').trim();
  if (appValue.length > 0 && appValue !== 'default') return appValue;
  const desktopValue = (desktopProfileId ?? '').trim();
  return desktopValue.length > 0 ? desktopValue : 'default';
}

export function createDefaultAppSettings(appId: string): AppSettings {
  return {
    appId: appId,
    customTitle: '',
    customCover: '',
    favorite: false,
    box64Preset: BOX64_PRESET_DEFAULT,
    executable: '',
    arguments: [],
    workingDirectory: '',
    displayModeOverride: null,
    inputProfileId: 'default',
    prefixId: 'default'
  };
}

export function normalizeLaunchPath(path: string): string {
  return path.trim().replace(/\\/g, '/').replace(/\/+/g, '/').toLowerCase();
}

/**
 * Wine maps the app-owned Download library to Z:\, so a program started
 * from Explorer is reported as Z:\games\... while the catalog stores the
 * same file as /storage/Users/currentUser/Download/<bundle>/games/....
 * Collapse only those two app-library forms; arbitrary C:\Games paths must
 * remain distinct from the app-owned games directory.
 */
export function normalizeCatalogLaunchIdentity(path: string): string {
  const normalized = normalizeLaunchPath(path);
  const gamesMarker = '/games/';
  if (normalized.startsWith('z:/games/')) return normalized.substring(3);
  if (normalized.startsWith('/storage/users/currentuser/download/')) {
    const markerIndex = normalized.indexOf(gamesMarker);
    if (markerIndex >= 0) return normalized.substring(markerIndex + 1);
  }
  return normalized;
}

/**
 * Merge ArkTS-owned sessions with the native Wine process registry.  A native
 * path is used only when it identifies one catalog card unambiguously.
 */
export function resolveRunningAppIds(apps: AppDescriptor[], sessionAppIds: Set<string>,
  nativeRunningPaths: Set<string>): Set<string> {
  const result: Set<string> = new Set<string>();
  sessionAppIds.forEach((appId: string): void => { result.add(appId); });

  const normalizedNativePaths: Set<string> = new Set<string>();
  nativeRunningPaths.forEach((path: string): void => {
    const normalized = normalizeCatalogLaunchIdentity(path);
    if (normalized.length > 0) normalizedNativePaths.add(normalized);
  });

  const catalogPathCounts: Map<string, number> = new Map<string, number>();
  apps.forEach((app: AppDescriptor): void => {
    const normalized = normalizeCatalogLaunchIdentity(app.launchTarget.executable);
    if (normalized.length > 0) {
      catalogPathCounts.set(normalized, (catalogPathCounts.get(normalized) ?? 0) + 1);
    }
  });
  apps.forEach((app: AppDescriptor): void => {
    const normalized = normalizeCatalogLaunchIdentity(app.launchTarget.executable);
    if (normalized.length > 0 && catalogPathCounts.get(normalized) === 1 &&
      normalizedNativePaths.has(normalized)) result.add(app.id);
  });
  return result;
}

export function stableAppId(source: AppSource, path: string): string {
  const value = source + ':' + normalizeLaunchPath(path);
  let hash = 2166136261;
  for (let index = 0; index < value.length; index++) {
    hash ^= value.charCodeAt(index);
    hash = Math.imul(hash, 16777619);
  }
  return source + '-' + (hash >>> 0).toString(16).padStart(8, '0');
}

interface IcpElementRaw {
  type?: string;
  shape?: string;
  bindings?: string[];
  scale?: number;
  /** VintagePomelo extension; other Winlator-compatible readers ignore it. */
  widthScale?: number;
  heightScale?: number;
  panelWidthRatio?: number;
  panelHeightRatio?: number;
  x?: number;
  y?: number;
  toggleSwitch?: boolean;
  text?: string;
  iconId?: number;
  orientation?: string;
  outputMode?: string;
}

interface IcpProfileRaw {
  id?: number;
  name?: string;
  cursorSpeed?: number;
  elements?: IcpElementRaw[];
}

function parseElementType(value: string): ControlElementType | null {
  if (value === 'PANEL') return ControlElementType.PANEL;
  if (value === 'BUTTON') return ControlElementType.BUTTON;
  if (value === 'D_PAD') return ControlElementType.D_PAD;
  if (value === 'STICK') return ControlElementType.STICK;
  if (value === 'TRACKPAD') return ControlElementType.TRACKPAD;
  if (value === 'RANGE_BUTTON') return ControlElementType.RANGE_BUTTON;
  return null;
}

function parseElementShape(value: string, type: ControlElementType): ControlElementShape {
  if (value === 'RECT') return ControlElementShape.RECT;
  if (value === 'ROUND_RECT') return ControlElementShape.ROUND_RECT;
  if (value === 'SQUARE') return ControlElementShape.SQUARE;
  if (value === 'CIRCLE') return ControlElementShape.CIRCLE;
  return type === ControlElementType.TRACKPAD || type === ControlElementType.RANGE_BUTTON
    ? ControlElementShape.ROUND_RECT : ControlElementShape.CIRCLE;
}

function clampNumber(value: number | undefined, min: number, max: number, fallback: number): number {
  if (value === undefined || Number.isNaN(value)) return fallback;
  return Math.max(min, Math.min(max, value));
}

function parseIcpBinding(value: string): InputBinding {
  const named = evdevNameToBinding(value);
  if (named.kind === 'MOUSE') return mouseBinding(named.code);
  if (named.kind === 'KEY') return keyBinding(named.code);
  return { kind: InputBindingKind.NONE, code: 0 };
}

/**
 * 解析 Winlator .icp JSON。未知元素类型跳过，未知字段忽略。
 * 导入的元素位置/绑定/形状尽量保留，透明度与可见性使用默认值。
 */
export function importProfileFromIcpJson(json: string, profileId: string, name?: string): InputProfile {
  const raw = JSON.parse(json) as IcpProfileRaw;
  const elements: ControlElement[] = [];
  const raws = raw.elements ?? [];
  for (let index = 0; index < raws.length; index++) {
    const el = raws[index];
    const type = parseElementType(el.type ?? '');
    if (type === null) continue;
    const rawBindings = (el.bindings ?? [])
      .slice(0, 8)
      .map((value: string): InputBinding => parseIcpBinding(value))
      .filter((binding: InputBinding): boolean => binding.kind !== InputBindingKind.NONE);
    let bindings: InputBinding[] = [];
    let keyList: number[] = [];
    if (type === ControlElementType.BUTTON) {
      bindings = rawBindings.slice(0, 4);
    } else if (type === ControlElementType.D_PAD || type === ControlElementType.STICK) {
      bindings = [
        rawBindings[0] ?? { kind: InputBindingKind.NONE, code: 0 },
        rawBindings[1] ?? { kind: InputBindingKind.NONE, code: 0 },
        rawBindings[2] ?? { kind: InputBindingKind.NONE, code: 0 },
        rawBindings[3] ?? { kind: InputBindingKind.NONE, code: 0 }
      ];
    } else if (type === ControlElementType.RANGE_BUTTON) {
      keyList = rawBindings.map((binding: InputBinding): number => binding.code);
      bindings = keyList.length > 0 ? [keyBinding(keyList[0])] : [];
    }
    elements.push({
      id: 'imp-' + index.toString(),
      type: type,
      shape: parseElementShape(el.shape ?? '', type),
      x: clampNumber(el.x, 0, 1, 0.5),
      y: clampNumber(el.y, 0, 1, 0.5),
      scale: clampNumber(el.scale, 0.1, 5, 1),
      widthScale: clampNumber(el.widthScale, 0.2, 12, 1),
      heightScale: clampNumber(el.heightScale, 0.2, 12, 1),
      panelWidthRatio: type === ControlElementType.PANEL ?
        clampNumber(el.panelWidthRatio, 0.1, 1, 0.84) : undefined,
      panelHeightRatio: type === ControlElementType.PANEL ?
        clampNumber(el.panelHeightRatio, 0.1, 1, 0.48) : undefined,
      opacity: -1,
      bindings: bindings.map(cloneInputBinding),
      toggleSwitch: el.toggleSwitch ?? false,
      text: (el.text ?? '').trim().slice(0, 24),
      iconId: 0,
      visible: true,
      keyList: keyList,
      orientation: el.orientation === 'horizontal' ? 'horizontal' : 'vertical',
      outputMode: el.outputMode === 'mouse' ? 'mouse' : 'keys'
    });
  }
  return {
    id: profileId,
    name: (name ?? raw.name ?? '导入方案').trim().slice(0, 40),
    schemaVersion: INPUT_PROFILE_SCHEMA_VERSION,
    cursorSpeed: clampNumber(raw.cursorSpeed, 0.1, 5, 1),
    overlayOpacity: 70,
    gamepadMappings: cloneGamepadMappings(DEFAULT_GAMEPAD_MAPPINGS),
    elements: elements.map(cloneControlElement)
  };
}

function exportBindingName(binding: InputBinding): string {
  if (binding.kind === InputBindingKind.MOUSE) return evdevNameToBindingName('MOUSE', binding.code);
  if (binding.kind === InputBindingKind.KEY) return evdevNameToBindingName('KEY', binding.code);
  return 'NONE';
}

function exportBindingNames(element: ControlElement): string[] {
  const names: string[] = [];
  const slotCount = 4;
  for (let slot = 0; slot < slotCount; slot++) {
    if (element.type === ControlElementType.RANGE_BUTTON) {
      const codes = element.keyList ?? [];
      names.push(slot < codes.length ? evdevNameToBindingName('KEY', codes[slot]) : 'NONE');
      continue;
    }
    if (element.type === ControlElementType.TRACKPAD) {
      names.push('NONE');
      continue;
    }
    const binding = (element.bindings ?? [])[slot];
    names.push(binding ? exportBindingName(binding) : 'NONE');
  }
  return names;
}

/**
 * 导出为 Winlator 兼容的 .icp JSON。widthScale 是本项目用于宽键的
 * 可选扩展字段，其他兼容读取器会按 JSON 规则忽略它。
 */
export function exportProfileToIcpJson(profile: InputProfile): string {
  const elements = (profile.elements ?? []).map((element: ControlElement): IcpElementRaw => ({
    type: element.type,
    shape: element.shape,
    bindings: exportBindingNames(element),
    scale: element.scale,
    widthScale: element.widthScale ?? 1,
    heightScale: element.heightScale ?? 1,
    panelWidthRatio: element.panelWidthRatio,
    panelHeightRatio: element.panelHeightRatio,
    x: element.x,
    y: element.y,
    toggleSwitch: element.toggleSwitch,
    text: element.text,
    iconId: element.iconId ?? 0,
    orientation: element.orientation ?? 'vertical',
    outputMode: element.outputMode ?? 'keys'
  }));
  const payload: IcpProfileRaw = {
    id: 0,
    name: profile.name,
    cursorSpeed: profile.cursorSpeed,
    elements: elements
  };
  return JSON.stringify(payload);
}

function evdevNameToBindingName(kind: string, code: number): string {
  if (kind === 'MOUSE') return bindingToEvdevName('MOUSE', code);
  return bindingToEvdevName('KEY', code);
}

export function resolveDisplayMode(override: DisplayMode | null, defaultMode: DisplayMode): DisplayMode {
  // 单应用全屏模式已移除，override 仅保留存储兼容，恒不生效。
  return override ?? defaultMode;
}

/**
 * Resolve the user-facing mode without treating the explicit Wine desktop
 * card as a normal Windows program.
 */
export function resolveLaunchDisplayMode(kind: LaunchKind, override: DisplayMode | null,
  defaultMode: DisplayMode): DisplayMode {
  // 单应用全屏模式已移除：统一使用桌面模式，让游戏在 Wine 内自行全屏。
  return DisplayMode.DESKTOP;
}

export function resolveWinePresentationMode(kind: LaunchKind, displayMode: DisplayMode,
  managedWindowsHost: boolean, virtualDesktop: boolean = false): WinePresentationMode {
  if (kind === LaunchKind.WINE_DESKTOP) return WinePresentationMode.DESKTOP;
  // PC 虚拟桌面模式 (VIRTUAL): 程序运行在桌面合成器内 (与平板同逻辑)。
  // 仅 PC 融合模式 (FUSION) 每个程序独立窗口。
  if (managedWindowsHost && !virtualDesktop) return WinePresentationMode.MANAGED_WINDOWS;
  return WinePresentationMode.DESKTOP;
}

export function canTransitionEngineState(from: EngineState, to: EngineState): boolean {
  if (from === to) return true;
  if (to === EngineState.ERROR || to === EngineState.STOPPED) return true;
  if (from === EngineState.STOPPED) return to === EngineState.PREPARING;
  if (from === EngineState.PREPARING) return to === EngineState.READY || to === EngineState.SWITCHING;
  if (from === EngineState.READY) return to === EngineState.SWITCHING;
  if (from === EngineState.SWITCHING) return to === EngineState.PREPARING;
  if (from === EngineState.ERROR) return to === EngineState.PREPARING;
  return false;
}
