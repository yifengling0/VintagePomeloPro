import testNapi from 'libentry.so';
import { InputStateTracker } from './InputStateTracker';

export interface PointerPosition {
  x: number;
  y: number;
  valid: boolean;
}

export interface PointerInput {
  rawDx: number;
  rawDy: number;
  fromMouse: boolean;
}

export class InputRouter {
  private static instance: InputRouter | null = null;
  private activeToplevelId: number = 0;
  private windowKeys: Map<number, InputStateTracker> = new Map<number, InputStateTracker>();
  private pressedButtons: Set<number> = new Set<number>();
  private pointerX: number = 0;
  private pointerY: number = 0;
  private hasPointer: boolean = false;

  static getInstance(): InputRouter {
    if (!InputRouter.instance) InputRouter.instance = new InputRouter();
    return InputRouter.instance;
  }

  setActiveToplevel(toplevelId: number): void {
    if (this.activeToplevelId !== toplevelId) {
      this.releaseWindowSource(this.activeToplevelId, 'router');
      this.releasePointerButtons();
    }
    this.activeToplevelId = toplevelId;
  }

  getActiveToplevel(): number {
    return this.activeToplevelId;
  }

  sendKey(evdevCode: number, pressed: boolean): void {
    this.sendWindowKey(this.activeToplevelId, 'router', evdevCode, pressed);
  }

  sendWindowKey(toplevelId: number, source: string, code: number, pressed: boolean): void {
    if (toplevelId <= 0 || code <= 0) return;
    let state = this.windowKeys.get(toplevelId);
    if (!state) {
      if (!pressed) return;
      state = new InputStateTracker();
      this.windowKeys.set(toplevelId, state);
    }
    const repeat = pressed && state.isKeyHeldBy(source, code);
    const changed = pressed ? state.pressKey(source, code) : state.releaseKey(source, code);
    if (changed || repeat) testNapi.sendKeyEvent(toplevelId, code, pressed);
    if (state.snapshot().heldKeys === 0) this.windowKeys.delete(toplevelId);
  }

  releaseWindowSource(toplevelId: number, source: string): void {
    const state = this.windowKeys.get(toplevelId);
    if (!state) return;
    state.releaseSource(source).keys.forEach((code: number): void => {
      testNapi.sendKeyEvent(toplevelId, code, false);
    });
    if (state.snapshot().heldKeys === 0) this.windowKeys.delete(toplevelId);
  }

  sendPointer(action: number, x: number, y: number, button: number, input?: PointerInput): void {
    if (this.activeToplevelId <= 0) return;
    this.rememberPointer(x, y);
    if (action === MouseAction.Press && button > 0) this.pressedButtons.add(button);
    if (action === MouseAction.Release && button > 0) this.pressedButtons.delete(button);
    testNapi.sendPointerEvent(this.activeToplevelId, action, x, y, button,
      input?.rawDx ?? 0, input?.rawDy ?? 0, input?.fromMouse ?? false);
  }

  sendPointerDelta(deltaX: number, deltaY: number): void {
    this.sendPointer(MouseAction.Move, this.pointerX + deltaX, this.pointerY + deltaY, 0);
  }

  rememberPointer(x: number, y: number): void {
    this.pointerX = x;
    this.pointerY = y;
    this.hasPointer = true;
  }

  getPointer(): PointerPosition {
    return { x: this.pointerX, y: this.pointerY, valid: this.hasPointer };
  }

  sendPointerButton(action: number, button: number): void {
    this.sendPointer(action, this.pointerX, this.pointerY, button);
  }

  sendScrollAtPointer(axis: number, value: number, step: number = 40): void {
    this.sendScroll(axis, value, step, this.pointerX, this.pointerY);
  }

  sendScroll(axis: number, value: number, step: number, x: number, y: number): void {
    if (this.activeToplevelId <= 0) return;
    testNapi.sendScrollEvent(this.activeToplevelId, axis, value, step, x, y);
  }

  releaseAllKeys(): void {
    this.windowKeys.forEach((state: InputStateTracker, toplevelId: number): void => {
      state.releaseAll().keys.forEach((code: number): void => {
        testNapi.sendKeyEvent(toplevelId, code, false);
      });
    });
    this.windowKeys.clear();
    this.releasePointerButtons();
  }

  private releasePointerButtons(): void {
    if (this.activeToplevelId > 0) {
      this.pressedButtons.forEach((button: number): void => {
        testNapi.sendPointerEvent(this.activeToplevelId, MouseAction.Release,
          this.pointerX, this.pointerY, button);
      });
    }
    this.pressedButtons.clear();
  }
}

export interface GamepadSink {
  onButton(evdevCode: number, pressed: boolean): void;
  onPointerDelta(deltaX: number, deltaY: number): void;
}

export class KeyMouseGamepadSink implements GamepadSink {
  onButton(evdevCode: number, pressed: boolean): void {
    InputRouter.getInstance().sendKey(evdevCode, pressed);
  }

  onPointerDelta(deltaX: number, deltaY: number): void {
    InputRouter.getInstance().sendPointerDelta(deltaX, deltaY);
  }
}
