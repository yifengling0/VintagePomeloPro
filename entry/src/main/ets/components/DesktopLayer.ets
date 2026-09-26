import { hilog } from '../service/LogService';
import { common } from '@kit.AbilityKit';
import { display, window } from '@kit.ArkUI';
import testNapi from 'libentry.so';
import { WineWindowManager } from '../service/WineWindowManager';
import { InputDeviceMapper } from '../service/InputDeviceMapper';
import { EVDEV_KEY_ENTER, OH2EVDEV } from '../common/KeyMap';
import { mapMouseButton } from '../common/MouseMap';
import {
  DesktopWindowMode,
  GlobalSettings,
  InputProfile,
  createDefaultGlobalSettings,
  createDefaultInputProfile,
  resolveCornerSafeMarginPx
} from '../model/AppModels';
import { AppSettingsStore } from '../service/AppSettingsStore';
import { InputRouter } from '../service/InputRouter';
import { InputDispatcher } from '../service/InputDispatcher';
import { GamepadActivityListener, GamepadConnectionListener, GamepadManager } from '../service/GamepadManager';
import { InputProfileService } from '../service/InputProfileService';
import { InputOverlayWindowCoordinator } from '../service/InputOverlayWindowCoordinator';
import { InputEventGate } from '../service/InputEventGate';
import { WineEngineBlockingOverlay } from '../components/WineEngineBlockingOverlay';
import { FpsHud } from '../components/FpsHud';

const DOMAIN = 0x0000;

class DesktopController extends XComponentController {
  private surfaceId: string = '';
  private rendererRootId: number = 0;

  onSurfaceCreated(surfaceId: string): void {
    this.surfaceId = surfaceId;
    const rootId = WineWindowManager.getInstance().getDesktopRootId();
    hilog.info(DOMAIN, 'LIFE', '[XComp] onSurfaceCreated rootId=%{public}d surfaceId=%{public}s', rootId, surfaceId);
    this.bindDesktopRoot(rootId, true);
  }
  onSurfaceChanged(surfaceId: string, rect: SurfaceRect): void {
    const rootId = this.rendererRootId || WineWindowManager.getInstance().getDesktopRootId();
    hilog.info(DOMAIN, 'LIFE', '[XComp] onSurfaceChanged rootId=%{public}d %{public}dx%{public}d', rootId, rect.surfaceWidth, rect.surfaceHeight);
    if (rootId > 0) {
      try {
        testNapi.resizeRenderer(rootId, rect.surfaceWidth, rect.surfaceHeight);
        // 浮窗模式: 桌面分辨率由 prepareOutputForOrientation 设定, XComponent
        // 只做 letterbox 显示口。动画中/缩略态/全屏态都不要 syncOutputSize,
        // 否则 explorer 会再提交 desktop-shell。
        if (AppSettingsStore.getInstance().getGlobalSettings().desktopWindowMode === DesktopWindowMode.FLOATING) {
          hilog.info(DOMAIN, 'LIFE', '[XComp] floating: skip root resize, letterbox full desktop');
          return;
        }
        const s = WineWindowManager.getInstance().getEffectiveScale();
        const outW = Math.round(rect.surfaceWidth / s);
        const outH = Math.round(rect.surfaceHeight / s);
        // surface 是真实可见区: 用它同步 Wayland 输出尺寸, 保证桌面 1:1
        // 铺满且触摸坐标对齐 (窗口 rect 在安全区模式下比 surface 大)。
        WineWindowManager.getInstance().syncOutputSize(rect.surfaceWidth, rect.surfaceHeight);
        testNapi.notifyToplevelResize(rootId, outW, outH);
      } catch (_) {}
    }
  }
  onSurfaceDestroyed(_s: string): void {
    const rootId = this.rendererRootId;
    hilog.warn(DOMAIN, 'LIFE', '[XComp] onSurfaceDestroyed rootId=%{public}d', rootId);
    if (rootId > 0) {
      try { testNapi.destroyRenderer(rootId); } catch (_) {}
    }
    this.rendererRootId = 0;
    this.surfaceId = '';
  }

  switchDesktopRoot(rootId: number): boolean {
    return this.bindDesktopRoot(rootId || WineWindowManager.getInstance().getDesktopRootId(), false);
  }

  rebindRenderer(): boolean {
    const rootId = WineWindowManager.getInstance().getDesktopRootId();
    if (rootId <= 0 || this.surfaceId.length === 0 || this.rendererRootId !== rootId) return false;
    try {
      // 调试压测保留 ArkUI 提供的 NativeWindow。真实 Surface 变化由
      // onSurfaceCreated/onSurfaceDestroyed 走完整 renderer 生命周期。
      testNapi.refreshRenderer(rootId);
      InputOverlayWindowCoordinator.getInstance().markRendererRebound();
      return true;
    } catch (_) {
      return false;
    }
  }

