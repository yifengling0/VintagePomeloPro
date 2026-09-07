import { common, Want } from '@kit.AbilityKit';
import { window, display } from '@kit.ArkUI';
import { hilog } from './LogService';
import { BusinessError } from '@kit.BasicServicesKit';
import { pointer } from '@kit.InputKit';
import testNapi from 'libentry.so';
import {
  DesktopWindowMode,
  ScreenOrientationSetting,
  resolveCornerSafeMarginPx,
  WinePresentationMode
} from '../model/AppModels';
import { PopupWindowManager } from './PopupWindowManager';
import { ArgbWindowManager } from './ArgbWindowManager';
import { ModalWindowManager } from './ModalWindowManager';
import { AppSettingsStore } from './AppSettingsStore';
import { DesktopModeService, MODE_VIRTUAL, MODE_FUSION } from './DesktopModeService';
import { DeviceCapabilityPolicy } from '../common/DeviceCapabilityPolicy';

const DOMAIN = 0x0000;
const TAG = 'WineWM';
const DESKTOP_SESSION_ID = 'desktop';
export type SessionAssociationCallback = (sessionId: string, clientPid: number, toplevelId: number) => void;

export class WineWindowManager {
  private static instance: WineWindowManager | null = null;
  private windowStage: window.WindowStage | null = null;
  private mainWindow: window.Window | null = null;
  private uiAbilityContext: common.UIAbilityContext | null = null;
  // 设备显示密度 + 屏幕尺寸 (用于缩放)
  private displayScale: number = 1.0;
  private scaleBoost: number = 1.0;  // 放大系数, 使 effectiveScale = displayScale × boost = 2.0
  private outputScalePercent: number = 100;
  // 标题栏 + 边框总高度 (VP), 用于窗口 resize
  // = getWindowDecorHeight() + decorBorderOffset
  private measuredTitleBarH: number = 56;
  // API 返回的纯标题栏高度 (loadContent 后获取)
  private apiTitleBarH: number = 37;
  // 边框/阴影等 API 不涵盖的额外装饰 (VP), surface 校准自动测量
  private decorBorderOffset: number = 19;  // 56 - 37
  // 校准计数: 前几轮不做 EMA 平滑, 快速收敛
  private barCalibrationCount: number = 0;

  // -- Desktop 模式 (Tablet / PC 虚拟桌面) --
  // 公开: WineEngineService 镜像给 ArkTS 上层
  isDesktopMode: boolean = true;
  // 公开: 由 applyModePolicy 统一判定 (PC 按 DesktopModeService 设置)
  presentationMode: WinePresentationMode = WinePresentationMode.DESKTOP;
  // -- 光标隐藏门禁 (dinput 相对模式驱动, 见 applyPointerVisibility) --
  // pointerLocked: wine 最近的真实"相对模式锁定"请求 (不受展示形态门禁过滤,
  // 只是原始事实); effectiveHidden: 我们实际对系统光标做的显隐。两者分离,
  // 以便"悬浮窗 ↔ 完整桌面"切换时按 pointerLocked × 是否允许隐藏重算 — 否则
  // 悬浮态被抑制的隐藏, 切到完整桌面后没有新事件通知 → 光标残留显示 (游戏里
  // 光标浮出)。default=false, 与融合模式"只拦桌面"的判据兼容。
  private pointerLocked: boolean = false;
  private effectiveHidden: boolean = false;
  // 触发相对模式的约束 surface 的 toplevelId (解锁时=0)。用于区分"桌面 shell
  // 自身藏光标 (启动瞬时 toplevelId==desktopRootId)"与"游戏子窗口真相对模式"。
  private pointerLockToplevel: number = 0;
  private desktopRootId: number = 0;
  // 桌面启动器/悬浮窗可见性 (浮窗模式): Index 悬浮窗态=true → DesktopLayer
  // 跳过 root resize (桌面分辨率保持全屏, 缩略显示靠 letterbox); 全屏态=false
  desktopLauncherVisible: boolean = true;
  /** Index 浮窗放大/缩小动画进行中: 跳过 Wine 桌面 resize，避免 explorer root switch。 */
  floatingLayoutAnimating: boolean = false;
  private desktopAbilityRunning: boolean = false;
  private desktopAbilityForeground: boolean = false;
  private desktopAbilityRootId: number = 0;
  private desktopAbilityCtx: common.UIAbilityContext | null = null;
  // 主窗口 windowStage (ARGB 异型子窗口/popup 回退宿主)
  private mainWindowStage: window.WindowStage | null = null;

  /**
   * 桌面模式统一判定 (与上游 master 同构):
   * - Pad/phone 恒走桌面合成器 (isDesktopMode=true)
   * - PC/2in1 按 DesktopModeService 用户设置 (MODE_FUSION=独立窗口 / MODE_VIRTUAL=虚拟桌面)
   * 调用点: app 启动 (init) + 引擎每次启动前 (WineEngineService), 切换后重启引擎即生效。
   */
  applyModePolicy(): void {
    const isPadOrPhone = DeviceCapabilityPolicy.isTabletDevice() ||
      DeviceCapabilityPolicy.isPhoneDevice();
    if (isPadOrPhone) {
      this.isDesktopMode = true;
      this.presentationMode = WinePresentationMode.DESKTOP;
    } else {
      const mode = DesktopModeService.getInstance().mode;
      this.isDesktopMode = (mode === MODE_VIRTUAL);
      this.presentationMode = this.isDesktopMode ?
        WinePresentationMode.DESKTOP : WinePresentationMode.MANAGED_WINDOWS;
    }
    this.desktopRootId = 0;
    testNapi.setDesktopMode(this.isDesktopMode);
    hilog.info(DOMAIN, TAG, '[MW] applyModePolicy=%{public}s nativeDesktop=%{public}s',
      this.presentationMode, this.isDesktopMode ? 'yes' : 'no');
  }

  // toplevelId -> UIAbilityContext (用于 terminateSelf)
  private abilityContexts: Map<number, common.UIAbilityContext> = new Map();
  // toplevelId -> window.Window (用于直接操作 resize/title)
  private windows: Map<number, window.Window> = new Map();
  // toplevelId -> window.WindowStage (用于创建 popup 子窗口)
  private windowStages: Map<number, window.WindowStage> = new Map();
  // toplevelId -> 已销毁标记 (用于竞态检测)
  private destroyed: Set<number> = new Set();
  // 正在启动中的 toplevel (防止重复 startAbility)
  private pendingStarts: Set<number> = new Set();
  // PC 融合模式 modal 对话框 (无独立 Ability, 子窗口承载在 ModalWindowManager)
  private modalToplevelIds: Set<number> = new Set();
  // 系统窗口加载状态与 Wine 全屏状态分开记录。fullscreen 事件经常早于
  // Ability/loadContent 到达，不能在 onAfterLoadContent 时猜测窗口模式。
  private loadedWindows: Set<number> = new Set();
  private fullscreenWindows: Set<number> = new Set();
  private pendingRaises: Set<number> = new Set();
  private deferredRaiseTimers: Map<number, number> = new Map();
  // 窗口就绪前到达的 pending 事件
  private pendingEvents: Map<number, Array<[string, string]>> = new Map();
  // toplevelId -> surface 物理尺寸 (用于标题栏高度校准)
  private surfaceSizes: Map<number, [number, number]> = new Map();
  // toplevelId -> 最近一次 resize 的目标 VP 高度 (用于校准 titleBarH)
  private lastResizeTargetH: Map<number, number> = new Map();
  // toplevelId -> 最近一次 resize 的 wine 原始尺寸 (用于校准后重新 resize)
  private lastResizeWineDim: Map<number, [number, number]> = new Map();
  // toplevelId -> 最大化前的尺寸 (recover 后手动恢复)
  private lastPreMaximizeDim: Map<number, [number, number]> = new Map();
  // toplevelId -> maximize 操作进行中 (诊断 OHOS 状态机冲突)
  private maximizeInFlight: Set<number> = new Set();
  // toplevelId -> 最近一次设置的标题 (loadContent 后会重置, 需要重新 apply)
  private lastTitles: Map<number, string> = new Map();
  // toplevelId -> 是否 ARGB (layered/shaped 异型) 窗口, 仅记录 (后续子窗口方案使用)
  private lastArgb: Map<number, boolean> = new Map();
  // Wine move_grab 等待 startMoving() — 标记哪些 toplevel 在等待拖拽
  private moveGrabPending: Set<number> = new Set();
  private sessionAssociationCallback: SessionAssociationCallback | null = null;

  static getInstance(): WineWindowManager {
    if (!WineWindowManager.instance) {
      WineWindowManager.instance = new WineWindowManager();
    }
    return WineWindowManager.instance;
  }

  getMainWindow(): window.Window | null {
    return this.mainWindow;
  }

  setSessionAssociationCallback(callback: SessionAssociationCallback): void {
    this.sessionAssociationCallback = callback;
  }

