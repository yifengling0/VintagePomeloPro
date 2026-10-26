import { window } from '@kit.ArkUI';
import { hilog } from './LogService';
import {
  DesktopWindowMode,
  DisplayMode,
  WineInputSession,
  resolveCornerSafeMarginPx
} from '../model/AppModels';
import { InputDispatcher } from './InputDispatcher';
import { AppSettingsStore } from './AppSettingsStore';

export interface InputOverlaySnapshot {
  session: WineInputSession;
  profileId: string;
  enabled: boolean;
  visible: boolean;
  /** Engine preparation blocks the Wine surface while retaining this window. */
  blocking: boolean;
  controlsVisible: boolean;
  opacity: number;
  createdCount: number;
  destroyedCount: number;
  rendererRebinds: number;
  zLevel: number;
  stressRunning: boolean;
  stressCompleted: number;
  lastError: string;
}

export type InputOverlayListener = (snapshot: InputOverlaySnapshot) => void;

interface OverlayWindowRect {
  left: number;
  top: number;
  width: number;
  height: number;
}

const OVERLAY_DOMAIN = 0x0000;
const OVERLAY_TAG = 'InputOverlay';
const OVERLAY_HIDDEN_ORIGIN: number = -10000;

/** 透明 ArkTS 子窗口的生命周期所有者，不受 Wine XComponent 帧提交影响。 */
export class InputOverlayWindowCoordinator {
  private static instance: InputOverlayWindowCoordinator | null = null;
  private overlayWindow: window.Window | null = null;
  private stage: window.WindowStage | null = null;
  private parentWindow: window.Window | null = null;
  private listeners: Set<InputOverlayListener> = new Set<InputOverlayListener>();
  private rendererStressDriver: (() => boolean) | null = null;
  private stressRemaining: number = 0;
  private attaching: boolean = false;
  private boundsSyncGeneration: number = 0;
  private visibilityGeneration: number = 0;
  private visibilityOp: Promise<void> = Promise.resolve();
  private blockingOp: Promise<void> = Promise.resolve();
  private engineBusy: boolean = false;
  private launchBusy: boolean = false;
  private snapshot: InputOverlaySnapshot = {
    session: { sessionId: '', displayMode: DisplayMode.DESKTOP, toplevelId: 0, active: false },
    profileId: 'default', enabled: true, visible: false, controlsVisible: true, opacity: 72,
    blocking: false,
    createdCount: 0, destroyedCount: 0, rendererRebinds: 0, zLevel: 0,
    stressRunning: false, stressCompleted: 0, lastError: ''
  };

  static getInstance(): InputOverlayWindowCoordinator {
    if (!InputOverlayWindowCoordinator.instance) {
      InputOverlayWindowCoordinator.instance = new InputOverlayWindowCoordinator();
    }
    return InputOverlayWindowCoordinator.instance;
  }

  subscribe(listener: InputOverlayListener): void {
    this.listeners.add(listener);
    listener(this.getSnapshot());
  }

  unsubscribe(listener: InputOverlayListener): void {
    this.listeners.delete(listener);
  }

  getSnapshot(): InputOverlaySnapshot {
    return {
      session: {
        sessionId: this.snapshot.session.sessionId,
        displayMode: this.snapshot.session.displayMode,
        toplevelId: this.snapshot.session.toplevelId,
        active: this.snapshot.session.active
      },
      profileId: this.snapshot.profileId,
      enabled: this.snapshot.enabled,
      visible: this.snapshot.visible,
      blocking: this.snapshot.blocking,
      controlsVisible: this.snapshot.controlsVisible,
      opacity: this.snapshot.opacity,
      createdCount: this.snapshot.createdCount,
      destroyedCount: this.snapshot.destroyedCount,
      rendererRebinds: this.snapshot.rendererRebinds,
      zLevel: this.snapshot.zLevel,
      stressRunning: this.snapshot.stressRunning,
      stressCompleted: this.snapshot.stressCompleted,
      lastError: this.snapshot.lastError
    };
  }

