import { AppDescriptor } from '../model/AppModels';
import { hilog } from '../service/LogService';

const DOMAIN = 0x0000;
const TAG = 'LargeTileGrid';

/** 单个瓦片的布局盒 (页面内绝对定位)。 */
export interface TileBox {
  app: AppDescriptor;
  col: number;
  row: number;
  colSpan: number;
  rowSpan: number;
}

const GAP: number = 10;
/** 翻页小球条带高度 (vp): 瓦片区之外, 紧贴导航栏上方。 */
const DOTS_STRIP: number = 20;
const MIN_ROWS = 2;
const MAX_ROWS = 8;

/**
 * 大屏瓦片网格 (Switch 主机风): 大小混排瓦片 + 横向翻页 + 页点指示器。
 * 仅在大屏 (对角线 > 8.8") 或小屏强制预览时替代环形封面流;
 * 顶栏/底栏等其他结构不变。
 * 布局: 固定行数 (设备档位上限), 方形基本单元, 按 bento 序列 [2x2,1x1,1x1,2x1,...]
 * 货架式装箱; 装不下自动降级为 1x1, 一页装满开新页。
 */
@Component
export struct LargeTileGrid {
  /** @Prop: 父级 (顺序/筛选) 变化时同步进来; @Watch 触发布局版本号自增, 强制页面 ForEach 重建。 */
  @Prop @Watch('onAppsChanged') apps: AppDescriptor[] = [];
  /** 期望行数 (调用方已按设备档位收口)。 */
  @Prop @Watch('onAppsChanged') rows: number = 3;
  runningIds: Set<string> = new Set<string>();
  onLaunch: (app: AppDescriptor) => void = (_a: AppDescriptor): void => {};
  onLongPress: (app: AppDescriptor) => void = (_a: AppDescriptor): void => {};
  /** 拖拽换位: dragId 的瓦片被放到 dropId 的瓦片上 (父级负责换序并持久化)。 */
  onSwap: (dragId: string, dropId: string) => void = (_d: string, _p: string): void => {};
  @State private canvasW: number = 0;
  @State private canvasH: number = 0;
  @State private currentPage: number = 0;
  /** 布局版本号: apps/rows 变化时自增, 拼进页面 ForEach 的 key (key 不变不重建)。 */
  @State private layoutVersion: number = 0;
  /** 拖拽状态机: 长按拿起 (lifted) → 移动 (drag) → 松手换位/取消。 */
  @State private liftedId: string = '';
  @State private dragId: string = '';
  @State private dragDX: number = 0;
  @State private dragDY: number = 0;
  @State private dropTargetId: string = '';

  private onAppsChanged(): void {
    this.layoutVersion++;
    if (this.currentPage >= this.packPages().length) {
      this.currentPage = 0;
    }
  }

  /** 贴图与瓦片形状对等: 横条 (2x1) 优先用游戏的背景横图, 竖条/方形用竖版封面。 */
  private artOf(t: TileBox): string {
    if (t.colSpan > t.rowSpan && t.app.backgroundPath && t.app.backgroundPath.length > 0) {
      const bg = t.app.backgroundPath;
      return bg.startsWith('file://') ? bg : 'file://' + bg;
    }
    return this.coverOf(t.app);
  }

  private effRows(): number {
    return Math.max(MIN_ROWS, Math.min(MAX_ROWS, Math.floor(this.rows)));
  }

  private cellH(): number {
    const rows = this.effRows();
    return (this.canvasH - DOTS_STRIP - (rows - 1) * GAP) / rows;
  }

  private cellW(): number {
    return this.cellH();
  }

  private cols(): number {
    if (this.canvasW <= 0 || this.cellW() <= 0) return 3;
    return Math.max(3, Math.min(14, Math.floor((this.canvasW + GAP) / (this.cellW() + GAP))));
  }

  private coverOf(app: AppDescriptor): string {
    const raw = app.coverPath && app.coverPath.length > 0 ? app.coverPath
      : (app.backgroundPath ? app.backgroundPath : '');
    if (raw.length === 0) return '';
    return raw.startsWith('file://') ? raw : 'file://' + raw;
  }

  /** bento 混排序列: 大方 → 小×2 → 横条 → 小 → 竖条 → 小 → 横条 → 小×2 */
  private static PATTERN: number[][] = [
    [2, 2], [1, 1], [1, 1], [2, 1], [1, 1],
    [1, 2], [1, 1], [2, 1], [1, 1], [1, 1]
  ];

