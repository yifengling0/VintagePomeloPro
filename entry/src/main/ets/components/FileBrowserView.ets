import { fileIo as fs } from '@kit.CoreFileKit';
import { promptAction } from '@kit.ArkUI';
import { hilog } from '../service/LogService';
import { IconExtractor } from '../service/IconExtractor';
import { AppCatalogService } from '../service/AppCatalogService';
import { AppSettingsStore } from '../service/AppSettingsStore';
import { LauncherAdapterRegistry } from '../service/LauncherAdapterRegistry';
import { WineEngineService } from '../service/WineEngineService';
import {
  AppDescriptor,
  AppSource,
  EngineState,
  LaunchKind,
  stableAppId
} from '../model/AppModels';

const DOMAIN = 0x0000;
const TAG = 'Files';
const DRIVE_C = '/data/storage/el2/base/files/.wine/drive_c';

interface FileEntry {
  name: string;
  isDir: boolean;
  size: number;
  mtime: number;
  icon: string;
}

interface DriveRoot {
  key: string;
  label: string;
  sub: string;
  path: string;
}

@Component
export struct FileBrowserView {
  @Prop topInsetVp: number = 0;
  @Prop bottomInsetVp: number = 0;
  @State engineState: EngineState = EngineState.STOPPED;
  @State curDir: string = DRIVE_C;
  @State entries: FileEntry[] = [];
  @State atRootList: boolean = true;
  @State private viewW: number = 0;
  private iconFail: Set<string> = new Set<string>();

  private onEngine = (state: EngineState, _message: string): void => {
    this.engineState = state;
  };

  aboutToAppear(): void {
    WineEngineService.getInstance().subscribe(this.onEngine);
    this.engineState = WineEngineService.getInstance().getState();
    this.backToRootList();
  }

  aboutToDisappear(): void {
    WineEngineService.getInstance().unsubscribe(this.onEngine);
  }

  private compactRows(): boolean {
    return this.viewW > 0 && this.viewW < 560;
  }

  private zRoot(): string {
    const z = AppCatalogService.getInstance().getWineHomeDirectory() ||
      AppCatalogService.getInstance().getLibraryRoot();
    return z.endsWith('/') ? z.substring(0, z.length - 1) : z;
  }

  private roots(): DriveRoot[] {
    const z = this.zRoot();
    const list: DriveRoot[] = [
      { key: 'C', label: 'C:\\', sub: '系统目录 (Wine 容器)', path: DRIVE_C }
    ];
    if (z.length > 0) {
      list.push({ key: 'Z', label: 'Z:\\', sub: '下载目录 (与系统共享)', path: z });
    }
    return list;
  }

  private rootOf(path: string): DriveRoot | null {
    for (const r of this.roots()) {
      if (path === r.path || path.startsWith(r.path + '/')) {
        return r;
      }
    }
    return null;
  }

  private backToRootList(): void {
    this.atRootList = true;
    this.entries = [];
  }

  private enterRoot(root: DriveRoot): void {
    this.atRootList = false;
    this.curDir = root.path;
    this.listDir(root.path);
  }

  private goUp = (): void => {
    const root = this.rootOf(this.curDir);
    if (!root || this.curDir === root.path) {
      this.backToRootList();
      return;
    }
    const slash = this.curDir.lastIndexOf('/');
    const parent = slash > 0 ? this.curDir.substring(0, slash) : root.path;
    this.curDir = parent.length > 0 ? parent : root.path;
    this.listDir(this.curDir);
  };

  private fullPath(name: string): string {
    return this.curDir + '/' + name;
  }

  private isExe(name: string): boolean {
    const lower = name.toLowerCase();
    return lower.endsWith('.exe') || lower.endsWith('.bat') || lower.endsWith('.cmd');
  }

  private rowKindOf(entry: FileEntry): string {
    if (entry.isDir) {
      return 'dir';
    }
    return this.isExe(entry.name) ? 'exe' : 'other';
  }