  // -- dump 当前状态 (诊断用) --
  dumpState(): void {
    hilog.info(DOMAIN, TAG, '[MW-DUMP] state: abilityCtx=%{public}d windows=%{public}d pendingStarts=%{public}d pendingEvents=%{public}d destroyed=%{public}d',
      this.abilityContexts.size, this.windows.size, this.pendingStarts.size,
      this.pendingEvents.size, this.destroyed.size);
    this.pendingStarts.forEach(id => {
      hilog.info(DOMAIN, TAG, '[MW-DUMP]   pendingStart: #%{public}d', id);
    });
    this.pendingEvents.forEach((events, id) => {
      hilog.info(DOMAIN, TAG, '[MW-DUMP]   pendingEvents #%{public}d: %{public}d items', id, events.length);
    });
    this.windows.forEach((_, id) => {
      hilog.info(DOMAIN, TAG, '[MW-DUMP]   active window: #%{public}d', id);
    });
  }

  async init(windowStage: window.WindowStage, abilityContext: common.UIAbilityContext): Promise<void> {
    this.windowStage = windowStage;
    this.mainWindowStage = windowStage;
    this.mainWindow = windowStage.getMainWindowSync();
    this.uiAbilityContext = abilityContext;

    hilog.info(DOMAIN, TAG, '[MW] init: mainWindow=%{public}s ctx=%{public}s',
      this.mainWindow ? 'ok' : 'NULL', this.uiAbilityContext ? 'ok' : 'NULL');

    // 桌面模式统一判定: PC/2in1 按 DesktopModeService 设置 (融合/虚拟桌面),
    // Pad/phone 恒走桌面合成器。同步 native setDesktopMode (explorer 是否启动)。
    try {
      DesktopModeService.getInstance().init(this.uiAbilityContext);
    } catch (_) {
      hilog.warn(DOMAIN, TAG, '[MW] DesktopModeService init failed, fallback policy');
    }
    this.applyModePolicy();
    hilog.info(DOMAIN, TAG, '[MW] configured presentation mode=%{public}s', this.presentationMode);

    // 获取设备显示密度 + 主窗口可用区域
    try {
      const d = display.getDefaultDisplaySync();
      const safe = this.desktopDisplaySafeSize(d.width, d.height);
      this.syncOutputSize(safe.width, safe.height);
    } catch (_) {
      this.displayScale = 1.61;
      this.scaleBoost = 2.0 / 1.61;
      hilog.warn(DOMAIN, TAG, '[MW] display query failed, using displayScale=1.61 boost=1.24');
      testNapi.setDisplayScale(this.displayScale * this.scaleBoost);
    }

    hilog.info(DOMAIN, TAG, '[MW] registering toplevel callback...');
    testNapi.setToplevelCallback((id: number, event: string, data: string) => {
      hilog.info(DOMAIN, TAG, '[MW]  cb toplevel #%{public}d event=%{public}s data=%{public}s',
        id, event, data);
      this.onToplevelEvent(id, event, data);
    });
    hilog.info(DOMAIN, TAG, '[MW] toplevel callback registered OK');

    // dinput 相对模式 (游戏隐藏光标无限转视角) 的 host 侧配套:
    // 注册主窗口 id 供 native OH_WindowManager_LockCursor 冻结光标;
    // 锁定状态回调里隐藏/恢复系统光标 (隐藏能力只有 ArkTS API)
    if (this.mainWindow) {
      testNapi.registerHostWindow(this.mainWindow.getWindowProperties().id);
    }
    testNapi.setPointerLockCallback((locked: boolean, fromToplevel: number) => {
      // 记录 wine 真实锁定请求与来源 toplevel (不受门禁过滤), 再由
      // applyPointerVisibility 按展示形态 + 来源决定是否 setPointerVisible —
      // 悬浮窗态抑制; 完整桌面态: 桌面 root surface (shell 自身) 也抑制, 仅
      // 子窗口 (真游戏) 跟随。解锁 (locked=false) 必然走 SHOW 分支。
      this.pointerLocked = locked;
      this.pointerLockToplevel = fromToplevel;
      this.applyPointerVisibility();
    });

    // A launcher resume recreates EntryAbility while the Wine desktop root can
    // still be alive.  No new desktop_root event is emitted in that case, so
    // explicitly bring the existing desktop compositor back to the foreground.
    if (this.isDesktopMode && this.desktopRootId > 0) {
      hilog.info(DOMAIN, TAG, '[MW] restoring existing DesktopAbility root=%{public}d',
        this.desktopRootId);
      this.startDesktopAbility(this.desktopRootId);
    }
  }

  setPresentationMode(mode: WinePresentationMode): void {
    this.presentationMode = mode;
    this.isDesktopMode = mode === WinePresentationMode.DESKTOP;
    this.desktopRootId = 0;
    this.desktopAbilityRunning = false;
    this.desktopAbilityForeground = false;
    this.desktopAbilityRootId = 0;
    this.destroyed.clear();
    this.deferredRaiseTimers.forEach((timer: number): void => clearTimeout(timer));
    this.deferredRaiseTimers.clear();
    this.loadedWindows.clear();
    this.fullscreenWindows.clear();
    this.pendingRaises.clear();
    this.modalToplevelIds.clear();
    testNapi.setDesktopMode(this.isDesktopMode);
    hilog.info(DOMAIN, TAG, '[MW] setPresentationMode=%{public}s nativeDesktop=%{public}s',
      mode, this.isDesktopMode ? 'yes' : 'no');
  }

