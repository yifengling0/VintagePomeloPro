import {
  ControlBarDirection,
  ControlElement,
  ControlElementShape,
  ControlElementType,
  InputBinding,
  InputBindingKind,
  InputProfile,
  MOUSE_LEFT_BUTTON,
  MOUSE_RIGHT_BUTTON,
  createDefaultInputProfile,
  resolveVirtualJoystickDirection
} from '../model/AppModels';
import { InputDispatcher } from '../service/InputDispatcher';
import { InputOverlayWindowCoordinator } from '../service/InputOverlayWindowCoordinator';
import { mapMouseButton } from '../common/MouseMap';
import { display } from '@kit.ArkUI';
import testNapi from 'libentry.so';

type ToolbarAction = 'toolbar' | 'editor' | 'mapping' | 'controls' | 'mouse' | 'keyboard' | 'home' | 'wake';

interface KeyboardShieldRect {
  left: number;
  top: number;
  right: number;
  bottom: number;
}

interface OverlayPoint {
  x: number;
  y: number;
}

interface TrackpadState {
  startX: number;
  startY: number;
  lastX: number;
  lastY: number;
  moved: boolean;
  startTime: number;
}

interface StickVisualState {
  elementId: string;
  dx: number;
  dy: number;
  active: boolean;
}

/**
 * 覆盖层几何仍按 Winlator（描边宽度 overlayW/400、形状半径）。
 * 按键用偏深半透明底托住白字；十字键只画四向臂，不加整块光晕以免画出方框。
 * 整体透明度由 elementOpacity() 作用在控件 .opacity() 上。
 */
const OVERLAY_WHITE = '#FFFFFF';
const OVERLAY_BLACK = '#000000';
const OVERLAY_KEYCAP = '#2C2C2C';
const OVERLAY_HIGHLIGHT = '#0277BD';
const OVERLAY_CLEAR = '#00000000';

/** Toggle 按钮长按锁定阈值（毫秒）：短按=点按，长按=锁定保持。 */
const LONG_PRESS_LOCK_MS = 400;

/**
 * Winlator 几何 + 盖世双色对比的元素化虚拟输入覆盖层。
 * 控件由 profile.elements 驱动，每个元素使用归一化坐标任意排布，
 * 支持 BUTTON / D_PAD / STICK / TRACKPAD / RANGE_BUTTON 五种类型，
 * 多指并行注入不串键。
 */
@Component
export struct VirtualInputOverlay {
  @Prop @Watch('onProfileChanged') profile: InputProfile = createDefaultInputProfile();
  /** 编辑器预览强制重绘用。 */
  @Prop @Watch('onRenderRevisionChanged') renderRevision: number = 0;
  @Prop opacityValue: number = 100;
  @Prop interactive: boolean = true;
  @Prop hideToolbar: boolean = false;
  @Prop @Watch('onControlsVisibleChanged') controlsVisible: boolean = true;
  @Prop @Watch('onControlBarDirectionChanged') controlBarDirection: ControlBarDirection = ControlBarDirection.AUTO;
  @Prop @Watch('onControlBarPositionChanged') controlBarPositionX: number = 1;
  @Prop @Watch('onControlBarPositionChanged') controlBarPositionY: number = 0;
  @Prop @Watch('onTouchpadModeChanged') touchpadMode: boolean = false;
  /** 触摸板箭头指针；关闭后仍移动与点击，只是不画光标。 */
  @Prop showCursor: boolean = true;
  /** 浮窗模式全屏态: 控制栏显示 home 按钮 (缩回 Index 悬浮窗)。 */
  @StorageProp('desktopFloatingFullscreen') @Watch('onFloatingFullscreenChanged')
  desktopFloatingFullscreen: boolean = false;
  /** 编辑器模式：不注入按键，元素可拖动/缩放并支持选中框。 */
  @Prop editMode: boolean = false;
  @Prop @Watch('onCrosshairStateChanged') selectedElementId: string = '';
  private crosshairContext: CanvasRenderingContext2D = new CanvasRenderingContext2D();
  onOpenLayoutEditor: () => void = (): void => {};
  onOpenKeyMappingEditor: () => void = (): void => {};
  onToggleHostKeyboard: () => void = (): void => {};
  onControlsVisibleChange: (visible: boolean) => void = (_visible: boolean): void => {};
  onTouchpadModeChange: (enabled: boolean) => void = (_enabled: boolean): void => {};
  onControlBarPositionChange: (x: number, y: number) => void =
    (_x: number, _y: number): void => {};
  onUnhandledTouch: (event: TouchEvent) => void = (_event: TouchEvent): void => {};
  onUnhandledMouse: (event: MouseEvent) => void = (_event: MouseEvent): void => {};
  onUnhandledAxis: (event?: AxisEvent) => void = (_event?: AxisEvent): void => {};
  onElementMove: (id: string, x: number, y: number) => void = (_id: string, _x: number, _y: number): void => {};
  onElementScale: (id: string, scale: number) => void = (_id: string, _scale: number): void => {};
  onElementSelected: (id: string) => void = (_id: string): void => {};

  @State private overlayW: number = 0;
  @State private overlayH: number = 0;
  @State private pressedTokens: string = '';
  @State private stickStates: StickVisualState[] = [];
  @State private rangeTokens: string = '';
  @State private toolbarExpanded: boolean = true;
  @State private toolbarAutoHidden: boolean = false;
  @State private toolbarX: number = 4;
  @State private toolbarY: number = 8;
  @State private controlsVisibleVisual: boolean = true;
  @State private touchpadModeVisual: boolean = false;
  private activeElementTouches: Map<string, number> = new Map<string, number>();
  /** Toggle buttons stay here after the finger lifts so profile/page changes can release them safely. */
  private lockedToggleElements: Map<string, ControlElement> = new Map<string, ControlElement>();
  /** Toggle 按钮按下时刻（长按锁定判定）。 */
  private toggleDownTimes: Map<string, number> = new Map<string, number>();
  private touchOrigins: Map<string, OverlayPoint> = new Map<string, OverlayPoint>();
  private stickLastDeltas: Map<string, OverlayPoint> = new Map<string, OverlayPoint>();
  private trackpadStates: Map<string, TrackpadState> = new Map<string, TrackpadState>();
  private editDragElementId: string = '';
  private editDragTouchId: number = -1;
  private editDragStartX: number = 0;
  private editDragStartY: number = 0;
  private editDragOriginX: number = 0;
  private editDragOriginY: number = 0;
  private editPinchElementId: string = '';
  private editPinchStartDistance: number = 0;
  private editPinchStartScale: number = 1;
  private lastActionTouchAt: number = 0;
  private toolbarTouchId: number = -1;
  private toolbarTouchAction: ToolbarAction | null = null;
  private toolbarTouchStartX: number = 0;
  private toolbarTouchStartY: number = 0;
  private toolbarTouchOriginX: number = 0;
  private toolbarTouchOriginY: number = 0;
  private toolbarTouchMoved: boolean = false;
  private toolbarMouseAction: ToolbarAction | null = null;
  private toolbarMouseTracking: boolean = false;
  private toolbarMouseStartX: number = 0;
  private toolbarMouseStartY: number = 0;
  private toolbarMouseOriginX: number = 0;
  private toolbarMouseOriginY: number = 0;
  private toolbarMouseMoved: boolean = false;

  // -- 触摸板模式 (虚拟光标, 对齐 upstream main-ui DesktopLayer.handleTouchpad) --
  // 标准触控板语义: 单指滑动=移动光标, tap=左键, 双指轻点=右键,
  // 双击后按住拖动=拖拽窗口/元素, 双指拖动=滚动(作用于光标处)。
  // 与直接触控("指哪点哪")互斥 — 触控板"光标在哪点哪"。
  private static readonly DRAG_THRESHOLD: number = 10;      // 物理 px, 区分点击抖动
  private static readonly DOUBLE_TAP_MS: number = 300;       // 双击判定窗口
  private static readonly DOUBLE_TAP_DIST: number = 24;      // 双击允许光标位移 (px)
  private static readonly TWO_FINGER_TAP_MS: number = 250;   // 双指轻点=右键 判定窗口
  private static readonly SCROLL_STEP_PX: number = 30;       // 滚动步进 (px)
  private static readonly TRACKPAD_LONG_PRESS_MS: number = 600; // 视角板长按=右键
  private vcX: number = 0;        // 虚拟光标 (物理 px)
  private vcY: number = 0;
  private vcInit: boolean = false;
  private scrWPx: number = 0;
  private scrHPx: number = 0;
  @State private cursorVpX: number = -100;  // 箭头尖 vp 坐标 (屏外=隐藏)
  @State private cursorVpY: number = -100;
  // 双指轻点=右键: 两指快速落下又抬起且中点几乎不动
  private tpTwoFingerStartAt: number = 0;
  private tpTwoFingerStartMidX: number = 0;
  private tpTwoFingerStartMidY: number = 0;
  private tpDown: boolean = false;
  private tpMoved: boolean = false;
  private tpDownX: number = 0;
  private tpDownY: number = 0;
  private tpLastX: number = 0;
  private tpLastY: number = 0;
  private tpDownTime: number = 0;
  private tpDragArmed: boolean = false;  // 双击后按住: 拖拽预备
  private tpDragging: boolean = false;
  private tpDragTl: number = 0;
  private tpTwoFinger: boolean = false;
  private tpTwoFingerLastMidX: number = 0;
  private tpTwoFingerLastMidY: number = 0;
  private tpScrollAccumX: number = 0;
  private tpScrollAccumY: number = 0;
  private tpLastTapTime: number = 0;
  private tpLastTapVcX: number = 0;
  private tpLastTapVcY: number = 0;
  private lastToolbarTouchAt: number = 0;
  private lastToolbarButtonTouchAt: number = 0;
  private toolbarAutoHideTimer: number = -1;
  private mouseElementId: string = '';
  /** Mouse-binding Down/Up must share one point so a release does not warp to the widget. */
  private mouseBindingPoints: Map<string, OverlayPoint> = new Map<string, OverlayPoint>();

  // ---- 通用几何与颜色 ----

  private elementBaseSize(element: ControlElement): number {
    if (element.type === ControlElementType.D_PAD) return 156;
    if (element.type === ControlElementType.STICK) return 120;
    return 56;
  }

  /** Keep touch targets visually balanced from phones through large tablets. */
  private viewportControlScale(): number {
    if (this.overlayW <= 0 || this.overlayH <= 0) return 1;
    const longSide = Math.max(this.overlayW, this.overlayH);
    const shortSide = Math.min(this.overlayW, this.overlayH);
    return Math.max(0.85, Math.min(1.65, Math.min(longSide / 809, shortSide / 365)));
  }

  private elementWidth(element: ControlElement): number {
    if (element.type === ControlElementType.PANEL) {
      return this.overlayW * Math.max(0.1, Math.min(1, element.panelWidthRatio ?? 0.84));
    }
    const viewportScale = this.viewportControlScale();
    const widthScale = element.widthScale ?? 1;
    const keyWidthRatio = element.keyWidthRatio;
    if (keyWidthRatio !== undefined && keyWidthRatio > 0) {
      // 键盘键帽: keyWidthRatio 定基础宽度, 缩放与按键宽度系数继续生效。
      return this.overlayW * keyWidthRatio * (element.scale ?? 1) * widthScale;
    }
    if (element.type === ControlElementType.TRACKPAD) {
      return 150 * (element.scale ?? 1) * widthScale * viewportScale;
    }
    if (element.type === ControlElementType.RANGE_BUTTON) {
      return (element.orientation === 'horizontal' ? 180 : 44) *
        (element.scale ?? 1) * widthScale * viewportScale;
    }
    return this.elementBaseSize(element) * (element.scale ?? 1) * widthScale * viewportScale;
  }

  private elementHeight(element: ControlElement): number {
    if (element.type === ControlElementType.PANEL) {
      return this.overlayH * Math.max(0.1, Math.min(1, element.panelHeightRatio ?? 0.48));
    }
    const viewportScale = this.viewportControlScale();
    const heightScale = element.heightScale ?? 1;
    const keyHeightRatio = element.keyHeightRatio;
    if (keyHeightRatio !== undefined && keyHeightRatio > 0) {
      return this.overlayH * keyHeightRatio * (element.scale ?? 1) * heightScale;
    }
    if (element.type === ControlElementType.TRACKPAD) {
      return 100 * (element.scale ?? 1) * heightScale * viewportScale;
    }
    if (element.type === ControlElementType.RANGE_BUTTON) {
      return (element.orientation === 'horizontal' ? 44 : 180) *
        (element.scale ?? 1) * heightScale * viewportScale;
    }
    return this.elementBaseSize(element) * (element.scale ?? 1) * heightScale * viewportScale;
  }

  private elementCenter(element: ControlElement): OverlayPoint {
    return { x: this.overlayW * (element.x ?? 0.5), y: this.overlayH * (element.y ?? 0.5) };
  }

  private elementOpacity(element: ControlElement): number {
    const elementOpacity = (element.opacity ?? -1) >= 0 ? (element.opacity ?? 70) : this.profile.overlayOpacity;
    const base = Math.max(10, Math.min(100, elementOpacity));
    const global = Math.max(10, Math.min(100, this.opacityValue));
    return Math.max(0.16, Math.min(1, base / 100 * global / 100));
  }

