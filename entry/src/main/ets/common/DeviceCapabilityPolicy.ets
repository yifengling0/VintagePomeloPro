import deviceInfo from '@ohos.deviceInfo';
import { window, display } from '@kit.ArkUI';
import { hilog } from '../service/LogService';
import { ScreenOrientationSetting } from '../model/AppModels';

const DOMAIN = 0x0000;
/** 大屏判定阈值 (物理对角线, 英寸)。≥8 寸走大屏模式, <8 寸保持单行封面流。 */
const LARGE_SCREEN_INCHES = 8.0;

export class DeviceCapabilityPolicy {
  private static cachedDiagonalInches: number = -1;

  static deviceType(): string {
    return deviceInfo.deviceType;
  }

  /**
   * 屏幕物理对角线 (英寸)。px / DPI 计算, 与当前旋转方向无关;
   * DPI 不可用时回退 densityPixels*160。取 0 表示无法测量。
   */
  static screenDiagonalInches(): number {
    if (DeviceCapabilityPolicy.cachedDiagonalInches >= 0) {
      return DeviceCapabilityPolicy.cachedDiagonalInches;
    }
    let inches = 0;
    try {
      const d = display.getDefaultDisplaySync();
      const density = d.densityPixels > 0 ? d.densityPixels : 3.5;
      const dpiX = d.xDPI > 0 ? d.xDPI : density * 160;
      const dpiY = d.yDPI > 0 ? d.yDPI : density * 160;
      const wIn = d.width / dpiX;
      const hIn = d.height / dpiY;
      inches = Math.sqrt(wIn * wIn + hIn * hIn);
    } catch (_) {
      inches = 0;
    }
    DeviceCapabilityPolicy.cachedDiagonalInches = inches;
    hilog.info(DOMAIN, 'DeviceCapabilityPolicy', 'screen diagonal=%{public}s large=%{public}s',
      inches > 0 ? inches.toFixed(2) + 'in' : 'unknown',
      String(DeviceCapabilityPolicy.isLargeScreen()));
    return inches;
  }

  /** ≥8 寸 → 大屏设备 (后续大屏模式的开关; 当前仅检测上报, 不改变 UI 行为)。 */
  static isLargeScreen(): boolean {
    const inches = DeviceCapabilityPolicy.screenDiagonalInches();
    return inches > 0 && inches >= LARGE_SCREEN_INCHES;
  }

  /** 设置页展示用: "5.6 英寸"。 */
  static screenSizeLabel(): string {
    const inches = DeviceCapabilityPolicy.screenDiagonalInches();
    return inches > 0 ? inches.toFixed(1) + ' 英寸' : '未知';
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

  /**
   * 启动器/库页 UI 的方向策略: 只走横板 — 左横屏 ↔ 右横屏跟随传感器自动旋转
   * (受系统控制中心旋转开关约束), 不再进入竖屏。
   * 2in1/PC 是自由缩放的桌面窗口, 不加方向约束。
   */
  static applyLauncherLandscapeRotation(target: window.Window, tag: string): void {
    if (DeviceCapabilityPolicy.usesManagedWineWindows()) return;
    target.setPreferredOrientation(window.Orientation.AUTO_ROTATION_LANDSCAPE_RESTRICTED).then(() => {
      hilog.info(DOMAIN, tag, 'Launcher landscape rotation enabled');
    }).catch((error: Error) => {
      hilog.warn(DOMAIN, tag, 'Launcher landscape rotation unavailable: %{public}s', JSON.stringify(error));
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