  setOutputScalePercent(value: number): void {
    this.outputScalePercent = Math.max(75, Math.min(150, value));
    try {
      const currentDisplay = display.getDefaultDisplaySync();
      const safe = this.desktopDisplaySafeSize(currentDisplay.width, currentDisplay.height);
      this.syncOutputSize(safe.width, safe.height);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, '[MW] setOutputScalePercent failed: %{public}s', JSON.stringify(error));
    }
  }

  syncOutputSize(widthPx: number, heightPx: number, forceToplevelResize: boolean = false): void {
    if (widthPx <= 0 || heightPx <= 0) return;
    try {
      const currentDisplay = display.getDefaultDisplaySync();
      this.displayScale = Math.max(0.1, currentDisplay.densityPixels);
      const effectiveScale = 2.0 * this.outputScalePercent / 100;
      this.scaleBoost = effectiveScale / this.displayScale;
      const outW = Math.max(1, Math.round(widthPx / effectiveScale));
      const outH = Math.max(1, Math.round(heightPx / effectiveScale));
      testNapi.setDisplayScale(effectiveScale);
      testNapi.setOutputSize(outW, outH);
      // setOutputSize updates compositor bookkeeping, but an existing Wine
      // Explorer desktop also needs an xdg configure. Some phone rotations do
      // not deliver a reliable XComponent surface-change callback, leaving the
      // old portrait desktop as a narrow strip in a landscape Ability.
      // PC 虚拟桌面浮窗: 桌面分辨率固定 (init setOutputSize = 屏幕/scale), 窗口
      // 只是显示口 (letterbox 等比完整呈现) — 跳过 notifyToplevelResize, 否则
      // 浮窗尺寸/标题栏偏差触发 Wine 桌面重排, 底部任务栏/开始菜单被裁。
      // Index 浮窗: 缩略/缩放动画中 letterbox, 只在 prepareOutputForOrientation
      // (force) 时改 Wine 桌面尺寸, 避免 explorer 再提交一个 desktop-shell。
      if (this.presentationMode === WinePresentationMode.DESKTOP && this.desktopRootId > 0 &&
        !(AppSettingsStore.getInstance().usesManagedWineWindows() && this.isDesktopMode) &&
        this.shouldNotifyDesktopToplevelResize(forceToplevelResize)) {
        testNapi.notifyToplevelResize(this.desktopRootId, outW, outH);
      }
      hilog.info(DOMAIN, TAG,
        '[MW] host size=%{public}dx%{public}d output=%{public}dx%{public}d scale=%{public}s',
        widthPx, heightPx, outW, outH, effectiveScale.toFixed(2));
    } catch (error) {
      hilog.warn(DOMAIN, TAG, '[MW] syncOutputSize failed: %{public}s', JSON.stringify(error));
    }
  }

  /**
   * Set the compositor's first output rectangle before Wine/Wayland starts.
   * The host display can still be in the previous sensor orientation when a
   * new DesktopAbility is launched, so normalize the dimensions explicitly
   * instead of waiting for the later window-size callback.
   */
  prepareOutputForOrientation(setting: ScreenOrientationSetting): void {
    try {
      const d = display.getDefaultDisplaySync();
      let width = d.width;
      let height = d.height;
      const isLandscape = width >= height;
      // SENSOR_LANDSCAPE still means the desktop must start in a landscape
      // rectangle; the sensor only chooses between the two landscape sides.
      // Treating it as neither landscape nor portrait created a 640x1416
      // Wayland output while the Ability rotated to 2832x1280 afterwards.
      const wantsLandscape = setting !== ScreenOrientationSetting.PORTRAIT;
      if (wantsLandscape && !isLandscape) {
        const swapped = width;
        width = height;
        height = swapped;
      } else if (!wantsLandscape && isLandscape) {
        const swapped = width;
        width = height;
        height = swapped;
      }
      const safe = this.desktopDisplaySafeSize(width, height);
      this.syncOutputSize(safe.width, safe.height, true);
      hilog.info(DOMAIN, TAG, '[MW] prepared output orientation=%{public}s size=%{public}dx%{public}d',
        setting, width, height);
    } catch (error) {
      hilog.warn(DOMAIN, TAG, '[MW] prepare output orientation failed: %{public}s', JSON.stringify(error));
    }
  }

  /**
   * 切边模式: 只缩宽度让出左右圆角避让边距, 高度保持充满。
   * 桌面矩形 (margin..W-margin, 0..H) 完全避开四角圆角, 且上下满屏;
   * 使 explorer 桌面壳出生时就按正确分辨率布局。全屏模式返回原尺寸。
   */
  desktopDisplaySafeSize(width: number, height: number): window.Size {
    const mode = AppSettingsStore.getInstance().getGlobalSettings().desktopWindowMode
      ?? DesktopWindowMode.FULLSCREEN;
    if (mode !== DesktopWindowMode.SAFE_AREA) {
      return { width: width, height: height };
    }
    const margin = resolveCornerSafeMarginPx(width, height);
    return {
      width: Math.max(1, width - margin * 2),
      height: height
    };
  }

  // -- WineWindowAbility 调用: 注册 / 注销 --

  registerAbility(toplevelId: number, ctx: common.UIAbilityContext): void {
    this.abilityContexts.set(toplevelId, ctx);
    hilog.info(DOMAIN, TAG, '[MW]  registerAbility #%{public}d (total=%{public}d)',
      toplevelId, this.abilityContexts.size);
  }

  registerWindow(toplevelId: number, win: window.Window): void {
    this.windows.set(toplevelId, win);
    hilog.info(DOMAIN, TAG, '[MW]  registerWindow #%{public}d (total=%{public}d, pendingStarts=%{public}s)',
      toplevelId, this.windows.size, this.pendingStarts.has(toplevelId) ? 'yes' : 'no');
  }

  registerWindowStage(toplevelId: number, stage: window.WindowStage): void {
    this.windowStages.set(toplevelId, stage);
    hilog.info(DOMAIN, TAG, '[MW]  registerWindowStage #%{public}d (total=%{public}d)',
      toplevelId, this.windowStages.size);
  }

  getWindowStage(toplevelId: number): window.WindowStage | null {
    return this.windowStages.get(toplevelId) ?? null;
  }

  /** 主窗口 windowStage (浮窗模式 Index 承载桌面时创建虚拟输入 overlay 用)。 */
  getMainWindowStage(): window.WindowStage | null {
    return this.mainWindowStage;
  }

  // effectiveScale = displayScale × scaleBoost (Wine 逻辑坐标 → 物理像素)
  getEffectiveScale(): number {
    return this.displayScale * this.scaleBoost;
  }

  getDesktopMode(): boolean {
    return this.isDesktopMode;
  }

  // 最近通过 resize 事件报告给 ArkTS 的 Wine 内容尺寸 (PC 模式防环用:
  // onSurfaceChanged 反馈 configure 前比对, 一致说明窗口尺寸由我们按
  // 内容尺寸设置, 不必再通知 Wine, 避免反馈环把窗口模式游戏推到
  // 非内容尺寸)
  getLastResizeWineDim(toplevelId: number): [number, number] | null {
    return this.lastResizeWineDim.get(toplevelId) ?? null;
  }

  unregisterAbility(toplevelId: number): void {
    const hadCtx = this.abilityContexts.has(toplevelId);
    const hadWin = this.windows.has(toplevelId);
    const hadStage = this.windowStages.has(toplevelId);
    const hadPend = this.pendingEvents.has(toplevelId);
    const hadStart = this.pendingStarts.has(toplevelId);
    const hadSurf = this.surfaceSizes.has(toplevelId);
    const hadResizeTarget = this.lastResizeTargetH.has(toplevelId);
    const hadWineDim = this.lastResizeWineDim.has(toplevelId);
    const hadTitle = this.lastTitles.has(toplevelId);
    const hadMaxFlight = this.maximizeInFlight.has(toplevelId);
    // 级联销毁该窗口的全部 popup 子窗口
    PopupWindowManager.getInstance().destroyPopupsForToplevel(toplevelId);
    ModalWindowManager.getInstance().destroyForOwner(toplevelId);
    this.abilityContexts.delete(toplevelId);
    this.windows.delete(toplevelId);
    this.windowStages.delete(toplevelId);
    this.loadedWindows.delete(toplevelId);
    this.fullscreenWindows.delete(toplevelId);
    this.pendingRaises.delete(toplevelId);
    this.cancelDeferredRaise(toplevelId);
    this.pendingEvents.delete(toplevelId);
    this.pendingStarts.delete(toplevelId);
    this.surfaceSizes.delete(toplevelId);
    this.lastResizeTargetH.delete(toplevelId);
    this.lastResizeWineDim.delete(toplevelId);
    this.lastPreMaximizeDim.delete(toplevelId);
    this.lastTitles.delete(toplevelId);
    this.lastArgb.delete(toplevelId);
    this.maximizeInFlight.delete(toplevelId);
    hilog.info(DOMAIN, TAG, '[MW]  unregisterAbility #%{public}d (hadCtx=%{public}s hadWin=%{public}s hadStage=%{public}s hadPend=%{public}s hadStart=%{public}s hadSurf=%{public}s hadResizeTarget=%{public}s hadWineDim=%{public}s hadTitle=%{public}s hadMaxFlight=%{public}s)',
      toplevelId, hadCtx ? 'yes' : 'no', hadWin ? 'yes' : 'no', hadStage ? 'yes' : 'no',
      hadPend ? 'yes' : 'no', hadStart ? 'yes' : 'no', hadSurf ? 'yes' : 'no',
      hadResizeTarget ? 'yes' : 'no', hadWineDim ? 'yes' : 'no',
      hadTitle ? 'yes' : 'no', hadMaxFlight ? 'yes' : 'no');
  }

  isDestroyed(toplevelId: number): boolean {
    const result = this.destroyed.has(toplevelId);
    if (result) {
      hilog.warn(DOMAIN, TAG, '[MW] isDestroyed #%{public}d = true', toplevelId);
    }
    return result;
  }

  // Wine xdg_toplevel.move 触发 → 标记等待 startMoving()
  // WineWindow 的 onMouse 在下一帧 MOVE 时检查此标记并调用 startMoving()
  shouldStartMoving(toplevelId: number): boolean {
    return this.moveGrabPending.has(toplevelId);
  }

  getWindow(toplevelId: number): window.Window | undefined {
    return this.windows.get(toplevelId);
  }

  private cancelDeferredRaise(id: number): void {
    const timer = this.deferredRaiseTimers.get(id);
    if (timer !== undefined) {
      clearTimeout(timer);
      this.deferredRaiseTimers.delete(id);
    }
  }

  /**
   * Wine fullscreen 通常先于 Ability 注册到达。新窗口的系统层级提升延迟一
   * 小段时间，并在执行前重新检查 fullscreen，避免 loadContent 与
   * set_fullscreen 竞态造成窗口几何突变和鼠标坐标漂移。
   */
  private scheduleManagedRaise(id: number, reason: string): void {
    this.cancelDeferredRaise(id);
    const timer = setTimeout((): void => {
      this.deferredRaiseTimers.delete(id);
      if (this.isDesktopMode || this.destroyed.has(id) || !this.loadedWindows.has(id) ||
          this.fullscreenWindows.has(id)) {
        hilog.info(DOMAIN, TAG,
          '[MW] deferred raise #%{public}d skipped reason=%{public}s fullscreen=%{public}s loaded=%{public}s',
          id, reason, this.fullscreenWindows.has(id) ? 'yes' : 'no',
          this.loadedWindows.has(id) ? 'yes' : 'no');
        return;
      }
      hilog.info(DOMAIN, TAG, '[MW] deferred raise #%{public}d apply reason=%{public}s', id, reason);
      this.raiseManagedWindow(id);
    }, 250);
    this.deferredRaiseTimers.set(id, timer);
  }

  private raiseManagedWindow(id: number): void {
    if (this.fullscreenWindows.has(id)) return;
    const win = this.getWindow(id);
    if (!win) return;
    win.raiseToAppTop().catch((err: Error) => {
      hilog.warn(DOMAIN, TAG, '[MW] raise window #%{public}d failed: %{public}s', id, JSON.stringify(err));
    });
  }

  // -- loadContent 回调: 窗口装饰就绪后校准标题栏高度 + 重设标题 --
  onAfterLoadContent(toplevelId: number): void {
    const win = this.windows.get(toplevelId);
    if (!win) return;
    this.loadedWindows.add(toplevelId);

    if (this.fullscreenWindows.has(toplevelId)) {
      this.pendingRaises.delete(toplevelId);
      hilog.info(DOMAIN, TAG, '[MW] onAfterLoadContent #%{public}d: fullscreen, skip system raise', toplevelId);
    } else if (this.pendingRaises.delete(toplevelId)) {
      this.scheduleManagedRaise(toplevelId, 'pending-wine-raise');
    } else if (!this.isDesktopMode) {
      // 第二次打开窗口时 Wine 可能不再发 raise；延迟补一次，并让随后到达的
      // fullscreen 事件有机会取消它。
      this.scheduleManagedRaise(toplevelId, 'new-window-focus');
    }

    // 1. 通过 API 获取标题栏高度 (必须在 loadContent 之后)
    //    getWindowDecorHeight 只返回标题栏文本区, 不含窗口边框。
    //    实际总装饰 = apiTitleBarH + decorBorderOffset
    try {
      const decorH = win.getWindowDecorHeight();
      hilog.info(DOMAIN, TAG, '[MW] onAfterLoadContent #%{public}d: getWindowDecorHeight = %{public}d', toplevelId, decorH);
      if (decorH >= 37 && decorH <= 112) {
        this.apiTitleBarH = decorH;
        const newBarH = decorH + this.decorBorderOffset;
        if (Math.abs(newBarH - this.measuredTitleBarH) >= 2) {
          this.measuredTitleBarH = newBarH;
          hilog.info(DOMAIN, TAG, '[MW] onAfterLoadContent #%{public}d: barH=%{public}d (API=%{public}d + offset=%{public}d)',
            toplevelId, newBarH, decorH, this.decorBorderOffset);
          const wd = this.lastResizeWineDim.get(toplevelId);
          if (wd) {
            this.applyEvent(toplevelId, 'resize', JSON.stringify({ w: wd[0], h: wd[1] }));
          }
        }
      }
    } catch (e) {
      hilog.warn(DOMAIN, TAG, '[MW] onAfterLoadContent #%{public}d: getWindowDecorHeight failed', toplevelId);
    }

    // 2.  方形窗口 (必须在 loadContent 之后, API 23 需要 FLOATING 模式)
    try {
      win.setWindowCornerRadius(0);
      hilog.info(DOMAIN, TAG, '[MW] onAfterLoadContent #%{public}d: setWindowCornerRadius(0) done', toplevelId);
    } catch (e) {
      hilog.error(DOMAIN, TAG, '[MW] onAfterLoadContent #%{public}d: setWindowCornerRadius failed: %{public}s',
        toplevelId, JSON.stringify(e));
    }

    // 2. loadContent 会把窗口标题重置为默认值, 需重新应用 Wine 标题
    const title = this.lastTitles.get(toplevelId);
    if (title) {
      hilog.info(DOMAIN, TAG, '[MW] onAfterLoadContent #%{public}d: reapply title "%{public}s"', toplevelId, title);
      win.setWindowTitle(title);
    }
  }

  // -- 兼容旧接口 --
  reapplyTitle(toplevelId: number): void {
    this.onAfterLoadContent(toplevelId);
  }

  // -- WineWindowAbility 在 onWindowStageCreate 中调用: 应用积压的事件 --
  flushPendingEvents(toplevelId: number): void {
    // popup 积压事件 (windowStage 就绪后补建)
    const stage = this.getWindowStage(toplevelId);
    const win = this.getWindow(toplevelId);
    if (stage && win) {
      PopupWindowManager.getInstance().flushPending(toplevelId, stage, win, this.getEffectiveScale());
      ModalWindowManager.getInstance().flushForOwner(toplevelId, stage, win, this.getEffectiveScale());
    }

    const pending = this.pendingEvents.get(toplevelId);
    if (!pending || pending.length === 0) {
      hilog.info(DOMAIN, TAG, '[MW] flushPendingEvents #%{public}d: none', toplevelId);
      return;
    }
    this.pendingEvents.delete(toplevelId);

    hilog.info(DOMAIN, TAG, '[MW]  flushPendingEvents #%{public}d: %{public}d items', toplevelId, pending.length);
    for (let i = 0; i < pending.length; i++) {
      hilog.info(DOMAIN, TAG, '[MW]   flush[%{public}d/%{public}d] event=%{public}s data=%{public}s',
        i + 1, pending.length, pending[i][0], pending[i][1]);
      this.applyEvent(toplevelId, pending[i][0], pending[i][1]);
    }
    hilog.info(DOMAIN, TAG, '[MW] flushPendingEvents #%{public}d done', toplevelId);
  }

  // -- toplevel 事件分发 --
  private onToplevelEvent(id: number, event: string, data: string): void {
    hilog.info(DOMAIN, TAG, '[MW] onToplevelEvent #%{public}d event=%{public}s hasWindow=%{public}s',
      id, event, this.windows.has(id) ? 'yes' : 'no');

    // PC 融合模式 modal: 不建 Ability。桌面模式由 compositor 组员化+输入拦截处理。
    if (!this.isDesktopMode && (event === 'modal' || this.modalToplevelIds.has(id))) {
      if (event === 'modal') {
        try {
          const obj: Record<string, number> = JSON.parse(data) as Record<string, number>;
          if (obj.modal === 1) {
            this.modalToplevelIds.add(id);
            this.pendingStarts.delete(id);
            const ownerId = Number(obj.owner ?? 0);
            ModalWindowManager.getInstance().show(id, obj,
              this.windowStages.get(ownerId) ?? null,
              this.getWindow(ownerId) ?? null,
              this.getEffectiveScale());
          } else {
            this.modalToplevelIds.delete(id);
            ModalWindowManager.getInstance().hide(id);
          }
        } catch (err) {
          hilog.error(DOMAIN, TAG, '[MW] modal event bad data #%{public}d: %{public}s',
            id, JSON.stringify(err));
        }
        return;
      }
      if (event === 'resize') {
        ModalWindowManager.getInstance().onResize(id, data, this.getEffectiveScale());
        return;
      }
      if (event === 'destroyed') {
        this.modalToplevelIds.delete(id);
        ModalWindowManager.getInstance().hide(id);
        return;
      }
      if (event === 'created') {
        hilog.info(DOMAIN, TAG, '[MW] skip Ability for modal toplevel #%{public}d', id);
        return;
      }
      return;
    }

    switch (event) {
      case 'created':
        try {
          const initial = JSON.parse(data) as Record<string, string | number>;
          if (this.sessionAssociationCallback &&
            (this.presentationMode === WinePresentationMode.MANAGED_WINDOWS ||
              this.isDesktopMode)) {
            this.sessionAssociationCallback(initial.sessionId as string ?? '', initial.clientPid as number ?? 0, id);
          }
        } catch (_) {
        }
        if (this.isDesktopMode) {
          hilog.info(DOMAIN, TAG, '[MW] Desktop mode: toplevel #%{public}d created, skipping Ability', id);
        } else {
          hilog.info(DOMAIN, TAG, '[MW] managed windows -> startWineWindowAbility(#%{public}d)', id);
          this.startWineWindowAbility(id, data);
        }
        break;
      case 'desktop_root':
        if (this.presentationMode === WinePresentationMode.DESKTOP) {
          hilog.info(DOMAIN, TAG, '[MW] desktop_root #%{public}d -> start DesktopAbility', id);
          if (this.sessionAssociationCallback) this.sessionAssociationCallback(DESKTOP_SESSION_ID, 0, id);
          this.desktopRootId = id;
          // 浮窗模式: Index 承载桌面 (desktopActive 驱动 @StorageProp)
          AppStorage.setOrCreate('desktopActive', true);
          this.startDesktopAbility(id);
        } else {
          // A stale explorer root may arrive while changing from an older PC
          // desktop session. Managed-window hosts expose only actual program
          // toplevels and must never promote that root to an ArkUI Ability.
          hilog.warn(DOMAIN, TAG, '[MW] suppress desktop_root #%{public}d in managed-window mode', id);
        }
        break;
      case 'destroyed':
        hilog.info(DOMAIN, TAG, '[MW] -> onToplevelDestroyed(#%{public}d)', id);
        this.onToplevelDestroyed(id);
        break;
      case 'fullscreen':
        this.fullscreenWindows.add(id);
        this.pendingRaises.delete(id);
        this.cancelDeferredRaise(id);
        hilog.info(DOMAIN, TAG, '[MW] fullscreen #%{public}d: cancel managed raise', id);
        // 桌面模式: 全屏由 C++ 合成器直接处理 (缩放绘制 + 输入映射), ArkTS 无需动作
        if (this.isDesktopMode && id !== this.desktopRootId) return;
        // PC 模式: 游戏 xdg fullscreen → OHOS 沉浸式全屏 (盖住 dock/任务栏)。
        // 游戏画面由 popup (全屏拉伸) 承载, 主窗口最大化保证无系统 UI 露出。
        if (this.windows.has(id)) {
          this.applyEvent(id, event, data);
        } else {
          let q = this.pendingEvents.get(id);
          if (!q) { q = []; this.pendingEvents.set(id, q); }
          q.push([event, data]);
        }
        break;
      case 'unfullscreen':
        this.fullscreenWindows.delete(id);
        hilog.info(DOMAIN, TAG, '[MW] unfullscreen #%{public}d', id);
        if (this.isDesktopMode && id !== this.desktopRootId) return;
        if (this.windows.has(id)) {
          this.applyEvent(id, event, data);
        } else {
          let q = this.pendingEvents.get(id);
          if (!q) { q = []; this.pendingEvents.set(id, q); }
          q.push([event, data]);
        }
        break;
      case 'raise':
        if (!this.isDesktopMode && !this.fullscreenWindows.has(id)) {
          if (this.loadedWindows.has(id)) {
            this.scheduleManagedRaise(id, 'wine-z-order');
          } else {
            this.pendingRaises.add(id);
            hilog.info(DOMAIN, TAG, '[MW] raise #%{public}d before loadContent -> queued', id);
          }
        }
        break;
      case 'surface':
        // Desktop 模式: 非 root toplevel 事件跳过
        if (this.isDesktopMode && id !== this.desktopRootId) return;
        // C++ 回报 XComponent surface 的物理像素尺寸, 用于动态校准标题栏高度
        try {
          const obj: Record<string, string | number> = JSON.parse(data) as Record<string, string | number>;
          const sw = obj.w as number;
          const sh = obj.h as number;
          if (sw && sh && this.displayScale > 0) {
            // 总是保存最新的 surface 物理尺寸
            this.surfaceSizes.set(id, [sw, sh]);
            hilog.info(DOMAIN, TAG, '[MW] surface #%{public}d: physical=%{public}dx%{public}d scale=%{public}s',
              id, sw, sh, this.displayScale.toFixed(2));

            //  动态校准 decorBorderOffset:
            //    totalBarH = lastResizeTargetH - surfacePhysicalH/scale
            //    decorBorderOffset = totalBarH - apiTitleBarH
            const lastH = this.lastResizeTargetH.get(id);
            let calibrated = false;
            if (lastH) {
              const contentVPH = Math.round(sh / this.displayScale);
              const totalBarH = lastH - contentVPH;
              if (totalBarH > 0 && totalBarH < 200) {
                // 用 API 值计算边框偏移量
                const newOffset = totalBarH - this.apiTitleBarH;
                const oldOffset = this.decorBorderOffset;
                const oldBarH = this.measuredTitleBarH;
                // 前两轮直接用, 之后 EMA 平滑
                this.barCalibrationCount++;
                if (this.barCalibrationCount <= 2) {
                  this.decorBorderOffset = newOffset;
                } else {
                  this.decorBorderOffset = Math.round(oldOffset * 0.7 + newOffset * 0.3);
                }
                this.measuredTitleBarH = this.apiTitleBarH + this.decorBorderOffset;
                calibrated = (Math.abs(this.measuredTitleBarH - oldBarH) >= 1);
                hilog.info(DOMAIN, TAG, '[MW] surface #%{public}d: calibrate round#%{public}d totalBar=%{public}d offset=%{public}d->%{public}d barH=%{public}d (lastH=%{public}d contentVPH=%{public}d API=%{public}d)',
                  id, this.barCalibrationCount, totalBarH, oldOffset, this.decorBorderOffset,
                  this.measuredTitleBarH, lastH, contentVPH, this.apiTitleBarH);
              }
            }
            // 如果校准后 titleBarH 变了, 用保存的 wine 尺寸重新 resize
            if (calibrated && this.windows.has(id)) {
              const wd = this.lastResizeWineDim.get(id);
              if (wd) {
                const newData = JSON.stringify({ w: wd[0], h: wd[1] });
                hilog.info(DOMAIN, TAG, '[MW] surface #%{public}d: re-resize with calibrated bar=%{public}d', id, this.measuredTitleBarH);
                this.applyEvent(id, 'resize', newData);
              }
            }
          }
        } catch (_) { /* ignore parse error */ }
        break;
      case 'move_start':
        this.moveGrabPending.add(id);
        hilog.info(DOMAIN, TAG, '[MW] move_grab_start #%{public}d → pending startMoving()', id);
        break;
      case 'move_end':
        this.moveGrabPending.delete(id);
        hilog.info(DOMAIN, TAG, '[MW] move_grab_end #%{public}d', id);
        break;
      case 'title':
      case 'resize':
        if (this.isDesktopMode && id !== this.desktopRootId) return;
        if (ArgbWindowManager.getInstance().has(id)) {
          // ARGB 异型窗口: resize 调整子窗口尺寸; title 不需要 (子窗口无标题栏)。
          // 注意不能进 pendingEvents 队列 — ARGB 窗口永远不会注册 ability, 永不 flush
          if (event === 'resize') {
            ArgbWindowManager.getInstance().onResize(id, data, this.getEffectiveScale());
          }
          break;
        }
        if (this.windows.has(id)) {
          hilog.info(DOMAIN, TAG, '[MW] -> applyEvent directly #%{public}d %{public}s', id, event);
          this.applyEvent(id, event, data);
        } else {
          hilog.info(DOMAIN, TAG, '[MW] -> queue pending #%{public}d %{public}s (window not ready)', id, event);
          let q = this.pendingEvents.get(id);
          if (!q) { q = []; this.pendingEvents.set(id, q); }
          q.push([event, data]);
          hilog.info(DOMAIN, TAG, '[MW]   pending queue size now: %{public}d', q.length);
        }
        break;
      case 'minimized':
      case 'restored':
      case 'maximized':
      case 'unmaximized':
      case 'limits':
      case 'argb':
        if (this.isDesktopMode && id !== this.desktopRootId) return;
        // ARGB 异型窗口: 这些事件无意义且会泄漏进 pendingEvents (永不注册 ability)
        if (ArgbWindowManager.getInstance().has(id)) break;
        if (this.windows.has(id)) {
          this.applyEvent(id, event, data);
        } else {
          let q = this.pendingEvents.get(id);
          if (!q) { q = []; this.pendingEvents.set(id, q); }
          q.push([event, data]);
        }
        break;
      case 'argb_created':
      case 'argb_move':
      case 'mask_dirty':
        // ARGB 异型窗口 (子窗口 + setWindowMask 路线, desktop 模式不涉及)
        if (this.isDesktopMode) return;
        if (!this.mainWindowStage) return;
        {
          const argb = ArgbWindowManager.getInstance();
          const s = this.getEffectiveScale();
          if (event === 'argb_created') {
            argb.onCreated(id, data, this.mainWindowStage!, s);
          } else if (event === 'argb_move') {
            argb.onMove(id, data, s);
          } else {
            argb.onMaskDirty(id, s);
          }
        }
        break;
      case 'popup_show':
      case 'popup_move':
      case 'popup_resize':
      case 'popup_hide':
        // PC 模式 popup 子窗口 (desktop 模式 C++ 不会发这些事件)
        // 父窗口可能是普通 ability 窗口, 也可能是 ARGB 异型子窗口 (回退)
        if (this.isDesktopMode) return;
        // 父窗口 Wine 尺寸传给 PopupWindowManager: 全屏游戏把渲染 surface 放
        // popup (popup 尺寸==模拟分辨率==父窗口尺寸), 需判定并拉伸到宿主屏幕;
        // 普通菜单 popup 不受影响。
        PopupWindowManager.getInstance().onPopupEvent(
          id, event, data,
          this.getWindowStage(id) ?? this.mainWindowStage,
          this.getWindow(id) ?? ArgbWindowManager.getInstance().getWindow(id),
          this.getEffectiveScale(),
          this.lastResizeWineDim.get(id) ?? null);
        break;
      default:
        hilog.warn(DOMAIN, TAG, '[MW] unknown event #%{public}d %{public}s', id, event);
        break;
    }
  }

  /**
   * PC 沉浸全屏: 铺满屏幕并禁用顶/底边 hover 标题栏、关闭条和 Dock。
   * 默认无参 maximize() 是 ENTER_IMMERSIVE, 会弹出关闭条抢走边缘鼠标。
   */
  enterImmersiveFullscreen(win: window.Window, logId: number = 0): void {
    const label = logId > 0 ? '#' + logId.toString() : 'desktop';
    try {
      hilog.info(DOMAIN, TAG, '[MW] fullscreen %{public}s → DISABLE_TITLE_AND_DOCK_HOVER', label);
      win.maximize(window.MaximizePresentation.ENTER_IMMERSIVE_DISABLE_TITLE_AND_DOCK_HOVER).then(() => {
        hilog.info(DOMAIN, TAG, '[MW] fullscreen %{public}s OK (DISABLE_HOVER)', label);
      }).catch((err: Error) => {
        hilog.warn(DOMAIN, TAG, '[MW] fullscreen %{public}s DISABLE_HOVER failed, fallback: %{public}s',
          label, JSON.stringify(err));
        win.maximize().then(() => {
          hilog.info(DOMAIN, TAG, '[MW] fullscreen %{public}s OK (ENTER_IMMERSIVE fallback)', label);
        }).catch((fallbackErr: Error) => {
          hilog.warn(DOMAIN, TAG, '[MW] fullscreen %{public}s fallback failed: %{public}s',
            label, JSON.stringify(fallbackErr));
        });
      });
    } catch (err) {
      hilog.warn(DOMAIN, TAG, '[MW] fullscreen %{public}s exception: %{public}s',
        label, JSON.stringify(err));
    }
  }

  /**
   * PC 退出沉浸全屏。手机/平板只需藏/恢复系统栏；PC 必须先 EXIT_IMMERSIVE
   * 才能让标题栏和关闭/最小化回来，否则 recover() 会停在无标题的沉浸态。
   */
  async exitImmersiveFullscreen(win: window.Window, recoverWindowed: boolean = false,
    logId: number = 0): Promise<void> {
    const label = logId > 0 ? '#' + logId.toString() : 'desktop';
    try {
      hilog.info(DOMAIN, TAG, '[MW] exit immersive %{public}s → EXIT_IMMERSIVE', label);
      await win.maximize(window.MaximizePresentation.EXIT_IMMERSIVE);
      hilog.info(DOMAIN, TAG, '[MW] exit immersive %{public}s OK', label);
    } catch (err) {
      hilog.warn(DOMAIN, TAG, '[MW] exit immersive %{public}s failed: %{public}s',
        label, JSON.stringify(err));
    }
    if (recoverWindowed) {
      try {
        await win.recover();
        hilog.info(DOMAIN, TAG, '[MW] recover after exit immersive %{public}s OK', label);
      } catch (err) {
        hilog.warn(DOMAIN, TAG, '[MW] recover after exit immersive %{public}s failed: %{public}s',
          label, JSON.stringify(err));
      }
    }
  }

  // -- apply 单个事件到 window.Window --
  private applyEvent(toplevelId: number, event: string, data: string): void {
    const win = this.windows.get(toplevelId);
    if (!win) {
      hilog.warn(DOMAIN, TAG, '[MW] applyEvent #%{public}d: window NOT FOUND (unregistered?)', toplevelId);
      return;
    }

    switch (event) {
      case 'title':
        try {
          const obj: Record<string, string | number> = JSON.parse(data) as Record<string, string | number>;
          if (obj.title) {
            const titleStr = obj.title as string;
            this.lastTitles.set(toplevelId, titleStr);
            hilog.info(DOMAIN, TAG, '[MW] setWindowTitle #%{public}d -> "%{public}s"', toplevelId, titleStr);
            win.setWindowTitle(titleStr);
          }
        } catch (e) {
          hilog.warn(DOMAIN, TAG, '[MW] applyEvent title parse error: %{public}s', JSON.stringify(e));
        }
        break;
      case 'resize':
        try {
          const obj: Record<string, string | number> = JSON.parse(data) as Record<string, string | number>;
          const w = obj.w as number;
          const h = obj.h as number;
          if (w && h) {
            const barH = 0; //  标题栏已通过 setWindowDecorVisible(false) 隐藏
            const s = this.displayScale * this.scaleBoost;  // effective scale = 2.0
            const winW = Math.round(w * s);
            const winH = Math.round(h * s + barH);
            //  保存目标尺寸, 供 surface 事件校准 titleBarH + 重新 resize
            this.lastResizeTargetH.set(toplevelId, winH);
            this.lastResizeWineDim.set(toplevelId, [w, h]);
            hilog.info(DOMAIN, TAG, '[MW-RESIZE] tl=%{public}d wine=%{public}dx%{public}d window=%{public}dx%{public}d scale=%{public}s',
              toplevelId, w, h, winW, winH, s.toFixed(1));
            win.resize(winW, winH);
          }
        } catch (e) {
          hilog.warn(DOMAIN, TAG, '[MW] applyEvent resize parse error: %{public}s', JSON.stringify(e));
        }
        break;
      case 'limits':
        try {
          const obj: Record<string, string | number> = JSON.parse(data) as Record<string, string | number>;
          const minW = obj.minW as number;
          const minH = obj.minH as number;
          const maxW = obj.maxW as number;
          const maxH = obj.maxH as number;
          const s = this.displayScale * this.scaleBoost;
          const limits: window.WindowLimits = {};
          if (minW > 0) limits.minWidth = Math.round(minW * s);
          if (minH > 0) limits.minHeight = Math.round(minH * s);
          if (maxW > 0) limits.maxWidth = Math.round(maxW * s);
          if (maxH > 0) limits.maxHeight = Math.round(maxH * s);
          const maxInFlight = this.maximizeInFlight.has(toplevelId);
          hilog.info(DOMAIN, TAG, '[MW-LIMITS] tl=%{public}d wine=[%{public}d,%{public}d-%{public}d,%{public}d] → ohos=[%{public}d,%{public}d-%{public}d,%{public}d] maxInFlight=%{public}s',
            toplevelId, minW, minH, maxW, maxH,
            limits.minWidth ?? 0, limits.minHeight ?? 0,
            limits.maxWidth ?? 0, limits.maxHeight ?? 0,
            maxInFlight ? 'yes' : 'no');
          win.setWindowLimits(limits).then(() => {
            hilog.info(DOMAIN, TAG, '[MW-LIMITS] setWindowLimits OK tl=%{public}d', toplevelId);
          }).catch((e: Error) => {
            hilog.warn(DOMAIN, TAG, '[MW] setWindowLimits #%{public}d failed: %{public}s',
              toplevelId, JSON.stringify(e));
          });
        } catch (e) {
          hilog.warn(DOMAIN, TAG, '[MW] applyEvent limits parse error: %{public}s', JSON.stringify(e));
        }
        break;
      case 'minimized':
        hilog.info(DOMAIN, TAG, '[MW] minimizeWindow #%{public}d', toplevelId);
        win.minimize().catch((e: Error) => {
          hilog.warn(DOMAIN, TAG, '[MW] minimize #%{public}d failed: %{public}s', toplevelId, JSON.stringify(e));
        });
        break;
      case 'restored':
        hilog.info(DOMAIN, TAG, '[MW] showWindow #%{public}d', toplevelId);
        win.showWindow().catch((e: Error) => {
          hilog.warn(DOMAIN, TAG, '[MW] showWindow #%{public}d failed: %{public}s', toplevelId, JSON.stringify(e));
        });
        break;
      case 'maximized':
        {
          const alreadyInFlight = this.maximizeInFlight.has(toplevelId);
          const preDim = this.lastResizeWineDim.get(toplevelId);
          hilog.info(DOMAIN, TAG, '[MW] maximize #%{public}d alreadyInFlight=%{public}s preMax=%{public}s',
            toplevelId, alreadyInFlight ? 'yes' : 'no', JSON.stringify(preDim));
          this.maximizeInFlight.add(toplevelId);
          if (preDim) {
            this.lastPreMaximizeDim.set(toplevelId, [preDim[0], preDim[1]]);
          }
          hilog.info(DOMAIN, TAG, '[MW] → win.maximize() START #%{public}d', toplevelId);
          win.maximize().then(() => {
            hilog.info(DOMAIN, TAG, '[MW] win.maximize() OK #%{public}d', toplevelId);
          }).catch((e: Error) => {
            hilog.warn(DOMAIN, TAG, '[MW] maximize #%{public}d failed: %{public}s', toplevelId, JSON.stringify(e));
          });
        }
        break;
      case 'unmaximized':
        hilog.info(DOMAIN, TAG, '[MW] recover #%{public}d (unmaximize)', toplevelId);
        win.recover().catch((e: Error) => {
          hilog.warn(DOMAIN, TAG, '[MW] unmaximize #%{public}d failed: %{public}s', toplevelId, JSON.stringify(e));
        });
        break;
      case 'fullscreen':
        // PC 模式游戏 xdg fullscreen: OHOS 沉浸式全屏 (铺满屏幕、盖住 dock)。
        // ENTER_IMMERSIVE_DISABLE_TITLE_AND_DOCK_HOVER (API 14+): 进入沉浸
        // 全屏且鼠标悬停屏幕边缘热区时不唤出标题栏/关闭条/Dock — 默认
        // ENTER_IMMERSIVE 会 hover 弹出关闭条, 抢走游戏边缘鼠标事件。
        this.enterImmersiveFullscreen(win, toplevelId);
        break;
      case 'unfullscreen':
        // 退出沉浸式全屏, 恢复普通窗口; 尺寸由 C++ 侧 configure(preFs)
        // → resize 事件恢复 (tl_unset_fullscreen 用 preFs 尺寸)
        this.exitImmersiveFullscreen(win, false, toplevelId).catch((err: Error) => {
          hilog.warn(DOMAIN, TAG, '[MW] unfullscreen #%{public}d failed: %{public}s',
            toplevelId, JSON.stringify(err));
        });
        break;
      case 'argb':
        /*
         * layered/shaped (异型) 窗口标记, 仅记录状态 (后续子窗口方案使用)。
         * 已验证 2in1 主窗口无法做到透视:
         * - XComponent surface 无 alpha 通道 (EGL_ALPHA_SIZE=0, SET_FORMAT 无效)
         * - 窗口背景色 alpha 被系统钳制 ('#80FF0000' 显示为不透明红)
         * - setWindowMask 仅限子窗/全局悬浮窗 (主窗口 1300004)
         * 故不做窗口背景色设置 (无效操作)。
         */
        try {
          const obj: Record<string, number> = JSON.parse(data) as Record<string, number>;
          const isArgb = obj.argb === 1;
          this.lastArgb.set(toplevelId, isArgb);
          hilog.info(DOMAIN, TAG, '[MW] argb state #%{public}d = %{public}s (record only)',
            toplevelId, isArgb ? 'ARGB' : 'XRGB');
        } catch (e) {
          hilog.warn(DOMAIN, TAG, '[MW] applyEvent argb parse error: %{public}s', JSON.stringify(e));
        }
        break;
    }
  }

  // -- startAbility 启动 multiton 实例 --
  private startWineWindowAbility(id: number, data: string): void {
    if (this.modalToplevelIds.has(id)) {
      hilog.info(DOMAIN, TAG, '[MW] startWineWindowAbility #%{public}d: modal, skip Ability', id);
      return;
    }
    if (this.pendingStarts.has(id)) {
      hilog.warn(DOMAIN, TAG, '[MW] startWineWindowAbility #%{public}d: already in flight, skip', id);
      return;
    }
    this.pendingStarts.add(id);
    hilog.info(DOMAIN, TAG, '[MW] startWineWindowAbility #%{public}d: pendingStarts now %{public}d',
      id, this.pendingStarts.size);

    // 解析初始参数
    let title = '';
    let initW = 0;
    let initH = 0;
    try {
      const obj: Record<string, string | number> = JSON.parse(data) as Record<string, string | number>;
      if (obj.title) title = obj.title as string;
      // created 事件的 Wine 内容尺寸 (w/h): 传入 Ability 设置窗口初始尺寸,
      // 避免 OHOS 默认窗口尺寸 → onSurfaceChanged 反馈 → 窗口模式游戏
      // 接受 configure resize → 内容不缩放 (画面缩在左上角 + 反馈环)
      initW = Number(obj.w) || 0;
      initH = Number(obj.h) || 0;
    } catch (_) { /* use defaults */ }

    hilog.info(DOMAIN, TAG, '[MW]  startAbility(WineWindowAbility) #%{public}d title="%{public}s"',
      id, title);

    if (!this.uiAbilityContext) {
      hilog.error(DOMAIN, TAG, '[MW] FATAL: no uiAbilityContext, cannot start ability');
      this.pendingStarts.delete(id);
      return;
    }

    const want: Want = {
      bundleName: 'com.vintage.pomelopro',
      abilityName: 'WineWindowAbility',
      parameters: { toplevelId: id, title: title, w: initW, h: initH }
    };

    hilog.info(DOMAIN, TAG, '[MW] startAbility want: bundle=%{public}s ability=%{public}s params=%{public}s',
      want.bundleName ?? '', want.abilityName ?? '', JSON.stringify(want.parameters));

    this.uiAbilityContext.startAbility(want).then(() => {
      hilog.info(DOMAIN, TAG, '[MW] OK startAbility #%{public}d resolved (AMS accepted)', id);
      this.pendingStarts.delete(id);
    }).catch((err: Error) => {
      hilog.error(DOMAIN, TAG, '[MW] ERR startAbility #%{public}d REJECTED: %{public}s',
        id, JSON.stringify(err));
      this.pendingStarts.delete(id);
      // 通知 Wine 窗口创建失败
      hilog.info(DOMAIN, TAG, '[MW] -> sendToplevelClose #%{public}d (fail recovery)', id);
      testNapi.sendToplevelClose(id);
    });
  }

  // -- Wine toplevel destroyed -> terminateSelf --
  private onToplevelDestroyed(id: number): void {
    hilog.info(DOMAIN, TAG, '[MW] onToplevelDestroyed #%{public}d', id);
    this.destroyed.add(id);

    // Desktop 模式: root 销毁 → 关闭 DesktopAbility, 回到主页面。
    if (this.isDesktopMode && id === this.desktopRootId) {
      hilog.info(DOMAIN, TAG, '[MW] desktop root #%{public}d destroyed, closing DesktopAbility', id);
      this.desktopRootId = 0;
      // 浮窗模式: 桌面 root 销毁 → Index 桌面层隐藏 (desktopActive 驱动 @StorageProp)
      AppStorage.setOrCreate('desktopActive', false);
      if (this.desktopAbilityCtx) {
        this.desktopAbilityCtx.terminateSelf().catch((err: Error) => {
          hilog.warn(DOMAIN, TAG, '[MW] ERR terminateSelf DesktopAbility: %{public}s', JSON.stringify(err));
        });
        this.desktopAbilityCtx = null;
      }
      this.desktopAbilityRunning = false;
      this.desktopAbilityForeground = false;
      this.desktopAbilityRootId = 0;
      return;
    }
    // Desktop 模式: 非 root toplevel 销毁, C++ 自动停止合成
    if (this.isDesktopMode) {
      this.unregisterAbility(id);
      return;
    }

    // PC: ARGB 异型子窗口销毁 (级联销毁其 popup)
    if (ArgbWindowManager.getInstance().has(id)) {
      hilog.info(DOMAIN, TAG, '[MW] onToplevelDestroyed #%{public}d: destroy ARGB subwindow', id);
      ArgbWindowManager.getInstance().destroy(id);
      testNapi.destroyToplevel(id);
      this.unregisterAbility(id);
      return;
    }

    // Managed-window compatibility path
    if (!this.abilityContexts.has(id) && !this.pendingStarts.has(id)) {
      hilog.info(DOMAIN, TAG, '[MW] onToplevelDestroyed #%{public}d: already cleaned up by ability, skip', id);
      return;
    }
    hilog.info(DOMAIN, TAG, '[MW] -> destroyToplevel #%{public}d', id);
    testNapi.destroyToplevel(id);
    const ctx = this.abilityContexts.get(id);
    if (ctx) {
      hilog.info(DOMAIN, TAG, '[MW] -> terminateSelf #%{public}d', id);
      ctx.terminateSelf().catch((err: Error) => {
        hilog.warn(DOMAIN, TAG, '[MW] ERR terminateSelf #%{public}d failed: %{public}s',
          id, JSON.stringify(err));
      });
    }
    this.unregisterAbility(id);
    hilog.info(DOMAIN, TAG, '[MW] onToplevelDestroyed #%{public}d done', id);
  }

  // Desktop 模式: 打开 DesktopAbility
  private startDesktopAbility(rootId: number): void {
    // 浮窗模式 (平板/手机): 桌面由 Index 页面承载 (DesktopLayer floating 切换),
    // 不启动 DesktopAbility — 放大/缩小都在当前画面 (Index) 内完成, 不切窗口。
    if (AppSettingsStore.getInstance().getGlobalSettings().desktopWindowMode === DesktopWindowMode.FLOATING) {
      hilog.info(DOMAIN, TAG, '[MW] floating mode: desktop hosted by Index, skip DesktopAbility #%{public}d', rootId);
      return;
    }
    // PC/2in1 融合模式 (FUSION, isDesktopMode=false): 每程序独立窗口, 不启动
    // 桌面合成器 → suppress desktop root Ability。
    // PC/2in1 虚拟桌面模式 (VIRTUAL, isDesktopMode=true): 用 DesktopAbility
    // 独立浮动窗口承载 Wine 桌面 (DesktopAbility 内按 PC 分支浮动 + 保留标题栏)。
    if (AppSettingsStore.getInstance().usesManagedWineWindows() && !this.isDesktopMode) {
      hilog.warn(DOMAIN, TAG, '[MW] suppress DesktopAbility #%{public}d on managed-window host', rootId);
      return;
    }
    // 同一个 explorer root 可能同时收到 created/promote 两次事件。只让
    // 第一个事件唤起 Ability，避免 PC 上每次启动都弹出重复桌面窗口。
    if (this.desktopAbilityRunning && this.desktopAbilityRootId === rootId) {
      hilog.info(DOMAIN, TAG, '[MW] DesktopAbility already running for root #%{public}d, skip', rootId);
      return;
    }
    this.desktopAbilityRunning = true;
    this.desktopAbilityForeground = false;
    this.desktopAbilityRootId = rootId;
    // 浮窗模式: 桌面窗口打开 → Index 小浮窗隐藏 (AppStorage 驱动 @StorageProp)
    AppStorage.setOrCreate('desktopWindowActive', true);
    try {
      const want: Want = {
        bundleName: 'com.vintage.pomelopro',
        abilityName: 'DesktopAbility',
        parameters: {
          toplevelId: rootId,
          // Capture the setting at session creation. DesktopAbility applies it
          // before loadContent so Wine never starts against a stale portrait
          // output rectangle.
          desktopOrientation: AppSettingsStore.getInstance().getGlobalSettings().desktopOrientation
            ?? ScreenOrientationSetting.SENSOR_LANDSCAPE
        }
      };
      this.uiAbilityContext!.startAbility(want).catch((err: Error) => {
        hilog.error(DOMAIN, TAG, '[MW] startAbility(DesktopAbility) failed: %{public}s',
          JSON.stringify(err));
        if (this.desktopAbilityRootId === rootId) this.desktopAbilityRunning = false;
      });
      hilog.info(DOMAIN, TAG, '[MW] startAbility(DesktopAbility) #%{public}d OK', rootId);
    } catch (e) {
      hilog.error(DOMAIN, TAG, '[MW] startDesktopAbility error: %{public}s', JSON.stringify(e));
      if (this.desktopAbilityRootId === rootId) this.desktopAbilityRunning = false;
    }
  }

  /**
   * 浮窗模式: 重新拉起桌面窗口 (Index 小浮窗点击放大)。平板/手机浮窗模式下
   * 用户缩小/关窗后 root 保留 (DesktopAbility.onDestroy 不杀 root), 引擎会话
   * 后台继续; 点击小浮窗经此重新 startAbility(DesktopAbility) 全屏呈现。
   * 复用 startDesktopAbility 的防重 (desktopAbilityRunning) — 窗口已打开时跳过。
   */
  openDesktopWindow(): void {
    const rootId = this.desktopRootId;
    if (rootId <= 0) {
      hilog.warn(DOMAIN, TAG, '[MW] openDesktopWindow: no desktop root, skip');
      return;
    }
    this.startDesktopAbility(rootId);
  }

  /**
   * 浮窗模式: 收回桌面窗口 (控制栏 home 按钮)。关闭 DesktopAbility 窗口但
   * 不杀 root (DesktopAbility.onDestroy 保留引擎会话), Index 小浮窗重现。
   */
  closeDesktopWindow(): void {
    if (this.desktopAbilityCtx) {
      this.desktopAbilityCtx.terminateSelf().catch((err: Error) => {
        hilog.warn(DOMAIN, TAG, '[MW] closeDesktopWindow terminateSelf failed: %{public}s',
          JSON.stringify(err));
      });
    } else {
      hilog.warn(DOMAIN, TAG, '[MW] closeDesktopWindow: no desktop ctx');
    }
  }

  /** PC 虚拟桌面 hover 菜单: 最小化桌面窗口 (经 DesktopAbility ctx 取主窗口)。 */
  async minimizeDesktopWindow(): Promise<void> {
    if (!this.desktopAbilityCtx) {
      hilog.warn(DOMAIN, TAG, '[MW] minimizeDesktopWindow: no desktop ctx');
      return;
    }
    try {
      const win = await window.getLastWindow(this.desktopAbilityCtx);
      if (win) {
        await win.minimize();
        hilog.info(DOMAIN, TAG, '[MW] minimizeDesktopWindow OK');
      }
    } catch (e) {
      hilog.warn(DOMAIN, TAG, '[MW] minimizeDesktopWindow failed: %{public}s', JSON.stringify(e));
    }
  }

  /** PC 虚拟桌面: 先 EXIT_IMMERSIVE 再 recover，标题栏才能回来。 */
  async recoverDesktopWindow(): Promise<void> {
    if (!this.desktopAbilityCtx) {
      hilog.warn(DOMAIN, TAG, '[MW] recoverDesktopWindow: no desktop ctx');
      return;
    }
    try {
      const win = await window.getLastWindow(this.desktopAbilityCtx);
      if (win) {
        await this.exitImmersiveFullscreen(win, true, 0);
        AppStorage.setOrCreate('vdImmersive', false);
        hilog.info(DOMAIN, TAG, '[MW] recoverDesktopWindow OK');
      }
    } catch (e) {
      hilog.warn(DOMAIN, TAG, '[MW] recoverDesktopWindow failed: %{public}s', JSON.stringify(e));
    }
  }

  /** PC 虚拟桌面 hover 菜单: 全屏 → 90% 浮窗 (进入可缩放窗口模式)。 */
  async restoreDesktopWindow(): Promise<void> {
    if (!this.desktopAbilityCtx) {
      hilog.warn(DOMAIN, TAG, '[MW] restoreDesktopWindow: no desktop ctx');
      return;
    }
    try {
      const win = await window.getLastWindow(this.desktopAbilityCtx);
      if (win) {
        await this.exitImmersiveFullscreen(win, false, 0);
        const d = display.getDefaultDisplaySync();
        const w = Math.max(640, Math.round(d.width * 0.9));
        const h = Math.max(480, Math.round(d.height * 0.9));
        await win.resize(w, h);
        await win.moveWindowTo(Math.round((d.width - w) / 2), Math.round((d.height - h) / 2));
        AppStorage.setOrCreate('vdImmersive', false);
        hilog.info(DOMAIN, TAG, '[MW] restoreDesktopWindow OK %{public}dx%{public}d', w, h);
      }
    } catch (e) {
      hilog.warn(DOMAIN, TAG, '[MW] restoreDesktopWindow failed: %{public}s', JSON.stringify(e));
    }
  }

  isDesktopAbilityRunning(rootId: number): boolean {
    return this.desktopAbilityRunning && this.desktopAbilityRootId === rootId;
  }

  isDesktopAbilityForeground(rootId: number): boolean {
    return this.desktopAbilityForeground && this.desktopAbilityRootId === rootId;
  }

  markDesktopAbilityForeground(rootId: number): void {
    if (rootId <= 0) return;
    this.desktopAbilityRootId = rootId;
    this.desktopAbilityRunning = true;
    this.desktopAbilityForeground = true;
    hilog.info(DOMAIN, TAG, '[MW] DesktopAbility foreground for root #%{public}d', rootId);
    // 完整桌面窗口前台: 若 wine 已相对模式, 从悬浮禁止隐藏重算为允许隐藏
    this.applyPointerVisibility();
  }

  markDesktopAbilityBackground(rootId: number): void {
    if (this.desktopAbilityRootId !== rootId) return;
    this.desktopAbilityForeground = false;
    hilog.info(DOMAIN, TAG, '[MW] DesktopAbility background for root #%{public}d', rootId);
    // 回到悬浮/后台: 即使 wine 相对模式仍存活也禁止隐藏 → 恢复系统光标
    this.applyPointerVisibility();
  }

  markDesktopAbilityStopped(rootId: number): void {
    if (this.desktopAbilityRootId !== rootId) return;
    this.desktopAbilityRunning = false;
    this.desktopAbilityForeground = false;
    // 浮窗模式: 桌面窗口关闭/缩小 → Index 小浮窗重现 (AppStorage 驱动 @StorageProp)
    AppStorage.setOrCreate('desktopWindowActive', false);
    hilog.info(DOMAIN, TAG, '[MW] DesktopAbility stopped for root #%{public}d', rootId);
    this.applyPointerVisibility();
  }

  getDesktopRootId(): number { return this.desktopRootId; }

  /** 浮窗模式: 同步桌面启动器/悬浮窗可见性 (DesktopLayer.onSurfaceChanged 门控)。 */
  setDesktopLauncherVisible(visible: boolean): void {
    this.desktopLauncherVisible = visible;
    hilog.info(DOMAIN, TAG, '[MW] setDesktopLauncherVisible=%{public}s',
      visible ? 'true' : 'false');
    // Pad 悬浮↔完整桌面切换: 光标隐藏意图可能因形态翻转而改变
    this.applyPointerVisibility();
  }

  setFloatingLayoutAnimating(animating: boolean): void {
    this.floatingLayoutAnimating = animating;
    hilog.info(DOMAIN, TAG, '[MW] setFloatingLayoutAnimating=%{public}s',
      animating ? 'true' : 'false');
  }

  private shouldNotifyDesktopToplevelResize(forceToplevelResize: boolean): boolean {
    if (forceToplevelResize) return true;
    if (AppSettingsStore.getInstance().getGlobalSettings().desktopWindowMode !==
      DesktopWindowMode.FLOATING) {
      return true;
    }
    return false;
  }

  // 光标隐藏门禁判据 (fromToplevel = 触发相对模式的约束 surface 所属 toplevel):
  //   融合模式   → 允许 (每个 app 独立窗口, setPointerVisible 按"当前窗口"
  //                作用, 只有发起相对模式的应用窗口隐藏, 天然满足"其它窗口
  //                不隐藏") — 不拦。
  //   PC 虚拟桌面 → 对齐 WineHua desktopSeparateWindow + desktopWindowActive:
  //                DesktopAbility 前台才允许; 不看 Pad 浮窗标志
  //                (desktopLauncherVisible 默认 true, PC 从不走 Index 放大清位)。
  //   Pad/手机    → 悬浮窗形态禁止; 完整桌面形态仅非桌面 root 允许。
  //                悬浮 = launcherVisible; 完整 = !launcherVisible
  //   完整桌面下 fromToplevel == desktopRootId → 桌面 shell 自身藏
  //   光标 (启动瞬时) → 禁止; 子窗口 (真实游戏/程序) → 允许。
  private canHideCursor(fromToplevel: number): boolean {
    if (!this.isDesktopMode) return true;
    if (DeviceCapabilityPolicy.usesManagedWineWindows()) {
      if (!this.desktopAbilityForeground) return false;
    } else {
      if (this.desktopLauncherVisible) return false;
      if (this.desktopAbilityRunning && !this.desktopAbilityForeground) return false;
    }
    return fromToplevel !== this.desktopRootId;
  }

  // 统一重算并应用光标显隐: shouldHide = pointerLocked × canHideCursor()。
  private applyPointerVisibility(): void {
    const canHide = this.canHideCursor(this.pointerLockToplevel);
    const shouldHide = this.pointerLocked && canHide;
    hilog.info(DOMAIN, TAG,
      '[MW]PtrVis → %{public}s (locked=%{public}s canHide=%{public}s tl=%{public}d desktop=%{public}s pcManaged=%{public}s launcher=%{public}s fg=%{public}s root=%{public}d)',
      shouldHide ? 'HIDE' : 'SHOW',
      String(this.pointerLocked), String(canHide), this.pointerLockToplevel,
      String(this.isDesktopMode),
      String(DeviceCapabilityPolicy.usesManagedWineWindows()),
      String(this.desktopLauncherVisible),
      String(this.desktopAbilityForeground), this.desktopRootId);
    if (shouldHide === this.effectiveHidden) return;
    this.effectiveHidden = shouldHide;
    pointer.setPointerVisible(!shouldHide).catch((e: BusinessError) => {
      hilog.warn(DOMAIN, TAG, '[MW] setPointerVisible failed: %{public}s', JSON.stringify(e));
    });
  }

  registerDesktopAbility(ctx: common.UIAbilityContext): void {
    hilog.info(DOMAIN, TAG, '[MW] registerDesktopAbility');
    this.desktopAbilityCtx = ctx;
  }

  unregisterDesktopAbility(): void {
    hilog.info(DOMAIN, TAG, '[MW] unregisterDesktopAbility');
    this.desktopAbilityCtx = null;
  }
  /** 与 native 启动路径相同的 Wine 逻辑尺寸，用于重建 explorer desktop shell。 */
  getDesktopShellArgument(): string {
    try {
      const currentDisplay = display.getDefaultDisplaySync();
      const effectiveScale = this.displayScale * this.scaleBoost;
      const width = Math.max(1, Math.round(currentDisplay.width / effectiveScale));
      const height = Math.max(1, Math.round(currentDisplay.height / effectiveScale));
      return '/desktop=shell,' + width + 'x' + height;
    } catch (_) {
      return '/desktop=shell,1280x720';
    }
  }
}
