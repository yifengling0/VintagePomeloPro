import testNapi from 'libentry.so';
import { DisplayMode, InputDiagnostics, WineInputSession } from '../model/AppModels';
import { InputRouter, PointerPosition } from './InputRouter';
import { InputStateTracker, ReleasedInputState } from './InputStateTracker';

export type InputDiagnosticsListener = (diagnostics: InputDiagnostics) => void;

/**
 * VirtualInputOverlay 的唯一输入出口。按下状态以 source 为单位引用计数，
 * 所有取消、切换、失焦都通过 releaseAll 回收，避免 Wine 侧残留卡键。
 */
export class InputDispatcher {
  private static instance: InputDispatcher | null = null;
  private session: WineInputSession = {
    sessionId: '', displayMode: DisplayMode.DESKTOP, toplevelId: 0, active: false
  };
  private tracker: InputStateTracker = new InputStateTracker();
  private injectedEvents: number = 0;
  private diagnosticsListeners: Set<InputDiagnosticsListener> = new Set<InputDiagnosticsListener>();

  static getInstance(): InputDispatcher {
    if (!InputDispatcher.instance) InputDispatcher.instance = new InputDispatcher();
    return InputDispatcher.instance;
  }

  setSession(session: WineInputSession): void {
    if (this.session.sessionId !== session.sessionId || this.session.toplevelId !== session.toplevelId) {
      this.releaseAll('session-change');
    }
    this.session = session;
    if (session.toplevelId > 0) InputRouter.getInstance().setActiveToplevel(session.toplevelId);
    this.publishDiagnostics();
  }

  getSession(): WineInputSession {
    return {
      sessionId: this.session.sessionId,
      displayMode: this.session.displayMode,
      toplevelId: this.session.toplevelId,
      active: this.session.active
    };
  }

  setActiveToplevel(toplevelId: number): void {
    if (toplevelId <= 0) return;
    if (this.session.toplevelId !== toplevelId) this.releaseAll('toplevel-change');
    this.session.toplevelId = toplevelId;
    InputRouter.getInstance().setActiveToplevel(toplevelId);
    this.publishDiagnostics();
  }

  resolveToplevel(x: number, y: number, allowFocusChange: boolean = true): number {
    let found = 0;
    try { found = testNapi.findToplevelAt(Math.round(x), Math.round(y)) as number; } catch (_) {}
    if (found > 0 && allowFocusChange) this.setActiveToplevel(found);
    return found > 0 ? found : this.session.toplevelId;
  }

  pressKey(source: string, evdevCode: number): void {
    if (!this.session.active || !this.tracker.pressKey(source, evdevCode)) return;
    // Hub buttons are additive. Keyboard is always sent so overlay coexist
    // with a physical pad (do not steal WASD / arrows into exclusive dinput).
    this.feedHubKey(evdevCode, true);
    InputRouter.getInstance().sendKey(evdevCode, true);
    this.injectedEvents++;
    this.publishDiagnostics();
  }

  releaseKey(source: string, evdevCode: number): void {
    if (!this.tracker.releaseKey(source, evdevCode)) return;
    this.feedHubKey(evdevCode, false);
    InputRouter.getInstance().sendKey(evdevCode, false);
    this.injectedEvents++;
    this.publishDiagnostics();
  }

  /** Overlay analog stick. stick 0 = left, 1 = right. Values are already canonical (+Y = Up). */
  setLogicalStick(stick: number, x: number, y: number): void {
    if (!this.isDinputHub()) return;
    try {
      testNapi.controllerSetStick(0, 0, stick, x, y);
    } catch (_) {
    }
    this.injectedEvents++;
    this.publishDiagnostics();
  }

  /** Overlay D-Pad. +Y = Up, values -1 / 0 / +1. */
  setLogicalHat(x: number, y: number): void {
    if (!this.isDinputHub()) return;
    try {
      testNapi.controllerSetHat(0, 0, x, y);
    } catch (_) {
    }
    this.injectedEvents++;
    this.publishDiagnostics();
  }

  lastPointer(): PointerPosition {
    return InputRouter.getInstance().getPointer();
  }

  rememberPointer(x: number, y: number): void {
    InputRouter.getInstance().rememberPointer(x, y);
  }

  /**
   * Overlay mouse keys: press/release at the last remembered pointer.
   * Does not resolveToplevel at a widget. If no pointer exists yet, inject
   * on the current session toplevel without warping to (0, 0) via findToplevelAt.
   */
  pointerButton(source: string, button: number, pressed: boolean): void {
    const last = this.lastPointer();
    if (last.valid) {
      if (pressed) this.pointerDown(source, last.x, last.y, button);
      else this.pointerUp(source, last.x, last.y, button);
      return;
    }
    if (pressed) {
      if (!this.session.active) return;
      this.tracker.markPointer(source);
      this.buttonDown(source, button);
      this.publishDiagnostics();
    } else {
      this.buttonUp(source, button);
      this.tracker.clearPointer(source);
      this.publishDiagnostics();
    }
  }

