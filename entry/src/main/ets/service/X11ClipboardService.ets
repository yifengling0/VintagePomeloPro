import { pasteboard } from '@kit.BasicServicesKit';
import { abilityAccessCtrl, common } from '@kit.AbilityKit';
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
  private static foregroundContext: common.UIAbilityContext | null = null;
  private static permissionAttempted: boolean = false;
  // Neither a system permission dialog nor a system read can be cancelled.
  private static permissionPending: boolean = false;
  private static reads: number = 0;
  private static readPending: boolean = false;
  private static readonly changed: () => void = (): void => { void X11ClipboardService.pushSystem(); };

  static start(): void {
    if (X11ClipboardService.active) return;
    X11ClipboardService.active = true;
    X11ClipboardService.epoch++;
    X11ClipboardService.hasText = false;
    X11ClipboardService.permissionAttempted = false;
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
    X11ClipboardService.readPending = false;
    if (X11ClipboardService.timer >= 0) clearInterval(X11ClipboardService.timer);
    X11ClipboardService.timer = -1;
    try { pasteboard.getSystemPasteboard().off('update', X11ClipboardService.changed); }
    catch (_) { /* The system service can be unavailable during teardown. */ }
  }

  static foreground(context: common.UIAbilityContext): void {
    X11ClipboardService.foregroundContext = context;
    X11ClipboardService.readSequence++;
    void X11ClipboardService.pushSystem();
  }

  static background(context: common.UIAbilityContext): void {
    if (X11ClipboardService.foregroundContext !== context) return;
    X11ClipboardService.foregroundContext = null;
    X11ClipboardService.readSequence++;
  }

  private static async readPermission(context: common.UIAbilityContext, epoch: number): Promise<boolean> {
    try {
      const manager = abilityAccessCtrl.createAtManager();
      if (manager.checkAccessTokenSync(context.applicationInfo.accessTokenId, 'ohos.permission.READ_PASTEBOARD') ===
        abilityAccessCtrl.GrantStatus.PERMISSION_GRANTED) return true;
      if (X11ClipboardService.permissionAttempted || X11ClipboardService.permissionPending) return false;
      X11ClipboardService.permissionAttempted = true;
      X11ClipboardService.permissionPending = true;
      try {
        const result = await manager.requestPermissionsFromUser(context, ['ohos.permission.READ_PASTEBOARD']);
        return result.authResults.length > 0 && result.authResults[0] === abilityAccessCtrl.GrantStatus.PERMISSION_GRANTED;
      } finally {
        X11ClipboardService.permissionPending = false;
        // A dialog may finish after a new session or another game window took
        // focus. Re-read for that owner instead of publishing the old request.
        if (epoch !== X11ClipboardService.epoch || context !== X11ClipboardService.foregroundContext) {
          void X11ClipboardService.pushSystem();
        }
      }
    } catch (_) { return false; }
  }

  private static async readSystemData(): Promise<pasteboard.PasteData | null> {
    if (X11ClipboardService.reads >= 4) {
      X11ClipboardService.readPending = true;
      return null;
    }
    X11ClipboardService.reads++;
    return new Promise<pasteboard.PasteData | null>((resolve): void => {
      const timer = setTimeout((): void => resolve(null), 1000);
      const finished = (): void => {
        clearTimeout(timer);
        X11ClipboardService.reads--;
        if (X11ClipboardService.readPending) {
          X11ClipboardService.readPending = false;
          void X11ClipboardService.pushSystem();
        }
      };
      try {
        pasteboard.getSystemPasteboard().getData().then((value: pasteboard.PasteData): void => {
          resolve(value); finished();
        }).catch((): void => { resolve(null); finished(); });
      } catch (_) { resolve(null); finished(); }
    });
  }

  // Called from a real PasteButton click. Its temporary system grant also
  // works when the persistent clipboard read permission has been denied.
  // Wait for the loop-owned selection before the caller sends Ctrl+V.
  static async prepareSystemPaste(): Promise<boolean> {
    if (!X11ClipboardService.active || !X11ClipboardService.foregroundContext || X11ClipboardService.busy) return false;
    const epoch = X11ClipboardService.epoch;
    const sequence = ++X11ClipboardService.readSequence;
    const deadline = Date.now() + 1000;
    try {
      const data = await X11ClipboardService.readSystemData();
      if (data === null || Date.now() >= deadline) return false;
      if (!X11ClipboardService.active || epoch !== X11ClipboardService.epoch ||
        sequence !== X11ClipboardService.readSequence || X11ClipboardService.busy) return false;
      const text = data.getPrimaryText();
      if (text.length > 262144) return false;
      displayRoute.setDisplayRouteClipboard(text);
      X11ClipboardService.lastText = text;
      X11ClipboardService.hasText = true;
      while (Date.now() < deadline) {
        if (!X11ClipboardService.active || epoch !== X11ClipboardService.epoch ||
          sequence !== X11ClipboardService.readSequence) return false;
        const value = displayRoute.getDisplayRouteClipboard();
        if (value.text === text) {
          X11ClipboardService.version = value.version;
          return true;
        }
        await new Promise<void>((resolve: () => void) => setTimeout(resolve, 10));
      }
    } catch (_) { /* Denial and teardown never affect game startup. */ }
    return false;
  }

  private static async pushSystem(): Promise<void> {
    const context = X11ClipboardService.foregroundContext;
    if (!X11ClipboardService.active || !context || X11ClipboardService.busy) return;
    const epoch = X11ClipboardService.epoch;
    const sequence = ++X11ClipboardService.readSequence;
    try {
      const board = pasteboard.getSystemPasteboard();
      // Metadata checks do not read clipboard contents or display a permission
      // dialog. Ask only when there is text to exchange with an active session.
      if (!await board.hasData() || !board.hasDataType(pasteboard.MIMETYPE_TEXT_PLAIN)) return;
      if (!X11ClipboardService.active || epoch !== X11ClipboardService.epoch ||
        context !== X11ClipboardService.foregroundContext || sequence !== X11ClipboardService.readSequence) return;
      if (!await X11ClipboardService.readPermission(context, epoch)) return;
      if (!X11ClipboardService.active || epoch !== X11ClipboardService.epoch ||
        context !== X11ClipboardService.foregroundContext) return;
      if (sequence !== X11ClipboardService.readSequence) {
        void X11ClipboardService.pushSystem();
        return;
      }
      const data = await X11ClipboardService.readSystemData();
      if (data === null) return;
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
    if (!X11ClipboardService.active || !X11ClipboardService.foregroundContext ||
      X11ClipboardService.permissionPending || X11ClipboardService.busy) return;
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
