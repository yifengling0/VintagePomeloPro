import { common } from '@kit.AbilityKit';
import { promptAction, router, window } from '@kit.ArkUI';
import { hilog } from '../service/LogService';
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

/** 一个待下载 depot: 共享 depot 解析后按归属 app 申请 key/token。 */
class DownloadPlanEntry {
  appId: number = 0;
  depotId: number = 0;
  manifestGid: string = '';

  constructor(appId: number, depotId: number, manifestGid: string) {
    this.appId = appId;
    this.depotId = depotId;
    this.manifestGid = manifestGid;
  }
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
  @State private downloadPreparing: boolean = false;
  @State private downloadMessage: string = '';
  @State private downloadPercent: number = 0;
  @State private clientQrUrl: string = '';
  @State private clientQrStatus: string = '';
  private pageAlive: boolean = false;
  private clientQrTimer: number = -1;
  private clientQrGeneration: number = 0;
  private clientQrStartedAt: number = 0;
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
    this.stopClientQr();
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
      app.directory.replace(/\\/g, '/').toLowerCase().endsWith('/games/steam/' + game.appId));
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
    if (!game || this.downloadActive || this.downloadPreparing || this.clientQrUrl.length > 0 ||
      this.steamInstalled || !/^\d+$/.test(game.appId)) return;
    void this.prepareDownload(game);
  }

  private async prepareDownload(game: SteamLibraryGame): Promise<void> {
    this.downloadPreparing = true;
    try {
      const service = SteamService.getInstance();
      await service.initialize(this.context);
      if (!service.steamId64) throw new Error('请先登录 Steam 账号');
      // 2026-10-02: 优先免扫码 —— 用已登录 refresh token 静默换取 client 令牌,
      // 换不到才出二维码 (首次或令牌被撤销时)。
      const clientToken = await service.ensureClientAuthorization();
      if (!clientToken) {
        await this.beginClientQr();
        return;
      }
      if (this.pageAlive) this.startAuthorizedDownload(game, clientToken);
    } catch (error) {
      if (this.pageAlive) {
        this.clientQrStatus = '';
        this.downloadMessage = '下载准备失败：' + describeSteamError(error as Object);
      }
    } finally {
      if (this.pageAlive) this.downloadPreparing = false;
    }
  }

  private async beginClientQr(): Promise<void> {
    const generation = ++this.clientQrGeneration;
    this.clientQrStatus = '正在生成 Steam 客户端授权二维码…';
    const url = await SteamService.getInstance().beginClientQrLogin();
    if (!this.pageAlive || generation !== this.clientQrGeneration) return;
    this.clientQrUrl = url;
    this.clientQrStartedAt = Date.now();
    this.clientQrStatus = '用手机 Steam App 扫码并确认；授权账号须与当前游戏库一致';
    this.scheduleClientQrPoll(generation);
  }

  private scheduleClientQrPoll(generation: number): void {
    if (!this.pageAlive || generation !== this.clientQrGeneration) return;
    this.clientQrTimer = setTimeout((): void => {
      this.clientQrTimer = -1;
      void this.pollClientQr(generation);
    }, 5000);
  }

  private async pollClientQr(generation: number): Promise<void> {
    try {
      if (Date.now() - this.clientQrStartedAt > 180000) {
        this.stopClientQr();
        this.clientQrStatus = '二维码已过期，请重新点击下载';
        return;
      }
      const result = await SteamService.getInstance().pollClientQrLogin();
      if (!this.pageAlive || generation !== this.clientQrGeneration) return;
      const refreshedUrl = SteamService.getInstance().getClientQrChallengeUrl();
      if (refreshedUrl.length > 0 && refreshedUrl !== this.clientQrUrl) {
        this.clientQrUrl = refreshedUrl;
        this.clientQrStartedAt = Date.now();
      }
      if (result === 'approved') {
        this.stopClientQr();
        this.clientQrStatus = '授权成功，正在开始下载';
        this.startDownload();
      } else if (result === 'invalid') {
        this.stopClientQr();
        this.clientQrStatus = '二维码已失效，请重新点击下载';
      } else {
        if (result === 'scanned') this.clientQrStatus = 'Steam 已收到扫码，请在手机上确认授权';
        this.scheduleClientQrPoll(generation);
      }
    } catch (error) {
      if (!this.pageAlive || generation !== this.clientQrGeneration) return;
      const message = describeSteamError(error as Object);
      if (message.indexOf('账号不一致') >= 0 || message.indexOf('不是客户端授权') >= 0 ||
        message.indexOf('轮询失败 (EResult') >= 0) {
        this.stopClientQr();
        this.clientQrStatus = message;
        return;
      }
      this.clientQrStatus = '网络暂时超时，继续等待手机确认';
      this.scheduleClientQrPoll(generation);
    }
  }

  private stopClientQr(): void {
    this.clientQrGeneration++;
    if (this.clientQrTimer >= 0) clearTimeout(this.clientQrTimer);
    this.clientQrTimer = -1;
    this.clientQrStartedAt = 0;
    this.clientQrUrl = '';
    SteamService.getInstance().cancelClientQrLogin();
  }

  private startAuthorizedDownload(game: SteamLibraryGame, clientToken: string): void {
    this.clientQrStatus = '';
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
    void this.performDownload(appId, game.name, clientToken, writer, downloader).finally((): void => {
      downloader.endDownload(appId);
      downloader.removeProgressListener(listener);
      if (this.progressListener === listener) this.progressListener = null;
      if (this.pageAlive) this.downloadActive = false;
    });
  }

  /** 常见 redistributable 归属 app (depotfromapp 指向它们时仅为运行库安装器, 跳过)。 */
  private static readonly REDIST_OWNER_APPS: Set<number> =
    new Set<number>([228986, 228987, 228988, 228990]);

  private async performDownload(appId: number, title: string, clientToken: string, writer: SteamDepotWriter,
    downloader: SteamDownloadService): Promise<void> {
    try {
      const service = SteamService.getInstance();
      await service.initialize(this.context);
      if (!service.steamId64) throw new Error('请先登录 Steam 账号');
      await downloader.ensureSession(service.accountName, clientToken, service.steamId64);
      if (this.pageAlive) this.downloadMessage = '正在查询游戏内容';
      const info = await downloader.fetchAppInfo(appId);
      if (info.depots.length === 0) throw new Error('Steam 未返回可下载的游戏内容');
      // 共享 depot 不再整体报错: redist 归属直接跳过; 引擎等内容从归属 app 解析
      // manifest 后照常下载 (depot key / CDN token 按归属 app 申请)。
      const plan: Array<DownloadPlanEntry> = [];
      let skippedRedist = 0;
      for (const depot of info.depots) {
        if (!depot.sharedDepot) {
          if (depot.manifestGid.length === 0) continue;
          plan.push(new DownloadPlanEntry(appId, depot.depotId, depot.manifestGid));
          continue;
        }
        if (SteamGameDetail.REDIST_OWNER_APPS.has(depot.depotFromApp)) {
          skippedRedist++;
          continue;
        }
        if (!depot.depotFromApp) {
          throw new Error('共享 depot 缺少归属应用 (depot ' + depot.depotId + ')');
        }
        if (this.pageAlive) this.downloadMessage = '正在解析共享 depot ' + depot.depotId;
        const ownerInfo = await downloader.fetchAppInfo(depot.depotFromApp);
        const ownerDepot = ownerInfo.depots.find((entry): boolean => entry.depotId === depot.depotId);
        if (!ownerDepot || ownerDepot.sharedDepot || ownerDepot.manifestGid.length === 0) {
          throw new Error('共享 depot ' + depot.depotId + ' 无法从归属 app ' +
            depot.depotFromApp + ' 解析出 manifest');
        }
        plan.push(new DownloadPlanEntry(depot.depotFromApp, depot.depotId, ownerDepot.manifestGid));
      }
      if (plan.length === 0) throw new Error('Steam 未返回可下载的游戏内容');
      await writer.begin(appId, title);
      for (const entry of plan) {
        if (this.pageAlive) this.downloadMessage = '正在下载 depot ' + entry.depotId;
        await downloader.downloadDepot(entry.appId, entry.depotId, entry.manifestGid, writer);
      }
      const path = await writer.publish();
      let catalogUpdated = true;
      try { await AppCatalogService.getInstance().scan(); }
      catch (_) { catalogUpdated = false; }
      if (this.pageAlive) {
        this.downloadMessage = (catalogUpdated ? '下载完成：' : '下载完成；请在首页手动刷新游戏库：') + path +
          (skippedRedist > 0 ? '（已跳过 ' + skippedRedist + ' 个运行库 depot）' : '');
        this.downloadPercent = 100;
        this.refreshLocalApp();
        promptAction.showToast({ message: 'Steam 游戏已加入游戏库' });
      }
    } catch (error) {
      await writer.discard();
      const message = describeSteamError(error as Object);
      // 2026-10-02: 失败原因此前只写 UI, 日志里表现为"无声停滞", 现场必须可查。
      hilog.warn(0x0000, 'SteamDownload', 'download failed: app=%{public}d message=%{public}s',
        appId, message);
      if (message.indexOf('Steam 客户端授权被拒') >= 0) {
        await SteamService.getInstance().clearClientRefreshToken();
      }
      if (this.pageAlive) this.downloadMessage = '下载失败：' + message;
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
        Button(this.downloadActive ? '下载中…' :
          (this.downloadPreparing ? '准备下载…' : '下载到本地'))
          .width('100%').height(42).borderRadius(12)
          .backgroundColor(skinColor('selected_background')).fontColor(skinColor('text_primary'))
          .enabled(!this.downloadActive && !this.downloadPreparing && this.clientQrUrl.length === 0)
          .onClick((): void => { this.startDownload(); }).id('steam-game-download')
        if (this.clientQrUrl.length > 0) {
          Column({ space: 8 }) {
            QRCode(this.clientQrUrl).width(160).height(160)
              .backgroundColor('#FFFFFFFF').id('steam-client-qr-image')
            Text(this.clientQrStatus).fontSize(12).fontColor(skinColor('text_secondary'))
              .width('100%').textAlign(TextAlign.Center)
            Button('取消授权').height(36).borderRadius(10)
              .backgroundColor(skinColor('selected_background'))
              .onClick((): void => {
                this.stopClientQr();
                this.clientQrStatus = '';
              }).id('steam-client-qr-cancel')
          }
          .width('100%').padding(12).borderRadius(12)
          .backgroundColor(skinColor('card_background'))
        } else if (this.clientQrStatus.length > 0) {
          Text(this.clientQrStatus).fontSize(12).fontColor(skinColor('text_secondary'))
            .width('100%')
        }
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
