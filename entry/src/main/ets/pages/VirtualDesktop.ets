import { hilog } from '@kit.PerformanceAnalysisKit';
import { display } from '@kit.ArkUI';
import { preferences } from '@kit.ArkData';
import { DesktopLayer } from '../components/DesktopLayer';
import { WineWindowManager } from '../service/WineWindowManager';

// 悬浮还原按钮位置持久化文件 (拖动落位跨会话记住, 参照 Pad 按钮落库)
const EXIT_BTN_PREF_FILE = 'vd_exit_btn';
const EXIT_BTN_PREF_X = 'x';
const EXIT_BTN_PREF_Y = 'y';

const DOMAIN = 0x0000;
const TAG = 'VD';
// 悬浮还原按钮尺寸 (vp)。拖动钳制用。
const EXIT_BTN_SIZE = 48;
// 拖动手势: 位移 > 5vp 判定为拖动 (不触发还原), 否则视为点击 → 还原
const EXIT_DRAG_THRESHOLD = 5;

// PC 虚拟桌面页面 (VirtualDesktopAbility 承载): 极简壳 — 桌面全部逻辑
// (renderer 生命周期 / 触控状态机 / 触控板 / 鼠标 / 键盘 / 滚轮) 复用
// components/DesktopLayer (feature/main-ui 桌面层组件, 原 master
// DesktopWindow.ets 的迁入进化版)。不带手柄/虚拟键盘 overlay:
// PC 有物理键鼠, 不需要 Pad 的悬浮按钮。
@Entry
@Component
struct VirtualDesktop {
  // 沉浸全屏态 (VDA windowStatusChange → AppStorage): 窗口最大化进入沉浸
  // 全屏时置 true → 显示右上角悬浮"退出全屏"按钮。DISABLE_HOVER 禁用了
  // 系统 hover 唤出还原, 唯一还原入口就是本按钮 (点击 recover() → VDA
  // statusType=4 → 置 false → 按钮隐藏, 标题栏回归)。
  @StorageProp('vdImmersive') immersive: boolean = false;
  // 悬浮按钮位置 (vp, 左上角), 支持拖拽避免遮挡桌面操作 (参照 Pad
  // PadButtonView 拖拽: windowX/windowY 相对窗口, position 变化不影响位移)
  @State btnX: number = 0;
  @State btnY: number = 0;
  private scrW: number = 0;   // 沉浸窗口 vp 尺寸 (= 屏幕, 沉浸态才显示按钮)
  private scrH: number = 0;
  private startX: number = 0;      // 手势起点 (windowX/Y)
  private startY: number = 0;
  private startBtnX: number = 0;   // 手势起点按钮位置
  private startBtnY: number = 0;
  private dragMoved: boolean = false;

  // 桌面分辨率恒等于屏幕/scale (WineWindowManager.init 的 setOutputSize):
  // desktopLauncherVisible 保持默认 true → DesktopLayer.onSurfaceChanged
  // 跳过 root resize, 窗口只是显示口 (任何尺寸 letterbox 等比缩放),
  // 用户缩放/最大化窗口不改变 Wine 桌面布局。
  // Index.onLauncherVisibleChange 在 desktopSeparateWindow 时不写回此值,
  // 不会干扰本窗口语义。
  aboutToAppear(): void {
    hilog.info(DOMAIN, TAG, '[VD] aboutToAppear');
    // 沉浸态窗口 = 全屏 → 用屏幕 vp 尺寸做按钮初始位置/拖动钳制基准
    const d = display.getDefaultDisplaySync();
    this.scrW = px2vp(d.width);
    this.scrH = px2vp(d.height);
    // 初始右上角: 距右 16% 屏宽 (右缘留白), 距顶 20vp
    this.btnX = this.scrW * 0.84 - EXIT_BTN_SIZE;
    this.btnY = 20;
    // 读取持久化位置 (用户上次拖到的落位), 有则覆盖默认
    this.loadBtnPos();
  }

  // 读持久化按钮位置 (Preferences)。无记录/读失败 → 保持 aboutToAppear 默认
  private loadBtnPos(): void {
    preferences.getPreferences(getContext(this), EXIT_BTN_PREF_FILE).then((prefs: preferences.Preferences) => {
      const x = prefs.getSync(EXIT_BTN_PREF_X, -1) as number;
      const y = prefs.getSync(EXIT_BTN_PREF_Y, -1) as number;
      if (x >= 0 && y >= 0) {
        // 换屏幕/分辨率后钳制回窗口内
        this.btnX = Math.min(Math.max(0, x), this.scrW - EXIT_BTN_SIZE);
        this.btnY = Math.min(Math.max(0, y), this.scrH - EXIT_BTN_SIZE);
        hilog.info(DOMAIN, TAG, '[VD] btn pos loaded x=%{public}d y=%{public}d', this.btnX, this.btnY);
      }
    }).catch((e: Error) => {
      hilog.warn(DOMAIN, TAG, '[VD] load btn pos failed: %{public}s', JSON.stringify(e));
    });
  }