  private displayPath(): string {
    if (this.atRootList) {
      return '此设备';
    }
    const root = this.rootOf(this.curDir);
    if (!root) {
      return this.curDir;
    }
    const rest = this.curDir === root.path ? '' : this.curDir.substring(root.path.length).replace(/\//g, '\\');
    return root.label + rest;
  }

  private fmtSize(size: number): string {
    if (size < 0) {
      return '—';
    }
    if (size < 1024) {
      return `${size} B`;
    }
    const units: string[] = ['KB', 'MB', 'GB', 'TB'];
    let v = size;
    let u = -1;
    while (v >= 1024 && u < units.length - 1) {
      v /= 1024;
      u++;
    }
    return `${v >= 100 ? v.toFixed(0) : v.toFixed(1)} ${units[u]}`;
  }

  private fmtTime(ms: number): string {
    if (ms <= 0) {
      return '';
    }
    const d = new Date(ms);
    const p = (v: number): string => (v < 10 ? '0' : '') + v;
    return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`;
  }

  private listDir(path: string): void {
    try {
      const names = fs.listFileSync(path);
      const list: FileEntry[] = [];
      for (const n of names) {
        try {
          const st = fs.statSync(path + '/' + n);
          list.push({
            name: n,
            isDir: st.isDirectory(),
            size: st.isDirectory() ? -1 : Number(st.size),
            mtime: Number(st.mtime),
            icon: ''
          });
        } catch (_) {
          list.push({ name: n, isDir: false, size: -1, mtime: 0, icon: '' });
        }
      }
      list.sort((a: FileEntry, b: FileEntry) => {
        if (a.isDir && !b.isDir) {
          return -1;
        }
        if (!a.isDir && b.isDir) {
          return 1;
        }
        if (!a.isDir && !b.isDir) {
          const aExe = this.isExe(a.name) ? 0 : 1;
          const bExe = this.isExe(b.name) ? 0 : 1;
          return aExe - bExe || a.name.localeCompare(b.name);
        }
        return a.name.localeCompare(b.name);
      });
      this.entries = list;
      this.loadIcons();
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'listDir fail: %{public}s err=%{public}s', path, JSON.stringify(e));
      this.entries = [];
    }
  }

  private async loadIcons(): Promise<void> {
    const paths: string[] = [];
    for (const e of this.entries) {
      if (!e.isDir && this.isExe(e.name) && !e.icon && !this.iconFail.has(this.fullPath(e.name))) {
        paths.push(this.fullPath(e.name));
      }
    }
    const dir = this.curDir;
    for (let i = 0; i < paths.length; i += 4) {
      const batch = paths.slice(i, i + 4);
      await Promise.all(batch.map(async (p: string) => {
        const icon = await IconExtractor.getInstance().extract(p);
        if (this.curDir !== dir) {
          return;
        }
        if (!icon) {
          this.iconFail.add(p);
          return;
        }
        const name = p.substring(p.lastIndexOf('/') + 1);
        this.entries = this.entries.map((e: FileEntry): FileEntry =>
          e.name === name && !e.icon ?
            { name: e.name, isDir: e.isDir, size: e.size, mtime: e.mtime, icon: icon } : e);
      }));
    }
  }

  private descriptorFor(path: string): AppDescriptor {
    const slash = Math.max(path.lastIndexOf('/'), path.lastIndexOf('\\'));
    const name = slash >= 0 ? path.substring(slash + 1) : path;
    const dir = slash >= 0 ? path.substring(0, slash) : '';
    return {
      id: stableAppId(AppSource.DOWNLOAD, path),
      title: name,
      source: AppSource.DOWNLOAD,
      directory: dir,
      coverPath: '',
      backgroundPath: '',
      available: true,
      launchTarget: {
        kind: LaunchKind.WINE_EXE,
        executable: path,
        arguments: [],
        workingDirectory: dir
      },
      displayModeOverride: null
    };
  }

  private async launchExe(path: string): Promise<void> {
    const settings = AppSettingsStore.getInstance().getGlobalSettings();
    const root = AppCatalogService.getInstance().getWineHomeDirectory();
    if (!root) {
      promptAction.showToast({ message: '下载目录尚未就绪，请先扫描 Windows 程序' });
      return;
    }
    try {
      const session = await LauncherAdapterRegistry.getInstance()
        .launch(this.descriptorFor(path), settings, root);
      if (!session) {
        const msg = WineEngineService.getInstance().getMessage();
        promptAction.showToast({
          message: msg === 'Wine 引擎已就绪' ? '启动失败，请稍后重试' : msg
        });
      }
    } catch (_) {
      promptAction.showToast({ message: '程序启动异常，请稍后重试' });
    }
  }

  private gamesRoot(): string {
    return AppCatalogService.getInstance().getGamesDirectory();
  }

  private inGames(path: string): boolean {
    const games = this.gamesRoot();
    if (!games) {
      return false;
    }
    return path === games || path.startsWith(games + '/');
  }

  private addToLibrary(path: string): void {
    if (this.inGames(path)) {
      promptAction.showToast({ message: '已在 Z:\\games 下，请到 Windows 程序页下拉扫描' });
      return;
    }
    promptAction.showToast({ message: '请将程序放到 Download/.../games 后再扫描入库' });
  }

  @Builder
  fileMenu(entry: FileEntry) {
    Menu() {
      if (this.rowKindOf(entry) === 'exe') {
        MenuItem({ content: '启动' })
          .onChange(() => {
            this.launchExe(this.fullPath(entry.name));
          })
        MenuItem({ content: '加入应用库' })
          .onChange(() => {
            this.addToLibrary(this.fullPath(entry.name));
          })
      }
    }
  }

  @Builder
  fileRow(entry: FileEntry) {
    Row() {
      if (this.rowKindOf(entry) === 'exe' && entry.icon) {
        Image(`file://${entry.icon}`)
          .width(26).height(26)
          .margin({ right: 8 })
          .objectFit(ImageFit.Contain)
      } else {
        SymbolGlyph(this.rowKindOf(entry) === 'dir' ? $r('sys.symbol.folder_fill') :
          this.rowKindOf(entry) === 'exe' ? $r('sys.symbol.square_grid_2x2') : $r('sys.symbol.doc'))
          .fontSize(20)
          .fontColor([this.rowKindOf(entry) === 'dir' ? $r('app.color.accent') :
            this.rowKindOf(entry) === 'exe' ? $r('app.color.accent') : $r('app.color.text_secondary')])
          .width(28)
          .margin({ right: 8 })
      }
      Text(entry.name)
        .fontSize(14)
        .fontColor(this.rowKindOf(entry) === 'other' ? $r('app.color.text_secondary') : $r('app.color.text_primary'))
        .layoutWeight(1)
        .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
      if (!this.compactRows()) {
        Text(this.rowKindOf(entry) === 'dir' ? '—' : this.fmtSize(entry.size))
          .width(72).fontSize(11)
          .fontColor($r('app.color.text_secondary'))
          .textAlign(TextAlign.End).maxLines(1)
        Text(this.fmtTime(entry.mtime))
          .width(126).fontSize(11)
          .fontColor($r('app.color.text_secondary'))
          .textAlign(TextAlign.End).maxLines(1)
      }
      Row() {
        if (this.rowKindOf(entry) === 'exe') {
          Button('启动')
            .height(32)
            .padding({ left: 8, right: 8 })
            .fontSize(12)
            .fontColor($r('app.color.text_primary'))
            .backgroundColor($r('app.color.selected_background'))
            .onClick(() => this.launchExe(this.fullPath(entry.name)))
        } else if (this.rowKindOf(entry) === 'dir') {
          SymbolGlyph($r('sys.symbol.chevron_right'))
            .fontSize(16)
            .fontColor([$r('app.color.text_secondary')])
        }
      }
      .width(this.compactRows() ? 64 : 88)
      .margin({ left: this.compactRows() ? 6 : 14 })
      .justifyContent(FlexAlign.End)
      .alignItems(VerticalAlign.Center)
    }
    .width('100%').height(44).padding({ left: 14, right: 14 })
    .alignItems(VerticalAlign.Center)
  }

  @Builder
  rootRow(root: DriveRoot) {
    Row() {
      SymbolGlyph(root.key === 'C' ? $r('sys.symbol.archivebox') : $r('sys.symbol.folder_fill'))
        .fontSize(22)
        .fontColor([$r('app.color.accent')])
        .width(30)
      Column({ space: 2 }) {
        Text(root.label)
          .fontColor($r('app.color.text_primary'))
        Text(root.sub)
          .fontSize(11)
          .fontColor($r('app.color.text_secondary'))
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)
      .margin({ left: 6 })
      SymbolGlyph($r('sys.symbol.chevron_right'))
        .fontSize(16)
        .fontColor([$r('app.color.text_secondary')])
    }
    .width('100%').height(56).padding({ left: 14, right: 14 })
    .alignItems(VerticalAlign.Center)
    .backgroundColor($r('app.color.card_background'))
    .onClick(() => {
      this.enterRoot(root);
    })
  }

  build() {
    Column() {
      Row({ space: 8 }) {
        if (!this.atRootList) {
          Button({ type: ButtonType.Circle }) {
            SymbolGlyph($r('sys.symbol.chevron_up'))
              .fontSize(18)
              .fontColor([$r('app.color.text_primary')])
          }
          .width(36).height(36)
          .backgroundColor($r('app.color.search_background'))
          .onClick((): void => { this.goUp(); })
        }
        Text(this.displayPath())
          .fontSize(14)
          .fontColor($r('app.color.text_secondary'))
          .height(36)
          .layoutWeight(1)
          .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
      }
      .width('100%').padding({ left: 14, right: 14, top: 8, bottom: 6 })
      .alignItems(VerticalAlign.Center)

      if (this.atRootList) {
        List({ space: 1 }) {
          ForEach(this.roots(), (root: DriveRoot): void => {
            ListItem() {
              this.rootRow(root)
            }
          }, (root: DriveRoot): string => root.key)
        }
        .width('100%').layoutWeight(1)
      } else {
        List({ space: 1 }) {
          ForEach(this.entries, (entry: FileEntry): void => {
            ListItem() {
              this.fileRow(entry)
            }
            .bindContextMenu(this.fileMenu(entry), ResponseType.LongPress)
            .onClick((): void => {
              if (entry.isDir) {
                this.curDir = this.fullPath(entry.name);
                this.listDir(this.curDir);
              }
            })
          }, (entry: FileEntry): string => entry.name + (entry.isDir ? '/d' : ''))
        }
        .width('100%').layoutWeight(1)
      }
    }
    .width('100%')
    .layoutWeight(1)
    .padding({ top: this.topInsetVp, bottom: this.bottomInsetVp })
    .onAreaChange((_oldValue: Area, area: Area): void => {
      this.viewW = area.width as number;
    })
  }
}