  private fits(used: boolean[][], row: number, col: number, w: number, h: number): boolean {
    const rows = this.effRows();
    const cols = this.cols();
    if (row < 0 || col < 0 || row + h > rows || col + w > cols) return false;
    for (let r = row; r < row + h; r++) {
      for (let c = col; c < col + w; c++) {
        if (used[r][c]) return false;
      }
    }
    return true;
  }

  private markUsed(used: boolean[][], row: number, col: number, w: number, h: number): void {
    for (let r = row; r < row + h; r++) {
      for (let c = col; c < col + w; c++) {
        used[r][c] = true;
      }
    }
  }

  private packPages(): TileBox[][] {
    const rows = this.effRows();
    const cols = this.cols();
    const pages: TileBox[][] = [];
    if (this.canvasW <= 0 || this.canvasH <= 0 || this.apps.length === 0) return pages;
    let used: boolean[][] = [];
    let cursorRow = 0;
    let cursorCol = 0;
    let patternIndex = 0;
    const startNewPage = (): void => {
      used = [];
      for (let r = 0; r < rows; r++) {
        const line: boolean[] = [];
        for (let c = 0; c < cols; c++) {
          line.push(false);
        }
        used.push(line);
      }
      cursorRow = 0;
      cursorCol = 0;
      pages.push([]);
    };
    startNewPage();
    const page = (): TileBox[] => pages[pages.length - 1];
    for (let i = 0; i < this.apps.length; i++) {
      const app = this.apps[i];
      // 从光标处找第一个空单元
      while (cursorRow < rows && used[cursorRow][cursorCol]) {
        cursorCol++;
        if (cursorCol >= cols) {
          cursorCol = 0;
          cursorRow++;
        }
      }
      if (cursorRow >= rows) {
        startNewPage();
        i--;
        continue;
      }
      // 按序列取跨度的候选 (试完整序列仍装不下则回退 1x1, 保证一定放得下)
      let placed = false;
      let needNewPage = false;
      for (let attempt = 0; attempt < LargeTileGrid.PATTERN.length + 1 && !placed; attempt++) {
        const span = LargeTileGrid.PATTERN[patternIndex % LargeTileGrid.PATTERN.length];
        const w = attempt >= LargeTileGrid.PATTERN.length ? 1 : span[0];
        const h = attempt >= LargeTileGrid.PATTERN.length ? 1 : span[1];
        if (this.fits(used, cursorRow, cursorCol, w, h)) {
          this.markUsed(used, cursorRow, cursorCol, w, h);
          page().push({ app: app, col: cursorCol, row: cursorRow, colSpan: w, rowSpan: h });
          if (attempt < LargeTileGrid.PATTERN.length) patternIndex++;
          cursorCol += w;
          if (cursorCol >= cols) {
            cursorCol = 0;
            cursorRow++;
          }
          placed = true;
        } else {
          // 该跨度放不下: 试序列里的下一件, 仍不行最终回退 1x1
          patternIndex++;
          if (cursorCol + w > cols) {
            cursorCol = 0;
            cursorRow++;
          }
          if (cursorRow >= rows) {
            needNewPage = true;
          }
        }
      }
      if (needNewPage || !placed) {
        startNewPage();
        i--;
      }
    }
    return pages;
  }

  /** 落点 (瓦片中心 + 拖动偏移) 命中的同页瓦片 id; 不含被拖瓦片自身。 */
  private findDropTargetAt(centerX: number, centerY: number, dragId: string): string {
    const pages = this.packPages();
    for (let p = 0; p < pages.length; p++) {
      const page = pages[p];
      let isDragPage = false;
      for (let i = 0; i < page.length; i++) {
        if (page[i].app.id === dragId) {
          isDragPage = true;
          break;
        }
      }
      if (!isDragPage) continue;
      for (let i = 0; i < page.length; i++) {
        const box = page[i];
        if (box.app.id === dragId) continue;
        const x = box.col * (this.cellW() + GAP);
        const y = box.row * (this.cellH() + GAP);
        if (centerX >= x && centerX <= x + this.cellW() * box.colSpan &&
          centerY >= y && centerY <= y + this.cellH() * box.rowSpan) {
          return box.app.id;
        }
      }
    }
    return '';
  }

