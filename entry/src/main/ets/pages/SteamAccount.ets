import { window } from '@kit.ArkUI';
import { common } from '@kit.AbilityKit';
import { pasteboard } from '@kit.BasicServicesKit';
import { router } from '@kit.ArkUI';
import { fileIo as fs, picker } from '@kit.CoreFileKit';
import { util } from '@kit.ArkTS';
import { DeviceCapabilityPolicy } from '../common/DeviceCapabilityPolicy';
import { applyStatusOnlyImmersiveShell, getStatusOnlyWindowInsets,
  observeStatusOnlyWindowInsets } from '../common/StatusOnlyImmersiveShell';
import { skinColor } from '../service/SkinColors';
import { SteamService, SteamUiState } from '../steam/SteamService';
import { SteamLibraryService } from '../steam/SteamLibrary';
import { SteamLibraryGame } from '../steam/vendor/models/SteamModels';
import { SteamGameArtwork } from '../components/SteamGameArtwork';

/** 库列表数据源（LazyForEach 增量渲染, 真实库可能几百款）。 */
class LibraryDataSource implements IDataSource {
  private listeners: DataChangeListener[] = [];
  private games: Array<SteamLibraryGame> = [];

  setGames(games: Array<SteamLibraryGame>): void {
    this.games = games;
    this.listeners.forEach((l: DataChangeListener): void => l.onDataReloaded());
  }

  totalCount(): number {
    return this.games.length;
  }

  getData(index: number): SteamLibraryGame {
    return this.games[index];
  }

  registerDataChangeListener(listener: DataChangeListener): void {
    if (this.listeners.indexOf(listener) < 0) {
      this.listeners.push(listener);
    }
  }

  unregisterDataChangeListener(listener: DataChangeListener): void {
    const i = this.listeners.indexOf(listener);
    if (i >= 0) {
      this.listeners.splice(i, 1);
    }
  }
}

/**
 * Steam 账号页：账号登录 / Steam Guard 令牌 / 游戏库 三段式。
 * 入口在主页顶栏头像按钮（弹窗），复杂流程在本页完成。
 */
@Entry
@Component
struct SteamAccount {
  private context: common.UIAbilityContext = getContext(this) as common.UIAbilityContext;
  @State private state: SteamUiState = SteamUiState.IDLE;
  @State private message: string = '';
  @State private accountName: string = '';
  @State private steamId64: string = '';
  @State private personaName: string = '';
  @State private avatarUrl: string = '';
  @State private inputAccount: string = '';
  @State private inputPassword: string = '';
  @State private inputCode: string = '';
  @State private probeResult: string = '检测 Steam 连接中…';
  @State private probeOk: boolean = false;
  @State private guardCode: string = '';
  @State private guardCodeSeconds: number = 0;
  @State private hasSharedSecret: boolean = false;
  @State private secretInput: string = '';
  @State private importing: boolean = false;
  @State private tokenMessage: string = '';
  @State private libLoading: boolean = false;
  @State private libError: string = '';
  @State private libCount: number = -1;
  @State private activeSection: string = 'login';
  @State private qrOpen: boolean = false;
  @State private qrUrl: string = '';
  @State private qrStatus: string = '';
  @State private availableWidth: number = 360;
  @State private safeTop: number = 0;
  @State private safeLeft: number = 0;
  @State private safeRight: number = 0;
  @State private safeBottom: number = 0;
  @State private libUsingCache: boolean = false;
  private libGames: LibraryDataSource = new LibraryDataSource();
  private libraryScroller: Scroller = new Scroller();
  private libraryScrollOffset: number = 0;
  private openingGame: boolean = false;
  private libSyncedOnce: boolean = false;
  private qrTimer: number = -1;
  private qrRetries: number = 0;
  private codeTimer: number = -1;
  private confirmTimer: number = -1;
  private pageAlive: boolean = false;
  private pageVisible: boolean = false;
  private qrGeneration: number = 0;
  private stopInsetObserver: (() => void) | null = null;

  aboutToAppear(): void {
    this.pageAlive = true;
    this.pageVisible = true;
    this.applyWindowChrome();
    this.startProbe();
    SteamService.getInstance().initialize(this.context).then((): void => {
      if (!this.pageAlive) return;
      this.refreshFromService();
      SteamService.getInstance().subscribe('SteamAccountPage',
        (state: SteamUiState, message: string): void => {
          this.state = state;
          this.message = message;
          this.refreshFromService();
          this.syncTimers();
          if (state === SteamUiState.LOGGED_IN) {
            this.inputPassword = '';
            this.inputCode = '';
            this.refreshGuardSecretFlag();
          }
        });
      if (SteamService.getInstance().steamId64.length > 0) {
        SteamService.getInstance().restoreSession().then((ok: boolean): void => {
          if (!this.pageAlive) return;
          this.refreshFromService();
          if (!ok) {
            this.message = '无法恢复会话，可重试登录；已有游戏库可离线查看';
            this.syncLibrary(false);
          }
          this.refreshGuardSecretFlag();
        });
      } else {
        this.refreshGuardSecretFlag();
      }
    }).catch((_error: Object): void => {
      if (this.pageAlive) this.message = '账号存储初始化失败，请返回后重试';
    });
  }

