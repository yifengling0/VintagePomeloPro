import { common } from '@kit.AbilityKit';
import { promptAction, router, window } from '@kit.ArkUI';
import { DeviceCapabilityPolicy } from '../common/DeviceCapabilityPolicy';
import { applyStatusOnlyImmersiveShell, getStatusOnlyWindowInsets,
  observeStatusOnlyWindowInsets } from '../common/StatusOnlyImmersiveShell';
import { AppDescriptor } from '../model/AppModels';
import { AppCatalogService } from '../service/AppCatalogService';
import { skinColor } from '../service/SkinColors';
import { SteamLibraryService } from '../steam/SteamLibrary';
import { SteamMatch } from '../steam/SteamMatch';
import { SteamLibraryGame } from '../steam/vendor/models/SteamModels';
import { SteamGameArtwork } from '../components/SteamGameArtwork';
import { SteamDownloadService, SteamDownloadProgress } from '../steam/SteamDownloadService';
import { SteamDepotWriter } from '../steam/SteamDepotFileSink';
import { SteamService } from '../steam/SteamService';
import { PicsAppInfo } from '../steam/SteamContentProtocol';
import { hilog } from '@kit.PerformanceAnalysisKit';

interface SteamGameDetailParams {
  appId?: string;
}

/** Steam library details describe ownership; launching requires a real local game. */
@Entry
@Component
struct SteamGameDetail {
  private context: common.UIAbilityContext = getContext(this) as common.UIAbilityContext;
  @State private game: SteamLibraryGame | null = null;
  @State private localApp: AppDescriptor | null = null;
  @State private availableWidth: number = 360;
  @State private safeTop: number = 0;
  @State private safeLeft: number = 0;
  @State private safeRight: number = 0;
  @State private safeBottom: number = 0;
  @State private downloadActive: boolean = false;
  @State private downloadPhase: string = '';
  @State private downloadMessage: string = '';
  @State private downloadPercent: number = 0;
  private pageAlive: boolean = false;
  private stopInsetObserver: (() => void) | null = null;

  aboutToAppear(): void {
    this.pageAlive = true;
    const params = router.getParams() as SteamGameDetailParams;
    const games = SteamLibraryService.getInstance().cachedGames();
    this.game = games.find((item: SteamLibraryGame): boolean => item.appId === params.appId) ?? null;
    this.refreshLocalApp();
    window.getLastWindow(this.context).then(async (pageWindow: window.Window): Promise<void> => {
      if (!this.pageAlive) return;
      DeviceCapabilityPolicy.applyLauncherLandscapeRotation(pageWindow, 'SteamGameDetail');
      await applyStatusOnlyImmersiveShell(pageWindow);
      if (!this.pageAlive) return;
      const updateInsets = (): void => {
        const insets = getStatusOnlyWindowInsets(pageWindow);
        this.safeTop = insets.top;
        this.safeLeft = insets.left;
        this.safeRight = insets.right;
        this.safeBottom = insets.bottom;
      };
      updateInsets();
      this.stopInsetObserver = observeStatusOnlyWindowInsets(pageWindow, updateInsets);
    }).catch((): void => {});
  }

  aboutToDisappear(): void {
    this.pageAlive = false;
    if (this.stopInsetObserver) this.stopInsetObserver();
    this.stopInsetObserver = null;
  }

  onPageShow(): void {
    this.refreshLocalApp();
  }

  private refreshLocalApp(): void {
    const game = this.game;
    if (!game) return;
    const games = SteamLibraryService.getInstance().cachedGames();
    const matches = AppCatalogService.getInstance().getApps().filter((app: AppDescriptor): boolean => {
      return SteamMatch.titlesMatch(app.title, game.name)
        && SteamMatch.bestMatch(app.title, games)?.appId === game.appId;
    });
    this.localApp = matches.length === 1 ? matches[0] : null;
  }

  private gameTitle(): string {
    return this.game ? (this.game.name || 'App ' + this.game.appId) : '游戏不可用';
  }

  private playHours(): string {
    const seconds = this.game?.playtimeForeverSeconds ?? 0;
    return seconds > 0 ? (seconds / 3600).toFixed(1) + ' 小时' : '未游玩';
  }