  private resetDrag(): void {
    this.liftedId = '';
    this.dragId = '';
    this.dragDX = 0;
    this.dragDY = 0;
    this.dropTargetId = '';
  }

  @Builder
  private Tile(t: TileBox) {
    Stack({ alignContent: Alignment.TopEnd }) {
      if (this.artOf(t).length > 0) {
        if (t.colSpan !== t.rowSpan) {
          // 宽/竖条瓦片: 底层同图模糊撑满 + 上层完整图 Contain,
          // 避免把竖版封面硬裁成横条 (或反之) 的怪异观感。
          Image(this.artOf(t))
            .objectFit(ImageFit.Cover)
            .draggable(false)
            .blur(20)
            .scale({ x: 1.15, y: 1.15 })
            .width('100%')
            .height('100%')
          Image(this.artOf(t))
            .objectFit(ImageFit.Contain)
            .draggable(false)
            .width('100%')
            .height('100%')
        } else {
          Image(this.artOf(t))
            .objectFit(ImageFit.Cover)
            .draggable(false)
            .width('100%')
            .height('100%')
        }
      } else {
        Text(t.app.title.length > 0 ? t.app.title.substring(0, 1) : '?')
          .fontSize(34).fontWeight(FontWeight.Bold)
          .fontColor($r('app.color.text_secondary'))
          .width('100%').height('100%')
          .textAlign(TextAlign.Center)
      }
      if (this.runningIds.has(t.app.id)) {
        Circle({ width: 10, height: 10 }).fill('#4ADE80')
          .margin({ top: 10, right: 10 })
      }
    }
    .width(this.cellW() * t.colSpan + (t.colSpan - 1) * GAP)
    .height(this.cellH() * t.rowSpan + (t.rowSpan - 1) * GAP)
    .position({ x: t.col * (this.cellW() + GAP), y: t.row * (this.cellH() + GAP) })
    .borderRadius(14)
    .backgroundColor($r('app.color.card_background'))
    .clip(true)
    .border({
      width: this.dropTargetId === t.app.id && this.dragId !== '' ? 2 : 1,
      color: this.dropTargetId === t.app.id && this.dragId !== '' ? $r('app.color.accent') : '#22FFFFFF'
    })
    // 拖拽视觉: 跟手位移 + 放大 + 投影 + 置顶
    .translate(this.dragId === t.app.id ? { x: this.dragDX, y: this.dragDY } : { x: 0, y: 0 })
    .scale(this.dragId === t.app.id ? { x: 1.05, y: 1.05 } : { x: 1, y: 1 })
    .zIndex(this.dragId === t.app.id ? 100 : 0)
    .shadow(this.dragId === t.app.id
      ? { radius: 24, color: '#66000000', offsetX: 0, offsetY: 8 }
      : { radius: 0, color: '#00000000', offsetX: 0, offsetY: 0 })
    // 挂 app id: uitest dumpLayout 可按 tile-<id> 反查每张卡的 bounds (验证换位)
    .id('tile-' + t.app.id)
    .onClick((): void => {
      if (this.dragId === '' && this.liftedId === '') this.onLaunch(t.app);
    })
    .gesture(GestureGroup(GestureMode.Parallel,
      // 长按拿起; 抬起时没拖动 → 打开原长按菜单
      LongPressGesture({ repeat: false, duration: 280 })
        .onAction((): void => {
          hilog.info(DOMAIN, TAG, 'lift %{public}s', t.app.id);
          if (this.dragId === '') this.liftedId = t.app.id;
        })
        .onActionEnd((): void => {
          hilog.info(DOMAIN, TAG, 'lpEnd lifted=%{public}s drag=%{public}s',
            this.liftedId, this.dragId);
          if (this.liftedId === t.app.id && this.dragId === '') {
            this.resetDrag();
            this.onLongPress(t.app);
          }
        })
        .onActionCancel((): void => {
          hilog.info(DOMAIN, TAG, 'lpCancel %{public}s', t.app.id);
          if (this.dragId !== t.app.id) this.liftedId = '';
        }),
      PanGesture({ fingers: 1, distance: 10 })
        .onActionStart((_e: GestureEvent): void => {
          hilog.info(DOMAIN, TAG, 'panStart lifted=%{public}s id=%{public}s',
            this.liftedId, t.app.id);
          if (this.liftedId === t.app.id && this.dragId === '') {
            this.dragId = t.app.id;
            this.dragDX = 0;
            this.dragDY = 0;
          }
        })
        .onActionUpdate((e: GestureEvent): void => {
          if (this.dragId !== t.app.id) return;
          this.dragDX = e.offsetX;
          this.dragDY = e.offsetY;
          this.dropTargetId = this.findDropTargetAt(
            t.col * (this.cellW() + GAP) + this.cellW() * t.colSpan / 2 + this.dragDX,
            t.row * (this.cellH() + GAP) + this.cellH() * t.rowSpan / 2 + this.dragDY,
            t.app.id);
        })
        .onActionEnd((_e: GestureEvent): void => {
          if (this.dragId !== t.app.id) return;
          const dragId = this.dragId;
          const centerX = t.col * (this.cellW() + GAP) + this.cellW() * t.colSpan / 2 + this.dragDX;
          const centerY = t.row * (this.cellH() + GAP) + this.cellH() * t.rowSpan / 2 + this.dragDY;
          const target = this.findDropTargetAt(centerX, centerY, dragId);
          this.resetDrag();
          if (target.length > 0) {
            hilog.info(DOMAIN, TAG, 'emitSwap %{public}s -> %{public}s', dragId, target);
            this.onSwap(dragId, target);
          } else if (centerX >= 0 && centerX <= this.canvasW &&
            centerY >= 0 && centerY <= this.canvasH - DOTS_STRIP) {
            // 落在空白处: 移到序列末尾 (dropId 传空串), 打包时自动进最近的空闲槽位
            hilog.info(DOMAIN, TAG, 'emitMoveToEnd %{public}s (%{public}d,%{public}d)',
              dragId, Math.round(centerX), Math.round(centerY));
            this.onSwap(dragId, '');
          }
        })
        .onActionCancel((): void => {
          hilog.info(DOMAIN, TAG, 'panCancel %{public}s', t.app.id);
          this.resetDrag();
        })
    ))
  }