  aboutToDisappear(): void {
    this.pageAlive = false;
    this.pageVisible = false;
    SteamService.getInstance().unsubscribe('SteamAccountPage');
    this.stopQrPoll();
    SteamService.getInstance().cancelQrLogin();
    this.stopTimers();
    this.inputPassword = '';
    if (this.stopInsetObserver) this.stopInsetObserver();
    this.stopInsetObserver = null;
  }

  onPageShow(): void {
    this.pageVisible = true;
    this.openingGame = false;
    if (this.pageAlive) {
      this.refreshFromService();
      this.syncTimers();
      this.restoreLibraryScroll();
      if (this.state === SteamUiState.LOGGED_IN) this.refreshGuardSecretFlag();
    }
  }

  onPageHide(): void {
    this.captureLibraryScroll();
    this.pageVisible = false;
    this.stopQrPoll();
    SteamService.getInstance().cancelQrLogin();
    this.qrOpen = false;
    this.qrUrl = '';
    this.stopTimers();
  }

  /** 登录页与库页/设置页保持同一旋转策略（跟随传感器, 不受系统旋转锁约束）。 */
  private applyWindowChrome(): void {
    window.getLastWindow(this.context).then(async (pageWindow: window.Window): Promise<void> => {
      DeviceCapabilityPolicy.applyLauncherLandscapeRotation(pageWindow, 'SteamAccount');
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
      if (this.stopInsetObserver) this.stopInsetObserver();
      this.stopInsetObserver = observeStatusOnlyWindowInsets(pageWindow, updateInsets);
    }).catch((_e: Error): void => {
    });
  }

  private refreshFromService(): void {
    const service = SteamService.getInstance();
    if (this.steamId64 !== service.steamId64) {
      this.libraryScrollOffset = 0;
      this.libSyncedOnce = false;
      this.libGames.setGames([]);
      this.libCount = -1;
      this.libError = '';
      this.libUsingCache = false;
    }
    this.state = service.state;
    this.message = service.message;
    this.accountName = service.accountName;
    this.steamId64 = service.steamId64;
    this.personaName = service.personaName;
    this.avatarUrl = service.avatarUrl;
    if (this.state === SteamUiState.LOGGED_IN && service.accessToken.length > 0 && !this.libSyncedOnce) {
      this.libSyncedOnce = true;
      this.syncLibrary(false);
    }
  }

  /** W4.2: 拉取/刷新库列表（force=true 跳过缓存走网络）。 */
  private syncLibrary(force: boolean): void {
    if (this.libLoading) return;
    const service = SteamService.getInstance();
    if (service.steamId64.length === 0) {
      this.libError = '尚未登录 Steam 账号';
      return;
    }
    this.libLoading = true;
    this.libError = '';
    const steamId = service.steamId64;
    SteamLibraryService.getInstance()
      .fetchOwnedGames(service.steamId64, service.accessToken, force)
      .then((games: Array<SteamLibraryGame>): void => {
        if (!this.pageAlive) return;
        this.libLoading = false;
        if (SteamService.getInstance().steamId64 !== steamId) return;
        this.libGames.setGames(games);
        this.libCount = games.length;
        this.libUsingCache = !SteamLibraryService.getInstance().hasFreshCache(steamId);
      })
      .catch((e: Object): void => {
        if (!this.pageAlive) return;
        this.libLoading = false;
        if (SteamService.getInstance().steamId64 !== steamId) return;
        const raw = JSON.stringify(e);
        if (raw.indexOf('HTTP 40') >= 0) {
          this.libError = '拉取失败（登录态过期），请重新登录后再同步';
        } else if (raw.indexOf('timeout') >= 0 || raw.indexOf('Timeout') >= 0) {
          this.libError = '拉取超时，请检查网络后重试';
        } else {
          this.libError = '拉取失败，请稍后重试';
        }
      });
  }

  private formatPlayHours(seconds: number): string {
    if (seconds <= 0) return '未游玩';
    return (seconds / 3600).toFixed(1) + ' 小时';
  }

