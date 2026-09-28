import { AppDescriptor, AppSource } from '../model/AppModels';
import { builtinCoverResource } from '../model/BuiltinArtwork';
import { GameArtworkService } from '../service/GameArtworkService';
import { skinColor } from '../service/SkinColors';

const GAP: number = 10;
const DOTS_HEIGHT: number = 22;
const PATTERN: number[][] = [
  [2, 2], [1, 1], [1, 1], [2, 1], [1, 1],
  [1, 2], [1, 1], [2, 1], [1, 1], [1, 1]
];

interface TileSlot {
  row: number;
  col: number;
}

/** Geometry is shared with the launcher's spatial keyboard and gamepad navigation. */
export interface BentoTileBox {
  app: AppDescriptor;
  row: number;
  col: number;
  colSpan: number;
  rowSpan: number;
  x: number;
  y: number;
  width: number;
  height: number;
}

@Component
export struct ConsoleBentoGrid {
  @Prop @Watch('rebuildLayout') apps: AppDescriptor[] = [];
  @Prop @Watch('locateSelected') selectedId: string = '';
  @Prop @Watch('clearArtFailures') artworkRevision: number = 0;
  @Prop @Watch('resetDrag') interactionEpoch: number = 0;
  @Prop runningIds: Set<string> = new Set<string>();
  artworkPath: (app: AppDescriptor) => string = (_app: AppDescriptor): string => '';
  onTileFocus: (app: AppDescriptor) => void = (_app: AppDescriptor): void => {};
  onOpen: (app: AppDescriptor) => void = (_app: AppDescriptor): void => {};
  onLongPress: (app: AppDescriptor) => void = (_app: AppDescriptor): void => {};
  onSwap: (dragId: string, dropId: string) => void = (_drag: string, _drop: string): void => {};
  onInsertAt: (dragId: string, visibleIndex: number) => void = (_drag: string, _index: number): void => {};
  onPagesChanged: (pages: BentoTileBox[][]) => void = (_pages: BentoTileBox[][]): void => {};

  @State private canvasW: number = 0;
  @State private canvasH: number = 0;
  @State private pages: BentoTileBox[][] = [];
  @State private currentPage: number = 0;
  @State private pageAnimationMs: number = 260;
  @State private layoutRevision: number = 0;
  @State private liftedId: string = '';
  @State private dragId: string = '';
  @State private dragDX: number = 0;
  @State private dragDY: number = 0;
  @State private dropTargetId: string = '';
  @State private failedArtPaths: string[] = [];
  private layoutKey: string = '';
  private reflowToken: number = 0;
  private suppressClickUntil: number = 0;

  private clearArtFailures(): void {
    this.failedArtPaths = [];
  }

  private rowCount(): number {
    const height = this.canvasH - DOTS_HEIGHT;
    if (height < 380) return 2;
    if (height < 760) return 3;
    return 4;
  }

  private cellSize(): number {
    const rows = this.rowCount();
    // ArkUI area sizes are in vp, so this adapts to device density and
    // resizable windows without using fixed physical-pixel thresholds.
    return Math.max(1, Math.min(220, (this.canvasW - 2 * GAP) / 3,
      (this.canvasH - DOTS_HEIGHT - (rows - 1) * GAP) / rows));
  }

  private columnCount(): number {
    const cell = this.cellSize();
    return Math.max(3, Math.min(8, Math.floor((this.canvasW + GAP) / (cell + GAP))));
  }

  private findSlot(used: boolean[][], width: number, height: number): TileSlot | null {
    for (let row = 0; row <= used.length - height; row++) {
      for (let col = 0; col <= used[0].length - width; col++) {
        let fits = true;
        for (let y = row; y < row + height && fits; y++) {
          for (let x = col; x < col + width; x++) {
            if (used[y][x]) { fits = false; break; }
          }
        }
        if (fits) return { row: row, col: col };
      }
    }
    return null;
  }

