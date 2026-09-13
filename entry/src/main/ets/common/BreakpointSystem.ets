import { display, mediaquery } from '@kit.ArkUI';

export type BreakpointType = 'xs' | 'sm' | 'md' | 'lg' | 'xl';
export type BreakpointCallback = (breakpoint: BreakpointType) => void;

/** Catalog chrome split: <960vp uses the phone shell, 960vp+ uses pad/PC. */
export function isWideBreakpoint(breakpoint: BreakpointType): boolean {
  return breakpoint === 'lg' || breakpoint === 'xl';
}

class BreakpointSystemImpl {
  private current: BreakpointType = 'lg';
  private callbacks: Set<BreakpointCallback> = new Set<BreakpointCallback>();
  private listeners: mediaquery.MediaQueryListener[] = [];
  private initialized: boolean = false;

  initialize(): void {
    if (this.initialized) return;
    this.initialized = true;
    this.updateFromDisplay();
    this.bind('(width < 360vp)', 'xs');
    this.bind('(360vp <= width < 720vp)', 'sm');
    this.bind('(720vp <= width < 960vp)', 'md');
    this.bind('(960vp <= width < 1280vp)', 'lg');
    this.bind('(width >= 1280vp)', 'xl');
  }

  private bind(query: string, value: BreakpointType): void {
    const listener = mediaquery.matchMediaSync(query);
    // matchMedia is scoped to the current app window. Apply the initial
    // result immediately so an app restored directly into a small window
    // does not render one frame using the physical display width.
    if (listener.matches) this.setCurrent(value);
    listener.on('change', (result: mediaquery.MediaQueryResult) => {
      if (result.matches) this.setCurrent(value);
    });
    this.listeners.push(listener);
  }

  private updateFromDisplay(): void {
    try {
      const info = display.getDefaultDisplaySync();
      this.updateFromWidth(info.width / (info.densityPixels || 1));
    } catch (_) {
      this.current = 'lg';
    }
  }

  updateFromWidth(widthVp: number): void {
    if (widthVp >= 1280) this.setCurrent('xl');
    else if (widthVp >= 960) this.setCurrent('lg');
    else if (widthVp >= 720) this.setCurrent('md');
    else if (widthVp >= 360) this.setCurrent('sm');
    else this.setCurrent('xs');
  }

  private setCurrent(value: BreakpointType): void {
    if (value === this.current) return;
    this.current = value;
    this.callbacks.forEach((callback: BreakpointCallback) => callback(value));
  }

  getCurrent(): BreakpointType {
    return this.current;
  }

  subscribe(callback: BreakpointCallback): void {
    this.callbacks.add(callback);
    callback(this.current);
  }

  unsubscribe(callback: BreakpointCallback): void {
    this.callbacks.delete(callback);
  }
}

export const BreakpointSystem = new BreakpointSystemImpl();
