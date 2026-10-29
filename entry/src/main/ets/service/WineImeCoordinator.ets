import { inputMethod } from '@kit.IMEKit';
import { window } from '@kit.ArkUI';
import testNapi from 'libentry.so';
import { DeviceCapabilityPolicy } from '../common/DeviceCapabilityPolicy';
import { hilog } from './LogService';

// One process-wide IME bridge, bound to the actual foreground host window.
// Wine's text-input protocol remains the authority for editable focus; raw
// gameplay keys are left alone until Wine requests a real text caret.
class WineImeHandlers {
  private current: () => boolean;
  constructor(current: () => boolean) { this.current = current; }
  insertText = (text: string): void => {
    if (this.current()) testNapi.wineTextInputCommit(text);
  };
  previewText = (text: string, _range: inputMethod.Range): void => {
    if (this.current()) testNapi.wineTextInputPreedit(text);
  };
  finishPreview = (): void => {
    if (this.current()) testNapi.wineTextInputPreedit('');
  };
  deleteLeft = (length: number): void => { this.deleteKeys(length, 14); };
  deleteRight = (length: number): void => { this.deleteKeys(length, 111); };
  functionKey = (key: inputMethod.FunctionKey): void => {
    if (key.enterKeyType === inputMethod.EnterKeyType.DONE ||
        key.enterKeyType === inputMethod.EnterKeyType.NEWLINE) this.deleteKeys(1, 28);
  };
  private deleteKeys(length: number, code: number): void {
    if (!this.current() || !Number.isFinite(length)) return;
    const top = testNapi.wineTextInputFocusedToplevel();
    if (top <= 0) return;
    for (let i = 0; i < Math.min(1024, Math.max(0, Math.floor(length))); i++) {
      testNapi.sendKeyEvent(top, code, true);
      testNapi.sendKeyEvent(top, code, false);
    }
  }
}

export class WineImeCoordinator {
  private static instance: WineImeCoordinator | null = null;
  private initialized: boolean = false;
  private host: window.Window | null = null;
  private hostTop: number = 0;
  private automatic: boolean = false;
  private generation: number = 0;
  private operations: Promise<void> = Promise.resolve();
  private controller: inputMethod.InputMethodController | null = null;
  private handlers: WineImeHandlers | null = null;
  private explicitKeyboard: boolean = false;
  private windowObservers: Map<window.Window, (event: window.WindowEventType) => void> = new Map();

  static getInstance(): WineImeCoordinator {
    if (!WineImeCoordinator.instance) WineImeCoordinator.instance = new WineImeCoordinator();
    return WineImeCoordinator.instance;
  }

  initialize(): void {
    if (this.initialized) return;
    this.initialized = true;
    testNapi.setImeCallback((_active: number, _x: number, _y: number, _w: number, _h: number): void => {
      // The callback crosses threads; query current focus instead of trusting
      // an old leave event delivered after another window acquired the caret.
      this.reconcile();
    });
  }

  observeWindow(top: number, host: window.Window, automatic: boolean = false): void {
    if (this.windowObservers.has(host)) return;
    // PC windows can remain foreground together. Ability lifecycle callbacks
    // do not report clicks between them; bind the IME to actual window focus.
    const listener = (event: window.WindowEventType): void => {
      if (event === window.WindowEventType.WINDOW_ACTIVE) this.foreground(top, host, automatic);
      else if (event === window.WindowEventType.WINDOW_INACTIVE ||
          event === window.WindowEventType.WINDOW_HIDDEN) this.background(host);
      else if (event === window.WindowEventType.WINDOW_DESTROYED) this.unobserveWindow(host);
    };
    try {
      host.on('windowEvent', listener);
      this.windowObservers.set(host, listener);
    } catch (error) {
      hilog.warn(0, 'WineIME', 'window focus observer failed: %{public}s', JSON.stringify(error));
    }
  }

