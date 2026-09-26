import { common, Want } from '@kit.AbilityKit';
import { mapMouseButton } from '../common/MouseMap';
import {
  EVDEV_KEY_BACKSPACE,
  EVDEV_KEY_ENTER,
  EVDEV_KEY_LEFTSHIFT,
  OH2EVDEV,
  asciiToEvdevKey
} from '../common/KeyMap';
import { InputRouter } from '../service/InputRouter';
import { GamepadManager } from '../service/GamepadManager';
import { WineWindowManager } from '../service/WineWindowManager';
import {
  DisplayMode,
  GlobalSettings,
  InputProfile,
  createDefaultGlobalSettings,
  createDefaultInputProfile
} from '../model/AppModels';
import { VirtualInputOverlay } from '../components/VirtualInputOverlay';
import { InputDispatcher } from '../service/InputDispatcher';
import { InputOverlayListener, InputOverlaySnapshot, InputOverlayWindowCoordinator } from '../service/InputOverlayWindowCoordinator';
import { InputProfileService } from '../service/InputProfileService';
import { AppSettingsStore } from '../service/AppSettingsStore';
import { AppSessionService } from '../service/AppSessionService';
import { InputEventGate } from '../service/InputEventGate';
import { EngineState } from '../model/AppModels';
import { EngineStateListener, WineEngineService } from '../service/WineEngineService';
import { WineEngineBlockingOverlay } from '../components/WineEngineBlockingOverlay';
import { hilog } from '@kit.PerformanceAnalysisKit';
import testNapi from 'libentry.so';

interface DirectTouch {
  x: number;
  y: number;
  startX: number;
  startY: number;
  moved: boolean;
  buttonDown: boolean;
  consumed: boolean;
  startTime: number;
}

interface TouchpadFinger {
  id: number;
  startX: number;
  startY: number;
  lastX: number;
  lastY: number;
  startTime: number;
  moved: boolean;
}

/** 全屏透明应用子窗口的 ArkTS 根页面。 */
@Entry
@Component
struct InputOverlayPage {
  @State private snapshot: InputOverlaySnapshot = InputOverlayWindowCoordinator.getInstance().getSnapshot();
  @State private profile: InputProfile = createDefaultInputProfile();
  @State private settings: GlobalSettings = createDefaultGlobalSettings();
  @State private cursorX: number = 0;
  @State private cursorY: number = 0;
  @State private cursorVisible: boolean = false;
  @State private cursorFlash: boolean = false;
  @State private touchpadMode: boolean = false;
  @State private hostKeyboardActive: boolean = false;
  @State private hostImeBuffer: string = '';
  @State private overlayW: number = 0;
  @State private overlayH: number = 0;
  @State private engineState: EngineState = WineEngineService.getInstance().getState();
  // 浮窗模式全屏态: 显示缩小按钮 (Index 桌面层容器被本 overlay 覆盖, 按钮须在此)
  @StorageProp('desktopFloatingFullscreen') desktopFloatingFullscreen: boolean = false;
  private listener: InputOverlayListener | null = null;
  private engineListener: EngineStateListener | null = null;
  private context: common.UIAbilityContext = getContext(this) as common.UIAbilityContext;
  private directTouches: Map<number, DirectTouch> = new Map();
  private twoFinger: boolean = false;
  private twoFingerTapEligible: boolean = false;
  private lastMidX: number = 0;
  private lastMidY: number = 0;
  private lastDirectTapAt: number = 0;
  private lastDirectTapX: number = 0;
  private lastDirectTapY: number = 0;
  private eventGate: InputEventGate = new InputEventGate();
  private hostImeController: TextInputController = new TextInputController();
  private hostImeContent: string = '';
  private hostImeReseedPending: boolean = false;
  private pendingHostCommit: string = '';
  private static readonly HOST_SENTINEL: string = '\uF8FF';
  private readonly TAP_TIMEOUT_MS: number = 450;
  private readonly LONG_PRESS_MS: number = 600;   // 直接触摸长按=右键 判定时长
  private readonly TAP_SLOP_PX: number = 48;
  private cursorHideTimer: number = -1;
  private cursorFlashTimer: number = -1;
  private touchpadFingers: Map<number, TouchpadFinger> = new Map<number, TouchpadFinger>();
  private touchpadTwoFinger: boolean = false;
  private touchpadSuppressSingleTap: boolean = false;
  private touchpadTwoFingerTapEligible: boolean = false;
  private touchpadLastMidX: number = 0;
  private touchpadLastMidY: number = 0;
  private touchpadLastTapAt: number = 0;
  private touchpadDragTouchId: number = -1;
  private touchpadDragCandidate: boolean = false;
  private touchpadDragActive: boolean = false;
  // 触摸板"轻点确认": 手指抬起后延迟一小段窗口再触发点击。若窗口内手指重新
  // 落下并滑动 (抬起→再触摸→移动 = 分段移动光标), 取消该点击 — 区分"轻点
  // (点击)"与"分段滑动(移动)"。双击 (两次轻点) 不受影响: 第二次轻点无移动
  // 时不取消, 两下确认窗口各自触发。
  private touchpadPendingTapTimer: number = -1;
  private touchpadReTouchSinceTap: boolean = false;
  private static readonly TOUCHPAD_TAP_CONFIRM_MS: number = 150;
  private mouseLeftActive: boolean = false;
  private mouseLeftReleaseTimer: number = -1;
  private lastEnterDownTs: number = 0;
  private static readonly ENTER_SYNTH_WINDOW_MS: number = 1000;