  // 拖动落位后持久化按钮位置
  private saveBtnPos(): void {
    preferences.getPreferences(getContext(this), EXIT_BTN_PREF_FILE).then((prefs: preferences.Preferences) => {
      prefs.putSync(EXIT_BTN_PREF_X, this.btnX);
      prefs.putSync(EXIT_BTN_PREF_Y, this.btnY);
      prefs.flush();
      hilog.info(DOMAIN, TAG, '[VD] btn pos saved x=%{public}d y=%{public}d', this.btnX, this.btnY);
    }).catch((e: Error) => {
      hilog.warn(DOMAIN, TAG, '[VD] save btn pos failed: %{public}s', JSON.stringify(e));
    });
  }

  // 悬浮按钮拖拽 + 点击判定 (onTouch 驱动, 同 Pad PadButtonView):
  // 位移 > EXIT_DRAG_THRESHOLD 判定为拖动 (移动按钮位置), 否则点击 → 还原
  private onExitBtnTouch(ev: TouchEvent): void {
    if (ev.type === TouchType.Down) {
      const t = ev.touches[0];
      if (t) {
        this.startX = t.windowX;
        this.startY = t.windowY;
      }
      this.startBtnX = this.btnX;
      this.startBtnY = this.btnY;
      this.dragMoved = false;
    } else if (ev.type === TouchType.Move) {
      const t = ev.touches[0];
      if (t) {
        if (!this.dragMoved &&
          (Math.abs(t.windowX - this.startX) > EXIT_DRAG_THRESHOLD ||
            Math.abs(t.windowY - this.startY) > EXIT_DRAG_THRESHOLD)) {
          this.dragMoved = true;
        }
        if (this.dragMoved) {
          // 钳制在窗口内 (沉浸窗口 = 屏幕)
          this.btnX = Math.min(Math.max(0, this.startBtnX + (t.windowX - this.startX)),
            this.scrW - EXIT_BTN_SIZE);
          this.btnY = Math.min(Math.max(0, this.startBtnY + (t.windowY - this.startY)),
            this.scrH - EXIT_BTN_SIZE);
        }
      }
    } else if (ev.type === TouchType.Up || ev.type === TouchType.Cancel) {
      if (!this.dragMoved) {
        hilog.info(DOMAIN, TAG, '[VD] immersive exit (full_screen_fill) → recoverDesktopWindow');
        WineWindowManager.getInstance().recoverDesktopWindow();
      } else {
        // 拖动落位 → 持久化 (跨会话记住按钮位置)
        this.saveBtnPos();
      }
      this.dragMoved = false;
    }
  }

  build() {
    Stack() {
      // floating=false: 独立窗口内无悬浮窗态, 输入全直通
      // 无手柄/虚拟键盘悬浮按钮 (用户决策: PC 虚拟桌面不带 Pad 的
      // GamepadOverlay/KeyboardOverlay — 有物理键鼠)
      DesktopLayer({ floating: false })

      // 沉浸全屏态的唯一还原入口 (DISABLE_HOVER 无系统 hover 还原)。
      // 沿用 Pad 悬浮按钮设计 (GamepadOverlay PadButtonView): 半透明圆钮
      // + 系统符号图标; 支持拖拽移动 (避免遮挡桌面操作), 点击才还原。
      // 普通窗口态隐藏 (标题栏自带还原/缩放/关闭)。
      if (this.immersive) {
        Stack() {
          // Stack 默认子组件居中 → SymbolGlyph 居中于圆钮
          SymbolGlyph($r('sys.symbol.full_screen_fill'))
            .fontSize(18)
            .fontColor([Color.White])
        }
        .width(EXIT_BTN_SIZE)
        .height(EXIT_BTN_SIZE)
        .borderRadius(EXIT_BTN_SIZE / 2)
        .backgroundColor('#33000000')
        .border({ width: 1, color: '#66FFFFFF', style: BorderStyle.Solid })
        .position({ x: this.btnX, y: this.btnY })
        .onTouch((ev: TouchEvent) => this.onExitBtnTouch(ev))
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#000000')
  }
}