  private makeUsed(rows: number, cols: number): boolean[][] {
    const used: boolean[][] = [];
    for (let row = 0; row < rows; row++) {
      const line: boolean[] = [];
      for (let col = 0; col < cols; col++) line.push(false);
      used.push(line);
    }
    return used;
  }

  private packPages(): BentoTileBox[][] {
    if (this.canvasW <= 0 || this.canvasH <= DOTS_HEIGHT || this.apps.length === 0) return [];
    const rows = this.rowCount();
    const cols = this.columnCount();
    const cell = this.cellSize();
    const startX = Math.max(0, (this.canvasW - cols * cell - (cols - 1) * GAP) / 2);
    const startY = Math.max(0, (this.canvasH - DOTS_HEIGHT - rows * cell - (rows - 1) * GAP) / 2);
    const pages: BentoTileBox[][] = [[]];
    let used = this.makeUsed(rows, cols);
    for (let index = 0; index < this.apps.length; index++) {
      const span = PATTERN[index % PATTERN.length];
      let width = span[0];
      let height = span[1];
      let slot = this.findSlot(used, width, height);
      if (!slot) {
        width = 1;
        height = 1;
        slot = this.findSlot(used, width, height);
      }
      if (!slot) {
        pages.push([]);
        used = this.makeUsed(rows, cols);
        width = Math.min(span[0], cols);
        height = Math.min(span[1], rows);
        slot = this.findSlot(used, width, height);
      }
      if (!slot) continue;
      for (let y = slot.row; y < slot.row + height; y++) {
        for (let x = slot.col; x < slot.col + width; x++) used[y][x] = true;
      }
      pages[pages.length - 1].push({
        app: this.apps[index], row: slot.row, col: slot.col,
        colSpan: width, rowSpan: height,
        x: startX + slot.col * (cell + GAP),
        y: startY + slot.row * (cell + GAP),
        width: width * cell + (width - 1) * GAP,
        height: height * cell + (height - 1) * GAP
      });
    }
    // Center independent row groups separately. A short final row otherwise
    // leaves its only wide tile at the left despite a centered page outline.
    for (let pageIndex = 0; pageIndex < pages.length; pageIndex++) {
      const page = pages[pageIndex];
      if (page.length === 0) continue;
      let minY = this.canvasH;
      let maxY = 0;
      for (let tileIndex = 0; tileIndex < page.length; tileIndex++) {
        const tile = page[tileIndex];
        minY = Math.min(minY, tile.y);
        maxY = Math.max(maxY, tile.y + tile.height);
      }
      const shiftY = (this.canvasH - DOTS_HEIGHT - maxY - minY) / 2;
      for (let tileIndex = 0; tileIndex < page.length; tileIndex++) {
        page[tileIndex].y += shiftY;
      }
      const grouped = new Set<number>();
      for (let tileIndex = 0; tileIndex < page.length; tileIndex++) {
        if (grouped.has(tileIndex)) continue;
        const group: number[] = [tileIndex];
        grouped.add(tileIndex);
        for (let cursor = 0; cursor < group.length; cursor++) {
          const member = page[group[cursor]];
          for (let otherIndex = 0; otherIndex < page.length; otherIndex++) {
            if (grouped.has(otherIndex)) continue;
            const other = page[otherIndex];
            if (member.row < other.row + other.rowSpan &&
              other.row < member.row + member.rowSpan) {
              grouped.add(otherIndex);
              group.push(otherIndex);
            }
          }
        }
        let minX = this.canvasW;
        let maxX = 0;
        for (let index = 0; index < group.length; index++) {
          const tile = page[group[index]];
          minX = Math.min(minX, tile.x);
          maxX = Math.max(maxX, tile.x + tile.width);
        }
        const shiftX = (this.canvasW - maxX - minX) / 2;
        for (let index = 0; index < group.length; index++) page[group[index]].x += shiftX;
      }
    }
    return pages;
  }

