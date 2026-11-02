import { pasteboard } from '@kit.BasicServicesKit';
import displayRoute from 'libdisplay_route_napi.so';

// Text only. A denied clipboard permission must never block engine startup.
export class X11ClipboardService {
  private static active: boolean = false;
  private static epoch: number = 0;
  private static readSequence: number = 0;
  private static timer: number = -1;
  private static version: number = 0;
  private static lastText: string = '';
  private static hasText: boolean = false;
  // Do not reset on stop: an already issued system write cannot be cancelled.
  private static busy: boolean = false;
  private static readonly changed: () => void = (): void => { void X11ClipboardService.pushSystem(); };

  static start(): void {
    if (X11ClipboardService.active) return;
    X11ClipboardService.active = true;
    X11ClipboardService.epoch++;
    X11ClipboardService.hasText = false;
    try {
      X11ClipboardService.version = displayRoute.getDisplayRouteClipboard().version;
      pasteboard.getSystemPasteboard().on('update', X11ClipboardService.changed);
    } catch (_) { /* A failed subscription does not disable guest selections. */ }
    X11ClipboardService.timer = setInterval((): void => { void X11ClipboardService.pullGuest(); }, 300);
    void X11ClipboardService.pushSystem();
  }

  static stop(): void {
    if (!X11ClipboardService.active) return;
    X11ClipboardService.active = false;
    X11ClipboardService.epoch++;
    if (X11ClipboardService.timer >= 0) clearInterval(X11ClipboardService.timer);
    X11ClipboardService.timer = -1;
    try { pasteboard.getSystemPasteboard().off('update', X11ClipboardService.changed); }
    catch (_) { /* The system service can be unavailable during teardown. */ }
  }

  private static async pushSystem(): Promise<void> {
    if (!X11ClipboardService.active || X11ClipboardService.busy) return;
    const epoch = X11ClipboardService.epoch;
    const sequence = ++X11ClipboardService.readSequence;
    try {
      const data = await pasteboard.getSystemPasteboard().getData();
      if (!X11ClipboardService.active || X11ClipboardService.epoch !== epoch ||
        sequence !== X11ClipboardService.readSequence || X11ClipboardService.busy) return;
      const text = data.getPrimaryText();
      if (text.length > 262144 || (X11ClipboardService.hasText && text === X11ClipboardService.lastText)) return;
      displayRoute.setDisplayRouteClipboard(text);
      X11ClipboardService.lastText = text;
      X11ClipboardService.hasText = true;
    } catch (_) { /* Clipboard access can be denied by the system. */ }
  }

  private static async pullGuest(): Promise<void> {
    if (!X11ClipboardService.active || X11ClipboardService.busy) return;
    const epoch = X11ClipboardService.epoch;
    try {
      const value = displayRoute.getDisplayRouteClipboard();
      if (value.version === X11ClipboardService.version) return;
      if (value.text.length > 262144 || (X11ClipboardService.hasText && value.text === X11ClipboardService.lastText)) {
        X11ClipboardService.version = value.version;
        return;
      }
      X11ClipboardService.busy = true;
      // Invalidate reads issued before this guest selection.
      X11ClipboardService.readSequence++;
      await pasteboard.getSystemPasteboard().setData(
        pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, value.text));
      if (!X11ClipboardService.active || epoch !== X11ClipboardService.epoch) return;
      X11ClipboardService.version = value.version;
      X11ClipboardService.lastText = value.text;
      X11ClipboardService.hasText = true;
    } catch (_) { /* Retry the current selection after transient system errors. */ }
    finally { X11ClipboardService.busy = false; }
  }
}