  async attach(stage: window.WindowStage, parentWindow: window.Window,
    sessionId: string, displayMode: DisplayMode, toplevelId: number,
    profileId: string = 'default', controlsVisible: boolean = true, opacity: number = 72,
    enabled: boolean = true, visible: boolean = true): Promise<void> {
    const session: WineInputSession = { sessionId, displayMode, toplevelId, active: true };
    this.snapshot.enabled = enabled;
    if (!enabled) {
      this.update(session, profileId, controlsVisible, opacity);
      await this.detach('virtual-input-disabled');
      return;
    }
    if (this.overlayWindow && this.stage === stage) {
      this.parentWindow = parentWindow;
      this.update(session, profileId, controlsVisible, opacity);
      await this.setVisible(visible);
      await this.syncBounds();
      if (visible) await this.raise();
      this.scheduleBoundsStabilization();
      return;
    }

    // DesktopAbility 的 onForeground / onNewWant 在首个窗口布局完成前可能
    // 紧邻触发。第一轮尚未 showWindow 时不能再创建同名窗口，否则第二轮会
    // 收到 1300002 并把已创建的覆盖层遗弃。
    if (this.attaching && this.stage === stage) {
      this.parentWindow = parentWindow;
      this.update(session, profileId, controlsVisible, opacity);
      return;
    }

    await this.detach('replace-overlay');
    this.stage = stage;
    this.parentWindow = parentWindow;
    this.update(session, profileId, controlsVisible, opacity);
    this.attaching = true;
    try {
      // 必须在创建时写入尺寸和 zLevel。部分 API 23 WindowManager 会在
      // showWindow 后忽略对零尺寸子窗口的层级调整，导致 Wine 的直连
      // Surface 完全盖住 ArkTS 控件。
      const initialRect = parentWindow.getWindowProperties().windowRect;
      let initLeft = initialRect.left;
      let initWidth = initialRect.width;
      if (AppSettingsStore.getInstance().getGlobalSettings().desktopWindowMode === DesktopWindowMode.SAFE_AREA) {
        const margin = resolveCornerSafeMarginPx(initialRect.width, initialRect.height);
        initLeft += margin;
        initWidth = Math.max(1, initWidth - margin * 2);
      }
      hilog.info(OVERLAY_DOMAIN, OVERLAY_TAG,
        'attach session=%{public}s visible=%{public}s rect=%{public}d,%{public}d %{public}dx%{public}d',
        sessionId, controlsVisible ? 'true' : 'false', initLeft, initialRect.top,
        initWidth, initialRect.height);
      let overlay: window.Window | null = null;
      let configuredCreateError: string = '';
      try {
        overlay = await stage.createSubWindowWithOptions('wine_input_overlay', {
          title: '', decorEnabled: false, isModal: false, zLevel: 9000,
          maximizeSupported: false,
          windowRect: {
            left: Math.round(initLeft), top: Math.round(initialRect.top),
            width: Math.max(1, Math.round(initWidth)),
            height: Math.max(1, Math.round(initialRect.height))
          }
        });
        hilog.info(OVERLAY_DOMAIN, OVERLAY_TAG, 'configured subwindow created');
      } catch (error) {
        // 有些 API 23 平板在全屏 Ability 中拒绝带 options 的子窗口，但
        // 基础 createSubWindow 仍可用。不能因为第一次创建被拒绝就放弃
        // 整个输入层，否则 Wine surface 会把所有 ArkTS 控件完全盖住。
        configuredCreateError = JSON.stringify(error);
        hilog.warn(OVERLAY_DOMAIN, OVERLAY_TAG, 'configured subwindow rejected: %{public}s', configuredCreateError);
      }
      if (!overlay) {
        try {
          overlay = await stage.createSubWindow('wine_input_overlay_fallback');
          hilog.info(OVERLAY_DOMAIN, OVERLAY_TAG, 'fallback subwindow created');
        } catch (fallbackError) {
          throw new Error('InputOverlayWindow unavailable: configured=' +
            configuredCreateError + '; fallback=' + JSON.stringify(fallbackError));
        }
      }
      if (!overlay) {
        throw new Error('InputOverlayWindow was not created');
      }
      this.overlayWindow = overlay;
      try { await overlay.setWindowFocusable(false); } catch (_) {}
      try { await overlay.setWindowDecorVisible(false); } catch (_) {}
      await overlay.loadContent('pages/InputOverlay');
      hilog.info(OVERLAY_DOMAIN, OVERLAY_TAG, 'overlay content loaded');
      await this.syncBounds();
      await overlay.showWindow();
      hilog.info(OVERLAY_DOMAIN, OVERLAY_TAG, 'overlay shown');
      // API 23 对刚 create 的子窗口调用该接口会返回 1300002；窗口 show 后
      // 再设透明背景才进入合法状态，也避免 create 成功却没有页面的假失败。
      try {
        overlay.setWindowBackgroundColor('#00000000');
      } catch (error) {
        this.snapshot.lastError = 'setWindowBackgroundColor: ' + JSON.stringify(error);
        hilog.warn(OVERLAY_DOMAIN, OVERLAY_TAG, 'set transparent background failed: %{public}s', this.snapshot.lastError);
      }
      try {
        await overlay.setSubWindowZLevel(9000);
        this.snapshot.zLevel = overlay.getSubWindowZLevel();
        hilog.info(OVERLAY_DOMAIN, OVERLAY_TAG, 'overlay zLevel=%{public}d', this.snapshot.zLevel);
      } catch (error) {
        this.snapshot.lastError = 'setSubWindowZLevel: ' + JSON.stringify(error);
        hilog.error(OVERLAY_DOMAIN, OVERLAY_TAG, 'set zLevel failed: %{public}s', this.snapshot.lastError);
      }
      try { await overlay.setRaiseByClickEnabled(false); } catch (_) {}
      this.snapshot.createdCount++;
      overlay.on('windowEvent', (eventType: number) => {
        if (eventType === window.WindowEventType.WINDOW_DESTROYED) {
          this.overlayWindow = null;
          this.snapshot.destroyedCount++;
          this.snapshot.visible = false;
          this.publish();
        }
      });
      await this.setVisible(visible);
      if (visible) await this.raise();
      this.scheduleBoundsStabilization();
      this.publish();
    } catch (error) {
      this.snapshot.lastError = 'create InputOverlayWindow: ' + JSON.stringify(error);
      this.overlayWindow = null;
      hilog.error(OVERLAY_DOMAIN, OVERLAY_TAG, 'attach failed: %{public}s', this.snapshot.lastError);
      this.publish();
    } finally {
      this.attaching = false;
    }
  }

