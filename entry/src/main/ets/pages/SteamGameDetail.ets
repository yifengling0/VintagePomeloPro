import { common } from '@kit.AbilityKit';
import { promptAction, router, window } from '@kit.ArkUI';
import { hilog } from '../service/LogService';
import { DeviceCapabilityPolicy } from '../common/DeviceCapabilityPolicy';
import { applyStatusOnlyImmersiveShell, getStatusOnlyWindowInsets,
  observeStatusOnlyWindowInsets } from '../common/StatusOnlyImmersiveShell';
import { AppDescriptor, SessionState } from '../model/AppModels';
import { AppCatalogService } from '../service/AppCatalogService';
import { skinColor } from '../service/SkinColors';
import { SteamLibraryService } from '../steam/SteamLibrary';
import { SteamMatch } from '../steam/SteamMatch';
import { SteamLibraryGame } from '../steam/vendor/models/SteamModels';
import { SteamGameArtwork } from '../components/SteamGameArtwork';

import { SteamLocalService } from '../steam/SteamLocalService';
import { AppSessionService } from '../service/AppSessionService';
import { AppSettingsStore } from '../service/AppSettingsStore';
import { SteamClientPanel } from '../components/SteamClientPanel';

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
  @State private launching: boolean = false;
  @State private actionMessage: string = '';
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
  }

  onPageShow(): void {
    this.refreshLocalApp();
  }

  private refreshLocalApp(): void {
    const game = this.game;
    if (!game) return;
    const games = SteamLibraryService.getInstance().cachedGames();
    const apps = AppCatalogService.getInstance().getApps();
    const managed = apps.find((app: AppDescriptor): boolean => app.steam?.appId === game.appId);
    const matches = apps.filter((app: AppDescriptor): boolean => !app.steam &&
      (app.directory.replace(/\\/g, '/').toLowerCase().endsWith('/games/steam/' + game.appId) ||
       (SteamMatch.titlesMatch(app.title, game.name) && SteamMatch.bestMatch(app.title, games)?.appId === game.appId)));
    this.localApp = managed ?? (matches.length === 1 ? matches[0] : null);
    this.steamInstalled = this.localApp?.available ?? false;
  }

  private async primaryAction(): Promise<void> {
    if (!this.game || this.launching) return;
    this.launching = true;
    this.actionMessage = '';
    try {
      const service = SteamLocalService.getInstance();
      const catalog = AppCatalogService.getInstance();
      if (!this.localApp?.available && !service.isInstalled()) {
        if (!await service.importZip()) return;
        await catalog.scan();
        this.refreshLocalApp();
      }
      const app = this.localApp?.available ? this.localApp : service.installCard(this.game.appId);
      const result = await AppSessionService.getInstance().launch(app,
        AppSettingsStore.getInstance().getGlobalSettings(), catalog.getWineHomeDirectory());
      if (!result) throw new Error('启动请求失败，请重试');
      this.actionMessage = result.state === SessionState.STARTING ? '已交给 Steam，请在客户端完成登录或安装' : '';
    } catch (error) { this.actionMessage = (error as Error).message; }
    finally { this.launching = false; }
  }

  private actionLabel(): string {
    if (this.launching) return '正在打开…';
    if (this.localApp?.available) return '开始游戏';
    if (this.localApp?.steam?.state === 'installing') return '在 Steam 中查看安装进度';
    return SteamLocalService.getInstance().isInstalled() ? '在 Steam 中安装' : '导入 Steam 并安装';
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
      Button(this.actionLabel()).width('100%').height(42).borderRadius(12)
        .backgroundColor(skinColor('accent')).fontColor('#FFFFFFFF').enabled(!this.launching)
        .onClick((): void => { void this.primaryAction(); }).id('steam-game-primary')
      if (this.actionMessage) Text(this.actionMessage).fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
      Button('查看 Steam 商店').width('100%').height(42).borderRadius(12)
        .backgroundColor(skinColor('accent')).fontColor('#FFFFFFFF')
        .onClick((): void => { this.openStore(); }).id('steam-game-store')
      if (this.localApp) {
        Button('本地游戏设置').width('100%').height(42).borderRadius(12)
          .backgroundColor(skinColor('selected_background')).fontColor(skinColor('text_primary'))
          .onClick((): void => { this.openLocalSettings(); }).id('steam-game-local-settings')
      } else {
        Text('安装由 Steam 客户端完成；完成后旧柚会自动生成游戏卡片。')
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
          SteamClientPanel({ onChanged: (): void => {
            void AppCatalogService.getInstance().scan().then((): void => this.refreshLocalApp());
          } })
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