  private selectedElementOrNull(): ControlElement | null {
    if (!this.editMode || this.selectedElementId.length === 0) return null;
    return this.elementById(this.selectedElementId);
  }

  /** 编辑器选中元素的红色十字参考线：贯穿全画布，便于网格对齐。 */
  @Builder
  private SelectedCrosshair(element: ControlElement | null) {
    if (element) {
      Canvas(this.crosshairContext)
        .width('100%').height('100%')
        .hitTestBehavior(HitTestMode.None)
        .onReady(() => this.drawCrosshair(element))
    }
  }

  private elementRadius(element: ControlElement): number {
    // 对齐 Winlator ControlElement.draw：ROUND_RECT 为半高胶囊，SQUARE 为 0.75s。
    if (element.shape === ControlElementShape.ROUND_RECT) {
      return this.elementHeight(element) * 0.5;
    }
    if (element.shape === ControlElementShape.RECT) {
      return 0;
    }
    if (element.shape === ControlElementShape.SQUARE) {
      return Math.min(this.elementWidth(element), this.elementHeight(element)) * 0.15;
    }
    return Math.min(this.elementWidth(element), this.elementHeight(element)) / 2;
  }

  /** snappingSize = viewWidth / 100，strokeWidth = snappingSize × 0.25。 */
  private overlayStrokeWidth(): number {
    return Math.max(1.25, this.overlayW / 400);
  }

  /** Pack #AARRGGBB. `rgb` may be #RGB, #RRGGBB, or #AARRGGBB. */
  private overlayArgb(rgb: string, alpha: number): string {
    const trimmed = rgb.startsWith('#') ? rgb.substring(1) : rgb;
    const body = trimmed.length > 6 ? trimmed.substring(trimmed.length - 6) : trimmed;
    const clamped = Math.max(0, Math.min(1, alpha));
    const value = Math.round(clamped * 255);
    const hex = value.toString(16).toUpperCase();
    const aHex = hex.length < 2 ? '0' + hex : hex;
    return '#' + aHex + body.toUpperCase();
  }

  /** Soft glow behind white strokes; not a hard second ring. */
  private overlayGlowColor(): string {
    return '#4D000000';
  }

  private overlayStrokeColor(element: ControlElement): string {
    if (this.editMode && this.selectedElementId === element.id) {
      return this.overlayArgb(OVERLAY_HIGHLIGHT, 0.95);
    }
    return this.overlayArgb(OVERLAY_WHITE, 0.95);
  }

  private overlayFill(element: ControlElement, pressed: boolean): string {
    if (pressed) {
      return this.overlayArgb(OVERLAY_WHITE, 0.70);
    }
    if ((element.keyWidthRatio ?? 0) > 0) {
      return this.overlayArgb(OVERLAY_KEYCAP, 0.42);
    }
    if (element.type === ControlElementType.D_PAD) {
      return this.overlayArgb(OVERLAY_BLACK, 0.14);
    }
    if (element.type === ControlElementType.TRACKPAD) {
      return this.overlayArgb(OVERLAY_BLACK, 0.12);
    }
    if (element.type === ControlElementType.RANGE_BUTTON) {
      return this.overlayArgb(OVERLAY_BLACK, 0.32);
    }
    // Buttons: dark enough that white labels read on Notepad, still see-through.
    return this.overlayArgb(OVERLAY_BLACK, 0.45);
  }

  private overlayThumbFill(element: ControlElement, active: boolean): string {
    if (active) {
      return this.overlayArgb(OVERLAY_WHITE, 0.45);
    }
    // Winlator ColorUtils.setAlphaComponent(light, 50).
    return this.overlayArgb(OVERLAY_WHITE, 0.20);
  }

  private overlayTextColor(pressed: boolean): string {
    return pressed ? '#FF000000' : '#FFFFFFFF';
  }

  /** 键盘键帽文字按实际键面尺寸放大, 保证平板/手机上清晰可读。 */
  private buttonLabelFontSize(element: ControlElement): number {
    if ((element.keyWidthRatio ?? 0) > 0) {
      const size = Math.min(this.elementWidth(element), this.elementHeight(element));
      return Math.max(13, Math.min(30, size * 0.38));
    }
    return 14 * (element.scale ?? 1);
  }

  /** 键盘区域不可见的点击屏蔽层: 防止键缝/键盘边缘的触摸穿透到 Wine 窗口。 */
  private keyboardShieldRect(): KeyboardShieldRect | null {
    let left = 1;
    let top = 1;
    let right = 0;
    let bottom = 0;
    let found = false;
    for (const element of this.visibleElements()) {
      const widthRatio = element.keyWidthRatio ?? 0;
      const heightRatio = element.keyHeightRatio ?? 0;
      if (widthRatio <= 0 || heightRatio <= 0) continue;
      found = true;
      const elementScale = element.scale ?? 1;
      left = Math.min(left, (element.x ?? 0.5) - widthRatio * elementScale / 2);
      right = Math.max(right, (element.x ?? 0.5) + widthRatio * elementScale / 2);
      top = Math.min(top, (element.y ?? 0.5) - heightRatio * elementScale / 2);
      bottom = Math.max(bottom, (element.y ?? 0.5) + heightRatio * elementScale / 2);
    }
    if (!found) return null;
    const margin = 0.014;
    const rect: KeyboardShieldRect = {
      left: Math.max(0, left - margin),
      top: Math.max(0, top - margin),
      right: Math.min(1, right + margin),
      // 键盘下方的空白区域同样拦截, 防止触摸穿透到下层 Wine 窗口。
      bottom: 1
    };
    return rect;
  }

  private keyboardShieldContains(x: number, y: number): boolean {
    if (this.editMode || this.overlayW <= 0 || this.overlayH <= 0) return false;
    const rect = this.keyboardShieldRect();
    if (!rect) return false;
    const nx = x / this.overlayW;
    const ny = y / this.overlayH;
    return nx >= rect.left && nx <= rect.right && ny >= rect.top && ny <= rect.bottom;
  }

  private rectContains(x: number, y: number, left: number, top: number, width: number, height: number): boolean {
    return x >= left && x <= left + width && y >= top && y <= top + height;
  }

  private elementContains(element: ControlElement, x: number, y: number): boolean {
    if (element.type === ControlElementType.PANEL) return false;
    let width = this.elementWidth(element);
    let height = this.elementHeight(element);
    if (element.type === ControlElementType.RANGE_BUTTON) {
      // 命中区比视觉条稍宽/高，方便手指点按。
      const horizontal = element.orientation === 'horizontal';
      const expand = 68 * (element.scale ?? 1);
      width = horizontal ? width : Math.max(width, expand);
      height = horizontal ? Math.max(height, expand) : height;
    }
    const center = this.elementCenter(element);
    const left = center.x - width / 2;
    const top = center.y - height / 2;
    if (element.shape === ControlElementShape.CIRCLE) {
      const radius = Math.min(width, height) / 2;
      const dx = x - center.x;
      const dy = y - center.y;
      return dx * dx + dy * dy <= radius * radius;
    }
    return this.rectContains(x, y, left, top, width, height);
  }

  /** 虚拟控件（含视角触摸板）在触摸板模式下仍要自己吃触摸。 */
  private isOverlayControl(element: ControlElement): boolean {
    return element.type === ControlElementType.BUTTON ||
      element.type === ControlElementType.RANGE_BUTTON ||
      element.type === ControlElementType.TRACKPAD ||
      element.type === ControlElementType.D_PAD ||
      element.type === ControlElementType.STICK;
  }

  private elementAt(x: number, y: number): ControlElement | null {
    const elements = this.profile.elements ?? [];
    for (let index = elements.length - 1; index >= 0; index--) {
      const element = elements[index];
      if (element.visible && this.elementContains(element, x, y)) return element;
    }
    return null;
  }

  private elementById(id: string): ControlElement | null {
    const elements = this.profile.elements ?? [];
    for (let index = 0; index < elements.length; index++) {
      if (elements[index].id === id) return elements[index];
    }
    return null;
  }

  private activeElementIdForTouch(touchId: number): string {
    for (const entry of this.activeElementTouches.entries()) {
      if (entry[1] === touchId) return entry[0];
    }
    return '';
  }

  private visibleElements(): ControlElement[] {
    return (this.profile.elements ?? []).filter((element: ControlElement): boolean =>
      element.visible || this.editMode);
  }

  private elementVisualKey(element: ControlElement): string {
    const bindingParts: string[] = [];
    (element.bindings ?? []).forEach((binding: InputBinding): void => {
      bindingParts.push(binding.kind + ':' + binding.code.toString());
    });
    (element.keyList ?? []).forEach((code: number): void => {
      bindingParts.push('k' + code.toString());
    });
    return element.id +
      '|' + (element.x ?? 0).toFixed(3) +
      '|' + (element.y ?? 0).toFixed(3) +
      '|' + (element.scale ?? 1).toFixed(2) +
      '|' + (element.widthScale ?? 1).toFixed(2) +
      '|' + (element.heightScale ?? 1).toFixed(2) +
      '|' + (element.panelWidthRatio ?? 0).toFixed(3) +
      '|' + (element.panelHeightRatio ?? 0).toFixed(3) +
      '|' + this.overlayW.toFixed(0) + 'x' + this.overlayH.toFixed(0) +
      '|' + (element.opacity ?? -1).toString() +
      '|' + (element.text ?? '') +
      '|' + (element.shape ?? '') +
      '|' + (element.orientation ?? 'vertical') +
      '|' + (element.toggleSwitch ? '1' : '0') +
      '|' + (element.visible ? '1' : '0') +
      '|' + bindingParts.join(',');
  }

  private isElementPressed(id: string): boolean {
    return this.pressedTokens.indexOf('|' + id + '|') >= 0;
  }

  private setElementPressed(id: string, pressed: boolean): void {
    const token = '|' + id + '|';
    if (pressed && !this.isElementPressed(id)) this.pressedTokens += token;
    if (!pressed && this.isElementPressed(id)) {
      this.pressedTokens = this.pressedTokens.replace(token, '');
    }
  }

  private stickState(id: string): StickVisualState {
    for (let index = 0; index < this.stickStates.length; index++) {
      if (this.stickStates[index].elementId === id) return this.stickStates[index];
    }
    return { elementId: id, dx: 0, dy: 0, active: false };
  }

  private setStickState(id: string, dx: number, dy: number, active: boolean): void {
    const next: StickVisualState[] = this.stickStates.filter((state: StickVisualState): boolean =>
      state.elementId !== id);
    next.push({ elementId: id, dx: dx, dy: dy, active: active });
    this.stickStates = next;
  }

  private rangeSelection(id: string): number {
    const token = '|' + id + ':';
    const start = this.rangeTokens.indexOf(token);
    if (start < 0) return -1;
    const valueStart = start + token.length;
    const valueEnd = this.rangeTokens.indexOf('|', valueStart);
    if (valueEnd < 0) return -1;
    return Number(this.rangeTokens.substring(valueStart, valueEnd));
  }

  private setRangeSelection(id: string, index: number): void {
    let tokens = this.rangeTokens;
    // 用字符串方式删除该元素旧的选区 token，避免 RegExp 中未转义的
    // '|'（正则“或”语义）导致旧 token 残留、indexOf 恒取第一个。
    const prefix = '|' + id + ':';
    let start = tokens.indexOf(prefix);
    while (start >= 0) {
      const valueEnd = tokens.indexOf('|', start + prefix.length);
      if (valueEnd < 0) break;
      tokens = tokens.substring(0, start) + tokens.substring(valueEnd);
      start = tokens.indexOf(prefix);
    }
    if (index >= 0) tokens += '|' + id + ':' + index.toString() + '|';
    this.rangeTokens = tokens;
  }

  private stickRadius(element: ControlElement): number {
    return Math.min(this.elementWidth(element), this.elementHeight(element)) / 2 -
      this.overlayStrokeWidth();
  }

  private stickKnobSize(element: ControlElement): number {
    // Winlator thumbRadius = 3.5s，外环半径 6s，直径比 7/12。
    return Math.min(this.elementWidth(element), this.elementHeight(element)) * (7 / 12);
  }

  private bindingSource(element: ControlElement, slot: string, touchId: number): string {
    // A toggle can be unlocked by a later finger/mouse event, so its input
    // source must not depend on the transient touch id.
    const sourceId = element.toggleSwitch ? 'toggle' : touchId.toString();
    return 'el-' + element.id + '-' + slot + '-' + sourceId;
  }

  private ensureVirtualCursor(): void {
    this.ensureScreenPx();
    if (!this.vcInit && this.scrWPx > 0 && this.scrHPx > 0) {
      this.vcInit = true;
      this.vcX = this.scrWPx / 2;
      this.vcY = this.scrHPx / 2;
      this.syncCursorIndicator();
      InputDispatcher.getInstance().rememberPointer(this.vcX, this.vcY);
    }
  }

  /** Overlay mouse keys click at the cursor, never at the widget. */
  private mouseInjectionPoint(): OverlayPoint | null {
    if (this.touchpadModeVisual) {
      this.ensureVirtualCursor();
      return { x: this.vcX, y: this.vcY };
    }
    const last = InputDispatcher.getInstance().lastPointer();
    if (last.valid) return { x: last.x, y: last.y };
    this.ensureScreenPx();
    if (this.scrWPx > 0 && this.scrHPx > 0) {
      return { x: this.scrWPx / 2, y: this.scrHPx / 2 };
    }
    return null;
  }