  unobserveWindow(host: window.Window | null): void {
    if (!host) return;
    const listener = this.windowObservers.get(host);
    if (listener) {
      this.windowObservers.delete(host);
      try { host.off('windowEvent', listener); } catch (_) {}
    }
    this.background(host);
  }

  foreground(top: number, host: window.Window, automatic: boolean = false): void {
    this.initialize();
    const nextAutomatic = automatic && DeviceCapabilityPolicy.usesManagedWineWindows();
    if (this.host === host && this.hostTop === top && this.automatic === nextAutomatic) return;
    this.host = host;
    this.hostTop = top;
    this.automatic = nextAutomatic;
    this.syncArmed();
    this.reconcile();
  }

  background(host: window.Window | null): void {
    // A late background/destroy from the old Ability cannot detach the new IME.
    if (!host || this.host !== host) return;
    this.host = null;
    this.automatic = false;
    this.syncArmed();
    this.reconcile();
  }

  setKeyboardVisible(visible: boolean): void {
    this.initialize();
    if (this.explicitKeyboard === visible) return;
    this.explicitKeyboard = visible;
    this.syncArmed();
    this.reconcile();
  }

  private syncArmed(): void {
    testNapi.wineTextInputSetArmed(this.host !== null && (this.automatic || this.explicitKeyboard));
  }

  private ownsFocus(): boolean {
    return this.host !== null && testNapi.wineTextInputActive() &&
      (this.hostTop === 0 || testNapi.wineTextInputFocusedToplevel() === this.hostTop);
  }

  hasAttachedSession(): boolean {
    return this.controller !== null && this.ownsFocus();
  }

  private removeHandlers(controller: inputMethod.InputMethodController): void {
    const handlers = this.handlers;
    if (!handlers) return;
    this.handlers = null;
    // Remove only this bridge's listeners, never an ArkUI TextInput's handlers.
    controller.off('insertText', handlers.insertText);
    controller.off('deleteLeft', handlers.deleteLeft);
    controller.off('deleteRight', handlers.deleteRight);
    controller.off('setPreviewText', handlers.previewText);
    controller.off('finishTextPreview', handlers.finishPreview);
    controller.off('sendFunctionKey', handlers.functionKey);
  }

  private reconcile(): void {
    const generation = ++this.generation;
    const run = this.operations.then(async (): Promise<void> => {
      if (generation !== this.generation) return;
      if (this.controller) {
        const old = this.controller;
        this.controller = null;
        this.removeHandlers(old);
        try { await old.detach(); } catch (_) {}
      }
      const host = this.host;
      if (generation !== this.generation || !host || !this.ownsFocus()) return;
      const controller = inputMethod.getController();
      const config: inputMethod.TextConfig = {
        inputAttribute: { textInputType: inputMethod.TextInputType.TEXT,
          enterKeyType: inputMethod.EnterKeyType.NEWLINE },
        windowId: host.getWindowProperties().id
      };
      // PC hardware IMEs need an input session, without opening a touch panel.
      // Tablet/phone retain the explicit keyboard toggle's existing behavior.
      await controller.attach(this.explicitKeyboard, config);
      if (generation !== this.generation || this.host !== host || !this.ownsFocus()) {
        try { await controller.detach(); } catch (_) {}
        return;
      }
      this.controller = controller;
      const handlers = new WineImeHandlers((): boolean => generation === this.generation &&
        this.host === host && this.ownsFocus());
      this.handlers = handlers;
      controller.on('insertText', handlers.insertText);
      controller.on('deleteLeft', handlers.deleteLeft);
      controller.on('deleteRight', handlers.deleteRight);
      controller.on('setPreviewText', handlers.previewText);
      controller.on('finishTextPreview', handlers.finishPreview);
      controller.on('sendFunctionKey', handlers.functionKey);
      hilog.info(0, 'WineIME', 'attached hostWindow=%{public}d toplevel=%{public}d', config.windowId, this.hostTop);
    });
    this.operations = run.catch((error: Error): void => {
      hilog.warn(0, 'WineIME', 'input session failed: %{public}s', error.message);
    });
  }
}
