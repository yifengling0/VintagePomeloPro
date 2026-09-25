import { vibrator } from '@kit.SensorServiceKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { GamepadSink, KeyMouseGamepadSink } from './InputRouter';
import { GamepadButtonMapping, createDefaultInputProfile } from '../model/AppModels';
import { hilog } from './LogService';
import testNapi from 'libentry.so';

export type GamepadConnectionListener = (connected: boolean) => void;
export type GamepadActivityListener = () => void;
export type GamepadNavigationListener = (buttonCode: number) => boolean;
export type ControllerOutputMode = 'dinput' | 'keyboard_legacy';

const DOMAIN: number = 0x0000;
const TAG: string = 'PadRumble';

class PadMotor {
  deviceId: number = 0;
  vibratorId: number = 0;
  deviceName: string = '';
  isHdHapticSupported: boolean = false;
  usePattern: boolean = true;
}

export class GamepadManager {
  private static instance: GamepadManager | null = null;
  private sink: GamepadSink = new KeyMouseGamepadSink();
  private pressed: Set<number> = new Set<number>();
  private deadzone: number = 0.18;
  private initialized: boolean = false;
  private axisKeys: Set<number> = new Set<number>();
  private connected: boolean = false;
  private outputMode: ControllerOutputMode = 'dinput';
  private connectionListeners: Set<GamepadConnectionListener> = new Set<GamepadConnectionListener>();
  private activityListeners: Set<GamepadActivityListener> = new Set<GamepadActivityListener>();
  private navigationListeners: Set<GamepadNavigationListener> = new Set<GamepadNavigationListener>();
  private navigationPressed: Set<number> = new Set<number>();
  private lastNavigationAxis: number = 0;
  private lastNavigationAt: number = 0;
  private lastNavigationAxisHandled: boolean = false;
  private buttonMappings: Map<number, number> = new Map<number, number>(
    createDefaultInputProfile().gamepadMappings.map((mapping: GamepadButtonMapping): [number, number] =>
      [mapping.buttonCode, mapping.evdevCode])
  );
  private padMotors: PadMotor[] = [];
  private lastLow: number = -1;
  private lastHigh: number = -1;
  private lastRumbleAt: number = 0;
  private rumbleListening: boolean = false;

  static getInstance(): GamepadManager {
    if (!GamepadManager.instance) GamepadManager.instance = new GamepadManager();
    return GamepadManager.instance;
  }

  setSink(sink: GamepadSink): void {
    this.releaseAll();
    this.sink = sink;
  }

  setOutputMode(mode: ControllerOutputMode): void {
    if (this.outputMode === mode) return;
    this.releaseAll();
    this.outputMode = mode;
    if (mode !== 'dinput') this.stopPadVibration();
    try {
      testNapi.controllerSetOutputMode(mode);
      if (mode === 'dinput') {
        testNapi.controllerSetEnabled(true);
      }
    } catch (_) {
    }
  }

  getOutputMode(): ControllerOutputMode {
    return this.outputMode;
  }

  setButtonMappings(mappings: GamepadButtonMapping[]): void {
    this.releaseAll();
    const source = mappings.length > 0 ? mappings : createDefaultInputProfile().gamepadMappings;
    this.buttonMappings = new Map<number, number>(
      source.map((mapping: GamepadButtonMapping): [number, number] => [mapping.buttonCode, mapping.evdevCode])
    );
  }

  initialize(deadzonePercent: number, outputMode?: ControllerOutputMode): void {
    this.setDeadzone(deadzonePercent);
    if (outputMode) this.outputMode = outputMode;
    try {
      testNapi.controllerSetOutputMode(this.outputMode);
      if (this.outputMode === 'dinput') testNapi.controllerSetEnabled(true);
    } catch (_) {
    }
    if (this.initialized) return;
    this.initialized = true;
    testNapi.setGamepadButtonCallback((buttonCode: number, pressed: boolean) => {
      this.handleKey(buttonCode, pressed);
    });
    testNapi.setGamepadAxisCallback((axisType: number, x: number, y: number) => {
      this.handleAxis(axisType, x, y);
    });
    testNapi.setGamepadDeviceCallback((connected: boolean) => {
      this.connected = connected;
      if (!connected) {
        this.releaseAll();
        this.stopPadVibration();
      }
      this.refreshPadMotors();
      this.connectionListeners.forEach((listener: GamepadConnectionListener) => listener(connected));
    });
    testNapi.setGamepadRumbleCallback((low: number, high: number, durationMs: number) => {
      this.applyRumble(low, high, durationMs);
    });
    this.watchVibrators();
    this.refreshPadMotors();
    try {
      testNapi.initGameController();
      this.connected = testNapi.isGamepadConnected();
      if (this.outputMode === 'dinput') {
        testNapi.controllerSetEnabled(true);
      }
    } catch (_) {
      this.connected = false;
    }
  }