  private injectMouseBinding(source: string, code: number, pressed: boolean): void {
    if (pressed) {
      const point = this.mouseInjectionPoint();
      if (point) {
        this.mouseBindingPoints.set(source, point);
        InputDispatcher.getInstance().pointerDown(source, point.x, point.y, code);
        return;
      }
      InputDispatcher.getInstance().pointerButton(source, code, true);
      return;
    }
    const held = this.mouseBindingPoints.get(source);
    this.mouseBindingPoints.delete(source);
    if (held) {
      InputDispatcher.getInstance().pointerUp(source, held.x, held.y, code);
      return;
    }
    InputDispatcher.getInstance().pointerButton(source, code, false);
  }

  private pressBinding(element: ControlElement, slot: string, bindingIndex: number, touchId: number,
    pointerX: number, pointerY: number): void {
    const binding = (element.bindings ?? [])[bindingIndex];
    if (!binding || binding.kind === InputBindingKind.NONE || binding.code <= 0) return;
    const source = this.bindingSource(element, slot, touchId);
    if (binding.kind === InputBindingKind.MOUSE) {
      this.injectMouseBinding(source, binding.code, true);
    } else {
      InputDispatcher.getInstance().pressKey(source, binding.code);
    }
  }

  private releaseBinding(element: ControlElement, slot: string, bindingIndex: number, touchId: number,
    pointerX: number, pointerY: number): void {
    const binding = (element.bindings ?? [])[bindingIndex];
    if (!binding || binding.kind === InputBindingKind.NONE || binding.code <= 0) return;
    const source = this.bindingSource(element, slot, touchId);
    if (binding.kind === InputBindingKind.MOUSE) {
      this.injectMouseBinding(source, binding.code, false);
    } else {
      InputDispatcher.getInstance().releaseKey(source, binding.code);
    }
    InputDispatcher.getInstance().releaseSource(source);
  }

  private applyButtonBindings(element: ControlElement, pressed: boolean, touchId: number,
    pointerX: number, pointerY: number): void {
    const bindings = element.bindings ?? [];
    for (let index = 0; index < bindings.length; index++) {
      if (pressed) this.pressBinding(element, 'b' + index.toString(), index, touchId, pointerX, pointerY);
      else this.releaseBinding(element, 'b' + index.toString(), index, touchId, pointerX, pointerY);
    }
  }

  private setDirectionSlot(element: ControlElement, slot: number, pressed: boolean, touchId: number): void {
    const binding = (element.bindings ?? [])[slot];
    if (!binding || binding.kind !== InputBindingKind.KEY || binding.code <= 0) return;
    const source = this.bindingSource(element, 'd' + slot.toString(), touchId);
    if (pressed) {
      InputDispatcher.getInstance().pressKey(source, binding.code);
    } else {
      InputDispatcher.getInstance().releaseKey(source, binding.code);
      InputDispatcher.getInstance().releaseSource(source);
    }
  }

  private hubGamepadActive(): boolean {
    try {
      return testNapi.controllerGetOutputMode() === 'dinput';
    } catch (_) {
      return false;
    }
  }

  /** Arrow-bound sticks are the right analog; WASD / everything else is left. */
  private stickIsRight(element: ControlElement): boolean {
    const binding = (element.bindings ?? [])[0];
    return !!binding && binding.kind === InputBindingKind.KEY && binding.code === 103;
  }

  private applyStickDirection(element: ControlElement, dx: number, dy: number, touchId: number): void {
    if (this.hubGamepadActive()) {
      const radius = this.stickRadius(element);
      const nx = radius > 0 ? Math.max(-1, Math.min(1, dx / radius)) : 0;
      // Screen +Y is down. Canonical stick +Y is Up (Touch adapter, not a WHGP fix).
      const ny = radius > 0 ? Math.max(-1, Math.min(1, -dy / radius)) : 0;
      InputDispatcher.getInstance().setLogicalStick(this.stickIsRight(element) ? 1 : 0, nx, ny);
    }
    // Analog is additive. Bound keys (WASD / arrows) still fire.
    const direction = resolveVirtualJoystickDirection(dx, dy, this.stickRadius(element), 15, true);
    this.setDirectionSlot(element, 0, direction.up, touchId);
    this.setDirectionSlot(element, 1, direction.right, touchId);
    this.setDirectionSlot(element, 2, direction.down, touchId);
    this.setDirectionSlot(element, 3, direction.left, touchId);
  }

  private releaseStickBindings(element: ControlElement, touchId: number): void {
    if (this.hubGamepadActive()) {
      if (element.type === ControlElementType.STICK && element.outputMode !== 'mouse') {
        InputDispatcher.getInstance().setLogicalStick(this.stickIsRight(element) ? 1 : 0, 0, 0);
      } else if (element.type === ControlElementType.D_PAD) {
        InputDispatcher.getInstance().setLogicalHat(0, 0);
      }
    }
    for (let slot = 0; slot < 4; slot++) {
      this.setDirectionSlot(element, slot, false, touchId);
    }
  }

  private rangeSlotTop(element: ControlElement): number {
    const scale = element.scale ?? 1;
    if (element.orientation === 'horizontal') return 0;
    return (element.text.length > 0 ? 20 : 0) * scale;
  }

  private rangeSlotHeight(element: ControlElement): number {
    const slots = element.keyList ?? [];
    if (slots.length === 0) return 0;
    const size = element.orientation === 'horizontal' ?
      this.elementWidth(element) : this.elementHeight(element);
    const slotArea = Math.max(1, size - this.rangeSlotTop(element));
    return slotArea / slots.length;
  }

  private rangeIndexAt(element: ControlElement, point: number): number {
    const slots = element.keyList ?? [];
    if (slots.length === 0) return -1;
    const horizontal = element.orientation === 'horizontal';
    const size = horizontal ? this.elementWidth(element) : this.elementHeight(element);
    const center = horizontal ? this.elementCenter(element).x : this.elementCenter(element).y;
    const relative = point - (center - size / 2) - this.rangeSlotTop(element);
    const slot = Math.floor(relative / this.rangeSlotHeight(element));
    return Math.max(0, Math.min(slots.length - 1, slot));
  }

  private applyRangeBinding(element: ControlElement, index: number, pressed: boolean, touchId: number): void {
    const slots = element.keyList ?? [];
    if (index < 0 || index >= slots.length) return;
    const source = this.bindingSource(element, 'r' + index.toString(), touchId);
    if (pressed) {
      InputDispatcher.getInstance().pressKey(source, slots[index]);
    } else {
      InputDispatcher.getInstance().releaseKey(source, slots[index]);
      InputDispatcher.getInstance().releaseSource(source);
    }
  }

  private releaseElementBindings(element: ControlElement, touchId: number): void {
    if (element.type === ControlElementType.BUTTON) {
      this.applyButtonBindings(element, false, touchId, 0, 0);
    } else if (element.type === ControlElementType.D_PAD || element.type === ControlElementType.STICK) {
      this.releaseStickBindings(element, touchId);
    } else if (element.type === ControlElementType.RANGE_BUTTON) {
      this.applyRangeBinding(element, this.rangeSelection(element.id), false, touchId);
    }
  }

  private releaseAllElementTouches(): void {
    this.lockedToggleElements.forEach((element: ControlElement): void => {
      this.applyButtonBindings(element, false, 0, 0, 0);
    });
    this.lockedToggleElements.clear();
    const entries = Array.from(this.activeElementTouches.entries());
    for (let index = 0; index < entries.length; index++) {
      const element = this.elementById(entries[index][0]);
      // Toggle buttons do not inject anything until TouchType.Up. A locked
      // toggle was already released from lockedToggleElements above.
      if (element && !element.toggleSwitch) this.releaseElementBindings(element, entries[index][1]);
    }
    this.activeElementTouches.clear();
    this.touchOrigins.clear();
    this.trackpadStates.clear();
    this.pressedTokens = '';
    this.stickStates = [];
    this.rangeTokens = '';
    this.mouseBindingPoints.clear();
  }

  // ---- 触摸处理 ----

  private handleElementTouch(event: TouchEvent, element: ControlElement): void {
    for (let index = 0; index < event.changedTouches.length; index++) {
      const touch = event.changedTouches[index];
      if (element.type === ControlElementType.BUTTON) {
        this.handleButtonElementTouch(event.type, touch, element);
      } else if (element.type === ControlElementType.D_PAD) {
        this.handleDpadElementTouch(event.type, touch, element);
      } else if (element.type === ControlElementType.STICK) {
        this.handleStickElementTouch(event.type, touch, element);
      } else if (element.type === ControlElementType.TRACKPAD) {
        this.handleTrackpadElementTouch(event.type, touch, element);
      } else {
        this.handleRangeElementTouch(event.type, touch, element);
      }
    }
  }

  private handleButtonElementTouch(type: TouchType, touch: TouchObject, element: ControlElement): void {
    const id = element.id;
    if (type === TouchType.Down) {
      if (this.activeElementTouches.has(id)) return;
      this.activeElementTouches.set(id, touch.id);
      this.lastActionTouchAt = Date.now();
      if (element.toggleSwitch) {
        // 长按锁定模式：Down 先按下；短按 Up 释放（普通点按），长按 Up 锁定保持。
        this.toggleDownTimes.set(id, Date.now());
        if (!this.isElementPressed(id)) {
          this.applyButtonBindings(element, true, touch.id, vp2px(Number(touch.x)), vp2px(Number(touch.y)));
          this.setElementPressed(id, true);
        }
        // 已锁定：保持按下，不重复注入。
      } else {
        this.applyButtonBindings(element, true, touch.id, vp2px(Number(touch.x)), vp2px(Number(touch.y)));
        this.setElementPressed(id, true);
      }
      return;
    }
    if ((type === TouchType.Up || type === TouchType.Cancel) &&
      this.activeElementTouches.get(id) === touch.id) {
      this.activeElementTouches.delete(id);
      if (element.toggleSwitch) {
        if (type === TouchType.Cancel) {
          // 取消手势：释放并清除锁定。
          this.toggleDownTimes.delete(id);
          if (this.isElementPressed(id)) {
            this.applyButtonBindings(element, false, touch.id, vp2px(Number(touch.x)), vp2px(Number(touch.y)));
            this.setElementPressed(id, false);
          }
          this.lockedToggleElements.delete(id);
          return;
        }
        const downTime = this.toggleDownTimes.get(id) ?? 0;
        this.toggleDownTimes.delete(id);
        const held = Date.now() - downTime;
        if (held >= LONG_PRESS_LOCK_MS) {
          // 长按：锁定按下（保持）。
          this.lockedToggleElements.set(id, element);
        } else {
          // 短按：释放（普通点按）或解锁已锁定的按键。
          if (this.isElementPressed(id)) {
            this.applyButtonBindings(element, false, touch.id, vp2px(Number(touch.x)), vp2px(Number(touch.y)));
            this.setElementPressed(id, false);
          }
          this.lockedToggleElements.delete(id);
        }
        return;
      }
      if (this.isElementPressed(id)) {
        this.applyButtonBindings(element, false, touch.id, vp2px(Number(touch.x)), vp2px(Number(touch.y)));
        this.setElementPressed(id, false);
      }
    }
  }

  private handleStickElementTouch(type: TouchType, touch: TouchObject, element: ControlElement): void {
    const id = element.id;
    if (type === TouchType.Down) {
      if (this.activeElementTouches.has(id)) return;
      this.activeElementTouches.set(id, touch.id);
      this.touchOrigins.set(id, { x: Number(touch.x), y: Number(touch.y) });
      this.stickLastDeltas.set(id, { x: 0, y: 0 });
      this.setStickState(id, 0, 0, true);
      return;
    }
    if (type === TouchType.Move && this.activeElementTouches.get(id) === touch.id) {
      const origin = this.touchOrigins.get(id);
      if (!origin) return;
      let dx = Number(touch.x) - origin.x;
      let dy = Number(touch.y) - origin.y;
      const radius = this.stickRadius(element);
      const distance = Math.sqrt(dx * dx + dy * dy);
      if (distance > radius) {
        dx = dx * radius / distance;
        dy = dy * radius / distance;
      }
      this.setStickState(id, dx, dy, true);
      if (element.outputMode === 'mouse') {
        // 鼠标视角：摇杆偏移增量 → 指针增量。偏移越大单次视角转动越快。
        const last = this.stickLastDeltas.get(id) ?? { x: 0, y: 0 };
        const rate = 0.9 * Math.max(0.1, Math.min(5, this.profile.cursorSpeed));
        const deltaX = (dx - last.x) * rate;
        const deltaY = (dy - last.y) * rate;
        this.stickLastDeltas.set(id, { x: dx, y: dy });
        if (deltaX !== 0 || deltaY !== 0) {
          InputDispatcher.getInstance().pointerDelta(
            'stick-mouse-' + id + '-' + touch.id.toString(), vp2px(deltaX), vp2px(deltaY));
        }
      } else {
        this.applyStickDirection(element, dx, dy, touch.id);
      }
      return;
    }
    if ((type === TouchType.Up || type === TouchType.Cancel) &&
      this.activeElementTouches.get(id) === touch.id) {
      this.activeElementTouches.delete(id);
      this.touchOrigins.delete(id);
      this.stickLastDeltas.delete(id);
      this.releaseStickBindings(element, touch.id);
      this.setStickState(id, 0, 0, false);
    }
  }