  private bindDesktopRoot(rootId: number, force: boolean): boolean {
    if (rootId <= 0 || this.surfaceId.length === 0) return false;
    try {
      if (this.rendererRootId > 0 && (this.rendererRootId !== rootId || force)) {
        testNapi.destroyRenderer(this.rendererRootId);
      } else if (this.rendererRootId === rootId && !force) {
        // 已绑定同一桌面根：无需重建 renderer，仅同步输入目标。
        InputOverlayWindowCoordinator.getInstance().setActiveToplevel(rootId);
        InputOverlayWindowCoordinator.getInstance().markRendererRebound();
        return true;
      }
      testNapi.createRenderer(rootId, BigInt(this.surfaceId));
      this.rendererRootId = rootId;
      InputOverlayWindowCoordinator.getInstance().setActiveToplevel(rootId);
      InputOverlayWindowCoordinator.getInstance().markRendererRebound();
      return true;
    } catch (_) {
      return false;
    }
  }
}

@Component
export struct DesktopLayer {
  @StorageLink('desktopToplevelId') @Watch('onDesktopToplevelChanged') private desktopToplevelId: number = 0;
  @StorageLink('desktopWindowMode') @Watch('onDesktopWindowModeChanged') private desktopWindowMode: string = '';
  // overlay 关掉宿主 IME 后跨窗口把焦点要回桌面 XComponent, 蓝牙可打印键才继续走 KeyEvent。
  @StorageLink('wineDesktopKeyFocusToken') @Watch('onDesktopKeyFocusToken')
    private wineDesktopKeyFocusToken: number = 0;
  @State private cornerSafeMargin: number = 0;
  // 悬浮窗态 (Index 桌面层容器处于小窗缩略显示): 桌面 XComponent 所有输入
  // 一律旁路 — 点击放大/拖动由 Index 悬浮交互层统一处理, 本组件不转发
  // 输入给 Wine (悬浮态小窗缩略渲染 + 输入命中映射由 renderer letterbox
  // ComputeFitRect 保证, 全屏态恢复后正常)。
  // 必须 @Prop 单向同步: 用普通属性时, 悬浮↔全屏切换 (Index 容器尺寸变但
  // 本组件实例不重建) floating 停留在旧值 true, 全屏态输入被持续旁路 —
  // 表现为"桌面在更新但点不动"。
  @Prop floating: boolean = false;
  controller: DesktopController = new DesktopController();
  @State private settings: GlobalSettings = createDefaultGlobalSettings();
  @State private inputProfile: InputProfile = createDefaultInputProfile();
  private gamepadConnectionCallback: GamepadConnectionListener | null = null;
  private gamepadActivityCallback: GamepadActivityListener | null = null;
  // 触控状态机: IDLE → PENDING → (DRAGGING | 长按 | 点击)
  // 双指进入 SCROLLING 模式, 覆盖单指状态
  private touchPending: boolean = false;
  private isDragging: boolean = false;
  private touchStartTime: number = 0;
  private touchStartPx: number = 0;
  private touchStartPy: number = 0;
  private pendingTl: number = 0;
  private pendingPx: number = 0;
  private pendingPy: number = 0;
  private motionLogCounter: number = 0;
  private static readonly LONG_PRESS_MS: number = 500;
  // 回车 down 时间戳 + 补发窗口: 软键盘/IME 系统吞 KeyDown 只派 KeyUp 时,
  // 无配对 up 补发 down+up, 保证 Wine 收到完整按键 (外部键盘兜底)
  private lastEnterDownTs: number = 0;
  private static readonly ENTER_SYNTH_WINDOW_MS: number = 1000;
  private static readonly DRAG_THRESHOLD: number = 10; // 物理像素, 区分点击抖动和拖拽
  // 双击检测
  private lastTapTime: number = 0;
  private lastTapPx: number = 0;
  private lastTapPy: number = 0;
  private lastTapTl: number = 0;
  private static readonly DOUBLE_TAP_MS: number = 500;
  private static readonly DOUBLE_TAP_DIST: number = 64;
  // 双指滚动
  private twoFingerActive: boolean = false;
  private twoFingerLastMidX: number = 0;
  private twoFingerLastMidY: number = 0;
  private twoFingerTl: number = 0;
  private scrollLogCounter: number = 0;
  private twoFingerAccumY: number = 0;
  private twoFingerAccumX: number = 0;
  private static readonly SCROLL_STEP_PX: number = 40; // 物理像素/滚动步, winewayland.drv 只处理 discrete
  // 物理鼠标右键: Press 时捕获目标 tl, Release 发给同一 tl (按下捕获语义)
  private rightBtnTl: number = 0;
  // 物理鼠标中键: 同右键的按下捕获 (OHOS 只对左键合成 touch, 右/中键
  // 仅经 onMouse 到达, 互不干扰, 故共用同一套捕获-配对状态机)
  private midBtnTl: number = 0;
  // 物理鼠标左键: 同右键的按下捕获; mouseLeftActive 用于抑制 OHOS 为左键
  // 合成派发的 touch 事件 (见 onTouch 注释 — 合成 touch 会把手势状态机的
  // "等 Up 再补发 Press+Release" 作用到鼠标上, 静止点击被压成 0ms 脉冲,
  // dinput 轮询类游戏 (PAL2) 看不见这次点击)
  private leftBtnTl: number = 0;
  private mouseLeftActive: boolean = false;
  // hover 移动抽样日志计数
  private hoverLogCounter: number = 0;
  private eventGate: InputEventGate = new InputEventGate();