  private lastPlayed(): string {
    const seconds = this.game?.lastPlayedEpochSeconds ?? 0;
    return seconds > 0 ? new Date(seconds * 1000).toLocaleDateString() : '从未游玩';
  }

  private openStore(): void {
    const appId = this.game?.appId ?? '';
    if (!/^\d+$/.test(appId)) return;
    void this.context.openLink('https://store.steampowered.com/app/' + appId + '/')
      .catch((): void => { promptAction.showToast({ message: '无法打开 Steam 商店，请检查浏览器和网络' }); });
  }

  private openLocalSettings(): void {
    const app = this.localApp;
    if (!app) return;
    void router.pushUrl({ url: 'pages/AppSettings', params: { appId: app.id } });
  }

  @Builder
  private TopBar() {
    Row({ space: 10 }) {
      Button({ type: ButtonType.Circle }) {
        SymbolGlyph($r('sys.symbol.chevron_left')).fontSize(22)
          .fontColor([skinColor('text_primary')])
      }
      .width(32).height(32).backgroundColor(skinColor('search_background'))
      .onClick((): void => { router.back(); })
      .accessibilityText('返回游戏库').id('steam-game-back')
      Text('游戏详情').fontSize(15).fontWeight(FontWeight.Medium)
        .fontColor(skinColor('text_primary')).layoutWeight(1)
    }
    .width('100%').height(40)
  }

  @Builder
  private Artwork() {
    SteamGameArtwork({ appId: this.game?.appId ?? '', imageUrl: this.game?.logoUrl ?? '' })
    .width('100%').aspectRatio(460 / 215).borderRadius(14).clip(true)
    .id('steam-game-artwork')
  }

  @Builder
  private Stat(label: string, value: string) {
    Column({ space: 6 }) {
      Text(label).fontSize(12).fontColor(skinColor('text_secondary'))
      Text(value).fontSize(17).fontWeight(FontWeight.Medium)
        .fontColor(skinColor('text_primary')).width('100%')
    }
    .width('100%').alignItems(HorizontalAlign.Start).padding(14).borderRadius(12)
    .backgroundColor(skinColor('search_background'))
  }

  @Builder
  private GameInfo() {
    Column({ space: 14 }) {
      Text(this.gameTitle()).fontSize(24).fontWeight(FontWeight.Bold)
        .fontColor(skinColor('text_primary')).width('100%').id('steam-game-detail-title')
      this.Stat('累计游玩', this.playHours())
      this.Stat('最近游玩', this.lastPlayed())
      this.DownloadSection()
      Button('查看 Steam 商店').width('100%').height(42).borderRadius(12)
        .backgroundColor(skinColor('accent')).fontColor('#FFFFFFFF')
        .onClick((): void => { this.openStore(); }).id('steam-game-store')
      if (this.localApp) {
        Button('本地游戏设置').width('100%').height(42).borderRadius(12)
          .backgroundColor(skinColor('selected_background')).fontColor(skinColor('text_primary'))
          .onClick((): void => { this.openLocalSettings(); }).id('steam-game-local-settings')
      } else {
        Text('从首页添加本地游戏后可启动。下载完成后重扫描会自动出现在首页。')
          .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
      }
    }
    .width('100%').alignItems(HorizontalAlign.Start)
  }

  /** W6: 下载区（按钮 + 进度）。只下已拥有内容（Steam license 服务端校验）。 */
  @Builder
  private DownloadSection() {
    Column({ space: 10 }) {
      if (!this.downloadActive) {
        Button(this.downloadMessage.length > 0 ? '重试下载' : '下载到本地（库内游戏）')
          .width('100%').height(42).borderRadius(12)
          .backgroundColor(skinColor('selected_background')).fontColor(skinColor('text_primary'))
          .onClick((): void => { this.startDownload(); })
          .id('steam-game-download')
      } else {
        Text(this.downloadPhase + (this.downloadMessage.length > 0 ? ' · ' + this.downloadMessage : ''))
          .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
          .id('steam-download-status')
        Progress({ value: this.downloadPercent, total: 100, type: ProgressType.Linear })
          .width('100%').color(skinColor('accent'))
          .id('steam-download-progress')
      }
      if (this.downloadMessage.length > 0 && !this.downloadActive) {
        Text(this.downloadMessage).fontSize(11).fontColor(skinColor('text_secondary')).width('100%')
      }
    }
    .width('100%').alignItems(HorizontalAlign.Start)
  }