  private rebuildLayout(): void {
    const key = this.canvasW.toFixed(0) + 'x' + this.canvasH.toFixed(0) + ':' +
      JSON.stringify(this.apps.map((app: AppDescriptor): string =>
        app.id + '\n' + app.title + '\n' + app.coverPath + '\n' + (app.backgroundPath ?? '')));
    if (key === this.layoutKey) return;
    this.layoutKey = key;
    this.pageAnimationMs = 0;
    const token = ++this.reflowToken;
    this.resetDrag();
    this.pages = this.packPages();
    this.layoutRevision++;
    this.locateSelected();
    this.onPagesChanged(this.pages);
    setTimeout((): void => {
      if (token === this.reflowToken) this.pageAnimationMs = 260;
    }, 120);
  }

  aboutToDisappear(): void {
    this.reflowToken++;
    this.resetDrag();
  }

  private locateSelected(): void {
    for (let page = 0; page < this.pages.length; page++) {
      if (this.pages[page].some((tile: BentoTileBox): boolean => tile.app.id === this.selectedId)) {
        if (this.currentPage !== page) this.currentPage = page;
        return;
      }
    }
    if (this.currentPage >= this.pages.length) this.currentPage = 0;
  }

  private resetDrag(): void {
    this.liftedId = '';
    this.dragId = '';
    this.dragDX = 0;
    this.dragDY = 0;
    this.dropTargetId = '';
  }

  private dropTarget(centerX: number, centerY: number, dragId: string): string {
    const page = this.pages[this.currentPage] ?? [];
    for (let index = 0; index < page.length; index++) {
      const tile = page[index];
      if (tile.app.id !== dragId && centerX >= tile.x && centerX <= tile.x + tile.width &&
        centerY >= tile.y && centerY <= tile.y + tile.height) return tile.app.id;
    }
    return '';
  }

  private blankInsertionIndex(centerX: number, centerY: number, dragId: string): number {
    const page = this.pages[this.currentPage] ?? [];
    let pageStart = 0;
    for (let index = 0; index < this.currentPage; index++) pageStart += this.pages[index].length;
    let nearest = -1;
    let distance = Number.MAX_SAFE_INTEGER;
    for (let index = 0; index < page.length; index++) {
      const tile = page[index];
      if (tile.app.id === dragId) continue;
      const dx = Math.max(tile.x - centerX, 0, centerX - tile.x - tile.width);
      const dy = Math.max(tile.y - centerY, 0, centerY - tile.y - tile.height);
      const score = dx * dx + dy * dy;
      if (score < distance) { distance = score; nearest = index; }
    }
    if (nearest < 0) return pageStart + page.length;
    const tile = page[nearest];
    const tileCenterY = tile.y + tile.height / 2;
    const before = centerY < tileCenterY ||
      (Math.abs(centerY - tileCenterY) < tile.height / 3 && centerX < tile.x + tile.width / 2);
    return pageStart + nearest + (before ? 0 : 1);
  }

  private backgroundOf(app: AppDescriptor): string {
    if (this.artworkRevision < 0) return '';
    return this.artworkPath(app);
  }

  private artOf(tile: BentoTileBox): string {
    const background = this.backgroundOf(tile.app);
    const cover = tile.app.coverPath;
    const preferred = tile.colSpan > tile.rowSpan && background ? background : (cover || background);
    if (preferred && !this.failedArtPaths.includes(preferred)) return preferred;
    const fallback = preferred === background ? cover : background;
    return fallback && !this.failedArtPaths.includes(fallback) ? fallback : '';
  }

  private imageSource(path: string): string {
    return path.indexOf('://') > 0 ? path : 'file://' + path;
  }