  private noteHardwareActivity(): void {
    if (this.settings.autoHideVirtualControls && this.settings.showVirtualControls) {
      InputOverlayWindowCoordinator.getInstance().setControlsVisible(false);
    }
  }

  aboutToAppear(): void {
    this.settings = AppSettingsStore.getInstance().getGlobalSettings();
    this.applyCornerSafeMargin();
    InputProfileService.getInstance().initialize(getContext(this) as common.UIAbilityContext).then(() => {
      const profile = InputProfileService.getInstance().getProfile(this.settings.desktopInputProfileId || 'default');
      this.inputProfile = profile;
      GamepadManager.getInstance().setButtonMappings(profile.gamepadMappings);
    });
    GamepadManager.getInstance().initialize(this.settings.gamepadDeadzone,
      this.settings.controllerOutputMode ?? 'dinput');
    this.gamepadConnectionCallback = (connected: boolean): void => {
      if (!connected && this.settings.autoHideVirtualControls && this.settings.showVirtualControls) {
        InputOverlayWindowCoordinator.getInstance().setControlsVisible(true);
      }
    };
    this.gamepadActivityCallback = (): void => {
      this.noteHardwareActivity();
    };
    GamepadManager.getInstance().subscribeConnection(this.gamepadConnectionCallback);
    GamepadManager.getInstance().subscribeActivity(this.gamepadActivityCallback);
    InputRouter.getInstance().setActiveToplevel(WineWindowManager.getInstance().getDesktopRootId());
    this.controller.switchDesktopRoot(WineWindowManager.getInstance().getDesktopRootId());
    InputOverlayWindowCoordinator.getInstance().registerRendererStressDriver(() => this.controller.rebindRenderer());
  }

  aboutToDisappear(): void {
    if (this.gamepadConnectionCallback) {
      GamepadManager.getInstance().unsubscribeConnection(this.gamepadConnectionCallback);
    }
    if (this.gamepadActivityCallback) {
      GamepadManager.getInstance().unsubscribeActivity(this.gamepadActivityCallback);
    }
    GamepadManager.getInstance().releaseAll();
    InputDispatcher.getInstance().releaseAll('desktop-page-disappear');
    InputRouter.getInstance().releaseAllKeys();
    InputOverlayWindowCoordinator.getInstance().unregisterRendererStressDriver();
  }

  private onDesktopToplevelChanged(): void {
    hilog.info(DOMAIN, 'LIFE', '[XComp] desktop toplevel changed to #%{public}d, renderer stays on desktop root',
      this.desktopToplevelId);
    // 桌面合成模式下 renderer 始终绑定桌面根：前台窗口"提升"只改变展示内容，
    // native 渲染与输入映射都按根 toplevel 查找 renderer（egl_renderer 的
    // RootCompositing 分支）。若随 desktopToplevelId 把 renderer 迁到非根
    // 窗口，输入侧按根 id 查不到 renderer，坐标会全部坍缩为 (0,0)（鼠标失效）。
    const rootId = WineWindowManager.getInstance().getDesktopRootId();
    if (rootId > 0 && this.controller.switchDesktopRoot(rootId)) {
      InputOverlayWindowCoordinator.getInstance().setActiveToplevel(rootId);
    }
  }

  private onDesktopWindowModeChanged(): void {
    this.applyCornerSafeMargin();
  }