  private formatLastPlayed(epochSeconds: number): string {
    if (epochSeconds <= 0) return '从未游玩';
    const days = Math.floor((Date.now() / 1000 - epochSeconds) / 86400);
    if (days <= 0) return '今天玩过';
    if (days === 1) return '昨天玩过';
    if (days < 30) return days + ' 天前玩过';
    if (days < 365) return Math.floor(days / 30) + ' 个月前玩过';
    return Math.floor(days / 365) + ' 年前玩过';
  }

  private openLibraryGame(game: SteamLibraryGame): void {
    if (this.openingGame) return;
    this.openingGame = true;
    router.pushUrl({
      url: 'pages/SteamGameDetail',
      params: { appId: game.appId }
    }).catch((): void => { this.openingGame = false; });
  }

  private startProbe(): void {
    SteamService.getInstance().probeNetwork().then(async (error: string | null): Promise<void> => {
      const failure = error ?? await SteamService.getInstance().probePasswordLogin();
      if (!this.pageAlive) return;
      this.probeOk = failure === null;
      this.probeResult = failure === null ? 'Steam 连接正常' : failure;
    });
  }

  private refreshGuardSecretFlag(): void {
    if (this.steamId64.length === 0) {
      this.hasSharedSecret = false;
      return;
    }
    SteamService.getInstance().currentGuardCode().then((code: string): void => {
      this.hasSharedSecret = code.length > 0;
      if (this.hasSharedSecret) {
        this.startGuardTimer();
      }
    });
  }

  /** W3: 文件选择器选 maFile → 读文本 → Service 解析入库。 */
  private pickMaFile(): void {
    const docPicker = new picker.DocumentViewPicker(this.context);
    const options = new picker.DocumentSelectOptions();
    this.importing = true;
    docPicker.select(options).then((uris: Array<string>): void => {
      this.importing = false;
      if (uris.length === 0 || !uris[0]) {
        return;
      }
      const text = this.readPickedText(uris[0]);
      if (text.length === 0) {
        this.tokenMessage = '读取文件失败（文件不可读或非文本）';
        return;
      }
      SteamService.getInstance().importMaFile(text).then((report: string): void => {
        this.tokenMessage = report;
        this.refreshGuardSecretFlag();
      });
    }).catch((_e: Object): void => {
      this.importing = false;
    });
  }

  private readPickedText(uri: string): string {
    let fd: number = -1;
    try {
      const file = fs.openSync(uri, fs.OpenMode.READ_ONLY);
      fd = file.fd;
      const stat = fs.statSync(fd);
      const buf = new ArrayBuffer(stat.size);
      fs.readSync(fd, buf);
      const bytes = new Uint8Array(buf);
      return util.TextDecoder.create('utf-8', { fatal: false }).decodeWithStream(bytes, { stream: false });
    } catch (_) {
      return '';
    } finally {
      if (fd >= 0) {
        try {
          fs.closeSync(fd);
        } catch (_) {
        }
      }
    }
  }

  /** 左栏分类切换（主从布局, 与设置页同交互）。 */
  private selectSection(section: string): void {
    this.captureLibraryScroll();
    this.activeSection = section;
    this.restoreLibraryScroll();
  }

  private captureLibraryScroll(): void {
    if (this.activeSection !== 'library' || this.libGames.totalCount() === 0) return;
    try {
      this.libraryScrollOffset = this.libraryScroller.currentOffset().yOffset;
      // Stop any remaining fling before the page is hidden.
      this.libraryScroller.scrollTo({ xOffset: 0, yOffset: this.libraryScrollOffset, animation: false });
    } catch (_) {}
  }

  private restoreLibraryScroll(): void {
    if (this.activeSection !== 'library' || this.libraryScrollOffset <= 0) return;
    setTimeout((): void => {
      if (!this.pageAlive || !this.pageVisible || this.activeSection !== 'library') return;
      try {
        this.libraryScroller.scrollTo({ xOffset: 0, yOffset: this.libraryScrollOffset, animation: false });
      } catch (_) {}
    }, 0);
  }

  /** 就地展开/收起扫码登录（同一页面内显示二维码, 不切视图）。 */
  private toggleQr(): void {
    if (this.qrOpen) {
      this.qrOpen = false;
      this.stopQrPoll();
      SteamService.getInstance().cancelQrLogin();
      this.qrUrl = '';
      this.qrStatus = '';
      return;
    }
    this.qrRetries = 0;
    this.qrOpen = true;
    this.startQrLogin();
  }

