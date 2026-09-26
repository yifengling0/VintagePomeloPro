import { AppDescriptor } from '../model/AppModels';

/** 单个瓦片的布局盒 (页面内绝对定位)。 */
export interface TileBox {
  app: AppDescriptor;
  col: number;
  row: number;
  colSpan: number;
  rowSpan: number;
}

const GAP: number = 10;
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
  apps: AppDescriptor[] = [];
  /** 期望行数 (调用方已按设备档位收口)。 */
  @Prop rows: number = 3;
  runningIds: Set<string> = new Set<string>();
  onLaunch: (app: AppDescriptor) => void = (_a: AppDescriptor): void => {};
  onLongPress: (app: AppDescriptor) => void = (_a: AppDescriptor): void => {};
  @State private canvasW: number = 0;
  @State private canvasH: number = 0;

  private effRows(): number {
    return Math.max(MIN_ROWS, Math.min(MAX_ROWS, Math.floor(this.rows)));
  }

  private cellH(): number {
    const rows = this.effRows();
    return (this.canvasH - (rows - 1) * GAP) / rows;
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

  @Builder
  private Tile(t: TileBox) {
    Stack({ alignContent: Alignment.TopEnd }) {
      if (this.coverOf(t.app).length > 0) {
        Image(this.coverOf(t.app))
          .objectFit(ImageFit.Cover)
          .draggable(false)
          .width('100%')
          .height('100%')
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
    .border({ width: 1, color: '#22FFFFFF' })
    .onClick((): void => { this.onLaunch(t.app); })
    .gesture(LongPressGesture({ repeat: false })
      .onAction((): void => { this.onLongPress(t.app); }))
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
            'p' + index.toString() + '-' + this.canvasW.toFixed(0) + 'x' + this.canvasH.toFixed(0) +
            '-' + this.effRows().toString() + '-' + this.cols().toString() + '-' +
            (page.length > 0 ? page[0].app.id : 'e'))
        }
        .indicator(new DotIndicator()
          .itemWidth(6).itemHeight(6)
          .selectedItemWidth(8).selectedItemHeight(8)
          .selectedColor($r('app.color.accent')))
        .loop(false)
        .duration(260)
        .width('100%')
        .height('100%')
        .id('large-tile-swiper')
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