  pointerDown(source: string, x: number, y: number, button: number = 272): void {
    if (!this.session.active) return;
    this.resolveToplevel(x, y);
    this.tracker.markPointer(source);
    this.buttonDown(source, button, x, y);
    this.publishDiagnostics();
  }

  buttonDown(source: string, button: number, x?: number, y?: number): void {
    if (!this.session.active || !this.tracker.pressButton(source, button)) return;
    if (x !== undefined && y !== undefined) InputRouter.getInstance().sendPointer(MouseAction.Press, x, y, button);
    else InputRouter.getInstance().sendPointerButton(MouseAction.Press, button);
    this.injectedEvents++;
    this.publishDiagnostics();
  }

  pointerMove(source: string, x: number, y: number, pointerActive: boolean = true): void {
    if (!this.session.active) return;
    if (pointerActive) this.tracker.markPointer(source);
    // 已按下的指针必须保持最初归属。否则拖动越过 popup 边界会触发
    // toplevel 切换并提前释放按键，表现为 Wine 中断拖拽。
    const state = this.tracker.snapshot();
    this.resolveToplevel(x, y, state.heldButtons === 0);
    InputRouter.getInstance().sendPointer(MouseAction.Move, x, y, 0);
    this.injectedEvents++;
    this.publishDiagnostics();
  }

  pointerDelta(source: string, deltaX: number, deltaY: number): void {
    if (!this.session.active) return;
    this.tracker.markPointer(source);
    InputRouter.getInstance().sendPointerDelta(deltaX, deltaY);
    this.injectedEvents++;
    this.publishDiagnostics();
  }

  pointerUp(source: string, x: number, y: number, button: number = 272): void {
    this.buttonUp(source, button, x, y);
    this.tracker.clearPointer(source);
    this.publishDiagnostics();
  }

  buttonUp(source: string, button: number, x?: number, y?: number): void {
    if (!this.tracker.releaseButton(source, button)) return;
    if (x !== undefined && y !== undefined) InputRouter.getInstance().sendPointer(MouseAction.Release, x, y, button);
    else InputRouter.getInstance().sendPointerButton(MouseAction.Release, button);
    this.injectedEvents++;
    this.publishDiagnostics();
  }

  scroll(source: string, axis: number, value: number, step: number = 40): void {
    if (!this.session.active || value === 0) return;
    this.tracker.markPointer(source);
    InputRouter.getInstance().sendScrollAtPointer(axis, value, step);
    this.injectedEvents++;
    this.publishDiagnostics();
  }

  releaseSource(source: string): void {
    this.releaseTracked(this.tracker.releaseSource(source));
    this.publishDiagnostics();
  }

  releaseAll(_reason: string): void {
    this.releaseTracked(this.tracker.releaseAll());
    // 兼容主窗口的实体输入路径：即使它没有经过 tracker，也必须收尾。
    InputRouter.getInstance().releaseAllKeys();
    if (this.isDinputHub()) {
      try { testNapi.controllerResetSource(0); } catch (_) {}
    }
    this.publishDiagnostics();
  }

  subscribeDiagnostics(listener: InputDiagnosticsListener): void {
    this.diagnosticsListeners.add(listener);
    listener(this.getDiagnostics());
  }

  unsubscribeDiagnostics(listener: InputDiagnosticsListener): void {
    this.diagnosticsListeners.delete(listener);
  }

  getDiagnostics(): InputDiagnostics {
    const state = this.tracker.snapshot();
    return {
      sessionId: this.session.sessionId,
      activePointers: state.activePointers,
      heldKeys: state.heldKeys,
      heldButtons: state.heldButtons,
      injectedEvents: this.injectedEvents
    };
  }

  private isDinputHub(): boolean {
    try {
      return testNapi.controllerGetOutputMode() === 'dinput';
    } catch (_) {
      return false;
    }
  }

  /** Overlay evdev → LogicalButton. Movement keys stay keyboard (D-pad / stick). */
  private mapEvdevToLogical(code: number): number {
    switch (code) {
      case 57: return 0;  // Space → A
      case 45: return 1;  // KEY_X → B
      case 16: return 4;  // KEY_Q → LB
      case 1: return 6;   // Esc → Back
      case 28: return 7;  // Enter → Start
      case 63: return 14;  // Home → Guide
      default: return -1;
    }
  }

  private feedHubKey(evdevCode: number, pressed: boolean): boolean {
    if (!this.isDinputHub()) return false;
    const button = this.mapEvdevToLogical(evdevCode);
    if (button < 0) return false;
    try {
      testNapi.controllerSetButton(0, 0, button, pressed);
    } catch (_) {
    }
    return true;
  }

  private releaseTracked(released: ReleasedInputState): void {
    released.keys.forEach((code: number): void => {
      this.feedHubKey(code, false);
      InputRouter.getInstance().sendKey(code, false);
      this.injectedEvents++;
    });
    released.buttons.forEach((button: number): void => {
      InputRouter.getInstance().sendPointerButton(MouseAction.Release, button);
      this.injectedEvents++;
    });
  }

  private publishDiagnostics(): void {
    const diagnostics = this.getDiagnostics();
    this.diagnosticsListeners.forEach((listener: InputDiagnosticsListener): void => listener(diagnostics));
  }
}