  isConnected(): boolean {
    return this.connected;
  }

  subscribeConnection(listener: GamepadConnectionListener): void {
    this.connectionListeners.add(listener);
    listener(this.connected);
  }

  unsubscribeConnection(listener: GamepadConnectionListener): void {
    this.connectionListeners.delete(listener);
  }

  subscribeActivity(listener: GamepadActivityListener): void {
    this.activityListeners.add(listener);
  }

  unsubscribeActivity(listener: GamepadActivityListener): void {
    this.activityListeners.delete(listener);
  }

  subscribeNavigation(listener: GamepadNavigationListener): void {
    this.navigationListeners.add(listener);
  }

  unsubscribeNavigation(listener: GamepadNavigationListener): void {
    this.navigationListeners.delete(listener);
    if (this.navigationListeners.size === 0) {
      this.lastNavigationAxis = 0;
      this.lastNavigationAxisHandled = false;
    }
  }

  private dispatchNavigation(buttonCode: number): boolean {
    let handled = false;
    this.navigationListeners.forEach((listener: GamepadNavigationListener): void => {
      if (listener(buttonCode)) handled = true;
    });
    return handled;
  }

  private notifyActivity(): void {
    this.activityListeners.forEach((listener: GamepadActivityListener) => listener());
  }

  setDeadzone(percent: number): void {
    this.deadzone = Math.max(0, Math.min(0.5, percent / 100));
  }

  getRumbleStatusText(): string {
    if (this.padMotors.length === 0) {
      return '震动: 无外接马达';
    }
    const names: string[] = [];
    for (let i = 0; i < this.padMotors.length; i++) {
      const name = this.padMotors[i].deviceName.length > 0
        ? this.padMotors[i].deviceName
        : ('id=' + this.padMotors[i].deviceId);
      if (names.indexOf(name) < 0) names.push(name);
    }
    return '震动: ' + names.join(',') + ' ×' + this.padMotors.length;
  }

  testRumble(): boolean {
    this.refreshPadMotors();
    if (this.padMotors.length === 0) return false;
    this.applyRumble(48000, 48000, 450);
    return true;
  }

  handleKey(keyCode: number, pressed: boolean): boolean {
    if (pressed && this.dispatchNavigation(keyCode)) {
      this.navigationPressed.add(keyCode);
      this.notifyActivity();
      return true;
    }
    if (!pressed && this.navigationPressed.has(keyCode)) {
      this.navigationPressed.delete(keyCode);
      return true;
    }
    // dinput: Physical already feeds ControllerHub in native; skip evdev mapping.
    if (this.outputMode === 'dinput') {
      if (this.buttonMappings.has(keyCode)) {
        this.notifyActivity();
        return true;
      }
      return false;
    }
    const evdev = this.buttonMappings.get(keyCode);
    if (!evdev) return false;
    if (pressed && !this.pressed.has(evdev)) {
      this.pressed.add(evdev);
      this.notifyActivity();
      this.sink.onButton(evdev, true);
    } else if (!pressed && this.pressed.has(evdev)) {
      this.pressed.delete(evdev);
      this.notifyActivity();
      this.sink.onButton(evdev, false);
    }
    return true;
  }

  handleRightStick(x: number, y: number): void {
    if (this.outputMode === 'dinput') {
      if (Math.abs(x) >= this.deadzone || Math.abs(y) >= this.deadzone) this.notifyActivity();
      return;
    }
    const dx = Math.abs(x) >= this.deadzone ? Math.round(x * 18) : 0;
    const dy = Math.abs(y) >= this.deadzone ? Math.round(y * 18) : 0;
    if (dx !== 0 || dy !== 0) {
      this.notifyActivity();
      this.sink.onPointerDelta(dx, dy);
    }
  }

