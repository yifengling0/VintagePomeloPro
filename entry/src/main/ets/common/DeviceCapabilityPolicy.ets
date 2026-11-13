import { isValueAddedVisibleOnDevice } from '../model/ValueAddedModels';
import deviceInfo from '@ohos.deviceInfo';
import { settings } from '@kit.BasicServicesKit';
import { common } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { hilog } from '../service/LogService';
import { ScreenOrientationSetting } from '../model/AppModels';

const DOMAIN = 0x0000;

const PC_MODE_KEY: string = 'settings.sceneboard.ispcmode';

export class DeviceCapabilityPolicy {
  private static modeContext: common.UIAbilityContext | null = null;
  private static observingPcMode: boolean = false;
  private static modeOwners: Set<common.UIAbilityContext> = new Set();
  private static pcMode: string = 'unknown';
  private static modeListeners: Set<() => void> = new Set();

  static startPcModeMonitoring(context: common.UIAbilityContext): void {
    DeviceCapabilityPolicy.modeOwners.add(context);
    if (DeviceCapabilityPolicy.modeContext) return;
    DeviceCapabilityPolicy.modeContext = context;
    DeviceCapabilityPolicy.refreshPcMode(context);
    try {
      DeviceCapabilityPolicy.observingPcMode = settings.registerKeyObserver(context,
        PC_MODE_KEY, settings.domainName.USER_PROPERTY, () => {
          DeviceCapabilityPolicy.refreshPcMode(context);
        });
      // Close the read/register race when the system switches mode at startup.
      DeviceCapabilityPolicy.refreshPcMode(context);
    } catch (error) {
      hilog.warn(DOMAIN, 'DeviceMode', 'PC mode observer unavailable: %{public}s', JSON.stringify(error));
    }
  }

  static stopPcModeMonitoring(owner: common.UIAbilityContext): void {
    DeviceCapabilityPolicy.modeOwners.delete(owner);
    if (DeviceCapabilityPolicy.modeContext !== owner) return;
    const context = DeviceCapabilityPolicy.modeContext;
    if (context && DeviceCapabilityPolicy.observingPcMode) {
      try { settings.unregisterKeyObserver(context, PC_MODE_KEY, settings.domainName.USER_PROPERTY); }
      catch (_) {}
    }
    DeviceCapabilityPolicy.observingPcMode = false;
    DeviceCapabilityPolicy.modeContext = null;
    for (const nextOwner of DeviceCapabilityPolicy.modeOwners) {
      DeviceCapabilityPolicy.startPcModeMonitoring(nextOwner);
      break;
    }
  }

  static refreshPcMode(context: common.UIAbilityContext): void {
    let next: string = 'unknown';
    try {
      const value = settings.getValueSync(context, PC_MODE_KEY, 'unknown', settings.domainName.USER_PROPERTY);
      if (value === 'true' || value === 'false') next = value;
    } catch (_) {
      // A transient settings failure cannot switch a live PC session to tablet.
      // Before the first successful read pcMode is unknown (static fallback).
      return;
    }
    if (next === DeviceCapabilityPolicy.pcMode) return;
    DeviceCapabilityPolicy.pcMode = next;
    AppStorage.setOrCreate('hostPcMode', next);
    hilog.info(DOMAIN, 'DeviceMode', 'PC mode=%{public}s deviceType=%{public}s managedWindows=%{public}s',
      next, DeviceCapabilityPolicy.deviceType(), DeviceCapabilityPolicy.usesManagedWineWindows() ? 'yes' : 'no');
    DeviceCapabilityPolicy.modeListeners.forEach((listener: () => void) => listener());
  }

  static addPcModeListener(listener: () => void): void { DeviceCapabilityPolicy.modeListeners.add(listener); }
  static removePcModeListener(listener: () => void): void { DeviceCapabilityPolicy.modeListeners.delete(listener); }

  static deviceType(): string {
    return deviceInfo.deviceType;
  }

  static supportsValueAddedFeatures(): boolean {
    return isValueAddedVisibleOnDevice(DeviceCapabilityPolicy.deviceType());
  }

  static isPhoneDevice(): boolean {
    return DeviceCapabilityPolicy.deviceType() === 'phone';
  }