  private startQrLogin(): void {
    this.stopQrPoll();
    const generation = this.qrGeneration;
    this.qrUrl = '';
    this.qrStatus = '正在生成二维码…';
    SteamService.getInstance().beginQrLogin().then((url: string): void => {
      if (!this.pageVisible || !this.qrOpen || generation !== this.qrGeneration) return;
      this.qrUrl = url;
      this.qrStatus = '打开 Steam 手机 App 扫码，然后在手机上确认登录';
      this.startQrPoll(generation);
    }).catch((e: Object): void => {
      if (!this.pageVisible || !this.qrOpen || generation !== this.qrGeneration) return;
      this.qrStatus = '';
      this.qrOpen = false;
      this.message = '生成二维码失败：' + JSON.stringify(e);
    });
  }

  private startQrPoll(generation: number): void {
    if (!this.pageVisible || !this.qrOpen || generation !== this.qrGeneration) return;
    this.qrTimer = setTimeout((): void => {
      this.qrTimer = -1;
      SteamService.getInstance().pollQrLogin().then((result: string): void => {
        if (!this.pageVisible || !this.qrOpen || generation !== this.qrGeneration) return;
        if (result === 'approved') {
          this.stopQrPoll();
          this.qrOpen = false;
          this.qrUrl = '';
          this.qrStatus = '';
        } else if (result === 'invalid') {
          this.stopQrPoll();
          this.qrRetries++;
          if (this.qrRetries >= 3) {
            this.qrRetries = 0;
            this.toggleQr();
            this.message = '二维码多次失效，请改用账号密码登录';
          } else {
            this.startQrLogin();
          }
        } else {
          this.startQrPoll(generation);
        }
      }).catch((_e: Object): void => {
        // 单次轮询失败 (网络抽风) 静默, 下一轮再试
        this.startQrPoll(generation);
      });
    }, 5000);
  }

  private stopQrPoll(): void {
    this.qrGeneration++;
    if (this.qrTimer >= 0) {
      clearTimeout(this.qrTimer);
      this.qrTimer = -1;
    }
  }

  private syncTimers(): void {
    if (!this.pageVisible) return;
    if (this.state === SteamUiState.NEEDS_CONFIRM) {
      this.startConfirmTimer();
    } else {
      this.stopConfirmTimer();
    }
  }

  private startGuardTimer(): void {
    if (!this.pageVisible) return;
    if (this.codeTimer >= 0) return;
    const tick = (): void => {
      SteamService.getInstance().currentGuardCode().then((code: string): void => {
        this.guardCode = code;
        const epoch = Math.floor(Date.now() / 1000);
        this.guardCodeSeconds = 30 - (epoch % 30);
      });
    };
    tick();
    this.codeTimer = setInterval(tick, 1000);
  }

  private stopGuardTimer(): void {
    if (this.codeTimer >= 0) {
      clearInterval(this.codeTimer);
      this.codeTimer = -1;
    }
  }

  private startConfirmTimer(): void {
    if (this.confirmTimer >= 0) return;
    this.confirmTimer = setInterval((): void => {
      SteamService.getInstance().pollOnce();
    }, 2500);
  }

  private stopConfirmTimer(): void {
    if (this.confirmTimer >= 0) {
      clearInterval(this.confirmTimer);
      this.confirmTimer = -1;
    }
  }

  private stopTimers(): void {
    this.stopGuardTimer();
    this.stopConfirmTimer();
  }

  @Builder
  private TopBar() {
    Row() {
      Button({ type: ButtonType.Circle }) {
        SymbolGlyph($r('sys.symbol.chevron_left')).fontSize(22)
          .fontColor([skinColor('text_primary')])
      }
      .width(32).height(32)
      .backgroundColor(skinColor('search_background'))
      .onClick((): void => { router.back(); })
      .id('steam-back-button')

      Text('Steam 账号').fontSize(15).fontWeight(FontWeight.Medium)
        .fontColor(skinColor('text_primary')).margin({ left: 10 })
        .layoutWeight(1)
    }
    .width('100%').height(40)
  }

  /** 左栏分类按钮（视觉语言与设置页 SectionButton 一致）。 */
  @Builder
  private SectionButton(label: string, section: string, icon: Resource) {
    Row({ space: 10 }) {
      SymbolGlyph(icon).fontSize(16).fontColor([skinColor('text_primary')])
      Text(label).fontSize(13).fontColor(skinColor('text_primary'))
        .fontWeight(this.activeSection === section ? FontWeight.Medium : FontWeight.Regular)
    }
    .width(this.availableWidth < 600 ? 112 : '100%').height(40).padding({ left: 10, right: 10 }).borderRadius(12)
    .backgroundColor(this.activeSection === section ? skinColor('selected_background') : '#00FFFFFF')
    .id('steam-nav-' + section)
    .onClick((): void => { this.selectSection(section); })
  }