  update(session: WineInputSession, profileId: string, controlsVisible: boolean, opacity: number): void {
    this.snapshot.session = session;
    this.snapshot.profileId = profileId || 'default';
    this.snapshot.controlsVisible = controlsVisible;
    this.snapshot.opacity = Math.max(25, Math.min(100, opacity));
    InputDispatcher.getInstance().setSession(session);
    this.publish();
  }

  /**
   * Keep the retained child window above the native Wine surface while the
   * engine is extracting or rebuilding its runtime. The blocker is rendered
   * by InputOverlayPage; this method only controls the window's hit-test and
   * visibility so a compositor frame cannot bypass it.
   */
  async setEngineBusy(busy: boolean): Promise<void> {
    this.engineBusy = busy;
    await this.syncBlocking();
  }

  async setLaunchBusy(busy: boolean): Promise<void> {
    this.launchBusy = busy;
    await this.syncBlocking();
  }

  private async syncBlocking(): Promise<void> {
    // Engine completion must not expose an empty native surface while Steam
    // is still starting. Serialize window operations and read the latest reasons.
    const pending = this.blockingOp.then((): Promise<void> => this.applyBlocking(this.engineBusy || this.launchBusy));
    this.blockingOp = pending.catch((): void => {});
    await pending;
  }

  private async applyBlocking(busy: boolean): Promise<void> {
    if (this.snapshot.blocking === busy && (!busy || this.snapshot.visible)) return;
    this.snapshot.blocking = busy;
    if (this.overlayWindow) {
      if (busy) {
        this.snapshot.visible = true;
        try { await this.overlayWindow.showWindow(); } catch (_) {}
        try { await this.overlayWindow.setWindowTouchable(true); } catch (error) {
          this.snapshot.lastError = 'setWindowTouchable(blocking): ' + JSON.stringify(error);
        }
        await this.raise();
      } else {
        const visible = this.snapshot.visible && this.snapshot.enabled;
        try { await this.overlayWindow.setWindowTouchable(visible); } catch (error) {
          this.snapshot.lastError = 'setWindowTouchable(ready): ' + JSON.stringify(error);
        }
        if (visible) await this.raise();
      }
    }
    this.publish();
  }