  static isTabletDevice(): boolean {
    return DeviceCapabilityPolicy.deviceType() === 'tablet';
  }

  static usesForkNativeChildProcess(): boolean {
    const type = DeviceCapabilityPolicy.deviceType();
    return type === 'phone' || type === 'tv';
  }

  static usesManagedWineWindows(): boolean {
    const type = DeviceCapabilityPolicy.deviceType();
    if (type === 'phone' || type === 'tv') return false;
    if (type === 'pc') return true;
    if (DeviceCapabilityPolicy.pcMode !== 'unknown') return DeviceCapabilityPolicy.pcMode === 'true';
    return type === '2in1';
  }

  static supportsSensorRotation(): boolean {
    return DeviceCapabilityPolicy.isPhoneDevice();
  }

  static enableSensorRotation(target: window.Window, tag: string): void {
    if (!DeviceCapabilityPolicy.supportsSensorRotation()) return;
    // 2026-09-28: 由 AUTO_ROTATION_RESTRICTED 改为不受限 AUTO_ROTATION ——
    // RESTRICTED 受系统控制中心"自动旋转"开关约束, 开关一关左右横旋就永远失效
    // (用户实测长期回归); 不受限变体无视该开关, 传感器直接驱动, 方向集合不变。
    target.setPreferredOrientation(window.Orientation.AUTO_ROTATION).then(() => {
      hilog.info(DOMAIN, tag, 'Phone auto rotation enabled (unrestricted)');
    }).catch((error: Error) => {
      hilog.warn(DOMAIN, tag, 'Phone auto rotation unavailable: %{public}s', JSON.stringify(error));
    });
  }

  static applyPortraitOrientation(target: window.Window, tag: string): Promise<void> {
    return target.setPreferredOrientation(window.Orientation.PORTRAIT).then(() => {
      hilog.info(DOMAIN, tag, 'Portrait orientation applied');
    }).catch((error: Error) => {
      hilog.warn(DOMAIN, tag, 'Portrait orientation unavailable: %{public}s', JSON.stringify(error));
    });
  }

  static applyDesktopOrientation(target: window.Window, setting: ScreenOrientationSetting, tag: string): Promise<void> {
    // Freeform PC windows follow the system workspace, not the game's orientation.
    if (DeviceCapabilityPolicy.usesManagedWineWindows()) return Promise.resolve();
    let orientation: window.Orientation = window.Orientation.AUTO_ROTATION_LANDSCAPE_RESTRICTED;
    if (setting === ScreenOrientationSetting.LANDSCAPE) {
      orientation = window.Orientation.LANDSCAPE;
    } else if (setting === ScreenOrientationSetting.PORTRAIT) {
      orientation = window.Orientation.PORTRAIT;
    }
    return target.setPreferredOrientation(orientation).then(() => {
      hilog.info(DOMAIN, tag, 'Desktop orientation applied: %{public}s', setting);
    }).catch((error: Error) => {
      hilog.warn(DOMAIN, tag, 'Desktop orientation unavailable: %{public}s', JSON.stringify(error));
    });
  }

  /** Wait until windowRect matches the requested portrait/landscape side. */
  static async waitForRequestedOrientation(target: window.Window, setting: ScreenOrientationSetting,
    tag: string): Promise<void> {
    if (DeviceCapabilityPolicy.usesManagedWineWindows()) return;
    const needLandscape = setting !== ScreenOrientationSetting.PORTRAIT;
    for (let attempt = 0; attempt < 16; attempt++) {
      try {
        const rect = target.getWindowProperties().windowRect;
        if (rect.width > 0 && rect.height > 0 &&
          (needLandscape ? rect.width >= rect.height : rect.height >= rect.width)) {
          hilog.info(DOMAIN, tag, 'final window rect=%{public}dx%{public}d orientation=%{public}s',
            rect.width, rect.height, setting);
          return;
        }
      } catch (_) {
      }
      await new Promise<void>((resolve: () => void) => setTimeout(resolve, 100));
    }
    try {
      const rect = target.getWindowProperties().windowRect;
      hilog.warn(DOMAIN, tag, 'orientation settle timeout rect=%{public}dx%{public}d requested=%{public}s',
        rect.width, rect.height, setting);
    } catch (_) {
    }
  }
}
