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
import { SteamDownloadService, SteamDownloadProgress, SteamDownloadProgressListener } from '../steam/SteamDownloadService';
import { SteamDepotWriter } from '../steam/SteamDepotFileSink';
import { SteamService } from '../steam/SteamService';
import { describeSteamError } from '../steam/SteamErrorText';

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
  @State private steamInstalled: boolean = false;
  @State private availableWidth: number = 360;
  @State private safeTop: number = 0;
  @State private safeLeft: number = 0;
  @State private safeRight: number = 0;
  @State private safeBottom: number = 0;
  @State private downloadActive: boolean = false;
  @State private downloadMessage: string = '';
  @State private downloadPercent: number = 0;
  private pageAlive: boolean = false;
  private stopInsetObserver: (() => void) | null = null;
  private progressListener: SteamDownloadProgressListener | null = null;

  aboutToAppear(): void {
    this.pageAlive = true;
    const params = router.getParams() as SteamGameDetailParams;
    const games = SteamLibraryService.getInstance().cachedGames();
    this.game = games.find((item: SteamLibraryGame): boolean => item.appId === params.appId) ?? null;
    this.refreshLocalApp();
    window.getLastWindow(this.context).then(async (pageWindow: window.Window): Promise<void> => {
      if (!this.pageAlive) return;
      DeviceCapabilityPolicy.enableSensorRotation(pageWindow, 'SteamGameDetail');
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
    if (this.progressListener) SteamDownloadService.getInstance().removeProgressListener(this.progressListener);
    this.progressListener = null;
  }

  onPageShow(): void {
    this.refreshLocalApp();
  }

  private refreshLocalApp(): void {
    const game = this.game;
    if (!game) return;
    const games = SteamLibraryService.getInstance().cachedGames();
    const apps = AppCatalogService.getInstance().getApps();
    this.steamInstalled = apps.some((app: AppDescriptor): boolean =>
      app.directory.replace(/\\/g, '/').endsWith('/games/steam/' + game.appId));
    const matches = apps.filter((app: AppDescriptor): boolean => {
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

  private startDownload(): void {
    const game = this.game;
    if (!game || this.downloadActive || this.steamInstalled || !/^\d+$/.test(game.appId)) return;
    const appId = Number(game.appId);
    const downloader = SteamDownloadService.getInstance();
    try { downloader.beginDownload(appId); }
    catch (_) { this.downloadMessage = '已有下载任务正在进行'; return; }
    this.downloadActive = true;
    this.downloadMessage = '正在连接 Steam';
    this.downloadPercent = 0;
    const listener: SteamDownloadProgressListener = (progress: SteamDownloadProgress): void => {
      if (!this.pageAlive || progress.appId !== appId) return;
      this.downloadPercent = progress.totalBytes > 0
        ? Math.min(100, Math.floor(progress.doneBytes * 100 / progress.totalBytes)) : 0;
      this.downloadMessage = '下载中 · ' + this.downloadPercent + '%';
    };
    this.progressListener = listener;
    downloader.onProgress(listener);
    const writer = new SteamDepotWriter();
    void this.performDownload(appId, game.name, writer, downloader).finally((): void => {
      downloader.endDownload(appId);
      downloader.removeProgressListener(listener);
      if (this.progressListener === listener) this.progressListener = null;
      if (this.pageAlive) this.downloadActive = false;
    });
  }

  private async performDownload(appId: number, title: string, writer: SteamDepotWriter,
    downloader: SteamDownloadService): Promise<void> {
    try {
      const service = SteamService.getInstance();
      await service.initialize(this.context);
      const refreshToken = await service.getRefreshToken();
      if (!refreshToken || !service.steamId64) throw new Error('请先登录 Steam 账号');
      await downloader.ensureSession(service.accountName, refreshToken, service.steamId64);
      if (this.pageAlive) this.downloadMessage = '正在查询游戏内容';
      const info = await downloader.fetchAppInfo(appId);
      if (info.depots.length === 0) throw new Error('Steam 未返回可下载的游戏内容');
      if (info.depots.some((depot): boolean => depot.sharedDepot)) {
        throw new Error('此游戏包含共享 depot，当前下载器尚不支持完整安装');
      }
      await writer.begin(appId, title);
      for (const depot of info.depots) {
        if (this.pageAlive) this.downloadMessage = '正在下载 depot ' + depot.depotId;
        await downloader.downloadDepot(appId, depot.depotId, depot.manifestGid, writer);
      }
      const path = await writer.publish();
      let catalogUpdated = true;
      try { await AppCatalogService.getInstance().scan(); }
      catch (_) { catalogUpdated = false; }
      if (this.pageAlive) {
        this.downloadMessage = catalogUpdated ? '下载完成：' + path :
          '下载完成；请在首页手动刷新游戏库：' + path;
        this.downloadPercent = 100;
        this.refreshLocalApp();
        promptAction.showToast({ message: 'Steam 游戏已加入游戏库' });
      }
    } catch (error) {
      await writer.discard();
      if (this.pageAlive) this.downloadMessage = '下载失败：' + describeSteamError(error as Object);
    }
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
      if (!this.steamInstalled) {
        Button(this.downloadActive ? '下载中…' : '下载到本地')
          .width('100%').height(42).borderRadius(12)
          .backgroundColor(skinColor('selected_background')).fontColor(skinColor('text_primary'))
          .enabled(!this.downloadActive)
          .onClick((): void => { this.startDownload(); }).id('steam-game-download')
        if (this.downloadActive) {
          Progress({ value: this.downloadPercent, total: 100, type: ProgressType.Linear })
            .width('100%').color(skinColor('accent'))
        }
        if (this.downloadMessage.length > 0) {
          Text(this.downloadMessage).fontSize(12).fontColor(skinColor('text_secondary'))
            .width('100%')
        }
      }
      Button('查看 Steam 商店').width('100%').height(42).borderRadius(12)
        .backgroundColor(skinColor('accent')).fontColor('#FFFFFFFF')
        .onClick((): void => { this.openStore(); }).id('steam-game-store')
      if (this.localApp) {
        Button('本地游戏设置').width('100%').height(42).borderRadius(12)
          .backgroundColor(skinColor('selected_background')).fontColor(skinColor('text_primary'))
          .onClick((): void => { this.openLocalSettings(); }).id('steam-game-local-settings')
      } else {
        Text('下载后将出现在游戏库；需要 Steam 客户端认证的游戏仍可能无法启动。')
          .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
      }
    }
    .width('100%').alignItems(HorizontalAlign.Start)
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