  aboutToAppear(): void {
    this.listener = (snapshot: InputOverlaySnapshot): void => {
      const profileChanged = this.snapshot.profileId !== snapshot.profileId;
      const sessionChanged = this.snapshot.session.sessionId !== snapshot.session.sessionId;
      const becameVisible = !this.snapshot.visible && snapshot.visible;
      this.snapshot = snapshot;
      this.settings = AppSettingsStore.getInstance().getGlobalSettings();
      if (sessionChanged) {
        this.directTouches.clear();
        this.twoFinger = false;
        this.twoFingerTapEligible = false;
        this.resetDirectTapSequence();
        this.resetTouchpadGesture();
        this.touchpadMode = false;
        this.cursorVisible = false;
      }
      // Editors update the contents of an existing profile ID. Reload when
      // returning to a Wine session as well as when the ID changes so saved
      // fixed/free layouts take effect without restarting the application.
      if (profileChanged || sessionChanged || becameVisible) this.loadProfile(snapshot.profileId);
    };
    InputOverlayWindowCoordinator.getInstance().subscribe(this.listener);
    this.engineListener = (state: EngineState): void => {
      this.engineState = state;
      const busy = state === EngineState.PREPARING || state === EngineState.SWITCHING;
      InputOverlayWindowCoordinator.getInstance().setEngineBusy(busy).catch(() => {});
    };
    WineEngineService.getInstance().subscribe(this.engineListener);
    InputOverlayWindowCoordinator.getInstance().setEngineBusy(
      this.engineState === EngineState.PREPARING || this.engineState === EngineState.SWITCHING).catch(() => {});
    this.settings = AppSettingsStore.getInstance().getGlobalSettings();
    this.loadProfile(this.snapshot.profileId);
  }

  aboutToDisappear(): void {
    if (this.listener) InputOverlayWindowCoordinator.getInstance().unsubscribe(this.listener);
    if (this.engineListener) WineEngineService.getInstance().unsubscribe(this.engineListener);
    this.cancelCursorTimers();
    if (this.mouseLeftReleaseTimer >= 0) clearTimeout(this.mouseLeftReleaseTimer);
    this.mouseLeftReleaseTimer = -1;
    this.mouseLeftActive = false;
    this.resetTouchpadGesture();
    InputDispatcher.getInstance().releaseAll('overlay-page-disappear');
    testNapi.wineTextInputSetArmed(false);
  }

  private cancelCursorTimers(): void {
    if (this.cursorHideTimer >= 0) clearTimeout(this.cursorHideTimer);
    if (this.cursorFlashTimer >= 0) clearTimeout(this.cursorFlashTimer);
    this.cursorHideTimer = -1;
    this.cursorFlashTimer = -1;
  }

  private showCursorAt(x: number, y: number, flash: boolean = false): void {
    if (!this.touchpadMode || !this.settings.virtualMouseConfig.showCursor) return;
    this.cursorX = x;
    this.cursorY = y;
    this.cursorVisible = true;
    if (this.cursorHideTimer >= 0) clearTimeout(this.cursorHideTimer);
    this.cursorHideTimer = setTimeout((): void => {
      this.cursorHideTimer = -1;
      this.cursorVisible = false;
    }, 900) as number;
    if (!flash) return;
    this.cursorFlash = true;
    if (this.cursorFlashTimer >= 0) clearTimeout(this.cursorFlashTimer);
    this.cursorFlashTimer = setTimeout((): void => {
      this.cursorFlashTimer = -1;
      this.cursorFlash = false;
    }, 90) as number;
  }

  private scrollThresholdPx(): number {
    const sensitivity = Math.max(50, Math.min(200, this.settings.virtualMouseConfig.mouseSensitivity));
    return Math.max(6, 18 * 100 / sensitivity);
  }

  private loadProfile(profileId: string): void {
    InputProfileService.getInstance().initialize(this.context).then(() => {
      this.profile = InputProfileService.getInstance().getProfile(profileId || 'default');
    }).catch(() => {});
  }

  private directSource(touchId: number): string {
    return 'direct-touch-' + touchId;
  }

  private midpointY(): number {
    const values = Array.from(this.directTouches.values());
    if (values.length < 2) return 0;
    return (values[0].y + values[1].y) / 2;
  }

  private midpointX(): number {
    const values = Array.from(this.directTouches.values());
    if (values.length < 2) return 0;
    return (values[0].x + values[1].x) / 2;
  }

  private injectClick(source: string, x: number, y: number, button: number): void {
    InputDispatcher.getInstance().pointerDown(source, x, y, button);
    InputDispatcher.getInstance().pointerUp(source, x, y, button);
  }

  private resetDirectTapSequence(): void {
    this.lastDirectTapAt = 0;
    this.lastDirectTapX = 0;
    this.lastDirectTapY = 0;
  }

  private injectDirectLeftTap(x: number, y: number): void {
    const now = Date.now();
    const interval = now - this.lastDirectTapAt;
    const spatialSlop = Math.max(this.TAP_SLOP_PX, vp2px(24));
    const nearPrevious = Math.abs(x - this.lastDirectTapX) <= spatialSlop &&
      Math.abs(y - this.lastDirectTapY) <= spatialSlop;
    const isSecondTap = this.lastDirectTapAt > 0 && interval <= 520 && nearPrevious;
    // Finger taps rarely land on the exact same pixel. Anchor the second
    // click to the first tap so Wine/Windows can recognise the pair using its
    // normal double-click state machine without changing single-click delay.
    this.injectClick('direct-rdp-left',
      isSecondTap ? this.lastDirectTapX : x,
      isSecondTap ? this.lastDirectTapY : y, 272);
    if (isSecondTap) {
      this.resetDirectTapSequence();
    } else {
      this.lastDirectTapAt = now;
      this.lastDirectTapX = x;
      this.lastDirectTapY = y;
    }
  }

