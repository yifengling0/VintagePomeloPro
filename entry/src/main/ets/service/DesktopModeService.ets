import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { deviceInfo } from '@kit.BasicServicesKit';

const DOMAIN: number = 0x0000;
const STORE_NAME: string = 'desktop_mode';
const KEY_MODE: string = 'mode';
export const MODE_VIRTUAL: string = 'virtual';   // 虚拟桌面: 桌面合成器 + 独立窗口
export const MODE_FUSION: string = 'fusion';     // 融合/多窗口: 每窗口独立 OHOS 窗口
// 默认模式按设备: Pad/phone 首装维持虚拟桌面, PC/2in1 维持融合 —
// 无存储值时各设备沿用既有体验, 只有用户显式切换才改变 (phone 由 applyModePolicy 强制虚拟)
const DEFAULT_MODE: string = (deviceInfo.deviceType === '2in1' || deviceInfo.deviceType === 'pc') ?
  MODE_FUSION : MODE_VIRTUAL;

// 桌面模式 (Pad/PC 可切, phone 恒虚拟桌面):
// 持久化用户选择。生效时机: 仅应用启动 — WineWindowManager.init 读此值
// 判定 isDesktopMode (explorer 是否启动/桌面合成器是否开启), 运行中改值
// 不影响当前会话, 故本服务不提供 subscribe, 设置页改完自行刷新显示即可
// (与 DisplaySettingsService 同款, 见其"重启生效"约定)。
export class DesktopModeService {
  private static instance: DesktopModeService | null = null;

  static getInstance(): DesktopModeService {
    if (!DesktopModeService.instance) {
      DesktopModeService.instance = new DesktopModeService();
    }
    return DesktopModeService.instance;
  }

  mode: string = DEFAULT_MODE;

  // 显式模式覆盖 (aa want 试验通道, 不落盘): 优先于 mode/持久化值, 仅内存
  // 生效 — 走实例单例字段而非 AppStorage (want 解析在 Ability 生命周期,
  // AppStorage 跨上下文读取实测不可靠)
  private overrideMode: string = '';

  setOverrideMode(v: string): void {
    if (v === MODE_VIRTUAL || v === MODE_FUSION) {
      this.overrideMode = v;
      hilog.info(DOMAIN, 'wine', 'desktop mode override set: %{public}s', v);
    }
  }

  getOverrideMode(): string {
    return this.overrideMode;
  }

  private store: preferences.Preferences | null = null;
  private initialized: boolean = false;

  init(ctx: common.Context): void {
    if (this.initialized) {
      return;
    }
    this.initialized = true;
    try {
      this.store = preferences.getPreferencesSync(ctx, { name: STORE_NAME });
      const v = this.store.getSync(KEY_MODE, DEFAULT_MODE) as string;
      if (v === MODE_VIRTUAL || v === MODE_FUSION) {
        this.mode = v;
      }
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'desktop mode load fail: %{public}s', JSON.stringify(e));
    }
  }

  /**
   * 持久化桌面模式并等待落盘完成。调用方 (设置页) 在切模式后可能立即重启
   * 应用 — 若 flush 未完成就重启, 新进程会读到旧值 (Preferences 丢失),
   * 导致重启后仍处于原模式。返回 Promise 让调用方 await 后再弹重启提示。
   */
  async setMode(v: string): Promise<void> {
    if (v !== MODE_VIRTUAL && v !== MODE_FUSION) {
      return;
    }
    this.mode = v;
    if (!this.store) {
      return;
    }
    try {
      this.store.putSync(KEY_MODE, this.mode);
      await this.store.flush();
    } catch (e) {
      hilog.error(DOMAIN, 'wine', 'desktop mode persist fail: %{public}s', JSON.stringify(e));
    }
  }
}
