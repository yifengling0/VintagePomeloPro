/**
 * 与 NAPI 无关的输入按下账本。一个 Wine 会话中，多个触点/控件可以映射
 * 到同一按键；只有第一个按下和最后一个释放才需要交给底层。
 */
export interface ReleasedInputState {
  keys: number[];
  buttons: number[];
}

export interface InputStateSnapshot {
  activePointers: number;
  heldKeys: number;
  heldButtons: number;
}

export class InputStateTracker {
  private sourceKeys: Map<string, Set<number>> = new Map();
  private sourceButtons: Map<string, Set<number>> = new Map();
  private keyRefs: Map<number, number> = new Map();
  private buttonRefs: Map<number, number> = new Map();
  private pointers: Set<string> = new Set();

  isKeyHeldBy(source: string, code: number): boolean {
    return this.sourceKeys.get(source)?.has(code) ?? false;
  }

  pressKey(source: string, code: number): boolean {
    if (code <= 0) return false;
    let keys = this.sourceKeys.get(source);
    if (!keys) {
      keys = new Set<number>();
      this.sourceKeys.set(source, keys);
    }
    if (keys.has(code)) return false;
    keys.add(code);
    const count = this.keyRefs.get(code) ?? 0;
    this.keyRefs.set(code, count + 1);
    return count === 0;
  }

  releaseKey(source: string, code: number): boolean {
    const keys = this.sourceKeys.get(source);
    if (!keys || !keys.has(code)) return false;
    keys.delete(code);
    if (keys.size === 0) this.sourceKeys.delete(source);
    const count = Math.max(0, (this.keyRefs.get(code) ?? 1) - 1);
    if (count === 0) {
      this.keyRefs.delete(code);
      return true;
    }
    this.keyRefs.set(code, count);
    return false;
  }

  pressButton(source: string, button: number): boolean {
    if (button <= 0) return false;
    let buttons = this.sourceButtons.get(source);
    if (!buttons) {
      buttons = new Set<number>();
      this.sourceButtons.set(source, buttons);
    }
    if (buttons.has(button)) return false;
    buttons.add(button);
    const count = this.buttonRefs.get(button) ?? 0;
    this.buttonRefs.set(button, count + 1);
    return count === 0;
  }

  releaseButton(source: string, button: number): boolean {
    const buttons = this.sourceButtons.get(source);
    if (!buttons || !buttons.has(button)) return false;
    buttons.delete(button);
    if (buttons.size === 0) this.sourceButtons.delete(source);
    const count = Math.max(0, (this.buttonRefs.get(button) ?? 1) - 1);
    if (count === 0) {
      this.buttonRefs.delete(button);
      return true;
    }
    this.buttonRefs.set(button, count);
    return false;
  }

  markPointer(source: string): void {
    this.pointers.add(source);
  }

  clearPointer(source: string): void {
    this.pointers.delete(source);
  }

  releaseSource(source: string): ReleasedInputState {
    const released: ReleasedInputState = { keys: [], buttons: [] };
    const keys = Array.from(this.sourceKeys.get(source) ?? []);
    const buttons = Array.from(this.sourceButtons.get(source) ?? []);
    keys.forEach((code: number): void => {
      if (this.releaseKey(source, code)) released.keys.push(code);
    });
    buttons.forEach((button: number): void => {
      if (this.releaseButton(source, button)) released.buttons.push(button);
    });
    this.clearPointer(source);
    return released;
  }

  releaseAll(): ReleasedInputState {
    const sources: Set<string> = new Set<string>();
    this.sourceKeys.forEach((_value: Set<number>, source: string): void => {
      sources.add(source);
    });
    this.sourceButtons.forEach((_value: Set<number>, source: string): void => {
      sources.add(source);
    });
    this.pointers.forEach((source: string): void => {
      sources.add(source);
    });
    const released: ReleasedInputState = { keys: [], buttons: [] };
    sources.forEach((source: string): void => {
      const sourceReleased = this.releaseSource(source);
      sourceReleased.keys.forEach((code: number): void => {
        released.keys.push(code);
      });
      sourceReleased.buttons.forEach((button: number): void => {
        released.buttons.push(button);
      });
    });
    this.sourceKeys.clear();
    this.sourceButtons.clear();
    this.keyRefs.clear();
    this.buttonRefs.clear();
    this.pointers.clear();
    return released;
  }

  snapshot(): InputStateSnapshot {
    return {
      activePointers: this.pointers.size,
      heldKeys: this.keyRefs.size,
      heldButtons: this.buttonRefs.size
    };
  }
}
