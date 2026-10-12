import testNapi from 'libentry.so';

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
  private pressedKeys: Set<number> = new Set<number>();
  private pressedButtons: Set<number> = new Set<number>();
  private pointerX: number = 0;
  private pointerY: number = 0;
  private hasPointer: boolean = false;

  static getInstance(): InputRouter {
    if (!InputRouter.instance) InputRouter.instance = new InputRouter();
    return InputRouter.instance;
  }

  setActiveToplevel(toplevelId: number): void {
    if (this.activeToplevelId !== toplevelId) this.releaseAllKeys();
    this.activeToplevelId = toplevelId;
  }

  getActiveToplevel(): number {
    return this.activeToplevelId;
  }

  sendKey(evdevCode: number, pressed: boolean): void {
    if (this.activeToplevelId <= 0 || evdevCode <= 0) return;
    if (pressed) this.pressedKeys.add(evdevCode);
    else this.pressedKeys.delete(evdevCode);
    testNapi.sendKeyEvent(this.activeToplevelId, evdevCode, pressed);
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
    if (this.activeToplevelId > 0) {
      this.pressedKeys.forEach((code: number): void => {
        testNapi.sendKeyEvent(this.activeToplevelId, code, false);
      });
      this.pressedButtons.forEach((button: number) => {
        testNapi.sendPointerEvent(this.activeToplevelId, MouseAction.Release,
          this.pointerX, this.pointerY, button);
      });
    }
    this.pressedKeys.clear();
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
