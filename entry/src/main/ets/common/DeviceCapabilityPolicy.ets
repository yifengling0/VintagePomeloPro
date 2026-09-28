import deviceInfo from '@ohos.deviceInfo';
import { window } from '@kit.ArkUI';
import { hilog } from '../service/LogService';
import { ScreenOrientationSetting } from '../model/AppModels';

const DOMAIN = 0x0000;

export class DeviceCapabilityPolicy {
  static deviceType(): string {
    return deviceInfo.deviceType;
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
    return type === '2in1' || type === 'pc';
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