  private setAxisKey(code: number, pressed: boolean): void {
    const active = this.axisKeys.has(code);
    if (pressed === active) return;
    if (pressed) this.axisKeys.add(code); else this.axisKeys.delete(code);
    this.sink.onButton(code, pressed);
  }

  handleAxis(axisType: number, x: number, y: number): void {
    // Controllers differ in how they expose directional input: the left
    // stick is axis 0, while many D-pads only report a Hat axis (axis 2)
    // instead of the four button callbacks. Both must drive launcher focus.
    if ((axisType === 0 || axisType === 2) && this.navigationListeners.size > 0) {
      const direction = Math.abs(x) > 0.55 && Math.abs(x) >= Math.abs(y) ?
        (x < 0 ? 2014 : 2015) : (Math.abs(y) > 0.55 ? (y < 0 ? 2012 : 2013) : 0);
      const now = Date.now();
      if (direction && (direction !== this.lastNavigationAxis || now - this.lastNavigationAt > 280)) {
        this.lastNavigationAxisHandled = this.dispatchNavigation(direction);
        if (this.lastNavigationAxisHandled) {
          this.lastNavigationAt = now;
          this.notifyActivity();
        }
      }
      if (!direction) this.lastNavigationAxisHandled = false;
      this.lastNavigationAxis = direction;
      if (direction && this.lastNavigationAxisHandled) return;
    }
    if (this.outputMode === 'dinput') {
      const active = Math.abs(x) >= this.deadzone || Math.abs(y) >= this.deadzone;
      if (active) this.notifyActivity();
      return;
    }
    if (axisType === 1) {
      this.handleRightStick(x, y);
      return;
    }
    const active = Math.abs(x) >= this.deadzone || Math.abs(y) >= this.deadzone;
    if (active) this.notifyActivity();
    if (axisType === 0) {
      this.setAxisKey(30, x < -this.deadzone);
      this.setAxisKey(32, x > this.deadzone);
      this.setAxisKey(17, y < -this.deadzone);
      this.setAxisKey(31, y > this.deadzone);
    } else if (axisType === 2) {
      this.setAxisKey(105, x < -this.deadzone);
      this.setAxisKey(106, x > this.deadzone);
      this.setAxisKey(103, y < -this.deadzone);
      this.setAxisKey(108, y > this.deadzone);
    } else if (axisType === 3) {
      this.setAxisKey(29, x > this.deadzone);
    } else if (axisType === 4) {
      this.setAxisKey(57, x > this.deadzone);
    }
  }

  releaseAll(): void {
    this.pressed.forEach((evdev: number) => this.sink.onButton(evdev, false));
    this.pressed.clear();
    this.axisKeys.forEach((evdev: number) => this.sink.onButton(evdev, false));
    this.axisKeys.clear();
    if (this.outputMode === 'dinput') {
      // Overlay / key-emulation leftovers only. Physical is owned by native kit.
      try { testNapi.controllerResetSource(0); } catch (_) {}
    }
  }

  private watchVibrators(): void {
    if (this.rumbleListening) return;
    this.rumbleListening = true;
    try {
      vibrator.on('vibratorStateChange', (_data: vibrator.VibratorStatusEvent): void => {
        this.refreshPadMotors();
      });
    } catch (error) {
      const e: BusinessError = error as BusinessError;
      hilog.warn(DOMAIN, TAG, 'vibrator.on failed code=%{public}d %{public}s', e.code, e.message);
    }
  }

  private motorKey(motor: PadMotor): string {
    return '' + motor.deviceId + ':' + motor.vibratorId;
  }