  @Builder
  private ProbeCard() {
    Column({ space: 10 }) {
      Text('连接自检').fontSize(13).fontWeight(FontWeight.Medium)
        .fontColor(skinColor('text_primary')).width('100%')
      Row({ space: 8 }) {
        Circle({ width: 8, height: 8 })
          .fill(this.probeOk ? '#4ADE80' : '#F87171')
        Text(this.probeResult).fontSize(12)
          .fontColor(skinColor('text_secondary')).layoutWeight(1)
      }
      .width('100%').padding(10)
      .backgroundColor(skinColor('card_background'))
      .borderRadius(10)
      .id('steam-probe')
      Button('重新检测').width('100%').height(36).borderRadius(10)
        .backgroundColor(skinColor('selected_background'))
        .fontColor(skinColor('text_primary'))
        .onClick((): void => {
          this.probeOk = false;
          this.probeResult = '检测 Steam 连接中…';
          this.startProbe();
        })
        .id('steam-probe-retry')
      Text('自检走 api.steampowered.com 免鉴权端点，与登录链路同通道。')
        .fontSize(11).fontColor(skinColor('text_secondary')).width('100%')
    }
    .width('100%').padding(14)
    .backgroundColor(skinColor('search_background'))
    .borderRadius(14)
  }

  /** 右栏分类未登录时的占位卡（文案为静态字面量, 不受 @Builder 传值刷新限制影响）。 */
  @Builder
  private SectionLockedHint(hint: string) {
    Column({ space: 10 }) {
      SymbolGlyph($r('sys.symbol.person')).fontSize(30)
        .fontColor([skinColor('text_secondary')])
      Text(hint).fontSize(12).fontColor(skinColor('text_secondary'))
        .width('100%').textAlign(TextAlign.Center)
      Button('去登录').width(160).height(36).borderRadius(10)
        .backgroundColor(skinColor('accent')).fontColor('#FFFFFFFF')
        .onClick((): void => { this.selectSection('login'); })
        .id('steam-goto-login')
    }
    .width('100%').padding(28)
    .backgroundColor(skinColor('search_background'))
    .borderRadius(14)
  }

  @Builder
  private LoginSection() {
    Column({ space: 10 }) {
      Text('登录 Steam').fontSize(13).fontWeight(FontWeight.Medium)
        .fontColor(skinColor('text_primary')).width('100%')

      if (this.state === SteamUiState.NEEDS_EMAIL_CODE ||
        this.state === SteamUiState.NEEDS_GUARD_CODE) {
        Text(this.state === SteamUiState.NEEDS_GUARD_CODE
          ? '输入手机令牌 App 上的 5 位验证码' : '输入邮箱收到的验证码')
          .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
        TextInput({ placeholder: '验证码', text: this.inputCode })
          .fontSize(14).height(44).borderRadius(12)
          .backgroundColor(skinColor('card_background'))
          .onChange((value: string): void => { this.inputCode = value; })
          .id('steam-code-input')
        Button('提交验证码').width('100%').height(44).borderRadius(12)
          .backgroundColor(skinColor('accent')).fontColor('#FFFFFFFF')
          .enabled(this.inputCode.trim().length > 0)
          .onClick((): void => {
            SteamService.getInstance().submitCode(this.inputCode);
          })
          .id('steam-code-submit')
      } else if (this.state === SteamUiState.NEEDS_CONFIRM) {
        LoadingProgress().width(36).height(36).color(skinColor('accent'))
        Text('新设备首次登录需要在手机上确认（只需一次，之后启动器自动记住本设备）：')
          .fontSize(13).fontColor(skinColor('text_primary')).width('100%')
        Text('打开手机 Steam App → Steam Guard（令牌页）→ 「确认登录」→ 找到这条登录请求 → 点「允许」。收不到推送也可以在这里手动确认。')
          .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
        TextInput({ placeholder: '备用：5 位令牌验证码（仅 Steam 允许时可用）', text: this.inputCode })
          .fontSize(14).height(44).borderRadius(12)
          .backgroundColor(skinColor('card_background'))
          .onChange((value: string): void => { this.inputCode = value; })
          .id('steam-confirm-code-input')
        Button('提交令牌验证码').width('100%').height(44).borderRadius(12)
          .backgroundColor(skinColor('accent')).fontColor('#FFFFFFFF')
          .enabled(this.inputCode.trim().length > 0)
          .onClick((): void => {
            SteamService.getInstance().submitCode(this.inputCode);
          })
          .id('steam-confirm-code-submit')
      } else {
        TextInput({ placeholder: 'Steam 账号名', text: this.inputAccount })
          .fontSize(14).height(44).borderRadius(12)
          .backgroundColor(skinColor('card_background'))
          .onChange((value: string): void => { this.inputAccount = value; })
          .id('steam-login-account')
        TextInput({ placeholder: '密码', text: this.inputPassword })
          .type(InputType.Password)
          .fontSize(14).height(44).borderRadius(12)
          .backgroundColor(skinColor('card_background'))
          .onChange((value: string): void => { this.inputPassword = value; })
          .id('steam-login-password')
        Button(this.state === SteamUiState.WORKING ? '登录中…' : '登录')
          .width('100%').height(44).borderRadius(12)
          .backgroundColor(skinColor('accent')).fontColor('#FFFFFFFF')
          .enabled(this.state !== SteamUiState.WORKING)
          .onClick((): void => {
            SteamService.getInstance().signIn(this.inputAccount, this.inputPassword);
          })
          .id('steam-login-button')
        Button(this.qrOpen ? '收起二维码' : '扫码登录（Steam 手机 App，免输密码）')
          .width('100%').height(40).borderRadius(12)
          .backgroundColor(skinColor('selected_background'))
          .fontColor(skinColor('text_primary'))
          .enabled(this.state !== SteamUiState.WORKING)
          .onClick((): void => { this.toggleQr(); })
          .id('steam-qr-login')
        if (this.qrOpen) {
          Column({ space: 8 }) {
            if (this.qrUrl.length > 0) {
              QRCode(this.qrUrl).width(160).height(160)
                .backgroundColor('#FFFFFFFF')
                .id('steam-qr-image')
            } else {
              LoadingProgress().width(28).height(28).color(skinColor('accent'))
                .margin({ top: 30, bottom: 30 })
            }
            Text(this.qrStatus)
              .fontSize(11).fontColor(skinColor('text_secondary'))
              .width('100%').textAlign(TextAlign.Center)
          }
          .width('100%').padding(12)
          .backgroundColor(skinColor('card_background'))
          .borderRadius(12)
          .id('steam-qr-inline')
        }
        Text('新设备登录可能需要邮箱验证码或 Steam 手机 App 确认。')
          .fontSize(11).fontColor(skinColor('text_secondary')).width('100%')
      }

      if (this.message.length > 0) {
        Text(this.message).fontSize(12)
          .fontColor(this.state === SteamUiState.ERROR ? '#F87171' : skinColor('text_secondary'))
          .width('100%').id('steam-message')
      }
      if (this.state === SteamUiState.WORKING) {
        LoadingProgress().width(28).height(28).color(skinColor('accent'))
      }
    }
    .width('100%').padding(14)
    .backgroundColor(skinColor('search_background'))
    .borderRadius(14)
  }