  async setVisible(visible: boolean): Promise<void> {
    const generation = ++this.visibilityGeneration;
    const resolvedVisible = visible && this.snapshot.enabled;
    this.snapshot.visible = resolvedVisible;
    if (!resolvedVisible) {
      InputDispatcher.getInstance().releaseAll('overlay-hidden');
    }
    const pending: Promise<void> = this.visibilityOp.then((): Promise<void> => {
      return this.applyVisibility(generation, resolvedVisible);
    }).catch((): Promise<void> => {
      return this.applyVisibility(generation, resolvedVisible);
    });
    this.visibilityOp = pending.then((): void => {}).catch((): void => {});
    await pending;
    if (generation === this.visibilityGeneration) {
      this.publish();
    }
  }

  /**
   * Subwindows have no hide(). Keep the retained overlay off-screen and
   * non-touchable instead of collapsing it to 1x1 at (0,0), which can surface
   * as a titled speck and race with the next syncBounds.
   */
  private async applyVisibility(generation: number, resolvedVisible: boolean): Promise<void> {
    if (generation !== this.visibilityGeneration) {
      return;
    }
    const overlay = this.overlayWindow;
    if (!overlay) {
      return;
    }
    if (resolvedVisible) {
      try {
        await overlay.showWindow();
      } catch (error) {
        this.snapshot.lastError = 'showWindow: ' + JSON.stringify(error);
      }
      if (generation !== this.visibilityGeneration) {
        return;
      }
      await this.syncBounds();
      if (generation !== this.visibilityGeneration) {
        return;
      }
      try {
        await overlay.setWindowTouchable(true);
      } catch (error) {
        this.snapshot.lastError = 'setWindowTouchable: ' + JSON.stringify(error);
      }
      await this.raise();
      return;
    }
    try {
      await overlay.setWindowTouchable(false);
    } catch (error) {
      this.snapshot.lastError = 'setWindowTouchable: ' + JSON.stringify(error);
    }
    if (generation !== this.visibilityGeneration) {
      return;
    }
    this.boundsSyncGeneration++;
    try {
      await overlay.moveWindowToGlobal(OVERLAY_HIDDEN_ORIGIN, OVERLAY_HIDDEN_ORIGIN);
    } catch (error) {
      this.snapshot.lastError = 'collapse overlay: ' + JSON.stringify(error);
    }
  }

  async setEnabled(enabled: boolean): Promise<void> {
    if (this.snapshot.enabled === enabled) return;
    this.snapshot.enabled = enabled;
    if (!enabled) {
      await this.detach('virtual-input-disabled');
      return;
    }
    this.publish();
  }

  setControlsVisible(visible: boolean): void {
    if (this.snapshot.controlsVisible === visible) return;
    this.snapshot.controlsVisible = visible;
    if (!visible) InputDispatcher.getInstance().releaseAll('virtual-controls-hidden');
    this.publish();
  }

  setOpacity(opacity: number): void {
    this.snapshot.opacity = Math.max(25, Math.min(100, opacity));
    this.publish();
  }

  async raise(): Promise<void> {
    if (!this.overlayWindow) return;
    try { await this.overlayWindow.raiseToAppTop(); } catch (error) {
      this.snapshot.lastError = 'raiseToAppTop: ' + JSON.stringify(error);
      this.publish();
    }
  }

  async syncBounds(): Promise<void> {
    if (!this.overlayWindow || !this.parentWindow) {
      return;
    }
    if (!this.snapshot.visible && !this.attaching) {
      return;
    }
    try {
      const rect = this.parentWindow.getWindowProperties().windowRect;
      // 切边模式: 输入覆盖层与桌面 XComponent 同步左右内缩, 使覆盖层
      // 局部坐标 == XComponent surface 坐标, 虚拟鼠标/触摸点击不再偏移。
      let left = rect.left;
      let width = rect.width;
      if (AppSettingsStore.getInstance().getGlobalSettings().desktopWindowMode === DesktopWindowMode.SAFE_AREA) {
        const margin = resolveCornerSafeMarginPx(rect.width, rect.height);
        left += margin;
        width = Math.max(1, width - margin * 2);
      }
      await this.overlayWindow.resize(Math.max(1, Math.round(width)), Math.max(1, Math.round(rect.height)));
      await this.overlayWindow.moveWindowToGlobal(Math.round(left), Math.round(rect.top));
    } catch (error) {
      this.snapshot.lastError = 'syncBounds: ' + JSON.stringify(error);
      this.publish();
    }
  }