  private handleDirectTouch(event: TouchEvent): void {
    if (!this.eventGate.shouldHandleTouch(event.source, event.sourceTool)) return;
    event.stopPropagation();
    for (let index = 0; index < event.changedTouches.length; index++) {
      const touch = event.changedTouches[index];
      // This page lives in a retained transparent subwindow and the event can
      // bubble from any virtual-control child. touch.x/y are relative to that
      // hit component, so their origin changes with the child/layout that won
      // hit testing. The overlay window itself is kept pixel-aligned with the
      // Wine XComponent; windowX/windowY therefore provide the stable input
      // space expected by native hit testing and letterbox unmapping.
      const x = vp2px(touch.windowX);
      const y = vp2px(touch.windowY);
      const source = this.directSource(touch.id);
      if (event.type === TouchType.Down) {
        this.directTouches.set(touch.id, {
          x, y, startX: x, startY: y, moved: false, buttonDown: false, consumed: false, startTime: Date.now()
        });
        if (this.directTouches.size === 2) {
          this.resetDirectTapSequence();
          this.twoFinger = true;
          this.twoFingerTapEligible = true;
          this.lastMidX = this.midpointX();
          this.lastMidY = this.midpointY();
          // 双指只负责滚动：取消可能中的点击/拖拽，但不触及虚拟手柄按键。
          this.directTouches.forEach((value: DirectTouch, activeTouchId: number): void => {
            value.consumed = true;
            if (value.buttonDown) InputDispatcher.getInstance().releaseSource(this.directSource(activeTouchId));
          });
        } else if (this.directTouches.size > 2) {
          this.twoFingerTapEligible = false;
        }
      } else if (event.type === TouchType.Move) {
        const previous = this.directTouches.get(touch.id);
        if (!previous) continue;
        const moved = previous.moved || Math.abs(x - previous.startX) + Math.abs(y - previous.startY) > this.TAP_SLOP_PX;
        this.directTouches.set(touch.id, {
          x, y, startX: previous.startX, startY: previous.startY, moved,
          buttonDown: previous.buttonDown, consumed: previous.consumed, startTime: previous.startTime
        });
        if (this.twoFinger && this.directTouches.size >= 2) {
          const midX = this.midpointX();
          const midY = this.midpointY();
          const deltaX = this.lastMidX - midX;
          const delta = this.lastMidY - midY;
          const threshold = this.scrollThresholdPx();
          if (Math.abs(deltaX) >= threshold) {
            this.twoFingerTapEligible = false;
            InputDispatcher.getInstance().scroll('direct-scroll', 1, deltaX > 0 ? 1 : -1);
            this.lastMidX = midX;
          }
          if (Math.abs(delta) >= threshold) {
            this.twoFingerTapEligible = false;
            InputDispatcher.getInstance().scroll('direct-scroll', 0, delta > 0 ? 1 : -1);
            this.lastMidY = midY;
          }
        } else if (moved && !previous.consumed) {
          if (!previous.buttonDown) {
            InputDispatcher.getInstance().pointerDown(source, previous.startX, previous.startY, 272);
            const current = this.directTouches.get(touch.id);
            if (current) current.buttonDown = true;
          }
          InputDispatcher.getInstance().pointerMove(source, x, y);
        }
      } else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
        const previous = this.directTouches.get(touch.id);
        if (!previous) continue;
        const elapsed = Date.now() - previous.startTime;
        const wasTwoFinger = this.twoFinger || previous.consumed;
        this.directTouches.delete(touch.id);
        if (wasTwoFinger) {
          if (previous.moved || elapsed >= this.TAP_TIMEOUT_MS || event.type === TouchType.Cancel) {
            this.twoFingerTapEligible = false;
          }
          InputDispatcher.getInstance().releaseSource(source);
          if (this.directTouches.size === 0) {
            // 直接触摸双指仅用于滚动, 右键改走单指长按 (双指轻点难触发)。
            InputDispatcher.getInstance().releaseSource('direct-scroll');
            this.twoFinger = false;
            this.twoFingerTapEligible = false;
          }
        } else if (previous.buttonDown) {
          this.resetDirectTapSequence();
          InputDispatcher.getInstance().pointerUp(source, x, y, 272);
        } else if (event.type === TouchType.Up && !previous.moved && elapsed < this.TAP_TIMEOUT_MS) {
          this.injectDirectLeftTap(x, y);
        } else if (event.type === TouchType.Up && !previous.moved && elapsed >= this.LONG_PRESS_MS) {
          // 直接触摸长按 = 右键
          this.injectClick('direct-longpress-right', x, y, 273);
          this.resetDirectTapSequence();
        } else {
          this.resetDirectTapSequence();
          InputDispatcher.getInstance().releaseSource(source);
        }
      }
    }
  }

  private touchpadMidpoint(): [number, number] {
    const values = Array.from(this.touchpadFingers.values());
    if (values.length < 2) return [0, 0];
    return [(values[0].lastX + values[1].lastX) / 2, (values[0].lastY + values[1].lastY) / 2];
  }

  private ensureTouchpadCursor(): void {
    const width = this.overlayW > 0 ? this.overlayW : 720;
    const height = this.overlayH > 0 ? this.overlayH : 480;
    if (this.cursorX <= 0 || this.cursorX >= width || this.cursorY <= 0 || this.cursorY >= height) {
      this.cursorX = width / 2;
      this.cursorY = height / 2;
    }
  }

  private resetTouchpadGesture(): void {
    this.releaseTouchpadDrag();
    this.touchpadFingers.clear();
    this.touchpadTwoFinger = false;
    this.touchpadSuppressSingleTap = false;
    this.touchpadTwoFingerTapEligible = false;
    this.touchpadLastTapAt = 0;
    this.cancelPendingTouchpadTap();
    InputDispatcher.getInstance().releaseSource('touchpad-scroll');
    InputDispatcher.getInstance().releaseSource('touchpad-cursor');
  }

  private cancelPendingTouchpadTap(): void {
    if (this.touchpadPendingTapTimer >= 0) {
      clearTimeout(this.touchpadPendingTapTimer);
      this.touchpadPendingTapTimer = -1;
    }
    this.touchpadReTouchSinceTap = false;
  }

  private firePendingTouchpadTap(): void {
    this.touchpadPendingTapTimer = -1;
    this.touchpadClick(272, 'touchpad-left');
    this.touchpadLastTapAt = Date.now();
  }

  /** 轻点候选: 延迟确认, 窗口内再次落下并滑动则取消 (抬起→再滑动=移动)。 */
  private armPendingTouchpadTap(): void {
    this.cancelPendingTouchpadTap();
    this.touchpadPendingTapTimer = setTimeout((): void => this.firePendingTouchpadTap(),
      InputOverlayPage.TOUCHPAD_TAP_CONFIRM_MS) as number;
  }

  /** 确认窗口内手指重新落下: 重启计时等待该触摸是"滑动(取消点击)"还是
   *  "轻点(双击)"。固定窗口会被慢速/延迟投递的 MOVE 击穿 (ArkUI MOVE 可能
   *  晚到), 重新计时给再触摸后的移动留足时间。 */
  private extendPendingTouchpadTap(): void {
    if (this.touchpadPendingTapTimer >= 0) {
      this.touchpadReTouchSinceTap = true;
      clearTimeout(this.touchpadPendingTapTimer);
      this.touchpadPendingTapTimer = setTimeout((): void => this.firePendingTouchpadTap(),
        InputOverlayPage.TOUCHPAD_TAP_CONFIRM_MS) as number;
    }
  }

  private touchpadDragInterval(): number {
    // A second touch has to cross ArkUI's overlay/subwindow dispatch before it
    // reaches this state machine. Keep the normal double-click preference as
    // the baseline, but allow enough time for a deliberate tap-hold gesture.
    return Math.max(800, Math.min(1200, this.settings.virtualMouseConfig.doubleClickInterval * 4));
  }

  private releaseTouchpadDrag(): void {
    if (this.touchpadDragActive) {
      InputDispatcher.getInstance().pointerUp(
        'touchpad-drag', vp2px(this.cursorX), vp2px(this.cursorY), 272);
    }
    this.touchpadDragActive = false;
    this.touchpadDragCandidate = false;
    this.touchpadDragTouchId = -1;
  }

  private setTouchpadMode(enabled: boolean): void {
    if (this.touchpadMode === enabled) return;
    InputDispatcher.getInstance().releaseAll('touchpad-mode-switch');
    this.directTouches.clear();
    this.twoFinger = false;
    this.twoFingerTapEligible = false;
    this.resetDirectTapSequence();
    this.resetTouchpadGesture();
    this.touchpadMode = enabled;
    // 触摸板光标统一由 VirtualInputOverlay 的虚拟光标指示点显示 (新状态机
    // 所在), 这里不再单独显示 wine-virtual-mouse-cursor, 避免切换瞬间双光点。
    if (!enabled) {
      this.cursorVisible = false;
      this.cancelCursorTimers();
    }
  }

  /** 点击式宿主键盘开关: 唤起/收起系统输入法, 并同步 text-input 激活。 */
  private toggleHostKeyboard(): void {
    this.hostKeyboardActive = !this.hostKeyboardActive;
    const focusController = this.getUIContext().getFocusController();
    hilog.info(0x0000, 'HostKb', 'toggle host keyboard now=%{public}s',
      this.hostKeyboardActive ? 'on' : 'off');
    if (this.hostKeyboardActive) {
      testNapi.wineTextInputSetArmed(true);
      this.hostImeReseedPending = true;
      this.hostImeBuffer = InputOverlayPage.HOST_SENTINEL;
      this.hostImeContent = InputOverlayPage.HOST_SENTINEL;
      // TextInput 刚从 focusable(false) 切过来, 等一帧再抢焦点, 否则 requestFocus 落空。
      setTimeout((): void => {
        try {
          this.getUIContext().getFocusController().requestFocus('wine-host-keyboard-input');
        } catch (_) {
        }
      }, 32);
    } else {
      focusController.clearFocus();
      testNapi.wineTextInputSetArmed(false);
      this.pendingHostCommit = '';
      this.hostImeBuffer = '';
      this.hostImeContent = '';
      // overlay 子窗口无法直接 requestFocus 父窗 XComponent; DesktopLayer 监听此 token。
      AppStorage.setOrCreate('wineDesktopKeyFocusToken', Date.now());
    }
  }

  /** 宿主键盘关闭时转发蓝牙/实体 KeyEvent; 打开时不拦截, 拼音走 TextInput/IME。 */
  private handleHardwareKey(ev: KeyEvent): void {
    if (this.hostKeyboardActive) return;
    const rootId = WineWindowManager.getInstance().getDesktopRootId();
    if (rootId <= 0) return;
    ev.stopPropagation();
    // Keyboard arrows share OHOS codes with gamepad D-pad. Do not let the
    // default DInput gamepad path consume physical keyboard events.
    if (ev.keySource === KeySource.JOYSTICK) {
      GamepadManager.getInstance().handleKey(ev.keyCode, ev.type === KeyType.Down);
      return;
    }
    const code = OH2EVDEV[ev.keyCode] ?? 0;
    if (code <= 0) return;
    const pressed = ev.type === KeyType.Down;
    if (InputRouter.getInstance().getActiveToplevel() <= 0) {
      InputRouter.getInstance().setActiveToplevel(rootId);
    }
    if (code === EVDEV_KEY_ENTER) {
      if (pressed) {
        this.lastEnterDownTs = Date.now();
      } else if (Date.now() - this.lastEnterDownTs > InputOverlayPage.ENTER_SYNTH_WINDOW_MS) {
        InputRouter.getInstance().sendKey(EVDEV_KEY_ENTER, true);
      }
    }
    InputRouter.getInstance().sendKey(code, pressed);
  }

  /** 哨兵保活: 程序性重填, onChange 按 oldContent→sentinel 识别并忽略。 */
  private reseedHostSentinel(): void {
    this.hostImeReseedPending = true;
    this.hostImeBuffer = InputOverlayPage.HOST_SENTINEL;
    this.hostImeContent = InputOverlayPage.HOST_SENTINEL;
  }

  private injectHostKey(code: number): void {
    const dispatcher = InputDispatcher.getInstance();
    dispatcher.pressKey('host-kb', code);
    dispatcher.releaseKey('host-kb', code);
  }

  private injectHostAsciiChar(ch: string): void {
    const mapped = asciiToEvdevKey(ch);
    if (!mapped) return;
    const dispatcher = InputDispatcher.getInstance();
    if (mapped.shift) dispatcher.pressKey('host-kb-shift', EVDEV_KEY_LEFTSHIFT);
    dispatcher.pressKey('host-kb', mapped.code);
    dispatcher.releaseKey('host-kb', mapped.code);
    if (mapped.shift) dispatcher.releaseKey('host-kb-shift', EVDEV_KEY_LEFTSHIFT);
  }

  private isAsciiOnly(text: string): boolean {
    for (let index = 0; index < text.length; index++) {
      if (asciiToEvdevKey(text.charAt(index)) === null) return false;
    }
    return true;
  }

  private flushPendingHostCommit(): void {
    if (this.pendingHostCommit.length === 0) return;
    if (testNapi.wineTextInputEnabled() && testNapi.wineTextInputCommit(this.pendingHostCommit)) {
      this.pendingHostCommit = '';
    }
  }

  private onHostImeChange(value: string, previewText?: PreviewText,
                          options?: TextChangeOptions): void {
    // 键盘已收起时, 清空/重填缓冲产生的 onChange 一律不注入 Wine。
    if (!this.hostKeyboardActive) return;
    const preview = previewText !== undefined && previewText.value !== undefined ?
      previewText.value : '';

    // 程序性哨兵重填: 只更新基线, 不注入。
    if (preview.length === 0 && this.hostImeReseedPending &&
        value === InputOverlayPage.HOST_SENTINEL) {
      this.hostImeReseedPending = false;
      this.hostImeContent = InputOverlayPage.HOST_SENTINEL;
      return;
    }
    this.hostImeReseedPending = false;

    // 拼音组合态: 经 text-input preedit 显示在 Wine, 不落字。
    if (preview.length > 0) {
      testNapi.wineTextInputPreedit(preview);
      return;
    }

    // 先冲刷上次挂起的中文提交。
    this.flushPendingHostCommit();

    // 用前缀+后缀共同部分做鲁棒差分, 不依赖输入法上报的 rangeBefore/After
    // (切换控件后个别输入法上报区间会漂移, 之前偶发退格丢失正是这里)。
    const oldContent = options !== undefined && options.oldContent !== undefined ?
      options.oldContent : this.hostImeContent;
    let commonPrefix = 0;
    while (commonPrefix < oldContent.length && commonPrefix < value.length &&
      oldContent.charAt(commonPrefix) === value.charAt(commonPrefix)) {
      commonPrefix++;
    }
    let commonSuffix = 0;
    while (commonSuffix < oldContent.length - commonPrefix &&
      commonSuffix < value.length - commonPrefix &&
      oldContent.charAt(oldContent.length - 1 - commonSuffix) ===
        value.charAt(value.length - 1 - commonSuffix)) {
      commonSuffix++;
    }
    const deleted = oldContent.substring(commonPrefix, oldContent.length - commonSuffix);
    const inserted = value.substring(commonPrefix, value.length - commonSuffix);
    this.hostImeContent = value;

    // 删除: 逐字符注入 Backspace。只删掉哨兵表示删入 Wine 旧内容,
    // 也计一次; 哨兵与其他内容一起被删时不计哨兵本身。
    let sentinelDeleted = 0;
    for (let index = 0; index < deleted.length; index++) {
      if (deleted.charAt(index) === InputOverlayPage.HOST_SENTINEL) sentinelDeleted++;
    }
    let backspaces = deleted.length;
    if (sentinelDeleted > 0) {
      backspaces = deleted === InputOverlayPage.HOST_SENTINEL ?
        1 : (deleted.length - sentinelDeleted);
    }
    for (let index = 0; index < backspaces; index++) {
      this.injectHostKey(EVDEV_KEY_BACKSPACE);
    }

    // 插入: 优先 commit_string (中文等任意 Unicode), 否则逐字符按键注入。
    if (inserted.length > 0) {
      if (testNapi.wineTextInputEnabled() && testNapi.wineTextInputCommit(inserted)) {
        // 已提交
      } else if (this.isAsciiOnly(inserted)) {
        for (let index = 0; index < inserted.length; index++) {
          this.injectHostAsciiChar(inserted.charAt(index));
        }
      } else {
        this.pendingHostCommit += inserted;
      }
    }

    if (value.indexOf(InputOverlayPage.HOST_SENTINEL) < 0) {
      this.reseedHostSentinel();
    }
  }

  private touchpadClick(button: number, source: string): void {
    const x = vp2px(this.cursorX);
    const y = vp2px(this.cursorY);
    this.injectClick(source, x, y, button);
    this.showCursorAt(this.cursorX, this.cursorY, true);
  }

  /** VintagePomelo full-screen touchpad: relative cursor, tap click, two-finger right/scroll. */
  private handleTouchpadTouch(event: TouchEvent): void {
    if (!this.eventGate.shouldHandleTouch(event.source, event.sourceTool)) return;
    event.stopPropagation();
    this.ensureTouchpadCursor();
    this.showCursorAt(this.cursorX, this.cursorY);
    const sensitivity = Math.max(0.5, Math.min(2, this.settings.virtualMouseConfig.mouseSensitivity / 100));
    const scrollThreshold = Math.max(3, 8 / sensitivity);

    for (let index = 0; index < event.changedTouches.length; index++) {
      const touch = event.changedTouches[index];
      const x = Number(touch.x);
      const y = Number(touch.y);
      if (event.type === TouchType.Down) {
        // 轻点确认窗口内手指重新落下: 可能是"抬起→再滑动"(取消点击) 或
        // 双击第二下 (保留点击)。重启确认计时, 由后续移动决定是否取消。
        this.extendPendingTouchpadTap();
        const beginDragCandidate = this.touchpadFingers.size === 0 && this.touchpadLastTapAt > 0 &&
          Date.now() - this.touchpadLastTapAt <= this.touchpadDragInterval();
        this.touchpadFingers.set(touch.id, {
          id: touch.id, startX: x, startY: y, lastX: x, lastY: y,
          startTime: Date.now(), moved: false
        });
        if (beginDragCandidate) {
          // Wine commits a title-bar double click while a second pointer stays
          // stationary.  Keep it as a candidate until the finger actually
          // moves, then inject the press immediately before the first motion.
          // This preserves tap/tap double-click while making tap-hold-drag
          // reliably move windows even when ArkUI delays MOVE delivery.
          this.touchpadDragCandidate = true;
          this.touchpadDragTouchId = touch.id;
          this.touchpadLastTapAt = 0;
        }
        if (this.touchpadFingers.size === 2) {
          this.releaseTouchpadDrag();
          this.touchpadLastTapAt = 0;
          this.touchpadTwoFinger = true;
          this.touchpadSuppressSingleTap = true;
          this.touchpadTwoFingerTapEligible = true;
          const midpoint = this.touchpadMidpoint();
          this.touchpadLastMidX = midpoint[0];
          this.touchpadLastMidY = midpoint[1];
        } else if (this.touchpadFingers.size > 2) {
          this.touchpadTwoFingerTapEligible = false;
        }
      } else if (event.type === TouchType.Move) {
        const finger = this.touchpadFingers.get(touch.id);
        if (!finger) continue;
        const deltaFromStartX = x - finger.startX;
        const deltaFromStartY = y - finger.startY;
        const movedPastSlop = Math.abs(deltaFromStartX) > 10 || Math.abs(deltaFromStartY) > 10;
        if (movedPastSlop) finger.moved = true;
        // 确认窗口内"抬起→再触摸"并开始滑动: 之前那次 Up 是分段移动, 取消点击。
        if (movedPastSlop && this.touchpadPendingTapTimer >= 0 && this.touchpadReTouchSinceTap) {
          this.cancelPendingTouchpadTap();
        }
        let dx = x - finger.lastX;
        let dy = y - finger.lastY;
        finger.lastX = x;
        finger.lastY = y;

        if (this.touchpadTwoFinger && this.touchpadFingers.size >= 2) {
          const midpoint = this.touchpadMidpoint();
          const scrollX = this.touchpadLastMidX - midpoint[0];
          const scrollY = this.touchpadLastMidY - midpoint[1];
          if (Math.abs(scrollX) >= scrollThreshold) {
            this.touchpadTwoFingerTapEligible = false;
            InputDispatcher.getInstance().scroll('touchpad-scroll', 1, scrollX > 0 ? 1 : -1);
            this.touchpadLastMidX = midpoint[0];
          }
          if (Math.abs(scrollY) >= scrollThreshold) {
            this.touchpadTwoFingerTapEligible = false;
            InputDispatcher.getInstance().scroll('touchpad-scroll', 0, scrollY > 0 ? 1 : -1);
            this.touchpadLastMidY = midpoint[1];
          }
        } else if (this.touchpadFingers.size === 1) {
          const isDragCandidate = this.touchpadDragCandidate && this.touchpadDragTouchId === touch.id;
          if (isDragCandidate && !movedPastSlop) continue;
          if (isDragCandidate) {
            this.touchpadDragCandidate = false;
            this.touchpadDragActive = true;
            // Include the distance travelled before the first MOVE so the
            // window does not visibly jump after a held second touch.
            dx = deltaFromStartX;
            dy = deltaFromStartY;
            InputDispatcher.getInstance().pointerDown(
              'touchpad-drag', vp2px(this.cursorX), vp2px(this.cursorY), 272);
            this.showCursorAt(this.cursorX, this.cursorY, true);
          }
          this.cursorX += dx * sensitivity;
          this.cursorY += dy * sensitivity;
          const maxX = Math.max(5, (this.overlayW > 0 ? this.overlayW : 720) - 5);
          const maxY = Math.max(5, (this.overlayH > 0 ? this.overlayH : 480) - 5);
          this.cursorX = Math.max(5, Math.min(maxX, this.cursorX));
          this.cursorY = Math.max(5, Math.min(maxY, this.cursorY));
          if (this.touchpadDragActive && this.touchpadDragTouchId === touch.id) {
            InputDispatcher.getInstance().pointerMove(
              'touchpad-drag', vp2px(this.cursorX), vp2px(this.cursorY));
          } else {
            InputDispatcher.getInstance().pointerMove(
              'touchpad-cursor', vp2px(this.cursorX), vp2px(this.cursorY), false);
          }
          this.showCursorAt(this.cursorX, this.cursorY);
        }
      } else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
        const finger = this.touchpadFingers.get(touch.id);
        if (!finger) continue;
        const elapsed = Date.now() - finger.startTime;
        const moved = finger.moved || Math.abs(x - finger.startX) > 10 || Math.abs(y - finger.startY) > 10;
        const suppressed = this.touchpadSuppressSingleTap;
        this.touchpadFingers.delete(touch.id);
        if (this.touchpadDragActive && this.touchpadDragTouchId === touch.id) {
          this.releaseTouchpadDrag();
          this.touchpadTwoFinger = false;
          continue;
        }
        if (this.touchpadDragCandidate && this.touchpadDragTouchId === touch.id) {
          // No motion arrived: this was a second normal tap, not a drag.
          this.touchpadDragCandidate = false;
          this.touchpadDragTouchId = -1;
          if (!moved && elapsed < this.TAP_TIMEOUT_MS && event.type === TouchType.Up) {
            this.touchpadClick(272, 'touchpad-left');
          }
          this.touchpadLastTapAt = 0;
          this.touchpadTwoFinger = false;
          continue;
        }
        if (suppressed) {
          if (moved || elapsed >= this.TAP_TIMEOUT_MS || event.type === TouchType.Cancel) {
            this.touchpadTwoFingerTapEligible = false;
          }
          if (this.touchpadFingers.size === 0) {
            if (event.type === TouchType.Up && this.touchpadTwoFingerTapEligible) {
              this.touchpadClick(273, 'touchpad-two-finger-right');
            }
            this.resetTouchpadGesture();
          } else if (this.touchpadFingers.size < 2) {
            this.touchpadTwoFinger = false;
          }
        } else {
          if (!moved && elapsed < this.TAP_TIMEOUT_MS && event.type === TouchType.Up) {
            // 确认窗口内出现第二次轻点 (双击): 先补发第一下的点击, 再为第二下
            // 建立确认 — 两下间隔约等于确认窗口, Windows 双击状态机可识别。
            if (this.touchpadPendingTapTimer >= 0 && this.touchpadReTouchSinceTap) {
              this.cancelPendingTouchpadTap();
              this.touchpadClick(272, 'touchpad-left');
              this.touchpadLastTapAt = Date.now();
            }
            // 轻点候选: 延迟确认 (若窗口内再触摸滑动则取消, 见 Move 分支)
            this.armPendingTouchpadTap();
          } else {
            this.touchpadLastTapAt = 0;
            this.cancelPendingTouchpadTap();
          }
          this.touchpadTwoFinger = false;
        }
      }
    }
  }

  private handleOverlayTouch(event: TouchEvent): void {
    // A physical left click can produce both MouseEvent and TouchEvent on API
    // 23. Keep the real press/release sequence and discard its synthesized
    // touch copy; DirectInput games otherwise see a duplicate collapsed click.
    if (this.mouseLeftActive) {
      event.stopPropagation();
      return;
    }
    if (this.touchpadMode) this.handleTouchpadTouch(event);
    else this.handleDirectTouch(event);
  }

  private hideForExternalInput(): void {
    const settings = AppSettingsStore.getInstance().getGlobalSettings();
    if (settings.autoHideVirtualControls && settings.showVirtualControls) {
      InputOverlayWindowCoordinator.getInstance().setControlsVisible(false);
    }
  }

  private handleDirectMouse(event: MouseEvent): void {
    if (!this.eventGate.shouldHandleMouse(event.source, event.sourceTool)) return;
    event.stopPropagation();
    const source = 'external-mouse';
    // MouseEvent.x/y are component-local too. Using the overlay-window
    // coordinates keeps physical mouse input in the same space as direct
    // touch, regardless of which transparent control forwarded the event.
    const x = vp2px(event.windowX);
    const y = vp2px(event.windowY);
    const button = mapMouseButton(event.button);
    if (event.action === MouseAction.Press) {
      if (button === 272) {
        if (this.mouseLeftReleaseTimer >= 0) clearTimeout(this.mouseLeftReleaseTimer);
        this.mouseLeftReleaseTimer = -1;
        this.mouseLeftActive = true;
      }
      InputDispatcher.getInstance().pointerDown(source, x, y, button);
    }
    else if (event.action === MouseAction.Release) {
      InputDispatcher.getInstance().pointerUp(source, x, y, button);
      if (button === 272) {
        if (this.mouseLeftReleaseTimer >= 0) clearTimeout(this.mouseLeftReleaseTimer);
        this.mouseLeftReleaseTimer = setTimeout((): void => {
          this.mouseLeftReleaseTimer = -1;
          this.mouseLeftActive = false;
        }, 120) as number;
      }
      this.hideForExternalInput();
    }
    else InputDispatcher.getInstance().pointerMove(source, x, y, false);
  }

  private handleDirectAxis(event?: AxisEvent): void {
    if (!event) return;
    const vertical = event.getVerticalAxisValue();
    const horizontal = event.getHorizontalAxisValue();
    if (vertical !== 0) InputDispatcher.getInstance().scroll('external-mouse', 0, vertical, event.scrollStep ?? 40);
    if (horizontal !== 0) InputDispatcher.getInstance().scroll('external-mouse', 1, horizontal, event.scrollStep ?? 40);
  }

  private async openInputEditor(route: string): Promise<void> {
    const sessionId = this.snapshot.session.sessionId;
    const session = AppSessionService.getInstance().getSession(sessionId);
    const want: Want = {
      bundleName: 'com.vintage.pomelopro',
      abilityName: 'EntryAbility',
      parameters: {
        overlayRoute: route,
        profileId: this.snapshot.profileId || 'default',
        profileName: this.profile.name || '默认键鼠手柄',
        appId: session?.appId ?? '',
        baseProfileId: this.settings.desktopInputProfileId || 'default',
        returnAbilityName: 'DesktopAbility',
        returnSessionId: this.snapshot.session.sessionId,
        returnToplevelId: this.snapshot.session.toplevelId.toString()
      }
    };
    const coordinator = InputOverlayWindowCoordinator.getInstance();
    try {
      // The overlay is a zLevel 9000 subwindow. Make it untouchable before
      // bringing the EntryAbility editor forward or it can intercept the
      // editor's drag/save controls during the Ability transition.
      await coordinator.setVisible(false);
      await this.context.startAbility(want);
    } catch (_) {
      // A failed navigation must not leave the active Wine session without
      // controls. DesktopAbility will normally restore the overlay from
      // onForeground after the editor returns.
      await coordinator.setVisible(true).catch((): void => {});
    }
  }

  build() {
    Stack() {
      // 宿主键盘载体: 平时不可见/不参与命中, 聚焦后唤起系统输入法。
      TextInput({ placeholder: '', text: this.hostImeBuffer, controller: this.hostImeController })
        .id('wine-host-keyboard-input')
        .focusable(this.hostKeyboardActive)
        .type(InputType.Normal)
        .enterKeyType(EnterKeyType.NEW_LINE)
        .width(40).height(40)
        .opacity(0.01)
        .position({ x: -60, y: -60 })
        .hitTestBehavior(HitTestMode.None)
        .onChange((value: string, previewText?: PreviewText, options?: TextChangeOptions): void => {
          this.onHostImeChange(value, previewText, options);
        })
        .onSubmit((_enterKey: EnterKeyType, event?: SubmitEvent): void => {
          this.injectHostKey(EVDEV_KEY_ENTER);
          if (event !== undefined) event.keepEditableState();
        })
        .zIndex(0)
      if (this.snapshot.visible) {
        VirtualInputOverlay({
          profile: this.profile,
          opacityValue: this.snapshot.opacity,
          controlsVisible: this.snapshot.controlsVisible,
          controlBarDirection: this.settings.controlBarDirection,
          controlBarPositionX: this.settings.controlBarPositionX,
          controlBarPositionY: this.settings.controlBarPositionY,
          touchpadMode: this.touchpadMode,
          showCursor: this.settings.virtualMouseConfig.showCursor,
          onOpenLayoutEditor: (): void => { this.openInputEditor('input-layout'); },
          onOpenKeyMappingEditor: (): void => { this.openInputEditor('key-mapping'); },
          onToggleHostKeyboard: (): void => { this.toggleHostKeyboard(); },
          onControlsVisibleChange: (visible: boolean): void => {
            InputOverlayWindowCoordinator.getInstance().setControlsVisible(visible);
            // 持久化虚拟按键开关状态: 桌面重开时按用户最后设定恢复, 不再每次自动弹出。
            AppSettingsStore.getInstance().setShowVirtualControls(visible).then((): void => {
              this.settings = AppSettingsStore.getInstance().getGlobalSettings();
            }).catch((): void => {});
          },
          onTouchpadModeChange: (enabled: boolean): void => this.setTouchpadMode(enabled),
          onControlBarPositionChange: (x: number, y: number): void => {
            AppSettingsStore.getInstance().setControlBarPosition(x, y).then((): void => {
              this.settings = AppSettingsStore.getInstance().getGlobalSettings();
            }).catch((): void => {});
          },
          onUnhandledTouch: (event: TouchEvent): void => this.handleOverlayTouch(event),
          onUnhandledMouse: (event: MouseEvent): void => this.handleDirectMouse(event),
          onUnhandledAxis: (event?: AxisEvent): void => this.handleDirectAxis(event)
        })
        .zIndex(10)
      }
      // 触摸板虚拟光标统一由 VirtualInputOverlay 显示 (新状态机), 此处禁用
      // 避免切换瞬间双光点。保留 cursorX/Y 状态供 InputDispatcher 逻辑使用。
      if (false && this.touchpadMode && this.cursorVisible && this.settings.virtualMouseConfig.showCursor) {
        Stack({ alignContent: Alignment.Center }) {
          Circle()
            .width(this.cursorFlash ? 10 : 7)
            .height(this.cursorFlash ? 10 : 7)
            .fill(this.cursorFlash ? '#FF4CAF50' : '#E6FFFFFF')
            .stroke(this.cursorFlash ? '#CCB9F6CA' : '#D9000000')
            .strokeWidth(1)
            .shadow({ radius: this.cursorFlash ? 8 : 4, color: '#66000000', offsetX: 0, offsetY: 1 })
        }
        .width(14).height(14)
        .position({ x: this.cursorX, y: this.cursorY })
        .markAnchor({ x: '50%', y: '50%' })
        .hitTestBehavior(HitTestMode.None)
        .id('wine-virtual-mouse-cursor')
        .zIndex(15)
      }
      Column()
        .width('100%').height('100%')
        .backgroundColor('#00000000')
        .hitTestBehavior(HitTestMode.Block)
        .id('wine-input-direct-surface')
        .zIndex(0)
        .onTouch((event: TouchEvent): void => this.handleOverlayTouch(event))
        .onMouse((event: MouseEvent): void => this.handleDirectMouse(event))
        .onAxisEvent((event?: AxisEvent): void => this.handleDirectAxis(event))
      WineEngineBlockingOverlay()
    }
    .width('100%').height('100%')
    .backgroundColor('#00000000')
    .id('wine-input-overlay-page')
    .onKeyEvent((ev: KeyEvent): void => this.handleHardwareKey(ev))
    .onAreaChange((_oldValue: Area, area: Area): void => {
      this.overlayW = area.width as number;
      this.overlayH = area.height as number;
    })
  }
}