  private onDesktopKeyFocusToken(): void {
    if (this.floating || this.wineDesktopKeyFocusToken <= 0) return;
    try {
      this.getUIContext().getFocusController().requestFocus('wine_desktop_xc');
    } catch (_) {
    }
  }

  /** 切边模式: XComponent 四周让出圆角避让边距 (黑色), 全屏模式无内缩。 */
  private applyCornerSafeMargin(): void {
    // 悬浮窗态 (Index 小浮窗): 无需圆角避让, XComponent 全客户区铺满
    if (this.floating) {
      this.cornerSafeMargin = 0;
      return;
    }
    // PC 虚拟桌面浮窗 (对齐 master VirtualDesktop/DesktopLayer — 其桌面窗口
    // 全部无 padding): 禁用圆角避让 padding, XComponent 全客户区铺满。否则
    // padding 内缩 XComponent 后, onMouse/onTouch 的组件相对坐标 (vp2px) 与
    // renderer surface/CoordTransform letterbox 基准错位, 鼠标光标相对游戏
    // 光标偏移。切边圆角避让仅平板全屏桌面需要。
    if (AppSettingsStore.getInstance().usesManagedWineWindows() &&
      WineWindowManager.getInstance().isDesktopMode) {
      this.cornerSafeMargin = 0;
      return;
    }
    const mode = (this.desktopWindowMode ||
      AppSettingsStore.getInstance().getGlobalSettings().desktopWindowMode) as DesktopWindowMode;
    if (mode !== DesktopWindowMode.SAFE_AREA) {
      this.cornerSafeMargin = 0;
      return;
    }
    try {
      const density = Math.max(1, display.getDefaultDisplaySync().densityPixels);
      const d = display.getDefaultDisplaySync();
      const marginPx = resolveCornerSafeMarginPx(d.width, d.height);
      // 只做圆角避让; 挖孔不做窗口级避让 (整窗内缩很难看), 按钮级避让由
      // 各页控件按 TYPE_CUTOUT 单独处理。
      this.cornerSafeMargin = Math.max(16, marginPx / density);
      hilog.info(DOMAIN, 'LIFE', '[XComp] corner-safe margin=%{public}dvp (%{public}dpx @density=%{public}s)',
        this.cornerSafeMargin, marginPx, density.toFixed(2));
    } catch (_) {
      this.cornerSafeMargin = 48;
    }
  }

  private handleHardwareKey(ev: KeyEvent): void {
    if (this.floating) return;  // 悬浮窗态: 输入旁路
    this.noteHardwareActivity();
    const rootId = WineWindowManager.getInstance().getDesktopRootId();
    if (rootId <= 0) return;
    ev.stopPropagation();
    hilog.info(DOMAIN, 'KBD-PIPE', '[ArkTS] desktop key code=%{public}d type=%{public}d source=%{public}d',
      ev.keyCode, ev.type, ev.keySource);
    // OHOS assigns the same 2012-2015 codes to keyboard arrows and gamepad D-pad.
    // Only joystick events belong to GamepadManager; its DInput path consumes
    // mapped codes without sending a Wine keyboard event.
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
    // 回车补发: 软键盘/IME 系统吞 KeyDown 只派 KeyUp → 无配对 up 补发 down+up
    if (code === EVDEV_KEY_ENTER) {
      if (pressed) {
        this.lastEnterDownTs = Date.now();
      } else if (Date.now() - this.lastEnterDownTs > DesktopLayer.ENTER_SYNTH_WINDOW_MS) {
        hilog.info(DOMAIN, 'KBD-PIPE', '[ArkTS] ENTER SYNTH down (missing down)');
        InputRouter.getInstance().sendKey(EVDEV_KEY_ENTER, true);
      }
    }
    InputRouter.getInstance().sendKey(code, pressed);
  }