  /**
   * Ability foreground animations can temporarily report the parent at the
   * off-screen transition coordinate (for example x=2910 on a 2880px panel).
   * Re-sample after the animation so the retained child window returns to the
   * real parent bounds instead of remaining just outside the display.
   */
  private scheduleBoundsStabilization(): void {
    const generation = ++this.boundsSyncGeneration;
    setTimeout((): void => {
      if (generation !== this.boundsSyncGeneration || !this.snapshot.visible) return;
      this.syncBounds().then((): void => { this.raise().catch((): void => {}); }).catch((): void => {});
    }, 120);
    setTimeout((): void => {
      if (generation !== this.boundsSyncGeneration || !this.snapshot.visible) return;
      this.syncBounds().then((): void => { this.raise().catch((): void => {}); }).catch((): void => {});
    }, 420);
  }

  setActiveToplevel(toplevelId: number): void {
    if (toplevelId <= 0) return;
    const previous = this.snapshot.session;
    // DesktopAbility 可能先于 explorer toplevel 创建。拿到真实 root 后补全
    // 会话 ID，保证 diagnostics、恢复和输入归属使用同一个实际会话。
    const sessionId = previous.displayMode === DisplayMode.DESKTOP &&
      (previous.sessionId.length === 0 || previous.sessionId === 'desktop-0')
      ? 'desktop-' + toplevelId : previous.sessionId;
    this.snapshot.session = {
      sessionId: sessionId,
      displayMode: previous.displayMode,
      toplevelId: toplevelId,
      active: previous.active
    };
    InputDispatcher.getInstance().setSession(this.snapshot.session);
    this.publish();
  }

  markRendererRebound(): void {
    this.snapshot.rendererRebinds++;
    this.publish();
  }

  registerRendererStressDriver(driver: () => boolean): void {
    this.rendererStressDriver = driver;
  }

  unregisterRendererStressDriver(): void {
    this.rendererStressDriver = null;
    this.stressRemaining = 0;
    this.snapshot.stressRunning = false;
    this.publish();
  }

  /** Debug HAP 自检入口：异步重绑定，避免阻塞 ArkTS UI 线程。 */
  runRendererStress(rounds: number = 300): boolean {
    if (!this.rendererStressDriver || this.snapshot.stressRunning || rounds <= 0) return false;
    this.stressRemaining = Math.min(300, Math.max(1, Math.round(rounds)));
    this.snapshot.stressRunning = true;
    this.snapshot.stressCompleted = 0;
    this.snapshot.lastError = '';
    this.publish();
    this.runNextStressFrame();
    return true;
  }

  private runNextStressFrame(): void {
    if (!this.snapshot.stressRunning) return;
    if (!this.rendererStressDriver || this.stressRemaining <= 0) {
      this.snapshot.stressRunning = false;
      this.publish();
      return;
    }
    try {
      if (!this.rendererStressDriver()) {
        this.snapshot.lastError = 'renderer stress driver rejected rebind';
        this.snapshot.stressRunning = false;
        this.publish();
        return;
      }
      this.snapshot.stressCompleted++;
      this.stressRemaining--;
    } catch (error) {
      this.snapshot.lastError = 'renderer stress: ' + JSON.stringify(error);
      this.snapshot.stressRunning = false;
      this.publish();
      return;
    }
    this.publish();
    setTimeout(() => this.runNextStressFrame(), 16);
  }

  /** Overlay window bounds in physical pixels. Size is the virtual-cursor clamp. */
  getOverlayWindowRect(): OverlayWindowRect {
    if (!this.overlayWindow) {
      return { left: 0, top: 0, width: 0, height: 0 };
    }
    try {
      return this.overlayWindow.getWindowProperties().windowRect;
    } catch (_) {
      return { left: 0, top: 0, width: 0, height: 0 };
    }
  }

  async detach(reason: string): Promise<void> {
    this.visibilityGeneration++;
    this.boundsSyncGeneration++;
    InputDispatcher.getInstance().releaseAll(reason);
    const overlay = this.overlayWindow;
    this.overlayWindow = null;
    this.stage = null;
    this.parentWindow = null;
    this.snapshot.visible = false;
    this.snapshot.session.active = false;
    if (overlay) {
      try { await overlay.destroyWindow(); } catch (_) {}
    }
    this.publish();
  }

  private publish(): void {
    const snapshot = this.getSnapshot();
    this.listeners.forEach((listener: InputOverlayListener): void => listener(snapshot));
  }
}