  private startDownload(): void {
    const game = this.game;
    if (!game || this.downloadActive) return;
    const service = SteamService.getInstance();
    if (service.steamId64.length === 0) {
      this.downloadMessage = '请先在「账号登录」完成登录';
      return;
    }
    this.downloadActive = true;
    this.downloadPhase = '建立下载会话';
    this.downloadMessage = '';
    this.downloadPercent = 0;
    const downloader = SteamDownloadService.getInstance();
    downloader.onProgress((progress: SteamDownloadProgress): void => {
      this.downloadPhase = progress.phase;
      this.downloadMessage = progress.message;
      this.downloadPercent = progress.totalBytes > 0
        ? Math.min(100, Math.floor(progress.doneBytes * 100 / progress.totalBytes)) : 0;
    });
    const writer = new SteamDepotWriter();
    const folderName = game.name.length > 0 ? game.name : ('App ' + game.appId);
    this.downloadMessage = '';
    service.getRefreshToken()
      .then((refreshToken: string): Promise<void> => {
        if (refreshToken.length === 0) {
          throw new Error('登录凭据不可用，请重新登录');
        }
        return downloader.ensureSession(service.accountName, refreshToken, service.steamId64);
      })
      .then((): Promise<PicsAppInfo> => {
        this.downloadPhase = '获取 depot 信息';
        return downloader.fetchAppInfo(Number(game.appId));
      })
      .then(async (info: PicsAppInfo): Promise<void> => {
        if (info.depots.length === 0) {
          throw new Error('没有可下载的 depot (未拥有或不可下载)');
        }
        // 只下第一个有 manifest 的 depot (v1: 主 depot; DLC/shared depot 后续)
        const depot = info.depots[0];
        await writer.begin(folderName, depot.depotId);
        this.downloadPhase = '下载中';
        await downloader.downloadDepot(Number(game.appId), depot.depotId, depot.manifestGid, writer);
      })
      .then((): void => {
        this.downloadActive = false;
        this.downloadPhase = '';
        this.downloadMessage = '下载完成，重扫描后出现在首页';
        promptAction.showToast({ message: '下载完成：' + writer.base() });
        void this.rescanCatalog();
      })
      .catch((e: Object): void => {
        this.downloadActive = false;
        this.downloadPhase = '';
        this.downloadMessage = '下载失败：' + JSON.stringify(e);
        hilog.warn(0x0000, 'SteamGameDetail', 'download failed: %{public}s', JSON.stringify(e));
      });
  }

  private rescanCatalog(): void {
    void AppCatalogService.getInstance().scan();
  }

  build() {
    Column({ space: 8 }) {
      this.TopBar()
      Scroll() {
        Column({ space: 16 }) {
          if (this.game) {
            if (this.availableWidth >= 640) {
              Row({ space: 24 }) {
                Column() { this.Artwork() }.layoutWeight(1)
                Column() { this.GameInfo() }.layoutWeight(1)
              }
              .width('100%').alignItems(VerticalAlign.Top)
            } else {
              this.Artwork()
              this.GameInfo()
            }
          } else {
            Text('游戏资料已更新，请返回游戏库重新选择。')
              .fontSize(14).fontColor(skinColor('text_secondary')).width('100%')
          }
        }
        .width('100%').constraintSize({ maxWidth: 1040 }).padding({ top: 8, bottom: 16 })
      }
      .width('100%').layoutWeight(1).scrollBar(BarState.Auto).align(Alignment.Top)
      .id('steam-game-detail-scroll')
    }
    .width('100%').height('100%')
    .padding({ left: 12 + this.safeLeft, right: 12 + this.safeRight,
      top: this.safeTop + 8, bottom: Math.max(12, this.safeBottom) })
    .backgroundColor(skinColor('page_background'))
    .onAreaChange((_old: Area, area: Area): void => {
      this.availableWidth = Math.max(0, Number(area.width) - 24 - this.safeLeft - this.safeRight);
    })
  }
}