  build() {
    Stack() {
      XComponent({
        id: 'wine_desktop_xc',
        type: XComponentType.SURFACE,
        controller: this.controller
      }).width('100%').height('100%').backgroundColor('#000')
        .focusable(true)
        .focusOnTouch(true)
        .defaultFocus(true)

      // ── 触控处理: 单指→左键/右键/拖拽, 双指→滚轮 ──
      .onTouch((ev: TouchEvent) => {
        // 悬浮窗态: 输入旁路 (点击/拖动由 Index 悬浮交互层接管)
        if (this.floating) return;
        if (!this.eventGate.shouldHandleTouch(ev.source, ev.sourceTool)) return;
        try {
          // ── 双指滚动 ──
          if (ev.touches.length >= 2) {
            // 取消单指状态
            if (this.touchPending || this.isDragging) {
              if (this.isDragging) {
                const btnLeft = mapMouseButton(1);
                testNapi.sendPointerEvent(this.pendingTl, MouseAction.Release, this.pendingPx, this.pendingPy, btnLeft);
              }
              this.touchPending = false;
              this.isDragging = false;
            }

            // 计算双指中点
            let midX = 0, midY = 0;
            for (let i = 0; i < ev.touches.length; i++) {
              midX += vp2px(ev.touches[i].x);
              midY += vp2px(ev.touches[i].y);
            }
            midX = Math.round(midX / ev.touches.length);
            midY = Math.round(midY / ev.touches.length);

            if (!this.twoFingerActive) {
              // 进入双指模式
              this.twoFingerActive = true;
              this.twoFingerLastMidX = midX;
              this.twoFingerLastMidY = midY;
              this.twoFingerAccumY = 0;
              this.twoFingerAccumX = 0;
              this.twoFingerTl = testNapi.findToplevelAt(midX, midY) as number ?? 0;
              this.scrollLogCounter = 0;
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] SCROLL-START tl=%{public}d fingers=%{public}d',
                this.twoFingerTl, ev.touches.length);
            } else {
              // 累积位移 → 超过阈值发送离散步
              this.twoFingerAccumY += this.twoFingerLastMidY - midY;  // 向上=正=向下滚
              this.twoFingerAccumX += midX - this.twoFingerLastMidX;  // 向右=正=向右滚
              this.twoFingerLastMidX = midX;
              this.twoFingerLastMidY = midY;

              if (this.twoFingerTl > 0) {
                // 垂直滚动
                while (Math.abs(this.twoFingerAccumY) >= DesktopLayer.SCROLL_STEP_PX) {
                  const sign = this.twoFingerAccumY > 0 ? 1 : -1;
                  testNapi.sendScrollEvent(this.twoFingerTl, 0, sign, DesktopLayer.SCROLL_STEP_PX, midX, midY);
                  this.twoFingerAccumY -= sign * DesktopLayer.SCROLL_STEP_PX;
                  if (++this.scrollLogCounter % 5 === 0) {
                    hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] SCROLL-V tl=%{public}d',
                      this.twoFingerTl);
                  }
                }
                // 水平滚动
                while (Math.abs(this.twoFingerAccumX) >= DesktopLayer.SCROLL_STEP_PX) {
                  const sign = this.twoFingerAccumX > 0 ? 1 : -1;
                  testNapi.sendScrollEvent(this.twoFingerTl, 1, sign, DesktopLayer.SCROLL_STEP_PX, midX, midY);
                  this.twoFingerAccumX -= sign * DesktopLayer.SCROLL_STEP_PX;
                }
              }
            }
            return;
          }

          // ── 单指模式: 恢复双指状态 ──
          if (this.twoFingerActive) {
            this.twoFingerActive = false;
            hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] SCROLL-END');
            return;
          }

          const t = ev.changedTouches[0];
          if (!t) return;
          // 物理鼠标左键的合成 touch: 已由 .onMouse 以真实时序直通, 此处忽略。
          // 若让它进手势状态机, 静止点击会被压成 Press+Release 同刻脉冲
          // (dinput 轮询不可见), 长按还会被误判成右键 — 鼠标有自己的右键。
          if (this.mouseLeftActive) return;
          const px = Math.round(vp2px(t.x));
          const py = Math.round(vp2px(t.y));
          InputRouter.getInstance().rememberPointer(px, py);

