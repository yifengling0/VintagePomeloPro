const SOURCE_UNKNOWN = 0;
const SOURCE_MOUSE = 1;
const SOURCE_TOUCHSCREEN = 2;

const TOOL_UNKNOWN = 0;
const TOOL_FINGER = 1;
const TOOL_PEN = 2;
const TOOL_MOUSE = 3;
const TOOL_TOUCHPAD = 4;

/** ArkUI can dispatch the same physical axis sample twice. Match the original
 * timestamp and payload, never a wall-clock interval or just the wheel value. */
export class AxisEventGate {
  private recent: string[] = [];

  shouldHandle(action: number, timestamp: number, vertical: number, horizontal: number,
    source: number, sourceTool: number): boolean {
    if (action === 3 || action === 4 || !Number.isFinite(vertical) || !Number.isFinite(horizontal) ||
      (vertical === 0 && horizontal === 0)) return false;
    // Missing timestamps cannot reliably identify duplicates.
    if (!Number.isFinite(timestamp) || timestamp <= 0) return true;
    const identity = `${timestamp}:${action}:${source}:${sourceTool}:${vertical}:${horizontal}`;
    if (this.recent.includes(identity)) return false;
    this.recent.push(identity);
    if (this.recent.length > 16) this.recent.shift();
    return true;
  }
}

/**
 * Separates real touch from physical mouse input. ArkUI can synthesize both a
 * TouchEvent and a MouseEvent for one physical action, so callback names alone
 * are not enough to route input on API 23.
 */
export class InputEventGate {
  private lastTouchAt: number = 0;
  private readonly syntheticMouseGuardMs: number;

  constructor(syntheticMouseGuardMs: number = 320) {
    this.syntheticMouseGuardMs = syntheticMouseGuardMs;
  }

  shouldHandleTouch(source: number, sourceTool: number, now: number = Date.now()): boolean {
    if (source === SOURCE_MOUSE || sourceTool === TOOL_MOUSE || sourceTool === TOOL_TOUCHPAD) return false;
    this.lastTouchAt = now;
    return source === SOURCE_TOUCHSCREEN || sourceTool === TOOL_FINGER || sourceTool === TOOL_PEN ||
      (source === SOURCE_UNKNOWN && sourceTool === TOOL_UNKNOWN);
  }

  shouldHandleMouse(source: number, sourceTool: number, now: number = Date.now()): boolean {
    if (source === SOURCE_TOUCHSCREEN || sourceTool === TOOL_FINGER || sourceTool === TOOL_PEN) return false;
    if (source === SOURCE_MOUSE || sourceTool === TOOL_MOUSE || sourceTool === TOOL_TOUCHPAD) return true;
    return now - this.lastTouchAt > this.syntheticMouseGuardMs;
  }
}