  private refreshPadMotors(): void {
    const previous: Map<string, boolean> = new Map<string, boolean>();
    for (let i = 0; i < this.padMotors.length; i++) {
      previous.set(this.motorKey(this.padMotors[i]), this.padMotors[i].usePattern);
    }
    const next: PadMotor[] = [];
    try {
      const infos: vibrator.VibratorInfo[] = vibrator.getVibratorInfoSync();
      for (let i = 0; i < infos.length; i++) {
        const info: vibrator.VibratorInfo = infos[i];
        if (info.isLocalVibrator) continue;
        const motor = new PadMotor();
        motor.deviceId = info.deviceId;
        motor.vibratorId = info.vibratorId;
        motor.deviceName = info.deviceName;
        motor.isHdHapticSupported = info.isHdHapticSupported;
        const prev = previous.get(this.motorKey(motor));
        if (prev === false) motor.usePattern = false;
        if (!motor.isHdHapticSupported) motor.usePattern = false;
        next.push(motor);
      }
    } catch (error) {
      const e: BusinessError = error as BusinessError;
      hilog.warn(DOMAIN, TAG, 'getVibratorInfoSync failed code=%{public}d %{public}s', e.code, e.message);
    }
    this.padMotors = next;
    hilog.info(DOMAIN, TAG, 'pad motors=%{public}d %{public}s', next.length, this.getRumbleStatusText());
  }

  private applyRumble(low: number, high: number, durationMs: number): void {
    if (this.outputMode !== 'dinput') return;
    const now = Date.now();
    if (low <= 0 && high <= 0) {
      this.lastLow = 0;
      this.lastHigh = 0;
      this.lastRumbleAt = now;
      this.stopPadVibration();
      return;
    }
    if (low === this.lastLow && high === this.lastHigh && (now - this.lastRumbleAt) < 60) {
      return;
    }
    this.lastLow = low;
    this.lastHigh = high;
    this.lastRumbleAt = now;

    if (this.padMotors.length === 0) this.refreshPadMotors();
    if (this.padMotors.length === 0) return;

    const duration = durationMs > 0 ? durationMs : 250;
    if (this.padMotors.length >= 2) {
      this.startMotor(this.padMotors[0], low, duration, true);
      this.startMotor(this.padMotors[1], high, duration, false);
      return;
    }
    this.startMotor(this.padMotors[0], Math.max(low, high), duration, true);
  }

  private startMotor(motor: PadMotor, intensityRaw: number, durationMs: number, isLow: boolean): void {
    if (intensityRaw <= 0) {
      this.stopMotor(motor);
      return;
    }
    const duration = Math.max(1, Math.min(5000, Math.round(durationMs)));
    const intensity = Math.max(1, Math.min(100, Math.round(intensityRaw / 655.35)));
    const attr: vibrator.VibrateAttribute = {
      id: motor.vibratorId,
      deviceId: motor.deviceId,
      usage: 'physicalFeedback'
    };
    if (motor.usePattern) {
      try {
        const builder = new vibrator.VibratorPatternBuilder();
        const param: vibrator.ContinuousParam = {
          intensity: intensity,
          frequency: isLow ? 28 : 82,
          index: 0
        };
        builder.addContinuousEvent(0, duration, param);
        vibrator.startVibration({ type: 'pattern', pattern: builder.build() }, attr).catch(
          (error: BusinessError): void => {
            hilog.warn(DOMAIN, TAG, 'pattern rumble fail code=%{public}d, fallback time', error.code);
            motor.usePattern = false;
            this.startTimeVibration(motor, attr, duration);
          });
        return;
      } catch (error) {
        const e: BusinessError = error as BusinessError;
        hilog.warn(DOMAIN, TAG, 'pattern build fail code=%{public}d', e.code);
        motor.usePattern = false;
      }
    }
    this.startTimeVibration(motor, attr, duration);
  }

  private startTimeVibration(motor: PadMotor, attr: vibrator.VibrateAttribute, duration: number): void {
    vibrator.startVibration({ type: 'time', duration: duration }, attr).catch((error: BusinessError): void => {
      hilog.warn(DOMAIN, TAG, 'time rumble fail dev=%{public}d vib=%{public}d code=%{public}d %{public}s',
        motor.deviceId, motor.vibratorId, error.code, error.message);
    });
  }

  private stopMotor(motor: PadMotor): void {
    vibrator.stopVibration({ deviceId: motor.deviceId, vibratorId: motor.vibratorId }).catch(
      (_error: BusinessError): void => {
      });
  }

  private stopPadVibration(): void {
    for (let i = 0; i < this.padMotors.length; i++) {
      this.stopMotor(this.padMotors[i]);
    }
  }
}