  build() {
    Column() {
      if (this.canvasW > 0 && this.canvasH > 0 && this.apps.length > 0) {
        Swiper() {
          ForEach(this.packPages(), (page: TileBox[], index: number): void => {
            Stack({ alignContent: Alignment.TopStart }) {
              ForEach(page, (t: TileBox): void => {
                this.Tile(t)
              }, (t: TileBox): string => t.app.id + '#' + t.col.toString() + '_' + t.row.toString())
            }
            .width('100%')
            .height('100%')
          }, (page: TileBox[], index: number): string =>
            'v' + this.layoutVersion.toString() + '-p' + index.toString() + '-' +
            this.canvasW.toFixed(0) + 'x' + this.canvasH.toFixed(0) +
            '-' + this.effRows().toString() + '-' + this.cols().toString() + '-' +
            (page.length > 0 ? page[0].app.id : 'e'))
        }
        .indicator(false)
        .loop(false)
        .duration(260)
        .onChange((index: number): void => { this.currentPage = index; })
        .layoutWeight(1)
        .width('100%')
        .id('large-tile-swiper')
        // 翻页小球条带: 独立于瓦片区 (不挤压行高), 紧贴导航栏上方。
        // key 里带上选中态, 否则 ForEach 不重跑 itemGenerator、选中小球不变色。
        Row({ space: 6 }) {
          ForEach(this.packPages(), (page: TileBox[], index: number): void => {
            Circle({ width: this.currentPage === index ? 8 : 6,
              height: this.currentPage === index ? 8 : 6 })
              .fill(this.currentPage === index ? $r('app.color.accent') : '#55FFFFFF')
          }, (page: TileBox[], index: number): string =>
            'dot-' + index.toString() + '-' + (this.currentPage === index ? 'on' : 'off'))
        }
        .height(DOTS_STRIP)
        .width('100%')
        .justifyContent(FlexAlign.Center)
      } else if (this.apps.length === 0) {
        Text('没有可显示的程序')
          .fontSize(14).fontColor($r('app.color.text_secondary'))
          .width('100%').textAlign(TextAlign.Center)
          .margin({ top: 40 })
      }
    }
    .width('100%')
    .height('100%')
    .onAreaChange((_o: Area, n: Area): void => {
      this.canvasW = Number(n.width);
      this.canvasH = Number(n.height);
    })
  }
}