  @Builder
  private ProfileCard() {
    Column({ space: 12 }) {
      Row({ space: 12 }) {
        if (this.avatarUrl.length > 0) {
          Image(this.avatarUrl)
            .width(56).height(56).borderRadius(16)
            .objectFit(ImageFit.Cover)
            .draggable(false)
        } else {
          Stack() {
            SymbolGlyph($r('sys.symbol.person')).fontSize(30)
              .fontColor([skinColor('text_secondary')])
          }
          .width(56).height(56).borderRadius(16)
          .backgroundColor(skinColor('selected_background'))
        }
        Column({ space: 3 }) {
          Text(this.personaName.length > 0 ? this.personaName : this.accountName)
            .fontSize(17).fontWeight(FontWeight.Bold)
            .fontColor(skinColor('text_primary')).width('100%')
          if (this.accountName.length > 0) {
            Text(this.accountName).fontSize(12)
              .fontColor(skinColor('text_secondary')).width('100%')
          }
          if (this.steamId64.length > 0) {
            Text('SteamID ' + this.steamId64).fontSize(11)
              .fontColor(skinColor('text_secondary')).width('100%')
          }
        }.alignItems(HorizontalAlign.Start).layoutWeight(1)
      }.width('100%')

      Button('登出').width('100%').height(40).borderRadius(12)
        .backgroundColor(skinColor('search_background'))
        .fontColor(skinColor('text_primary'))
        .onClick((): void => {
          SteamService.getInstance().signOut();
          this.stopGuardTimer();
          this.hasSharedSecret = false;
        })
        .id('steam-logout')
    }
    .width('100%').padding(14)
    .backgroundColor(skinColor('search_background'))
    .borderRadius(14)
  }