  private isDpadDirectionPressed(id: string, direction: string): boolean {
    return this.pressedTokens.indexOf('|' + id + ':' + direction + '|') >= 0;
  }

  private setDpadDirectionPressed(id: string, direction: string, pressed: boolean): void {
    const token = '|' + id + ':' + direction + '|';
    if (pressed && !this.isDpadDirectionPressed(id, direction)) this.pressedTokens += token;
    if (!pressed && this.isDpadDirectionPressed(id, direction)) {
      this.pressedTokens = this.pressedTokens.replace(token, '');
    }
  }

  private clearDpadDirection(id: string): void {
    this.setDpadDirectionPressed(id, 'up', false);
    this.setDpadDirectionPressed(id, 'down', false);
    this.setDpadDirectionPressed(id, 'left', false);
    this.setDpadDirectionPressed(id, 'right', false);
  }

  private setDpadSlot(element: ControlElement, slot: number, pressed: boolean, touchId: number): void {
    const direction = slot === 0 ? 'up' : slot === 1 ? 'right' : slot === 2 ? 'down' : 'left';
    if (this.isDpadDirectionPressed(element.id, direction) === pressed) return;
    this.setDpadDirectionPressed(element.id, direction, pressed);
    this.setDirectionSlot(element, slot, pressed, touchId);
  }

  private applyDpadDirection(element: ControlElement, x: number, y: number, touchId: number): void {
    const center = this.elementCenter(element);
    const dx = x - center.x;
    const dy = y - center.y;
    let up = false;
    let down = false;
    let left = false;
    let right = false;
    if (Math.abs(dx) > Math.abs(dy)) {
      if (dx > 0) right = true;
      else left = true;
    } else {
      if (dy > 0) down = true;
      else up = true;
    }
    if (this.hubGamepadActive()) {
      const hx = right ? 1 : (left ? -1 : 0);
      const hy = up ? 1 : (down ? -1 : 0);
      InputDispatcher.getInstance().setLogicalHat(hx, hy);
    }
    // Hat is additive. Bound keys still fire (and drive highlight).
    this.setDpadSlot(element, 0, up, touchId);
    this.setDpadSlot(element, 1, right, touchId);
    this.setDpadSlot(element, 2, down, touchId);
    this.setDpadSlot(element, 3, left, touchId);
  }

  /** 方向键：按下即按触摸位置触发对应方向，移动换向，抬起释放。 */
  private handleDpadElementTouch(type: TouchType, touch: TouchObject, element: ControlElement): void {
    const id = element.id;
    if (type === TouchType.Down) {
      if (this.activeElementTouches.has(id)) return;
      this.activeElementTouches.set(id, touch.id);
      this.lastActionTouchAt = Date.now();
      this.applyDpadDirection(element, Number(touch.x), Number(touch.y), touch.id);
      return;
    }
    if (type === TouchType.Move && this.activeElementTouches.get(id) === touch.id) {
      this.applyDpadDirection(element, Number(touch.x), Number(touch.y), touch.id);
      return;
    }
    if ((type === TouchType.Up || type === TouchType.Cancel) &&
      this.activeElementTouches.get(id) === touch.id) {
      this.activeElementTouches.delete(id);
      this.releaseStickBindings(element, touch.id);
      this.clearDpadDirection(id);
    }
  }

  private updateTrackpadThumb(element: ControlElement, x: number, y: number): void {
    const center = this.elementCenter(element);
    const maxX = Math.max(4, this.elementWidth(element) / 2 - 10);
    const maxY = Math.max(4, this.elementHeight(element) / 2 - 10);
    let dx = x - center.x;
    let dy = y - center.y;
    dx = Math.min(Math.max(dx, -maxX), maxX);
    dy = Math.min(Math.max(dy, -maxY), maxY);
    this.setStickState(element.id, dx, dy, true);
  }

  private handleTrackpadElementTouch(type: TouchType, touch: TouchObject, element: ControlElement): void {
    const id = element.id;
    if (type === TouchType.Down) {
      if (this.activeElementTouches.has(id)) return;
      this.activeElementTouches.set(id, touch.id);
      this.lastActionTouchAt = Date.now();
      this.setElementPressed(id, true);
      this.trackpadStates.set(id, {
        startX: Number(touch.x), startY: Number(touch.y),
        lastX: Number(touch.x), lastY: Number(touch.y), moved: false,
        startTime: Date.now()
      });
      this.updateTrackpadThumb(element, Number(touch.x), Number(touch.y));
      return;
    }
    if (type === TouchType.Move && this.activeElementTouches.get(id) === touch.id) {
      const state = this.trackpadStates.get(id);
      if (!state) return;
      const x = Number(touch.x);
      const y = Number(touch.y);
      const dx = x - state.lastX;
      const dy = y - state.lastY;
      state.lastX = x;
      state.lastY = y;
      this.updateTrackpadThumb(element, x, y);
      if (!state.moved && Math.abs(x - state.startX) + Math.abs(y - state.startY) >= 12) {
        state.moved = true;
      }
      if (state.moved) {
        const speed = Math.max(0.1, Math.min(5, this.profile.cursorSpeed));
        const dxPx = vp2px(dx) * speed;
        const dyPx = vp2px(dy) * speed;
        InputDispatcher.getInstance().pointerDelta(
          'trackpad-' + id + '-' + touch.id.toString(), dxPx, dyPx);
        if (this.touchpadModeVisual) this.applyCursorDelta(dxPx, dyPx);
      }
      return;
    }
    if ((type === TouchType.Up || type === TouchType.Cancel) &&
      this.activeElementTouches.get(id) === touch.id) {
      const state = this.trackpadStates.get(id);
      this.activeElementTouches.delete(id);
      this.trackpadStates.delete(id);
      this.setElementPressed(id, false);
      this.setStickState(id, 0, 0, false);
      if (type === TouchType.Up && state && !state.moved) {
        const elapsed = Date.now() - state.startTime;
        const button = elapsed >= VirtualInputOverlay.TRACKPAD_LONG_PRESS_MS ?
          MOUSE_RIGHT_BUTTON : MOUSE_LEFT_BUTTON;
        const source = 'trackpad-tap-' + id + '-' + touch.id.toString();
        this.injectMouseBinding(source, button, true);
        this.injectMouseBinding(source, button, false);
      }
    }
  }

  private handleRangeElementTouch(type: TouchType, touch: TouchObject, element: ControlElement): void {
    const id = element.id;
    const slots = element.keyList ?? [];
    if (slots.length === 0) return;
    if (type === TouchType.Down) {
      if (this.activeElementTouches.has(id)) return;
      this.activeElementTouches.set(id, touch.id);
      this.lastActionTouchAt = Date.now();
      const point = element.orientation === 'horizontal' ? Number(touch.x) : Number(touch.y);
      const index = this.rangeIndexAt(element, point);
      this.setRangeSelection(id, index);
      return;
    }
    if (type === TouchType.Move && this.activeElementTouches.get(id) === touch.id) {
      const point = element.orientation === 'horizontal' ? Number(touch.x) : Number(touch.y);
      const next = this.rangeIndexAt(element, point);
      this.setRangeSelection(id, next);
      return;
    }
    if ((type === TouchType.Up || type === TouchType.Cancel) &&
      this.activeElementTouches.get(id) === touch.id) {
      this.activeElementTouches.delete(id);
      if (type === TouchType.Up) {
        const index = this.rangeSelection(id);
        if (index >= 0) {
          this.applyRangeBinding(element, index, true, touch.id);
          this.applyRangeBinding(element, index, false, touch.id);
        }
      }
    }
  }

  // -- 触摸板模式: 虚拟光标状态机 (移植 upstream main-ui DesktopLayer.handleTouchpad) --
  private ensureScreenPx(): void {
    if (this.overlayW > 0 && this.overlayH > 0) {
      this.scrWPx = vp2px(this.overlayW);
      this.scrHPx = vp2px(this.overlayH);
      return;
    }
    const rect = InputOverlayWindowCoordinator.getInstance().getOverlayWindowRect();
    if (rect.width > 0 && rect.height > 0) {
      this.scrWPx = rect.width;
      this.scrHPx = rect.height;
      return;
    }
    try {
      const d = display.getDefaultDisplaySync();
      this.scrWPx = d.width;
      this.scrHPx = d.height;
    } catch (_) {
      this.scrWPx = 1280;
      this.scrHPx = 800;
    }
  }

  private syncCursorIndicator(): void {
    // 箭头热点在图片左上尖端，position 即热点，不要减半径、不要 markAnchor。
    this.cursorVpX = px2vp(this.vcX);
    this.cursorVpY = px2vp(this.vcY);
  }

  private clampCursor(): void {
    this.ensureScreenPx();
    this.vcX = Math.min(Math.max(this.vcX, 0), this.scrWPx);
    this.vcY = Math.min(Math.max(this.vcY, 0), this.scrHPx);
    this.syncCursorIndicator();
  }

  /** Windows 式：已在边缘且增量朝外则丢弃该轴，出界位移不累加。 */
  private applyCursorDelta(dxPx: number, dyPx: number): void {
    this.ensureScreenPx();
    let dx = dxPx;
    let dy = dyPx;
    if (this.vcX <= 0 && dx < 0) dx = 0;
    if (this.vcX >= this.scrWPx && dx > 0) dx = 0;
    if (this.vcY <= 0 && dy < 0) dy = 0;
    if (this.vcY >= this.scrHPx && dy > 0) dy = 0;
    this.vcX = Math.min(Math.max(this.vcX + dx, 0), this.scrWPx);
    this.vcY = Math.min(Math.max(this.vcY + dy, 0), this.scrHPx);
    this.syncCursorIndicator();
    InputDispatcher.getInstance().rememberPointer(this.vcX, this.vcY);
  }

  private fallbackToplevelId(): number {
    return InputOverlayWindowCoordinator.getInstance().getSnapshot().session.toplevelId;
  }

  // 性能: 光标移动时缓存目标 toplevel, 避免每次 move 都 native 查询
  // (findToplevelAt 跨 JSI/NAPI 开销大)。仅当光标明显跨窗口时才重查。
  private cachedMoveTl: number = -1;
  private cachedMoveTlX: number = -1e9;
  private cachedMoveTlY: number = -1e9;

  private tlAtCursor(): number {
    const x = Math.round(this.vcX);
    const y = Math.round(this.vcY);
    if (this.cachedMoveTl >= 0 &&
      Math.abs(x - this.cachedMoveTlX) < 4 && Math.abs(y - this.cachedMoveTlY) < 4) {
      return this.cachedMoveTl;
    }
    const found = (testNapi.findToplevelAt(x, y) as number) ?? 0;
    const tl = found > 0 ? found : this.fallbackToplevelId();
    this.cachedMoveTl = tl;
    this.cachedMoveTlX = x;
    this.cachedMoveTlY = y;
    return tl;
  }

  private clickAtCursor(btn: number): void {
    let tl = this.tlAtCursor();
    if (tl <= 0) tl = this.fallbackToplevelId();
    if (tl <= 0) return;
    testNapi.raiseToplevel(tl);
    const x = Math.round(this.vcX);
    const y = Math.round(this.vcY);
    InputDispatcher.getInstance().rememberPointer(x, y);
    testNapi.sendPointerEvent(tl, MouseAction.Press, x, y, btn);
    testNapi.sendPointerEvent(tl, MouseAction.Release, x, y, btn);
  }

  private resetTouchpadState(): void {
    if (this.tpDragging && this.tpDragTl > 0) {
      testNapi.sendPointerEvent(this.tpDragTl, MouseAction.Release,
        Math.round(this.vcX), Math.round(this.vcY), mapMouseButton(1));
    }
    this.tpDown = false;
    this.tpMoved = false;
    this.tpDragArmed = false;
    this.tpDragging = false;
    this.tpDragTl = 0;
    this.tpTwoFinger = false;
    this.tpLastTapTime = 0;
    this.cachedMoveTl = -1;
  }