          if (ev.type === TouchType.Down) {
            // 查找目标 toplevel (px,py 为物理坐标, findToplevelAt 内部通过 viewport 转逻辑坐标)
            const tl: number = testNapi.findToplevelAt(px, py) as number;
            if (tl <= 0) {
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] TOUCH skipped (no toplevel at %{public}d,%{public}d)', px, py);
              return;
            }
            hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] TOUCH tl=%{public}d px=(%{public}d,%{public}d)', tl, px, py);
            testNapi.raiseToplevel(tl);
            InputRouter.getInstance().setActiveToplevel(tl);
            // 进入 PENDING 状态: 暂不发送事件, 等判定长按/拖拽/点击
            this.touchPending = true;
            this.isDragging = false;
            this.touchStartTime = Date.now();
            this.touchStartPx = px;
            this.touchStartPy = py;
            this.pendingTl = tl;
            this.pendingPx = px;
            this.pendingPy = py;
            this.motionLogCounter = 0;

          } else if (ev.type === TouchType.Move) {
            if (!this.touchPending && !this.isDragging) return;

            const dist = Math.abs(px - this.touchStartPx) + Math.abs(py - this.touchStartPy);

            if (this.touchPending && dist >= DesktopLayer.DRAG_THRESHOLD) {
              // 进入拖拽模式: 补发 Press (Down 时未发)
              const btnLeft = mapMouseButton(1);
              this.touchPending = false;
              this.isDragging = true;
              // 打断双击序列 (拖拽不是点击)
              this.lastTapTime = 0;
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] DRAG-START tl=%{public}d px=(%{public}d,%{public}d) dist=%{public}d',
                this.pendingTl, px, py, dist);
              testNapi.sendPointerEvent(this.pendingTl, MouseAction.Press, this.pendingPx, this.pendingPy, btnLeft);
              testNapi.sendPointerEvent(this.pendingTl, MouseAction.Move, px, py, btnLeft);
            } else if (this.isDragging) {
              // 拖拽移动: 持续发送 Motion
              testNapi.sendPointerEvent(this.pendingTl, MouseAction.Move, px, py, 0);
              if (++this.motionLogCounter % 10 === 0) {
                hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] DRAG-MOVE tl=%{public}d px=(%{public}d,%{public}d) frame=%{public}d',
                  this.pendingTl, px, py, this.motionLogCounter);
              }
            }
            // PENDING 且未超过阈值: 不做任何事 (等 Up 判定长按/点击)

          } else if (ev.type === TouchType.Up) {
            if (this.isDragging) {
              // 拖拽结束
              const btnLeft = mapMouseButton(1);
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] DRAG-END tl=%{public}d px=(%{public}d,%{public}d)',
                this.pendingTl, px, py);
              testNapi.sendPointerEvent(this.pendingTl, MouseAction.Release, px, py, btnLeft);
              this.isDragging = false;

            } else if (this.touchPending) {
              // PENDING 状态: 按按下时长判定长按 vs 点击
              this.touchPending = false;
              const elapsed = Date.now() - this.touchStartTime;

              if (elapsed >= DesktopLayer.LONG_PRESS_MS) {
                // 长按 → 右键
                const btnRight = mapMouseButton(2);
                this.lastTapTime = 0; // 长按打断双击序列
                hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] LONGPRESS->RIGHTCLICK tl=%{public}d elapsed=%{public}dms px=(%{public}d,%{public}d)',
                  this.pendingTl, elapsed, this.pendingPx, this.pendingPy);
                testNapi.sendPointerEvent(this.pendingTl, MouseAction.Press, this.pendingPx, this.pendingPy, btnRight);
                testNapi.sendPointerEvent(this.pendingTl, MouseAction.Release, this.pendingPx, this.pendingPy, btnRight);
              } else {
                // 短按 → 左键 或 双击
                const btnLeft = mapMouseButton(1);
                const now = Date.now();
                const dist = Math.abs(this.pendingPx - this.lastTapPx) + Math.abs(this.pendingPy - this.lastTapPy);
                const isDoubleTap = (this.lastTapTime > 0 && (now - this.lastTapTime) < DesktopLayer.DOUBLE_TAP_MS
                                    && dist < DesktopLayer.DOUBLE_TAP_DIST && this.pendingTl === this.lastTapTl);

                if (isDoubleTap) {
                  // 第一次点击已在上一轮 Up 注入；第二轮只需再注入一次。
                  // 旧逻辑在这里额外注入两次，Windows 实际收到三击。
                  this.lastTapTime = 0;
                  testNapi.sendPointerEvent(this.pendingTl, MouseAction.Press, this.pendingPx, this.pendingPy, btnLeft);
                  testNapi.sendPointerEvent(this.pendingTl, MouseAction.Release, this.pendingPx, this.pendingPy, btnLeft);
                  hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] DOUBLE-TAP tl=%{public}d px=(%{public}d,%{public}d)',
                    this.pendingTl, this.pendingPx, this.pendingPy);
                } else {
                  // 单击
                  testNapi.sendPointerEvent(this.pendingTl, MouseAction.Press, this.pendingPx, this.pendingPy, btnLeft);
                  testNapi.sendPointerEvent(this.pendingTl, MouseAction.Release, this.pendingPx, this.pendingPy, btnLeft);
                  this.lastTapTime = now;
                  this.lastTapPx = this.pendingPx;
                  this.lastTapPy = this.pendingPy;
                  this.lastTapTl = this.pendingTl;
                  hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] TAP->LEFTCLICK tl=%{public}d px=(%{public}d,%{public}d)',
                    this.pendingTl, this.pendingPx, this.pendingPy);
                }
              }
            }
          } else if (ev.type === TouchType.Cancel) {
            // 系统取消触摸 (手势/通知等): 如果正在拖拽则补发 Release
            if (this.isDragging) {
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] CANCEL-DRAG tl=%{public}d px=(%{public}d,%{public}d)',
                this.pendingTl, px, py);
              const btnLeft = mapMouseButton(1);
              testNapi.sendPointerEvent(this.pendingTl, MouseAction.Release, px, py, btnLeft);
            } else {
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] CANCEL tl=%{public}d (no event sent)', this.pendingTl);
            }
            this.touchPending = false;
            this.isDragging = false;
          }
        } catch (e) {
          hilog.error(DOMAIN, 'CLICK-PIPE', '[ArkTS] TOUCH handler exception: %{public}s', JSON.stringify(e));
        }
      })

      // ── 物理鼠标: hover 移动 + 左右键真实时序直通 ──
      // 左键: OHOS 会同时派发 mouse 事件和合成 touch 事件 — 本处理器直通
      // mouse 事件, 合成 touch 由 onTouch 里的 mouseLeftActive 守卫丢弃
      // (手势状态机的"等 Up 再补发"只服务于手指, 作用于鼠标会把静止点击
      // 压成 0ms 脉冲)。右键不合成 touch 事件, 没有本处理器则完全丢失
      // (触屏长按合成右键不受影响)。
      .onMouse((ev: MouseEvent) => {
        // 悬浮窗态: 输入旁路 (物理鼠标同样不转发给 Wine)
        if (this.floating) return;
        if (!this.eventGate.shouldHandleMouse(ev.source, ev.sourceTool)) return;
        this.noteHardwareActivity();
        try {
          // Physical mouse events are routed here exclusively. Touch-generated
          // mouse events are rejected by InputEventGate above.
          if (ev.action === MouseAction.Move) {
            const px = Math.round(vp2px(ev.x));
            const py = Math.round(vp2px(ev.y));
            // 按键按住拖动: 发给按下捕获的 tl; 否则按当前位置找目标
            // (跨窗口 enter/leave 由注入层 ACT_MOVE 的焦点判定处理)
            let tl = this.leftBtnTl || this.rightBtnTl || this.midBtnTl;
            if (tl <= 0) {
              tl = testNapi.findToplevelAt(px, py) as number;
              if (tl <= 0) return;
            }
            if (++this.hoverLogCounter % 120 === 0) {
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE-MOVE tl=%{public}d px=(%{public}d,%{public}d) n=%{public}d',
                tl, px, py, this.hoverLogCounter);
            }
            // rawDelta 按设备校准 (与 WineWindow 同规则: 鼠标 2.5 / 触控板 0.625)
            // — 收口 ArkTS 时此处漏乘, 桌面模式相对模式增量只有融合窗口的
            // 1/2.5, 同一鼠标两种手感; 绝对模式有差分自标定, 不受影响
            let rawDx = ev.rawDeltaX ?? 0;
            let rawDy = ev.rawDeltaY ?? 0;
            const devId = ev.deviceId ?? 0;
            if (devId > 0) {
              const s = InputDeviceMapper.getInstance().getScale(devId);
              rawDx *= s;
              rawDy *= s;
            }
            testNapi.sendPointerEvent(tl, MouseAction.Move, px, py, 0, rawDx, rawDy);
            return;
          }
          if (ev.action !== MouseAction.Press && ev.action !== MouseAction.Release) return;
          const px = Math.round(vp2px(ev.x));
          const py = Math.round(vp2px(ev.y));
          // ── 物理鼠标左键: 真实时序直通 (按下即转发, 不等手势判定) ──
          // OHOS 对左键同时派发 mouse 事件和合成 touch 事件; 合成 touch 走
          // onTouch 手势状态机会把静止点击压成 Press+Release 同刻脉冲
          // (dinput 轮询类游戏 PAL2 因此看不见静止点击), 长按还会被误判为
          // 右键。这里直通真实时序, 并用 mouseLeftActive 抑制合成 touch。
          if (ev.button === MouseButton.Left) {
            const btnLeft = mapMouseButton(1);
            if (ev.action === MouseAction.Press) {
              this.mouseLeftActive = true;
              // 竞态: 合成 touch 可能先一步进入 PENDING/DRAG, 静默清除
              // (其后续 Move/Up 会被 mouseLeftActive 拦截, 不会双发)
              this.touchPending = false;
              this.isDragging = false;
              const tl: number = testNapi.findToplevelAt(px, py) as number;
              if (tl <= 0) {
                this.leftBtnTl = 0;
                hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE-LEFT skipped (no toplevel at %{public}d,%{public}d)', px, py);
                return;
              }
              this.leftBtnTl = tl;
              testNapi.raiseToplevel(tl);
              InputRouter.getInstance().setActiveToplevel(tl);
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE-LEFT press tl=%{public}d px=(%{public}d,%{public}d)', tl, px, py);
              testNapi.sendPointerEvent(tl, MouseAction.Press, px, py, btnLeft, 0, 0, true);
            } else {
              if (this.leftBtnTl <= 0) {
                this.mouseLeftActive = false;
                return;
              }
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE-LEFT release tl=%{public}d px=(%{public}d,%{public}d)', this.leftBtnTl, px, py);
              testNapi.sendPointerEvent(this.leftBtnTl, MouseAction.Release, px, py, btnLeft);
              this.leftBtnTl = 0;
              this.mouseLeftActive = false;
            }
            return;
          }
          // ── 右键 / 中键: 共用按下捕获-释放配对 ──
          // OHOS 只对左键合成 touch, 右/中键在 onTouch 无对应事件, 不会双发;
          // 两键各自独立捕获 tl, 同按 (罕见) 也互不串扰
          if (ev.button !== MouseButton.Right && ev.button !== MouseButton.Middle) return;
          const isRight = ev.button === MouseButton.Right;
          const btnCode = mapMouseButton(isRight ? 2 : 4);
          const btnName = isRight ? 'RIGHT' : 'MIDDLE';
          if (ev.action === MouseAction.Press) {
            const tl: number = testNapi.findToplevelAt(px, py) as number;
            if (tl <= 0) {
              if (isRight) this.rightBtnTl = 0; else this.midBtnTl = 0;
              hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE-%{public}s skipped (no toplevel at %{public}d,%{public}d)', btnName, px, py);
              return;
            }
            if (isRight) this.rightBtnTl = tl; else this.midBtnTl = tl;
            testNapi.raiseToplevel(tl);
            InputRouter.getInstance().setActiveToplevel(tl);
            hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE-%{public}s press tl=%{public}d px=(%{public}d,%{public}d)', btnName, tl, px, py);
            testNapi.sendPointerEvent(tl, MouseAction.Press, px, py, btnCode, 0, 0, true);
          } else {
            const btnTl = isRight ? this.rightBtnTl : this.midBtnTl;
            if (btnTl <= 0) return;
            hilog.info(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE-%{public}s release tl=%{public}d px=(%{public}d,%{public}d)', btnName, btnTl, px, py);
            testNapi.sendPointerEvent(btnTl, MouseAction.Release, px, py, btnCode);
            if (isRight) this.rightBtnTl = 0; else this.midBtnTl = 0;
          }
        } catch (e) {
          hilog.error(DOMAIN, 'CLICK-PIPE', '[ArkTS] MOUSE handler exception: %{public}s', JSON.stringify(e));
        }
      })

      // ── 滚轮事件 ──
      .onAxisEvent((ev?: AxisEvent) => {
        if (this.floating) return;  // 悬浮窗态: 输入旁路
        this.noteHardwareActivity();
        const rootId = WineWindowManager.getInstance().getDesktopRootId();
        if (rootId <= 0 || !ev) return;
        const wpx = Math.round(vp2px(ev.x));
        const wpy = Math.round(vp2px(ev.y));
        const vert = ev.getVerticalAxisValue();
        const horiz = ev.getHorizontalAxisValue();
        const step = ev.scrollStep ?? 0;
        const hit = testNapi.findToplevelAt(wpx, wpy) as number;
        const tl: number = hit > 0 ? hit : rootId;
        if (vert !== 0) testNapi.sendScrollEvent(tl, 0, vert, step, wpx, wpy);
        if (horiz !== 0) testNapi.sendScrollEvent(tl, 1, horiz, step, wpx, wpy);
      })
      .onKeyEvent((ev: KeyEvent): void => this.handleHardwareKey(ev))

      FpsHud({ toplevelId: 0, hide: this.floating })
        .position({ x: 10, y: 10 })
        .hitTestBehavior(HitTestMode.None)

      WineEngineBlockingOverlay()
    }
    .width('100%').height('100%').backgroundColor('#000')
    .padding({
      left: this.cornerSafeMargin,
      top: 0,
      right: this.cornerSafeMargin,
      bottom: 0
    })
    .id('wine-desktop-input-root')
  }
}