  @Builder
  private TokenCard() {
    Column({ space: 10 }) {
      Text('Steam 手机令牌').fontSize(13).fontWeight(FontWeight.Medium)
        .fontColor(skinColor('text_primary')).width('100%')
      if (this.hasSharedSecret) {
        Stack() {
          Text(this.guardCode.length > 0 ? this.guardCode : '-----')
            .fontSize(34).fontWeight(FontWeight.Bold)
            .fontColor(skinColor('accent'))
            .letterSpacing(6)
        }
        .width('100%').height(72).borderRadius(14)
        .backgroundColor(skinColor('selected_background'))
        .onClick((): void => {
          if (this.guardCode.length > 0) {
            const data = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, this.guardCode);
            pasteboard.getSystemPasteboard().setPasteData(data);
            this.message = '令牌码已复制';
          }
        })
        .id('steam-totp-code')
        Progress({ value: this.guardCodeSeconds, total: 30, type: ProgressType.Linear })
          .width('100%').color(skinColor('accent'))
        Text('与 Steam 手机 App 一致，30 秒刷新；点击复制')
          .fontSize(11).fontColor(skinColor('text_secondary')).width('100%')
      } else {
        Text('用 Steam 手机令牌的 maFile 导入（推荐），或手动粘贴 shared_secret（Base64）。')
          .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
        Button(this.importing ? '读取中…' : '导入 maFile 文件')
          .width('100%').height(38).borderRadius(10)
          .backgroundColor(skinColor('selected_background'))
          .fontColor(skinColor('text_primary'))
          .enabled(!this.importing)
          .onClick((): void => { this.pickMaFile(); })
          .id('steam-mafile-import')
        if (this.tokenMessage.length > 0) {
          Text(this.tokenMessage).fontSize(11)
            .fontColor(skinColor('text_secondary')).width('100%')
            .id('steam-mafile-message')
        }
        TextInput({ placeholder: 'shared_secret（Base64）', text: this.secretInput })
          .fontSize(12).height(38).borderRadius(10)
          .backgroundColor(skinColor('card_background'))
          .onChange((value: string): void => { this.secretInput = value; })
          .id('steam-secret-input')
        Button(this.importing ? '保存中…' : '保存密钥并启用令牌')
          .width('100%').height(38).borderRadius(10)
          .backgroundColor(skinColor('accent')).fontColor('#FFFFFFFF')
          .enabled(!this.importing && this.secretInput.trim().length > 0)
          .onClick((): void => {
            this.importing = true;
            SteamService.getInstance().saveSharedSecret(this.secretInput).then((): void => {
              this.importing = false;
              this.secretInput = '';
              this.refreshGuardSecretFlag();
            });
          })
          .id('steam-secret-save')
      }
    }
    .width('100%').padding(14)
    .backgroundColor(skinColor('search_background'))
    .borderRadius(14)
  }

  @Builder
  private LibraryCard() {
    Column({ space: 10 }) {
      Row({ space: 8 }) {
        Text('Steam 游戏库').fontSize(13).fontWeight(FontWeight.Medium)
          .fontColor(skinColor('text_primary'))
        Text(this.libCount >= 0 ? '共 ' + this.libCount + ' 款' : '')
          .fontSize(11).fontColor(skinColor('text_secondary'))
        Blank()
        Button(this.libLoading ? '同步中…' : '同步')
          .fontSize(12).height(28).borderRadius(8)
          .padding({ left: 14, right: 14 })
          .backgroundColor(skinColor('accent')).fontColor('#FFFFFFFF')
          .enabled(!this.libLoading && this.steamId64.length > 0)
          .onClick((): void => { this.syncLibrary(true); })
          .id('steam-lib-sync')
      }
      .width('100%')
      if (this.libError.length > 0) {
        Text(this.libError).fontSize(11).fontColor('#F87171').width('100%')
      }
      if (this.libUsingCache) {
        Text('当前显示离线缓存；联网后点同步更新')
          .fontSize(11).fontColor(skinColor('text_secondary')).width('100%')
      }
      if (this.libGames.totalCount() > 0) {
        List({ space: 8, scroller: this.libraryScroller }) {
          LazyForEach(this.libGames, (game: SteamLibraryGame) => {
            ListItem() {
              Button({ type: ButtonType.Normal }) {
                Row({ space: 10 }) {
                  SteamGameArtwork({ appId: game.appId, imageUrl: game.logoUrl, compact: true })
                    .width(92).height(43).borderRadius(6).clip(true)
                  Column({ space: 3 }) {
                    Text(game.name.length > 0 ? game.name : ('App ' + game.appId))
                      .fontSize(13).fontColor(skinColor('text_primary'))
                      .maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis }).width('100%')
                    Text(this.formatPlayHours(game.playtimeForeverSeconds)
                      + ' · ' + this.formatLastPlayed(game.lastPlayedEpochSeconds))
                      .fontSize(11).fontColor(skinColor('text_secondary'))
                  }
                  .alignItems(HorizontalAlign.Start)
                  .layoutWeight(1)
                  SymbolGlyph($r('sys.symbol.chevron_right')).fontSize(14)
                    .fontColor([skinColor('text_secondary')])
                }
                .width('100%').padding(10)
              }
              .width('100%').padding(0)
              .borderRadius(10)
              .backgroundColor(skinColor('card_background'))
              .onClick((): void => { this.openLibraryGame(game); })
              .accessibilityText((game.name.length > 0 ? game.name : 'App ' + game.appId) + '，查看详情')
              .id('steam-lib-game-' + game.appId)
            }
          }, (game: SteamLibraryGame) => game.appId)
        }
        .width('100%').layoutWeight(1)
        .scrollBar(BarState.Auto)
        .edgeEffect(EdgeEffect.Spring)
        .id('steam-lib-list')
      } else if (this.libLoading) {
        Column({ space: 12 }) {
          LoadingProgress().width(32).height(32).color(skinColor('accent'))
          Text('正在同步游戏库…').fontSize(12).fontColor(skinColor('text_secondary'))
        }
        .width('100%').layoutWeight(1).justifyContent(FlexAlign.Center)
      } else if (!this.libLoading) {
        Column() {
          Text(this.libCount === 0 ? '账号下没有游戏（免费库也可能为空）' : '点右上角“同步”拉取你的 Steam 游戏库')
            .fontSize(12).fontColor(skinColor('text_secondary')).textAlign(TextAlign.Center)
        }
        .width('100%').layoutWeight(1).justifyContent(FlexAlign.Center)
      }
    }
    .width('100%').height('100%').padding(14)
    .backgroundColor(skinColor('search_background'))
    .borderRadius(14)
    .id('steam-library-pane')
  }

  @Builder
  private DetailPane() {
    if (this.activeSection === 'library' && this.steamId64.length > 0) {
      // The library owns its scrolling and fills the pane; do not nest it in Scroll.
      Column() { this.LibraryCard() }
        .width('100%').height('100%').padding(4)
    } else {
      Scroll() {
        Column({ space: 12 }) {
          if (this.activeSection === 'probe') {
            this.ProbeCard()
          } else if (this.activeSection === 'token') {
            if (this.state === SteamUiState.LOGGED_IN && this.steamId64.length > 0) {
              this.TokenCard()
            } else {
              this.SectionLockedHint('登录后可查看手机令牌动态码，或导入 maFile。')
            }
          } else if (this.activeSection === 'library') {
            this.SectionLockedHint('登录后可同步 Steam 游戏库，并与本地游戏匹配封面和时长。')
          } else {
            if (this.state === SteamUiState.LOGGED_IN && this.steamId64.length > 0) {
              this.ProfileCard()
            } else {
              this.LoginSection()
            }
          }
        }
        .width('100%')
        .constraintSize({ maxWidth: 960 })
        .alignItems(HorizontalAlign.Start)
        .padding({ left: 4, right: 4, top: 4, bottom: 16 })
      }
      .width('100%').height('100%')
      .scrollBar(BarState.Auto)
      .align(Alignment.Top)
      .id('steam-detail-scroll')
    }
  }

  @Builder
  private SectionButtons() {
    this.SectionButton('账号登录', 'login', $r('sys.symbol.person'))
    this.SectionButton('手机令牌', 'token', $r('sys.symbol.timer'))
    this.SectionButton('游戏库', 'library', $r('sys.symbol.square_grid_2x2'))
    this.SectionButton('连接自检', 'probe', $r('sys.symbol.info_circle'))
  }

  build() {
    Column({ space: 8 }) {
      this.TopBar()
      if (this.availableWidth < 600) {
        Scroll() {
          Row({ space: 4 }) {
            this.SectionButtons()
          }
        }
        .width('100%').height(44)
        .scrollable(ScrollDirection.Horizontal)
        .scrollBar(BarState.Off)
        .id('steam-nav-horizontal')
        Column() { this.DetailPane() }
          .width('100%').layoutWeight(1)
      } else {
        Row({ space: 16 }) {
          List({ space: 4 }) {
            ListItem() { this.SectionButton('账号登录', 'login', $r('sys.symbol.person')) }
            ListItem() { this.SectionButton('手机令牌', 'token', $r('sys.symbol.timer')) }
            ListItem() { this.SectionButton('游戏库', 'library', $r('sys.symbol.square_grid_2x2')) }
            ListItem() { this.SectionButton('连接自检', 'probe', $r('sys.symbol.info_circle')) }
          }
          .width(this.availableWidth < 840 ? 164 : 204).height('100%')
          .padding(8).borderRadius(16)
          .backgroundColor(skinColor('search_background'))
          .scrollBar(BarState.Off)
          .id('steam-nav-sidebar')
          Column() { this.DetailPane() }
            .layoutWeight(1).height('100%')
        }
        .width('100%').layoutWeight(1)
        .alignItems(VerticalAlign.Top)
      }
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