  @Builder
  private Tile(tile: BentoTileBox) {
    Stack({ alignContent: Alignment.TopEnd }) {
      if (this.artOf(tile)) {
        if (tile.colSpan !== tile.rowSpan &&
          !(tile.colSpan > tile.rowSpan && this.artOf(tile) === this.backgroundOf(tile.app))) {
          Image(this.imageSource(this.artOf(tile)))
            .width('100%').height('100%').objectFit(ImageFit.Cover)
            .blur(18).scale({ x: 1.12, y: 1.12 }).draggable(false)
        }
        Image(this.imageSource(this.artOf(tile)))
          .width('100%').height('100%')
          .objectFit(tile.colSpan === tile.rowSpan ||
            (tile.colSpan > tile.rowSpan && this.artOf(tile) === this.backgroundOf(tile.app)) ?
            ImageFit.Cover : ImageFit.Contain)
          .draggable(false)
          .onError((): void => {
            const failed = this.artOf(tile);
            if (failed && !this.failedArtPaths.includes(failed)) {
              this.failedArtPaths = this.failedArtPaths.concat(failed);
            }
          })
      } else if (tile.app.source === AppSource.BUILTIN) {
        Image(builtinCoverResource(tile.app.id))
          .width('100%').height('100%').objectFit(ImageFit.Cover).draggable(false)
      } else {
        Text(tile.app.title.length > 0 ? tile.app.title.substring(0, 1) : '?')
          .fontSize(38).fontWeight(FontWeight.Bold).fontColor(skinColor('accent'))
          .width('100%').height('100%').textAlign(TextAlign.Center)
      }
      Column() {
        Text(tile.app.title)
          .fontSize(tile.colSpan > 1 ? 15 : 12).fontWeight(FontWeight.Medium)
          .fontColor('#FFFFFFFF').maxLines(tile.rowSpan > 1 ? 2 : 1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .width('100%')
      }
      .width('100%').padding({ left: 10, right: 10, top: 12, bottom: 10 })
      .backgroundColor('#99000000')
      .alignItems(HorizontalAlign.Start)
      .position({ x: 0, y: Math.max(0, tile.height - (tile.rowSpan > 1 ? 55 : 40)) })
      if (this.runningIds.has(tile.app.id)) {
        Circle({ width: 10, height: 10 }).fill('#4ADE80').margin({ top: 9, right: 9 })
      }
    }
    .width(tile.width).height(tile.height)
    .position({ x: tile.x, y: tile.y })
    .borderRadius(14).clip(true)
    .backgroundColor(skinColor('card_background'))
    .border({ width: this.dropTargetId === tile.app.id || this.selectedId === tile.app.id ? 2 : 1,
      color: this.dropTargetId === tile.app.id || this.selectedId === tile.app.id ?
        skinColor('accent') : '#33FFFFFF' })
    .translate(this.dragId === tile.app.id ? { x: this.dragDX, y: this.dragDY } : { x: 0, y: 0 })
    .scale(this.dragId === tile.app.id ? { x: 1.05, y: 1.05 } : { x: 1, y: 1 })
    .zIndex(this.dragId === tile.app.id ? 100 : 0)
    .shadow({ radius: this.dragId === tile.app.id ? 22 : 8,
      color: '#55000000', offsetX: 0, offsetY: this.dragId === tile.app.id ? 8 : 4 })
    .id('bento-tile-' + tile.app.id)
    .focusable(true)
    .accessibilityText(tile.app.title)
    .onFocus((): void => this.onTileFocus(tile.app))
    .onClick((): void => {
      if (!this.liftedId && !this.dragId && Date.now() >= this.suppressClickUntil) this.onOpen(tile.app);
    })
    .onTouch((event: TouchEvent): void => {
      if (event.type !== TouchType.Up || this.liftedId !== tile.app.id || this.dragId) return;
      // A long press released without a pan keeps the existing context menu.
      setTimeout((): void => {
        if (this.liftedId !== tile.app.id || this.dragId) return;
        this.resetDrag();
        this.onLongPress(tile.app);
      }, 0);
    })
    .onVisibleAreaChange([0.1], (visible: boolean): void => {
      if (visible) GameArtworkService.getInstance().enqueueIfMissing(tile.app);
    })
    .gesture(GestureGroup(GestureMode.Sequence,
      LongPressGesture({ repeat: false, duration: 280 })
        .onAction((): void => {
          if (!this.dragId) {
            this.liftedId = tile.app.id;
            this.suppressClickUntil = Date.now() + 300;
          }
        })
        .onActionCancel((): void => {
          if (this.dragId !== tile.app.id) this.liftedId = '';
        }),
      PanGesture({ fingers: 1, distance: 1 })
        .onActionStart((): void => {
          if (this.liftedId === tile.app.id && !this.dragId) this.dragId = tile.app.id;
        })
        .onActionUpdate((event: GestureEvent): void => {
          if (this.dragId !== tile.app.id) return;
          this.dragDX = event.offsetX;
          this.dragDY = event.offsetY;
          this.dropTargetId = this.dropTarget(tile.x + tile.width / 2 + this.dragDX,
            tile.y + tile.height / 2 + this.dragDY, tile.app.id);
        })
        .onActionEnd((): void => {
          if (this.dragId !== tile.app.id) return;
          if (Math.abs(this.dragDX) + Math.abs(this.dragDY) < 12) {
            this.suppressClickUntil = Date.now() + 300;
            this.resetDrag();
            this.onLongPress(tile.app);
            return;
          }
          const centerX = tile.x + tile.width / 2 + this.dragDX;
          const centerY = tile.y + tile.height / 2 + this.dragDY;
          const target = this.dropTarget(centerX, centerY, tile.app.id);
          this.suppressClickUntil = Date.now() + 300;
          if (target) {
            this.resetDrag();
            this.onSwap(tile.app.id, target);
          } else if (centerX >= 0 && centerX <= this.canvasW &&
            centerY >= 0 && centerY <= this.canvasH - DOTS_HEIGHT) {
            const index = this.blankInsertionIndex(centerX, centerY, tile.app.id);
            this.resetDrag();
            this.onInsertAt(tile.app.id, index);
          } else {
            this.resetDrag();
          }
        })
        .onActionCancel((): void => this.resetDrag())
    ).onCancel((): void => this.resetDrag()))
  }

  build() {
    Column() {
      if (this.pages.length > 0) {
        Swiper() {
          ForEach(this.pages, (page: BentoTileBox[], index: number): void => {
            Stack({ alignContent: Alignment.TopStart }) {
              ForEach(page, (tile: BentoTileBox): void => {
                this.Tile(tile)
              },
                (tile: BentoTileBox): string => tile.app.id + '-' + tile.col.toString() + '-' + tile.row.toString())
            }.width('100%').height('100%')
          }, (page: BentoTileBox[], index: number): string =>
            this.layoutRevision.toString() + '-' + index.toString())
        }
        .index(this.currentPage)
        .indicator(false).loop(false).duration(this.pageAnimationMs)
        .onChange((index: number): void => {
          this.currentPage = index;
          const page = this.pages[index] ?? [];
          if (page.length > 0 && !page.some((tile: BentoTileBox): boolean => tile.app.id === this.selectedId)) {
            this.onTileFocus(page[0].app);
          }
        })
        .layoutWeight(1).width('100%').id('console-bento-swiper')
        Row({ space: 6 }) {
          ForEach(this.pages, (_page: BentoTileBox[], index: number): void => {
            Circle({ width: this.currentPage === index ? 8 : 6,
              height: this.currentPage === index ? 8 : 6 })
              .fill(this.currentPage === index ? skinColor('accent') : '#77FFFFFF')
          }, (_page: BentoTileBox[], index: number): string =>
            'dot-' + index.toString() + '-' + this.currentPage.toString())
        }
        .width('100%').height(DOTS_HEIGHT).justifyContent(FlexAlign.Center)
      }
    }
    .width('100%').height('100%')
    .onAreaChange((_old: Area, area: Area): void => {
      const width = Number(area.width);
      const height = Number(area.height);
      if (Math.abs(width - this.canvasW) <= 1 && Math.abs(height - this.canvasH) <= 1) return;
      this.canvasW = width;
      this.canvasH = height;
      this.rebuildLayout();
    })
  }
}