  private handleTouchpad(ev: TouchEvent): void {
    this.ensureScreenPx();
    // 虚拟光标初始位置: 屏幕中心 (首次进入触摸板模式)
    if (!this.vcInit) {
      this.vcInit = true;
      this.vcX = this.scrWPx / 2;
      this.vcY = this.scrHPx / 2;
      this.syncCursorIndicator();
      InputDispatcher.getInstance().rememberPointer(this.vcX, this.vcY);
    }

    // 双指: 轻点=右键 (本地语义), 滑动=滚动作用于光标处 (触控板语义)
    if (ev.touches.length >= 2) {
      let midX = 0, midY = 0;
      for (let i = 0; i < ev.touches.length; i++) {
        midX += vp2px(ev.touches[i].x);
        midY += vp2px(ev.touches[i].y);
      }
      midX /= ev.touches.length;
      midY /= ev.touches.length;
      if (!this.tpTwoFinger) {
        if (this.tpDragging && this.tpDragTl > 0) {
          testNapi.sendPointerEvent(this.tpDragTl, MouseAction.Release,
            Math.round(this.vcX), Math.round(this.vcY), mapMouseButton(1));
        }
        this.tpDown = false;
        this.tpDragging = false;
        this.tpDragTl = 0;
        this.tpDragArmed = false;
        this.tpTwoFinger = true;
        this.tpTwoFingerStartAt = Date.now();
        this.tpTwoFingerStartMidX = midX;
        this.tpTwoFingerStartMidY = midY;
        this.tpTwoFingerLastMidX = midX;
        this.tpTwoFingerLastMidY = midY;
        this.tpScrollAccumX = 0;
        this.tpScrollAccumY = 0;
      } else {
        this.tpScrollAccumY += this.tpTwoFingerLastMidY - midY;
        this.tpScrollAccumX += midX - this.tpTwoFingerLastMidX;
        this.tpTwoFingerLastMidX = midX;
        this.tpTwoFingerLastMidY = midY;
        const tl = this.tlAtCursor();
        if (tl > 0) {
          while (Math.abs(this.tpScrollAccumY) >= VirtualInputOverlay.SCROLL_STEP_PX) {
            const sign = this.tpScrollAccumY > 0 ? 1 : -1;
            testNapi.sendScrollEvent(tl, 0, sign, VirtualInputOverlay.SCROLL_STEP_PX,
              Math.round(this.vcX), Math.round(this.vcY));
            this.tpScrollAccumY -= sign * VirtualInputOverlay.SCROLL_STEP_PX;
          }
          while (Math.abs(this.tpScrollAccumX) >= VirtualInputOverlay.SCROLL_STEP_PX) {
            const sign = this.tpScrollAccumX > 0 ? 1 : -1;
            testNapi.sendScrollEvent(tl, 1, sign, VirtualInputOverlay.SCROLL_STEP_PX,
              Math.round(this.vcX), Math.round(this.vcY));
            this.tpScrollAccumX -= sign * VirtualInputOverlay.SCROLL_STEP_PX;
          }
        }
      }
      return;
    }
    if (this.tpTwoFinger) {
      // 双指抬起: 若快速 (轻点) 且中点几乎没动 → 右键; 否则是滑动滚动结束
      this.tpTwoFinger = false;
      const elapsed = Date.now() - this.tpTwoFingerStartAt;
      const dmid = Math.abs(this.tpTwoFingerLastMidX - this.tpTwoFingerStartMidX) +
        Math.abs(this.tpTwoFingerLastMidY - this.tpTwoFingerStartMidY);
      if (elapsed < VirtualInputOverlay.TWO_FINGER_TAP_MS && dmid < VirtualInputOverlay.DRAG_THRESHOLD * 2) {
        this.clickAtCursor(mapMouseButton(2));
      }
      return;
    }

    const t = ev.changedTouches[0];
    if (!t) return;

    if (ev.type === TouchType.Down) {
      this.tpDown = true;
      this.tpMoved = false;
      this.tpDownX = t.x;
      this.tpDownY = t.y;
      this.tpLastX = t.x;
      this.tpLastY = t.y;
      this.tpDownTime = Date.now();
      // 双击后按住 → 拖拽预备 (标准触控板 drag: 第二次 Down 且光标没怎么挪)
      const dvc = Math.abs(this.vcX - this.tpLastTapVcX) + Math.abs(this.vcY - this.tpLastTapVcY);
      this.tpDragArmed = this.tpLastTapTime > 0
        && (Date.now() - this.tpLastTapTime) < VirtualInputOverlay.DOUBLE_TAP_MS
        && dvc < VirtualInputOverlay.DOUBLE_TAP_DIST * 2;
    } else if (ev.type === TouchType.Move) {
      if (!this.tpDown) return;
      const speed = Math.max(0.1, Math.min(5, this.profile.cursorSpeed));
      const dxPx = vp2px(t.x - this.tpLastX) * speed;
      const dyPx = vp2px(t.y - this.tpLastY) * speed;
      this.tpLastX = t.x;
      this.tpLastY = t.y;
      if (dxPx == 0 && dyPx == 0) return;
      if (!this.tpMoved) {
        const moved = Math.abs(vp2px(t.x - this.tpDownX)) + Math.abs(vp2px(t.y - this.tpDownY));
        if (moved >= VirtualInputOverlay.DRAG_THRESHOLD) {
          this.tpMoved = true;
        }
      }
      this.applyCursorDelta(dxPx, dyPx);
      if (this.tpDragArmed && !this.tpDragging && this.tpMoved) {
        // 拖拽开始: 在光标处按下左键 (拖动窗口/元素)
        this.tpDragging = true;
        this.tpDragTl = this.tlAtCursor();
        if (this.tpDragTl > 0) {
          testNapi.raiseToplevel(this.tpDragTl);
          testNapi.sendPointerEvent(this.tpDragTl, MouseAction.Press,
            Math.round(this.vcX), Math.round(this.vcY), mapMouseButton(1));
        }
      }
      let moveTl = this.tpDragging ? this.tpDragTl : this.tlAtCursor();
      if (moveTl <= 0) moveTl = this.fallbackToplevelId();
      if (moveTl > 0) {
        testNapi.sendPointerEvent(moveTl, MouseAction.Move,
          Math.round(this.vcX), Math.round(this.vcY), 0);
      }
    } else if (ev.type === TouchType.Up) {
      if (!this.tpDown) return;
      this.tpDown = false;
      if (this.tpDragging) {
        if (this.tpDragTl > 0) {
          testNapi.sendPointerEvent(this.tpDragTl, MouseAction.Release,
            Math.round(this.vcX), Math.round(this.vcY), mapMouseButton(1));
        }
        this.tpDragging = false;
        this.tpDragTl = 0;
        this.tpDragArmed = false;
        this.tpLastTapTime = 0;
        return;
      }
      this.tpDragArmed = false;
      if (!this.tpMoved) {
        // 轻点 = 左键 (右键走双指轻点, 保持虚拟鼠标本地语义)
        this.clickAtCursor(mapMouseButton(1));
        this.tpLastTapTime = Date.now();
        this.tpLastTapVcX = this.vcX;
        this.tpLastTapVcY = this.vcY;
      }
    } else if (ev.type === TouchType.Cancel) {
      this.resetTouchpadState();
    }
  }

  private handleOverlayTouch(event: TouchEvent): void {
    if (event.changedTouches.length === 0) return;
    if (!this.hideToolbar && this.handleToolbarOverlayTouch(event)) return;
    if (this.editMode) {
      event.stopPropagation();
      this.handleEditorTouch(event);
      return;
    }
    // 触摸板模式: 虚拟光标全局状态机 (吞掉触摸, 不穿透到下层 Wine)。
    // 按钮/摇杆/方向/视角框仍优先走各自控件；其余空白才是触控板。
    if (this.touchpadModeVisual) {
      event.stopPropagation();
      const touch = event.changedTouches[0];
      const x = Number(touch.x);
      const y = Number(touch.y);
      let claimed = false;
      if (this.overlayControlsActive()) {
        const activeId = this.activeElementIdForTouch(touch.id);
        if (activeId.length > 0) {
          const element = this.elementById(activeId);
          if (element && this.isOverlayControl(element)) {
            this.handleElementTouch(event, element);
            claimed = true;
          }
        } else if (event.type === TouchType.Down) {
          const target = this.elementAt(x, y);
          if (target && this.isOverlayControl(target)) {
            this.handleElementTouch(event, target);
            claimed = true;
          }
        }
      }
      if (claimed) return;
      this.handleTouchpad(event);
      return;
    }
    if (!this.interactive || !this.overlayControlsActive()) {
      this.onUnhandledTouch(event);
      return;
    }
    const touch = event.changedTouches[0];
    const x = Number(touch.x);
    const y = Number(touch.y);
    let claimed = false;
    const activeId = this.activeElementIdForTouch(touch.id);
    if (activeId.length > 0) {
      const element = this.elementById(activeId);
      if (element) {
        this.handleElementTouch(event, element);
        claimed = true;
      }
    } else if (event.type === TouchType.Down) {
      const target = this.elementAt(x, y);
      if (target) {
        this.handleElementTouch(event, target);
        claimed = true;
      }
    }
    if (claimed) event.stopPropagation();
    else if (this.keyboardShieldContains(x, y)) {
      // 键盘区域内的空白处吞掉触摸, 不穿透到下层 Wine 窗口。
      event.stopPropagation();
    } else {
      this.onUnhandledTouch(event);
    }
  }

  /** 编辑器交互：点按选中、单指拖动、双指缩放，全部由根触摸统一处理。 */
  private handleEditorTouch(event: TouchEvent): void {
    const touches = event.touches;
    if (event.type === TouchType.Down) {
      if (this.editDragTouchId >= 0) return;
      const touch = event.changedTouches[0];
      if (!touch) return;
      const x = Number(touch.x);
      const y = Number(touch.y);
      const target = this.elementAt(x, y);
      if (target) {
        this.editDragElementId = target.id;
        this.editDragTouchId = touch.id;
        this.editDragStartX = x;
        this.editDragStartY = y;
        this.editDragOriginX = target.x;
        this.editDragOriginY = target.y;
        this.onElementSelected(target.id);
      } else {
        this.editDragElementId = '';
        this.onElementSelected('');
      }
      return;
    }
    if (event.type === TouchType.Move) {
      if (touches.length >= 2) {
        const a = touches[0];
        const b = touches[1];
        const dx = Number(b.x) - Number(a.x);
        const dy = Number(b.y) - Number(a.y);
        const distance = Math.sqrt(dx * dx + dy * dy);
        if (this.editPinchElementId.length === 0) {
          const mid = this.elementAt((Number(a.x) + Number(b.x)) / 2, (Number(a.y) + Number(b.y)) / 2);
          if (mid) {
            this.editPinchElementId = mid.id;
            this.editPinchStartDistance = Math.max(1, distance);
            this.editPinchStartScale = mid.scale;
            this.onElementSelected(mid.id);
          }
        } else if (this.editPinchStartDistance > 0) {
          this.onElementScale(this.editPinchElementId,
            Math.max(0.5, Math.min(2, this.editPinchStartScale * distance / this.editPinchStartDistance)));
        }
        return;
      }
      const touch = event.changedTouches[0];
      if (!touch) return;
      if (this.editDragTouchId === touch.id && this.editDragElementId.length > 0) {
        const x = Number(touch.x);
        const y = Number(touch.y);
        const nx = Math.max(0, Math.min(1, this.editDragOriginX + (x - this.editDragStartX) / this.overlayW));
        const ny = Math.max(0, Math.min(1, this.editDragOriginY + (y - this.editDragStartY) / this.overlayH));
        this.onElementMove(this.editDragElementId, nx, ny);
      }
      return;
    }
    if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
      const touch = event.changedTouches[0];
      if (touch && this.editDragTouchId === touch.id) {
        this.editDragElementId = '';
        this.editDragTouchId = -1;
      }
      if (touches.length < 2) {
        this.editPinchElementId = '';
        this.editPinchStartDistance = 0;
      }
    }
  }

  private handleOverlayMouse(event: MouseEvent): void {
    if (!this.hideToolbar && this.handleToolbarOverlayMouse(event)) return;
    if (this.editMode) {
      event.stopPropagation();
      return;
    }
    if (!this.interactive || !this.overlayControlsActive() || Date.now() - this.lastActionTouchAt <= 320) {
      this.onUnhandledMouse(event);
      return;
    }
    const x = Number(event.x);
    const y = Number(event.y);
    if (event.action === MouseAction.Press) {
      const target = this.elementAt(x, y);
      if (!target || (target.type !== ControlElementType.BUTTON && target.type !== ControlElementType.RANGE_BUTTON)) {
        this.onUnhandledMouse(event);
        return;
      }
      event.stopPropagation();
      this.mouseElementId = target.id;
      if (target.type === ControlElementType.BUTTON) {
        if (!target.toggleSwitch) {
          this.applyButtonBindings(target, true, -100, vp2px(x), vp2px(y));
          this.setElementPressed(target.id, true);
        }
      } else {
        const index = this.rangeIndexAt(target, y);
        this.setRangeSelection(target.id, index);
        this.applyRangeBinding(target, index, true, -100);
      }
      return;
    }
    if (event.action === MouseAction.Release && this.mouseElementId.length > 0) {
      event.stopPropagation();
      const target = this.elementById(this.mouseElementId);
      this.mouseElementId = '';
      if (target) {
        if (target.type === ControlElementType.BUTTON) {
          if (target.toggleSwitch) {
            const next = !this.isElementPressed(target.id);
            this.applyButtonBindings(target, next, -100, vp2px(x), vp2px(y));
            this.setElementPressed(target.id, next);
            if (next) this.lockedToggleElements.set(target.id, target);
            else this.lockedToggleElements.delete(target.id);
          } else {
            this.applyButtonBindings(target, false, -100, vp2px(x), vp2px(y));
            this.setElementPressed(target.id, false);
          }
        } else {
          this.applyRangeBinding(target, this.rangeSelection(target.id), false, -100);
        }
      }
      return;
    }
    if (this.keyboardShieldContains(Number(event.x), Number(event.y))) {
      event.stopPropagation();
      return;
    }
    this.onUnhandledMouse(event);
  }

  // ---- 元素渲染 ----

  @Builder
  private PanelElement(element: ControlElement) {
    Stack()
      .width(this.elementWidth(element)).height(this.elementHeight(element))
      .borderRadius(this.elementRadius(element))
      .backgroundColor(OVERLAY_CLEAR)
      .border({ width: this.overlayStrokeWidth(), color: this.overlayStrokeColor(element) })
      .shadow({ radius: 3, color: this.overlayGlowColor(), offsetX: 0, offsetY: 0 })
      .opacity(this.elementOpacity(element))
      .hitTestBehavior(HitTestMode.None)
      .position({ x: this.elementCenter(element).x - this.elementWidth(element) / 2,
        y: this.elementCenter(element).y - this.elementHeight(element) / 2 })
      .id('virtual-element-' + element.id)
  }

  private isMouseButtonElement(element: ControlElement): boolean {
    const binding = (element.bindings ?? [])[0];
    return binding !== undefined && binding.kind === InputBindingKind.MOUSE &&
      (binding.code === MOUSE_LEFT_BUTTON || binding.code === MOUSE_RIGHT_BUTTON);
  }

  @Builder
  private MouseButtonIcon(element: ControlElement) {
    Image((element.bindings ?? [])[0].code === MOUSE_LEFT_BUTTON ?
      $r('app.media.virtual_mouse_left') : $r('app.media.virtual_mouse_right'))
      .width(Math.min(this.elementWidth(element), this.elementHeight(element)) * 0.58)
      .height(Math.min(this.elementWidth(element), this.elementHeight(element)) * 0.58)
      .objectFit(ImageFit.Contain)
      .fillColor(this.overlayTextColor(this.isElementPressed(element.id)))
  }

  @Builder
  private ButtonElement(element: ControlElement) {
    Stack({ alignContent: Alignment.Center }) {
      if (this.isMouseButtonElement(element)) {
        this.MouseButtonIcon(element)
      } else {
        Text(element.text.length > 0 ? element.text : '键')
          .fontSize(this.buttonLabelFontSize(element)).fontWeight(FontWeight.Medium).maxLines(1)
          .fontColor(this.overlayTextColor(this.isElementPressed(element.id)))
      }
    }
    .width(this.elementWidth(element)).height(this.elementHeight(element))
    .borderRadius(this.elementRadius(element))
    .backgroundColor(this.overlayFill(element, this.isElementPressed(element.id)))
    .border({ width: this.overlayStrokeWidth(), color: this.overlayStrokeColor(element) })
    .shadow({ radius: 3, color: this.overlayGlowColor(), offsetX: 0, offsetY: 0 })
    .opacity(this.elementOpacity(element))
    .hitTestBehavior(HitTestMode.None)
    .position({ x: this.elementCenter(element).x - this.elementWidth(element) / 2, y: this.elementCenter(element).y - this.elementHeight(element) / 2 })
    .id('virtual-element-' + element.id)
  }

  private dpadArmPoints(element: ControlElement, direction: string): number[][] {
    const width = this.elementWidth(element);
    const height = this.elementHeight(element);
    const cx = width / 2;
    const cy = height / 2;
    const arm = Math.min(width, height) * 0.30;
    const pad = this.overlayStrokeWidth();
    const gap = Math.min(width, height) * 0.07;
    const tip = arm * 0.42;
    if (direction === 'up') {
      const inner = cy - gap;
      return [
        [cx - arm / 2, pad],
        [cx + arm / 2, pad],
        [cx + arm / 2, inner - tip],
        [cx, inner],
        [cx - arm / 2, inner - tip]
      ];
    }
    if (direction === 'down') {
      const inner = cy + gap;
      return [
        [cx - arm / 2, height - pad],
        [cx + arm / 2, height - pad],
        [cx + arm / 2, inner + tip],
        [cx, inner],
        [cx - arm / 2, inner + tip]
      ];
    }
    if (direction === 'left') {
      const inner = cx - gap;
      return [
        [pad, cy - arm / 2],
        [pad, cy + arm / 2],
        [inner - tip, cy + arm / 2],
        [inner, cy],
        [inner - tip, cy - arm / 2]
      ];
    }
    const inner = cx + gap;
    return [
      [width - pad, cy - arm / 2],
      [width - pad, cy + arm / 2],
      [inner + tip, cy + arm / 2],
      [inner, cy],
      [inner + tip, cy - arm / 2]
    ];
  }

  @Builder
  private DPadArm(element: ControlElement, direction: string) {
    Polygon()
      .width(this.elementWidth(element))
      .height(this.elementHeight(element))
      .points(this.dpadArmPoints(element, direction))
      .fill(this.overlayFill(element, this.isDpadDirectionPressed(element.id, direction)))
      .stroke(this.overlayStrokeColor(element))
      .strokeWidth(this.overlayStrokeWidth())
  }

  @Builder
  private DPadElement(element: ControlElement) {
    Stack() {
      this.DPadArm(element, 'up')
      this.DPadArm(element, 'right')
      this.DPadArm(element, 'down')
      this.DPadArm(element, 'left')
    }
    .width(this.elementWidth(element)).height(this.elementHeight(element))
    .backgroundColor(OVERLAY_CLEAR)
    .opacity(this.elementOpacity(element))
    .hitTestBehavior(HitTestMode.None)
    .position({ x: this.elementCenter(element).x - this.elementWidth(element) / 2, y: this.elementCenter(element).y - this.elementHeight(element) / 2 })
    .id('virtual-element-' + element.id)
  }

  @Builder
  private StickElement(element: ControlElement) {
    Stack({ alignContent: Alignment.Center }) {
      Circle().width(this.elementWidth(element)).height(this.elementHeight(element))
        .fill(OVERLAY_CLEAR)
        .stroke(this.overlayStrokeColor(element))
        .strokeWidth(this.overlayStrokeWidth())
        .shadow({ radius: 3, color: this.overlayGlowColor(), offsetX: 0, offsetY: 0 })
      Stack({ alignContent: Alignment.Center }) {
        Circle().width(this.stickKnobSize(element)).height(this.stickKnobSize(element))
          .fill(this.overlayThumbFill(element, this.stickState(element.id).active))
          .stroke(this.overlayStrokeColor(element))
          .strokeWidth(this.overlayStrokeWidth())
          .shadow({ radius: 3, color: this.overlayGlowColor(), offsetX: 0, offsetY: 0 })
      }
      .translate({ x: this.stickState(element.id).dx, y: this.stickState(element.id).dy })
      if (element.text.length > 0) {
        Text(element.text).fontSize(12 * (element.scale ?? 1)).fontWeight(FontWeight.Medium)
          .fontColor(this.overlayTextColor(false))
          .translate({ x: 0, y: this.elementHeight(element) / 2 + 8 * (element.scale ?? 1) })
      }
    }
    .width(this.elementWidth(element)).height(this.elementHeight(element))
    .opacity(this.elementOpacity(element))
    .hitTestBehavior(HitTestMode.None)
    .position({ x: this.elementCenter(element).x - this.elementWidth(element) / 2, y: this.elementCenter(element).y - this.elementHeight(element) / 2 })
    .id('virtual-element-' + element.id)
  }

  @Builder
  private TrackpadElement(element: ControlElement) {
    Stack({ alignContent: Alignment.Center }) {
      Stack()
        .width('100%').height('100%')
        .borderRadius(this.elementHeight(element) * 0.15)
        .backgroundColor(this.overlayFill(element, this.isElementPressed(element.id)))
        .border({ width: this.overlayStrokeWidth(), color: this.overlayStrokeColor(element) })
        .shadow({ radius: 3, color: this.overlayGlowColor(), offsetX: 0, offsetY: 0 })
      if (this.stickState(element.id).active) {
        Circle()
          .width(Math.min(this.elementWidth(element), this.elementHeight(element)) * 0.28)
          .height(Math.min(this.elementWidth(element), this.elementHeight(element)) * 0.28)
          .fill(this.overlayThumbFill(element, true))
          .stroke(this.overlayStrokeColor(element))
          .strokeWidth(this.overlayStrokeWidth())
          .translate({ x: this.stickState(element.id).dx, y: this.stickState(element.id).dy })
      }
      if (element.text.length > 0) {
        Text(element.text).fontSize(12 * (element.scale ?? 1)).fontWeight(FontWeight.Medium)
          .fontColor(this.overlayTextColor(this.isElementPressed(element.id)))
      }
    }
    .width(this.elementWidth(element)).height(this.elementHeight(element))
    .backgroundColor(OVERLAY_CLEAR)
    .opacity(this.elementOpacity(element))
    .hitTestBehavior(HitTestMode.None)
    .position({ x: this.elementCenter(element).x - this.elementWidth(element) / 2, y: this.elementCenter(element).y - this.elementHeight(element) / 2 })
    .id('virtual-element-' + element.id)
  }

  @Builder
  private RangeButtonElement(element: ControlElement) {
    Stack() {
      Stack({ alignContent: Alignment.Top }) {
        if (element.text.length > 0 && element.orientation !== 'horizontal') {
          Text(element.text).fontSize(10 * (element.scale ?? 1))
            .fontColor(this.overlayTextColor(false))
            .padding(3).width('100%').textAlign(TextAlign.Center)
        }
        ForEach(element.keyList ?? [], (code: number, index: number): void => {
          Stack({ alignContent: Alignment.Center }) {
            if (index > 0 && this.rangeSelection(element.id) !== index &&
              this.rangeSelection(element.id) !== index - 1) {
              Row()
                .width(element.orientation === 'horizontal' ? this.overlayStrokeWidth() : '100%')
                .height(element.orientation === 'horizontal' ? '100%' : this.overlayStrokeWidth())
                .backgroundColor(this.overlayStrokeColor(element))
                .position(element.orientation === 'horizontal' ? { x: 0, y: 0 } : { x: 0, y: 0 })
            }
            Text(this.keyLabel(code)).fontSize((element.orientation === 'horizontal' ? 8 : 10) * (element.scale ?? 1))
              .fontColor(this.overlayTextColor(this.rangeSelection(element.id) === index))
          }
          .width(element.orientation === 'horizontal' ? this.rangeSlotHeight(element) : '100%')
          .height(element.orientation === 'horizontal' ? '100%' : this.rangeSlotHeight(element))
          .backgroundColor(this.rangeSelection(element.id) === index ?
            this.overlayFill(element, true) : OVERLAY_CLEAR)
          .position(element.orientation === 'horizontal' ?
            { x: this.rangeSlotTop(element) + index * this.rangeSlotHeight(element), y: 0 } :
            { x: 0, y: this.rangeSlotTop(element) + index * this.rangeSlotHeight(element) })
        }, (code: number, index: number): string => element.id + '-' + code.toString() + '-' + index.toString())
      }
      .width('100%').height('100%')
      .borderRadius(Math.min(this.elementWidth(element), this.elementHeight(element)) * 0.15)
      .backgroundColor(this.overlayFill(element, false))
      .border({ width: this.overlayStrokeWidth(), color: this.overlayStrokeColor(element) })
      .clip(true)
    }
    .width(this.elementWidth(element)).height(this.elementHeight(element))
    .shadow({ radius: 3, color: this.overlayGlowColor(), offsetX: 0, offsetY: 0 })
    .opacity(this.elementOpacity(element))
    .hitTestBehavior(HitTestMode.None)
    .position({ x: this.elementCenter(element).x - this.elementWidth(element) / 2, y: this.elementCenter(element).y - this.elementHeight(element) / 2 })
    .id('virtual-element-' + element.id)
  }

  @Builder
  private ElementView(element: ControlElement) {
    if (element.type === ControlElementType.PANEL) {
      this.PanelElement(element)
    } else if (element.type === ControlElementType.BUTTON) {
      this.ButtonElement(element)
    } else if (element.type === ControlElementType.D_PAD) {
      this.DPadElement(element)
    } else if (element.type === ControlElementType.STICK) {
      this.StickElement(element)
    } else if (element.type === ControlElementType.TRACKPAD) {
      this.TrackpadElement(element)
    } else {
      this.RangeButtonElement(element)
    }
  }

  private keyLabel(code: number): string {
    if (code === 57) return '空格';
    if (code === 1) return 'Esc';
    if (code === 28) return 'Enter';
    if (code === 42) return 'Shift';
    if (code === 29) return 'Ctrl';
    if (code >= 30 && code <= 38) return String.fromCharCode(65 + (code - 30));
    if (code >= 44 && code <= 50) return String.fromCharCode(90 - (code - 44));
    if (code >= 2 && code <= 10) return (code - 1).toString();
    if (code === 11) return '0';
    return code.toString();
  }

  // ---- 工具栏（沿用 VintagePomelo 可折叠控制栏） ----

  private resolveToolbarVertical(): boolean {
    if (this.controlBarDirection === ControlBarDirection.VERTICAL) return true;
    if (this.controlBarDirection === ControlBarDirection.HORIZONTAL) return false;
    return this.overlayH > this.overlayW;
  }

  private isAutoHideMode(): boolean {
    return this.controlBarDirection === ControlBarDirection.AUTO_HIDE;
  }

  private cancelToolbarAutoHide(): void {
    if (this.toolbarAutoHideTimer >= 0) {
      clearTimeout(this.toolbarAutoHideTimer);
      this.toolbarAutoHideTimer = -1;
    }
  }

  private startToolbarAutoHide(): void {
    this.cancelToolbarAutoHide();
    if (!this.isAutoHideMode() || this.toolbarAutoHidden) return;
    this.toolbarAutoHideTimer = setTimeout((): void => {
      this.toolbarAutoHideTimer = -1;
      if (this.isAutoHideMode()) this.toolbarAutoHidden = true;
    }, 3000) as number;
  }

  private onControlBarDirectionChanged(): void {
    this.toolbarAutoHidden = false;
    this.applyConfiguredToolbarPosition();
    this.startToolbarAutoHide();
  }

  private onControlBarPositionChanged(): void {
    this.applyConfiguredToolbarPosition();
  }

  private onTouchpadModeChanged(): void {
    this.touchpadModeVisual = this.touchpadMode;
    if (this.touchpadMode) {
      this.ensureScreenPx();
      if (!this.vcInit && this.scrWPx > 0 && this.scrHPx > 0) {
        this.vcInit = true;
        this.vcX = this.scrWPx / 2;
        this.vcY = this.scrHPx / 2;
      }
      this.clampCursor();
      InputDispatcher.getInstance().rememberPointer(this.vcX, this.vcY);
    }
  }

  private onProfileChanged(): void {
    this.releaseAllElementTouches();
  }

  private onRenderRevisionChanged(): void {
    this.redrawCrosshair();
  }

  private overlayControlsActive(): boolean {
    return this.editMode || this.controlsVisibleVisual;
  }

  private onControlsVisibleChanged(): void {
    this.controlsVisibleVisual = this.controlsVisible;
    if (this.controlsVisibleVisual) {
      return;
    }
    this.releaseAllElementTouches();
  }

  private onFloatingFullscreenChanged(): void {
    if (!this.desktopFloatingFullscreen) {
      return;
    }
    this.resetCursorToCenter();
  }

  private resetCursorToCenter(): void {
    this.ensureScreenPx();
    this.cachedMoveTl = -1;
    if (this.scrWPx <= 0 || this.scrHPx <= 0) {
      return;
    }
    this.vcInit = true;
    this.vcX = this.scrWPx / 2;
    this.vcY = this.scrHPx / 2;
    this.syncCursorIndicator();
    InputDispatcher.getInstance().rememberPointer(this.vcX, this.vcY);
  }

  private onCrosshairStateChanged(): void {
    this.redrawCrosshair();
  }

  private redrawCrosshair(): void {
    if (!this.editMode || this.selectedElementId.length === 0) {
      if (this.overlayW > 0 && this.overlayH > 0) {
        this.crosshairContext.clearRect(0, 0, this.overlayW, this.overlayH);
      }
      return;
    }
    const element = this.elementById(this.selectedElementId);
    if (element) this.drawCrosshair(element);
  }

  /** 画红色虚线十字线：段长 14、间隔 8，中心加实心点。Canvas 默认单位是 vp。 */
  private drawCrosshair(element: ControlElement): void {
    if (this.overlayW <= 0 || this.overlayH <= 0) return;
    const ctx = this.crosshairContext;
    ctx.clearRect(0, 0, this.overlayW, this.overlayH);
    const center = this.elementCenter(element);
    ctx.lineWidth = 2;
    ctx.strokeStyle = 'rgba(255, 59, 48, 0.85)';
    const dash = 14;
    const gap = 8;
    ctx.beginPath();
    let y = 0;
    while (y < this.overlayH) {
      const end = Math.min(y + dash, this.overlayH);
      ctx.moveTo(center.x, y);
      ctx.lineTo(center.x, end);
      y = end + gap;
    }
    ctx.stroke();
    ctx.beginPath();
    let x = 0;
    while (x < this.overlayW) {
      const end = Math.min(x + dash, this.overlayW);
      ctx.moveTo(x, center.y);
      ctx.lineTo(end, center.y);
      x = end + gap;
    }
    ctx.stroke();
    ctx.beginPath();
    ctx.arc(center.x, center.y, 5, 0, Math.PI * 2);
    ctx.fillStyle = 'rgba(255, 59, 48, 0.9)';
    ctx.fill();
  }

  aboutToAppear(): void {
    this.controlsVisibleVisual = this.controlsVisible;
    this.touchpadModeVisual = this.touchpadMode;
    this.applyConfiguredToolbarPosition();
    this.startToolbarAutoHide();
  }

  aboutToDisappear(): void {
    this.cancelToolbarAutoHide();
    this.releaseAllElementTouches();
  }

  /** 箭头光标随短边缩放：手机约 16–18vp，平板不超过 24vp。热点在箭头尖。 */
  private cursorSizeVp(): number {
    const shortSide = Math.min(
      this.overlayW > 0 ? this.overlayW : 360,
      this.overlayH > 0 ? this.overlayH : 640);
    return Math.round(Math.max(16, Math.min(24, shortSide * 0.048)));
  }

  private toolbarWidth(): number {
    if (!this.toolbarExpanded) return 44;
    if (this.resolveToolbarVertical()) return 52;
    // 展开宽度按实际按钮数动态计算 (toggle 28 + 各 36 按钮 + spacing + padding),
    // 否则加 home 按钮后固定 220 会溢出屏幕边缘。
    const expandedButtons = this.desktopFloatingFullscreen ? 5 : 4;
    return 44 * (expandedButtons + 1);
  }

  private toolbarHeight(): number {
    if (!this.toolbarExpanded) return 44;
    if (!this.resolveToolbarVertical()) return 52;
    // 垂直展开高度按实际按钮数动态计算 (toggle 28 + 各 36 按钮 + spacing + padding)
    const expandedButtons = this.desktopFloatingFullscreen ? 5 : 4;
    return 52 * (expandedButtons + 1);
  }

  private toolbarOriginX(): number {
    return this.toolbarX;
  }

  private toolbarOriginY(): number {
    return this.toolbarY;
  }

  private clampToolbarX(value: number): number {
    return Math.max(4, Math.min(Math.max(4, this.overlayW - this.toolbarWidth() - 4), value));
  }

  private clampToolbarY(value: number): number {
    return Math.max(8, Math.min(Math.max(8, this.overlayH - this.toolbarHeight() - 8), value));
  }

  private applyConfiguredToolbarPosition(): void {
    if (this.overlayW <= 0 || this.overlayH <= 0) return;
    const spanX = Math.max(0, this.overlayW - this.toolbarWidth() - 8);
    const spanY = Math.max(0, this.overlayH - this.toolbarHeight() - 16);
    this.toolbarX = this.clampToolbarX(4 + spanX * Math.max(0, Math.min(1, this.controlBarPositionX)));
    this.toolbarY = this.clampToolbarY(8 + spanY * Math.max(0, Math.min(1, this.controlBarPositionY)));
  }

  private persistToolbarPosition(): void {
    const spanX = Math.max(0, this.overlayW - this.toolbarWidth() - 8);
    const spanY = Math.max(0, this.overlayH - this.toolbarHeight() - 16);
    const normalizedX = spanX > 0 ? (this.toolbarX - 4) / spanX : 0;
    const normalizedY = spanY > 0 ? (this.toolbarY - 8) / spanY : 0;
    this.onControlBarPositionChange(
      Math.max(0, Math.min(1, normalizedX)), Math.max(0, Math.min(1, normalizedY)));
  }

  private toolbarPanelContains(x: number, y: number): boolean {
    if (this.toolbarAutoHidden && this.isAutoHideMode()) {
      return this.rectContains(x, y, this.toolbarOriginX(), this.toolbarOriginY(), 48, 48);
    }
    return this.rectContains(x, y, this.toolbarOriginX(), this.toolbarOriginY(),
      this.toolbarWidth(), this.toolbarHeight());
  }

  private toolbarActionAt(x: number, y: number): ToolbarAction | null {
    const originX = this.toolbarOriginX();
    const originY = this.toolbarOriginY();
    if (this.toolbarAutoHidden && this.isAutoHideMode()) {
      return this.rectContains(x, y, originX, originY, 48, 48) ? 'wake' : null;
    }
    if (!this.toolbarExpanded) {
      return this.rectContains(x, y, originX, originY, 44, 44) ? 'toolbar' : null;
    }
    if (this.resolveToolbarVertical()) {
      if (this.rectContains(x, y, originX, originY, 44, 40)) return 'toolbar';
      if (this.desktopFloatingFullscreen && this.rectContains(x, y, originX, originY + 40, 44, 44)) return 'home';
      if (this.rectContains(x, y, originX, originY + (this.desktopFloatingFullscreen ? 84 : 40), 44, 44)) return 'editor';
      if (this.rectContains(x, y, originX, originY + (this.desktopFloatingFullscreen ? 128 : 84), 44, 44)) return 'controls';
      if (this.rectContains(x, y, originX, originY + (this.desktopFloatingFullscreen ? 172 : 128), 44, 44)) return 'mouse';
      if (this.rectContains(x, y, originX, originY + (this.desktopFloatingFullscreen ? 216 : 172), 44, 44)) return 'keyboard';
      return null;
    }
    if (this.rectContains(x, y, originX, originY, 44, 52)) return 'toolbar';
    if (this.desktopFloatingFullscreen && this.rectContains(x, y, originX + 44, originY, 44, 52)) return 'home';
    if (this.rectContains(x, y, originX + (this.desktopFloatingFullscreen ? 88 : 44), originY, 44, 52)) return 'editor';
    if (this.rectContains(x, y, originX + (this.desktopFloatingFullscreen ? 132 : 88), originY, 44, 52)) return 'controls';
    if (this.rectContains(x, y, originX + (this.desktopFloatingFullscreen ? 176 : 132), originY, 44, 52)) return 'mouse';
    if (this.rectContains(x, y, originX + (this.desktopFloatingFullscreen ? 220 : 176), originY, 44, 52)) return 'keyboard';
    return null;
  }

  private runToolbarAction(action: ToolbarAction): void {
    this.toolbarAutoHidden = false;
    this.startToolbarAutoHide();
    if (action === 'editor') {
      this.onOpenLayoutEditor();
      return;
    }
    if (action === 'mapping') {
      this.onOpenKeyMappingEditor();
      return;
    }
    if (action === 'controls') {
      const nextVisible = !this.controlsVisibleVisual;
      this.controlsVisibleVisual = nextVisible;
      this.onControlsVisibleChange(nextVisible);
      return;
    }
    if (action === 'mouse') {
      const nextTouchpadMode = !this.touchpadModeVisual;
      this.touchpadModeVisual = nextTouchpadMode;
      this.onTouchpadModeChange(nextTouchpadMode);
      return;
    }
    if (action === 'keyboard') {
      this.onToggleHostKeyboard();
      return;
    }
    if (action === 'home') {
      // 浮窗模式全屏态: 缩回 Index 悬浮窗
      AppStorage.setOrCreate('desktopLauncherVisible', true);
      return;
    }
    if (action === 'wake') {
      this.startToolbarAutoHide();
      return;
    }
    this.toolbarExpanded = !this.toolbarExpanded;
    this.applyConfiguredToolbarPosition();
    this.startToolbarAutoHide();
  }

  private handleToolbarButtonTouch(event: TouchEvent, action: () => void): void {
    event.stopPropagation();
    this.toolbarAutoHidden = false;
    this.startToolbarAutoHide();
    if (event.type !== TouchType.Up) return;
    this.invokeToolbarButtonAction(action);
  }

  private handleToolbarButtonMouse(event: MouseEvent, action: () => void): void {
    event.stopPropagation();
    this.toolbarAutoHidden = false;
    this.startToolbarAutoHide();
    if (event.action === MouseAction.Release) this.invokeToolbarButtonAction(action);
  }

  private invokeToolbarButtonAction(action: () => void): void {
    const now = Date.now();
    if (now - this.lastToolbarButtonTouchAt < 250) return;
    this.lastToolbarButtonTouchAt = now;
    action();
  }

  private handleToolbarOverlayTouch(event: TouchEvent): boolean {
    const touch = event.changedTouches[0];
    if (!touch) return false;
    if (this.toolbarTouchId === touch.id) {
      event.stopPropagation();
      this.lastToolbarTouchAt = Date.now();
      const x = Number(touch.x);
      const y = Number(touch.y);
      if (event.type === TouchType.Move && this.toolbarTouchAction !== 'wake') {
        const dx = x - this.toolbarTouchStartX;
        const dy = y - this.toolbarTouchStartY;
        if (this.toolbarTouchMoved || Math.abs(dx) + Math.abs(dy) >= 6) {
          this.toolbarTouchMoved = true;
          this.toolbarX = this.clampToolbarX(this.toolbarTouchOriginX + dx);
          this.toolbarY = this.clampToolbarY(this.toolbarTouchOriginY + dy);
        }
      }
      if (event.type === TouchType.Up) {
        if (this.toolbarTouchMoved) this.persistToolbarPosition();
        else if (this.toolbarTouchAction) this.runToolbarAction(this.toolbarTouchAction);
      }
      if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
        this.toolbarTouchId = -1;
        this.toolbarTouchAction = null;
        this.toolbarTouchMoved = false;
        this.startToolbarAutoHide();
      }
      return true;
    }
    if (event.type !== TouchType.Down) return false;
    const x = Number(touch.x);
    const y = Number(touch.y);
    const action = this.toolbarActionAt(x, y);
    if (!action && !this.toolbarPanelContains(x, y)) return false;
    this.toolbarTouchId = touch.id;
    this.toolbarTouchAction = action;
    this.toolbarTouchStartX = x;
    this.toolbarTouchStartY = y;
    this.toolbarTouchOriginX = this.toolbarOriginX();
    this.toolbarTouchOriginY = this.toolbarOriginY();
    this.toolbarTouchMoved = false;
    this.toolbarAutoHidden = false;
    this.startToolbarAutoHide();
    this.lastToolbarTouchAt = Date.now();
    event.stopPropagation();
    return true;
  }

  private handleToolbarOverlayMouse(event: MouseEvent): boolean {
    const x = Number(event.x);
    const y = Number(event.y);
    const action = this.toolbarActionAt(x, y);
    if (event.action === MouseAction.Press && (action || this.toolbarPanelContains(x, y))) {
      this.toolbarMouseAction = action;
      this.toolbarMouseTracking = true;
      this.toolbarMouseStartX = x;
      this.toolbarMouseStartY = y;
      this.toolbarMouseOriginX = this.toolbarOriginX();
      this.toolbarMouseOriginY = this.toolbarOriginY();
      this.toolbarMouseMoved = false;
      this.toolbarAutoHidden = false;
      this.startToolbarAutoHide();
      event.stopPropagation();
      return true;
    }
    if (this.toolbarMouseTracking && event.action === MouseAction.Move) {
      const dx = x - this.toolbarMouseStartX;
      const dy = y - this.toolbarMouseStartY;
      if (this.toolbarMouseAction !== 'wake' &&
        (this.toolbarMouseMoved || Math.abs(dx) + Math.abs(dy) >= 6)) {
        this.toolbarMouseMoved = true;
        this.toolbarX = this.clampToolbarX(this.toolbarMouseOriginX + dx);
        this.toolbarY = this.clampToolbarY(this.toolbarMouseOriginY + dy);
      }
      event.stopPropagation();
      return true;
    }
    if (event.action === MouseAction.Release && this.toolbarMouseTracking) {
      const active = this.toolbarMouseAction;
      this.toolbarMouseAction = null;
      this.toolbarMouseTracking = false;
      event.stopPropagation();
      if (this.toolbarMouseMoved) this.persistToolbarPosition();
      else if (active && Date.now() - this.lastToolbarTouchAt > 350) this.runToolbarAction(active);
      this.toolbarMouseMoved = false;
      this.startToolbarAutoHide();
      return true;
    }
    if (action || this.toolbarPanelContains(x, y)) {
      event.stopPropagation();
      return true;
    }
    return false;
  }

  @Builder
  private ControlBarButton(icon: Resource, id: string, highlighted: boolean = false,
    tint: string = '#FFFFFFFF', highlightedBackground: string = '#44FFFFFF', stateful: boolean = false,
    action: () => void = (): void => {}) {
    Button({ type: ButtonType.Circle }) {
      Stack({ alignContent: Alignment.Center }) {
        SymbolGlyph(icon).fontSize(stateful ? 14 : 16).fontColor([tint])
          .translate({ y: stateful ? -3 : 0 })
        if (stateful) {
          Text(highlighted ? 'ON' : 'OFF')
            .fontSize(6.5).fontWeight(FontWeight.Bold)
            .fontColor(highlighted ? '#FFB9F6CA' : '#FFAAB0BA')
            .position({ x: '50%', y: 24 }).translate({ x: '-50%' })
        }
      }.width(36).height(36)
    }
    .width(36).height(36)
    .backgroundColor(highlighted ? highlightedBackground : '#BF282828')
    .border({ width: 1, color: '#33FFFFFF' })
    .shadow({ radius: 8, color: '#66000000', offsetX: 0, offsetY: 2 })
    .id(id)
    .hitTestBehavior(HitTestMode.Block)
    .onTouch((event: TouchEvent): void => this.handleToolbarButtonTouch(event, action))
    .onMouse((event: MouseEvent): void => this.handleToolbarButtonMouse(event, action))
    .onClick((): void => this.invokeToolbarButtonAction(action))
  }

  @Builder
  private ControlsVisibilityButton() {
    Button({ type: ButtonType.Circle }) {
      Stack({ alignContent: Alignment.Center }) {
        SymbolGlyph(this.controlsVisibleVisual ? $r('sys.symbol.gamecontroller') : $r('sys.symbol.eye_slash'))
          .fontSize(14).fontColor(['#FFFFFFFF']).translate({ y: -3 })
        Text(this.controlsVisibleVisual ? 'ON' : 'OFF')
          .fontSize(6.5).fontWeight(FontWeight.Bold)
          .fontColor(this.controlsVisibleVisual ? '#FFB9F6CA' : '#FFAAB0BA')
          .position({ x: '50%', y: 24 }).translate({ x: '-50%' })
      }.width(36).height(36)
    }
    .width(36).height(36)
    .backgroundColor(this.controlsVisibleVisual ? '#384F7CFF' : '#BF282828')
    .border({ width: 1, color: '#33FFFFFF' })
    .shadow({ radius: 8, color: '#66000000', offsetX: 0, offsetY: 2 })
    .id('wine-input-overlay-fold').hitTestBehavior(HitTestMode.Block)
    .onTouch((event: TouchEvent): void => this.handleToolbarButtonTouch(event,
      (): void => this.runToolbarAction('controls')))
    .onMouse((event: MouseEvent): void => this.handleToolbarButtonMouse(event,
      (): void => this.runToolbarAction('controls')))
    .onClick((): void => this.invokeToolbarButtonAction((): void => this.runToolbarAction('controls')))
  }

  @Builder
  private TouchpadModeButton() {
    Button({ type: ButtonType.Circle }) {
      Stack({ alignContent: Alignment.Center }) {
        SymbolGlyph($r('sys.symbol.hand_tap')).fontSize(14)
          .fontColor([this.touchpadModeVisual ? '#FFB9F6CA' : '#FFFFFFFF']).translate({ y: -3 })
        Text(this.touchpadModeVisual ? 'ON' : 'OFF')
          .fontSize(6.5).fontWeight(FontWeight.Bold)
          .fontColor(this.touchpadModeVisual ? '#FFB9F6CA' : '#FFAAB0BA')
          .position({ x: '50%', y: 24 }).translate({ x: '-50%' })
      }.width(36).height(36)
    }
    .width(36).height(36)
    .backgroundColor(this.touchpadModeVisual ? '#B0267A48' : '#BF282828')
    .border({ width: 1, color: '#33FFFFFF' })
    .shadow({ radius: 8, color: '#66000000', offsetX: 0, offsetY: 2 })
    .id('wine-input-touchpad-mode').hitTestBehavior(HitTestMode.Block)
    .onTouch((event: TouchEvent): void => this.handleToolbarButtonTouch(event,
      (): void => this.runToolbarAction('mouse')))
    .onMouse((event: MouseEvent): void => this.handleToolbarButtonMouse(event,
      (): void => this.runToolbarAction('mouse')))
    .onClick((): void => this.invokeToolbarButtonAction((): void => this.runToolbarAction('mouse')))
  }

  /** 无状态按钮: 点击即唤起/收起宿主系统键盘。 */
  @Builder
  private HostKeyboardButton() {
    Button({ type: ButtonType.Circle }) {
      SymbolGlyph($r('sys.symbol.keyboard')).fontSize(16).fontColor(['#FFFFFFFF'])
    }
    .width(36).height(36)
    .backgroundColor('#BF282828')
    .border({ width: 1, color: '#33FFFFFF' })
    .shadow({ radius: 8, color: '#66000000', offsetX: 0, offsetY: 2 })
    .id('wine-input-host-keyboard').hitTestBehavior(HitTestMode.Block)
    .onTouch((event: TouchEvent): void => this.handleToolbarButtonTouch(event,
      (): void => this.runToolbarAction('keyboard')))
    .onMouse((event: MouseEvent): void => this.handleToolbarButtonMouse(event,
      (): void => this.runToolbarAction('keyboard')))
    .onClick((): void => this.invokeToolbarButtonAction((): void => this.runToolbarAction('keyboard')))
  }

  @Builder
  private ExpandedToolbarButtons() {
    // 浮窗模式全屏态: 压缩按钮 (放最左边) — 缩回 Index 悬浮窗 (经 AppStorage 通知 Index)
    if (this.desktopFloatingFullscreen) {
      this.ControlBarButton($r('sys.symbol.arrow_down_right_and_arrow_up_left'), 'wine-input-home', false,
        '#FFFFFFFF', '#44FFFFFF', false, (): void => {
          AppStorage.setOrCreate('desktopLauncherVisible', true);
        })
    }
    this.ControlBarButton($r('sys.symbol.gearshape'), 'wine-input-layout-editor', false,
      '#FFFFFFFF', '#44FFFFFF', false, (): void => this.runToolbarAction('editor'))
    this.ControlsVisibilityButton()
    this.TouchpadModeButton()
    this.HostKeyboardButton()
  }

  @Builder
  private ToolbarToggleButton(vertical: boolean) {
    Button({ type: ButtonType.Circle }) {
      if (vertical) {
        SymbolGlyph(this.toolbarExpanded ? $r('sys.symbol.chevron_down') : $r('sys.symbol.chevron_up'))
          .fontSize(14).fontColor(['#FFFFFFFF'])
      } else {
        SymbolGlyph(this.toolbarExpanded ? $r('sys.symbol.chevron_right') : $r('sys.symbol.chevron_left'))
          .fontSize(14).fontColor(['#FFFFFFFF'])
      }
    }
    .width(28).height(28)
    .backgroundColor(this.toolbarExpanded ? '#1AFFFFFF' : '#BF282828')
    .border({ width: 1, color: '#33FFFFFF' })
    .id('wine-input-overlay-toggle')
    .hitTestBehavior(HitTestMode.None)
  }

  @Builder
  private Toolbar() {
    if (this.toolbarAutoHidden && this.isAutoHideMode()) {
      Button({ type: ButtonType.Normal }) {}
        .width(48).height(48)
        .backgroundColor('#00000000').opacity(0)
        .hitTestBehavior(HitTestMode.None)
        .position({ x: this.toolbarOriginX(), y: this.toolbarOriginY() })
        .id('wine-input-overlay-wake')
        .zIndex(30)
    } else if (this.resolveToolbarVertical()) {
      Column({ space: 8 }) {
        this.ToolbarToggleButton(true)

        if (this.toolbarExpanded) {
          this.ExpandedToolbarButtons()
        }
      }
      .padding(this.toolbarExpanded ? 8 : 4)
      .backgroundColor('#4D000000')
      .borderRadius(20)
      .hitTestBehavior(HitTestMode.Transparent)
      .position({ x: this.toolbarOriginX(), y: this.toolbarOriginY() })
      .id('wine-input-toolbar-vertical')
      .zIndex(30)
    } else {
      Row({ space: 8 }) {
        this.ToolbarToggleButton(false)

        if (this.toolbarExpanded) {
          this.ExpandedToolbarButtons()
        }
      }
      .padding(this.toolbarExpanded ? 8 : 4)
      .backgroundColor('#4D000000')
      .borderRadius(20)
      .hitTestBehavior(HitTestMode.Transparent)
      .position({ x: this.toolbarOriginX(), y: this.toolbarOriginY() })
      .id('wine-input-toolbar-horizontal')
      .zIndex(30)
    }
  }

  build() {
    Stack() {
      Text(this.renderRevision.toString()).width(0).height(0).opacity(0).hitTestBehavior(HitTestMode.None)
      if (this.touchpadModeVisual && this.showCursor && this.cursorVpX >= 0 && this.cursorVpY >= 0) {
        Image($r('app.media.virtual_mouse_arrow'))
          .width(this.cursorSizeVp())
          .height(this.cursorSizeVp())
          .objectFit(ImageFit.Contain)
          .interpolation(ImageInterpolation.Medium)
          .position({ x: this.cursorVpX, y: this.cursorVpY })
          .hitTestBehavior(HitTestMode.None)
          .zIndex(50)
      }
      if (this.overlayW > 0 && this.overlayH > 0 && (this.controlsVisibleVisual || this.editMode)) {
        // 十字线在元素下层：不遮挡按钮，仍能看到完整参考线
        if (this.selectedElementId.length > 0) {
          this.SelectedCrosshair(this.elementById(this.selectedElementId))
        }
        ForEach(this.visibleElements(), (element: ControlElement): void => {
          this.ElementView(element)
        }, (element: ControlElement): string => this.elementVisualKey(element))
      }
      if (!this.hideToolbar) this.Toolbar()
    }
    .width('100%').height('100%').hitTestBehavior(HitTestMode.Block).id('wine-vintage-gamepad-overlay')
    .onTouch((event: TouchEvent): void => this.handleOverlayTouch(event))
    .onMouse((event: MouseEvent): void => this.handleOverlayMouse(event))
    .onAxisEvent((event?: AxisEvent): void => this.onUnhandledAxis(event))
    .onAreaChange((_oldValue: Area, area: Area): void => {
      const nextW = area.width as number;
      const nextH = area.height as number;
      const jumped = Math.abs(nextW - this.overlayW) > 8 || Math.abs(nextH - this.overlayH) > 8;
      this.overlayW = nextW;
      this.overlayH = nextH;
      this.applyConfiguredToolbarPosition();
      if (jumped && this.touchpadModeVisual) {
        this.resetCursorToCenter();
      } else if (this.touchpadModeVisual && this.vcInit) {
        this.clampCursor();
      }
    })
  }
}
