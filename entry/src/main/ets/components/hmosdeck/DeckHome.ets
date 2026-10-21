import { AppDescriptor, AppSource } from '../../model/AppModels';
import { builtinCoverResource } from '../../model/BuiltinArtwork';
import { LinuxSteamAction, LinuxSteamGame, LinuxSteamSnapshot } from '../../model/LinuxSteamModels';
import { LinuxSteamLibraryService } from '../../service/LinuxSteamLibraryService';
import { LinuxSteamInstallService } from '../../service/LinuxSteamInstallService';
import { LinuxSteamArtworkListener, LinuxSteamArtworkService } from '../../service/LinuxSteamArtworkService';
import { LinuxSteamSessionListener, LinuxSteamSessionService } from '../../service/LinuxSteamSessionService';
import { GamepadManager, GamepadNavigationListener } from '../../service/GamepadManager';
import { HostWindowTasks, HostWindowTask } from '../../service/HostWindowTasks';
import { promptAction, LengthMetrics } from '@kit.ArkUI';
import { DeckBackdrop } from './DeckBackdrop';
import { DeckArtwork } from './DeckArtwork';
import { DeckMotion } from './DeckMotion';
import { DeviceCapabilityPolicy } from '../../common/DeviceCapabilityPolicy';
import { deckLayout, DeckLayout } from '../../model/DeckLayout';

// Native ArkUI shell; the official client continues to own Steam account and commerce UI.
@Component
export struct DeckHome {
  @Prop @Watch('visibilityChanged') active: boolean = false;
  @Prop localGames: AppDescriptor[] = [];
  onClassic: () => void = (): void => {};
  onSettings: () => void = (): void => {};
  onResources: () => void = (): void => {};
  onBackHandler: (handler: (() => boolean) | null) => void = (_handler: (() => boolean) | null): void => {};
  onMembership: () => void = (): void => {};
  onSupport: () => void = (): void => {};
  onLocalLaunch: (game: AppDescriptor) => Promise<void> = async (_game: AppDescriptor): Promise<void> => {};
  onLocalSettings: (game: AppDescriptor) => void = (_game: AppDescriptor): void => {};
  localArtwork: (game: AppDescriptor) => string = (game: AppDescriptor): string => game.backgroundPath ?? '';
  @State private games: LinuxSteamGame[] = [];
  @State private page: string = 'steam';
  @State private selected: string = '';
  @State private query: string = '';
  @State private libraryTab: string = 'steam';
  @State private widthVp: number = 1000;
  @State private heightVp: number = 650;
  @State private busy: boolean = false;
  @State private pendingTitle: string = '';
  @State private pendingCover: string = '';
  @State private pendingSource: string = 'Steam';
  @State private pendingMessage: string = '';
  @State private pendingBuiltin: Resource | undefined = undefined;
  @State private error: string = '';
  @State private session: LinuxSteamSnapshot = LinuxSteamSessionService.getInstance().snapshot();
  @State private tasks: HostWindowTask[] = [];
  @State private artRevision: number = 0;
  @State private sheet: boolean = false;
  @State private sheetLift: number = 0;
  @State private sheetSink: number = 0;
  @State private sheetPull: number = 0;
  @State private sheetOpen: boolean = false;
  @State private sheetPieces: boolean = false;
  @State private sheetKind: string = 'steam';
  @State private focus: string = '';
  @State private hovered: string = '';
  @State private pressed: string = '';
  @State private entered: boolean = false;
  @StorageLink('hmosdeck.motion') @Watch('motionChanged') private motion: boolean = true;
  private timer: number = -1;
  private sheetTimers: number[] = [];
  private sheetGeneration: number = 0;
  private entryTimer: number = -1;
  private listener: LinuxSteamSessionListener | null = null;
  private artworkListener: LinuxSteamArtworkListener | null = null;
  private navigation: GamepadNavigationListener | null = null;
  private navigationActive: boolean = false;
  private lastNavigation: number = 0;
  private scroll: Scroller = new Scroller();

  aboutToAppear(): void {
    this.onBackHandler((): boolean => this.handleBack());
    this.listener = (value: LinuxSteamSnapshot): void => { this.session = value; };
    LinuxSteamSessionService.getInstance().subscribe(this.listener);
    this.artworkListener = (): void => { this.artRevision++; };
    LinuxSteamArtworkService.getInstance().subscribe(this.artworkListener);
    this.navigation = (code: number): boolean => this.navigate(code);
    this.visibilityChanged();
    this.timer = setInterval((): void => { if (this.active && !this.busy) this.refresh(); }, 5000);
  }
  aboutToDisappear(): void {
    this.onBackHandler(null);
    if (this.timer >= 0) clearInterval(this.timer);
    this.cancelMotion();
    if (this.listener) LinuxSteamSessionService.getInstance().unsubscribe(this.listener);
    if (this.artworkListener) LinuxSteamArtworkService.getInstance().unsubscribe(this.artworkListener);
    this.detachNavigation();
  }
  private visibilityChanged(): void {
    if (this.active) {
      this.refresh();
      if (this.navigation && !this.navigationActive) {
        GamepadManager.getInstance().releaseAll();
        GamepadManager.getInstance().subscribeNavigation(this.navigation); this.navigationActive = true;
      }
      if (this.entryTimer >= 0) clearTimeout(this.entryTimer);
      this.entered = !this.motion;
      if (this.motion) this.entryTimer = setTimeout((): void => {
        this.entryTimer = -1;
        if (this.active) this.getUIContext().animateTo({ duration: 450, curve: DeckMotion.ease }, (): void => { this.entered = true; });
      }, 50);
    } else {
      this.detachNavigation(); this.pressed = ''; this.hovered = '';
      this.cancelMotion(); this.settleSheet(false);
    }
  }
  private handleBack(): boolean {
    if (!this.active) return false;
    if (this.busy) return true;
    if (this.sheetOpen) { this.closeSheet(); return true; }
    if (this.page !== 'steam') { this.choose('steam'); return true; }
    return false;
  }
  private detachNavigation(): void {
    if (this.navigationActive && this.navigation) GamepadManager.getInstance().unsubscribeNavigation(this.navigation);
    this.navigationActive = false;
  }
  private refresh(): void {
    this.games = LinuxSteamLibraryService.getInstance().scan();
    LinuxSteamArtworkService.getInstance().ensure(this.games);
    if (!this.games.some((g: LinuxSteamGame): boolean => g.appId === this.selected)) this.selected = this.games[0]?.appId ?? '';
    this.tasks = HostWindowTasks.getInstance().snapshot().filter((task: HostWindowTask): boolean =>
      task.source !== 'steam-linux' && task.title !== '运行窗口');
  }
  private cover(game: LinuxSteamGame): string {
    return this.artRevision >= 0 ? LinuxSteamArtworkService.getInstance().resolve(game.appId, game.cover) : '';
  }
  private filtered(): LinuxSteamGame[] {
    if (this.libraryTab !== 'steam') return [];
    const query = this.query.trim().toLocaleLowerCase();
    return this.games.filter((g: LinuxSteamGame): boolean => !query || g.name.toLocaleLowerCase().includes(query));
  }
  private filteredLocal(): AppDescriptor[] {
    const query = this.query.trim().toLocaleLowerCase();
    return this.localGames.filter((g: AppDescriptor): boolean =>
      (this.libraryTab === 'tools' ? g.source === AppSource.BUILTIN :
        this.libraryTab === 'local' && g.source !== AppSource.BUILTIN) &&
      (!query || g.title.toLocaleLowerCase().includes(query)));
  }
  private chooseLibraryTab(tab: string): void {
    HostWindowTasks.getInstance().markLauncherForeground();
    this.libraryTab = tab; this.error = '';
    this.scroll.scrollTo({ xOffset: 0, yOffset: 0 });
  }
  private localCover(game: AppDescriptor): string {
    // Tools have theme-specific bundled art; local game covers remain user-owned.
    if (game.source === AppSource.BUILTIN) return '';
    const path = game.coverPath || this.localArtwork(game);
    return path && !path.includes('://') ? 'file://' + path : path;
  }
  private localBuiltin(game: AppDescriptor): Resource | undefined {
    return game.source === AppSource.BUILTIN ? builtinCoverResource(game.id, true) : undefined;
  }
  private layout(): DeckLayout { return deckLayout(this.widthVp, this.heightVp); }
  private portrait(): boolean { return this.layout().portrait; }
  private tall(): boolean { return this.heightVp > this.widthVp; }
  private columns(): number { return this.layout().columns; }
  private coverHeight(): number { return this.layout().coverHeight; }
  private railWidth(): number { return this.layout().railWidth; }
  private inset(): number { return this.layout().inset; }
  private contentHeight(): number { return this.layout().contentHeight; }
  private railSelected(id: string): boolean { return id === 'deck-nav-' + (this.page === 'options' ? 'steam' : this.page); }
  private hot(id: string): boolean { return this.focus === id || this.hovered === id; }
  private buttonScale(id: string): number { return this.pressed === id ? 0.955 : this.hot(id) ? 1.02 : 1; }
  private tileScale(id: string): number { return this.pressed === id ? 0.97 : this.hot(id) ? 1.04 : 1; }
  private touch(id: string, event: TouchEvent): void {
    if (event.type === TouchType.Down) { HostWindowTasks.getInstance().markLauncherForeground(); this.pressed = id; }
    else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
      if (this.pressed === id) this.pressed = '';
    }
  }
  private choose(page: string): void {
    HostWindowTasks.getInstance().markLauncherForeground();
    this.closeSheet(); this.error = '';
    if (page === this.page) return;
    this.getUIContext().animateTo({ duration: this.motion ? 240 : 0, curve: DeckMotion.ease }, (): void => { this.page = page; });
  }
  private cancelSheetSteps(): void {
    this.sheetGeneration++;
    this.sheetTimers.forEach((timer: number): void => clearTimeout(timer));
    this.sheetTimers = [];
  }
  private cancelMotion(): void {
    this.cancelSheetSteps();
    if (this.entryTimer >= 0) clearTimeout(this.entryTimer);
    this.entryTimer = -1;
  }
  private sheetStep(delay: number, action: () => void): void {
    const generation = this.sheetGeneration;
    this.sheetTimers.push(setTimeout((): void => {
      if (this.active && this.sheetOpen && generation === this.sheetGeneration) action();
    }, delay));
  }
  private settleSheet(open: boolean): void {
    // Lifecycle cleanup must not start another animation on a disappearing page.
    this.sheetOpen = open; this.sheet = open; this.sheetPieces = open;
    this.sheetLift = open ? 1 : 0; this.sheetSink = open ? 1 : 0; this.sheetPull = 0;
  }
  private openSheet(kind: string = 'steam'): void {
    if (kind === 'steam') { this.choose('options'); return; }
    if (this.sheetOpen) return;
    this.cancelSheetSteps();
    const mounted = this.sheet;
    this.sheetKind = kind; this.sheet = true; this.sheetOpen = true; this.sheetPieces = false;
    this.focus = 'deck-more-options';
    if (!this.motion) { this.settleSheet(true); return; }
    // Mount at rest first; then pull/sink, spring upward, and reveal each piece.
    const firstFrame = mounted ? 0 : 50;
    this.sheetStep(firstFrame, (): void => {
      this.getUIContext().animateTo({ duration: 260, curve: DeckMotion.ease }, (): void => { this.sheetSink = 1; });
      this.getUIContext().animateTo({ curve: DeckMotion.sheet }, (): void => { this.sheetPull = 24; });
    });
    this.sheetStep(firstFrame + 120, (): void => {
      this.getUIContext().animateTo({ curve: DeckMotion.pullReturn }, (): void => { this.sheetPull = 0; });
      this.getUIContext().animateTo({ curve: DeckMotion.sheet }, (): void => { this.sheetLift = 1; });
    });
    this.sheetStep(firstFrame + 380, (): void => {
      this.sheetPieces = true;
      try { focusControl.requestFocus(this.focus); } catch (_) {}
    });
  }
  private closeSheet(): void {
    if (!this.sheetOpen) return;
    this.cancelSheetSteps(); this.sheetOpen = false; this.sheetPieces = false;
    if (!this.motion) { this.settleSheet(false); return; }
    const generation = this.sheetGeneration;
    this.getUIContext().animateTo({ duration: 300, curve: DeckMotion.ease }, (): void => { this.sheetSink = 0; });
    this.getUIContext().animateTo({ curve: DeckMotion.pullReturn }, (): void => { this.sheetPull = 0; });
    this.getUIContext().animateTo({ delay: 60, curve: DeckMotion.sheetClose, onFinish: (): void => {
      if (generation !== this.sheetGeneration || this.sheetOpen) return;
      this.sheet = false;
      this.focus = this.page === 'steam' ? 'deck-steam' : 'deck-nav-' + this.page;
      if (this.active) try { focusControl.requestFocus(this.focus); } catch (_) {}
    } }, (): void => { this.sheetLift = 0; });
  }
  private sheetHeight(): number { return Math.min(this.sheetKind === 'more' ? 430 : 500, this.heightVp - 30); }
  private focusIds(): string[] {
    if (this.sheet) return ['deck-sheet-close', ...(this.sheetKind === 'more' ?
      ['deck-more-options', 'deck-settings', 'deck-sheet-classic'] : [])];
    const ids = this.portrait() ? ['deck-nav-steam', 'deck-nav-library', 'deck-nav-running', 'deck-nav-more'] :
      ['deck-nav-steam', 'deck-nav-library', 'deck-nav-running', 'deck-nav-settings', 'deck-classic'];
    if (!this.portrait() && DeviceCapabilityPolicy.supportsValueAddedFeatures()) ids.push('deck-membership', 'deck-support');
    if (this.page === 'steam') ids.push('deck-steam', 'deck-open-sheet');
    else if (this.page === 'options') ids.push('deck-options-back', 'deck-components', 'deck-big-picture', 'deck-desktop', 'deck-steam-downloads', 'deck-account');
    else if (this.page === 'library') {
      ids.push('deck-category-steam', 'deck-category-tools', 'deck-category-local');
      for (const game of this.filtered()) ids.push('deck-game-' + game.appId);
      for (const game of this.filteredLocal()) ids.push('deck-local-' + game.id, 'deck-local-settings-' + game.id);
    } else for (const task of this.tasks) ids.push('deck-task-' + task.key);
    return ids;
  }
  private navigate(code: number): boolean {
    if (!this.active || HostWindowTasks.getInstance().hasForegroundTask()) return false;
    if (this.busy) return true;
    if (code === 2302 || code === 2070) {
      if (!this.handleBack()) this.onClassic();
      return true;
    }
    const ids = this.focusIds();
    const delta = code === 2014 ? -1 : code === 2015 ? 1 : code === 2012 ? -1 : code === 2013 ? 1 : 0;
    if (delta) {
      if (Date.now() - this.lastNavigation < 120) return true;
      this.lastNavigation = Date.now();
      const next = Math.max(0, Math.min(ids.length - 1, Math.max(0, ids.indexOf(this.focus)) + delta));
      this.focus = ids[next];
      try { focusControl.requestFocus(this.focus); } catch (_) {}
      return true;
    }
    if ([2301, 2050, 2054, 2016].includes(code)) { this.activateFocus(); return true; }
    return false;
  }
  private activateFocus(): void {
    const id = this.focus;
    if (id === 'deck-classic' || id === 'deck-sheet-classic') this.onClassic();
    else if (id === 'deck-settings' || id === 'deck-nav-settings') this.openSettings();
    else if (id === 'deck-membership') this.onMembership();
    else if (id === 'deck-support') this.onSupport();
    else if (id === 'deck-components') this.manageComponents();
    else if (id === 'deck-more-options') this.openSheet();
    else if (id === 'deck-options-back') this.choose('steam');
    else if (id === 'deck-nav-steam') this.choose('steam');
    else if (id === 'deck-nav-library') this.choose('library');
    else if (id === 'deck-nav-running') this.choose('running');
    else if (id === 'deck-open-sheet') this.openSheet();
    else if (id === 'deck-nav-more') this.openSheet('more');
    else if (id === 'deck-sheet-close') this.closeSheet();
    else if (id === 'deck-steam-downloads') void this.open('downloads');
    else if (id === 'deck-account') void this.open('account');
    else if (id === 'deck-big-picture') void this.open('bigPicture');
    else if (id === 'deck-desktop') void this.open('desktop');
    else if (id.startsWith('deck-category-')) this.chooseLibraryTab(id.substring(14));
    else if (id.startsWith('deck-local-settings-')) {
      const game = this.localGames.find((g: AppDescriptor): boolean => 'deck-local-settings-' + g.id === id);
      if (game) this.onLocalSettings(game);
    } else if (id.startsWith('deck-local-')) {
      const game = this.localGames.find((g: AppDescriptor): boolean => 'deck-local-' + g.id === id);
      if (game) void this.openLocal(game);
    } else if (id.startsWith('deck-game-')) {
      const game = this.games.find((g: LinuxSteamGame): boolean => 'deck-game-' + g.appId === id);
      if (game) void this.open('launch', game.appId);
    } else if (id.startsWith('deck-task-')) {
      const task = this.tasks.find((t: HostWindowTask): boolean => 'deck-task-' + t.key === id);
      if (task) void this.restoreTask(task.key);
    } else void this.open('library');
  }
  private dismissForRoute(): void {
    this.cancelSheetSteps(); this.settleSheet(false);
  }
  private manageComponents(): void { this.dismissForRoute(); this.onResources(); }
  private openSettings(): void { this.dismissForRoute(); this.onSettings(); }
  private motionChanged(): void {
    if (!this.motion) { this.cancelMotion(); this.settleSheet(this.sheetOpen); this.entered = true; }
  }
  private async restoreTask(key: string): Promise<void> {
    if (!await HostWindowTasks.getInstance().activate(key)) promptAction.showToast({ message: '窗口已关闭，请刷新后重试' });
  }
  private async beginLaunch(title: string, cover: string, source: string, message: string,
    builtin: Resource | undefined, action: () => Promise<void>): Promise<void> {
    if (this.busy) return;
    this.busy = true; this.error = ''; this.pendingTitle = title; this.pendingCover = cover;
    this.pendingSource = source; this.pendingMessage = message; this.pendingBuiltin = builtin;
    this.closeSheet();
    try {
      // Commit the feedback surface before synchronous filesystem/native setup.
      // A resolved Promise alone stays in the microtask queue and cannot paint.
      await new Promise<void>((resolve: () => void): void => { setTimeout(resolve, 48); });
      await action();
    } catch (error) {
      this.error = (error as Error).message;
      promptAction.showToast({ message: this.error, duration: 5000 });
    } finally { this.busy = false; }
  }
  private async openLocal(game: AppDescriptor): Promise<void> {
    if (this.busy) return;
    if (!game.available) { this.onLocalSettings(game); return; }
    await this.beginLaunch(game.title, this.localCover(game), '本地', '正在准备游戏窗口…',
      this.localBuiltin(game), async (): Promise<void> => { await this.onLocalLaunch(game); });
  }
  private async open(action: LinuxSteamAction, id: string = '0'): Promise<void> {
    if (this.busy) return;
    if (!LinuxSteamInstallService.getInstance().installed()) { this.manageComponents(); return; }
    const game = action === 'launch' ? this.games.find((g: LinuxSteamGame): boolean => g.appId === id) : undefined;
    await this.beginLaunch(game?.name ?? 'Steam', game ? this.cover(game) : '', 'Steam',
      game ? '正在交给 Steam 启动，请稍候…' : '正在打开 Steam…', undefined, async (): Promise<void> => {
        await LinuxSteamSessionService.getInstance().open(action, id);
      });
  }
  @Builder
  private RailItem(label: string, icon: Resource, id: string, selected: boolean, callback: () => void) {
    Button({ type: ButtonType.Normal }) {
      Column({ space: 5 }) {
        Stack({ alignContent: Alignment.Center }) {
          if (id === 'deck-membership') Image($r('app.media.vip_crown')).width(23).height(23).fillColor(this.hot(id) ? '#F2F4F7' : '#CDB582')
          else SymbolGlyph(icon).fontSize(23).fontColor([this.railSelected(id) ? '#1A9FFF' : this.hot(id) ? '#F2F4F7' : '#9AA3AF'])
        }.width(28).height(28)
        if (this.portrait() || this.heightVp >= 460) Text(label).fontSize(10).fontWeight(FontWeight.Medium).fontColor(this.railSelected(id) ? '#C9E6FF' : '#9AA3AF')
      }.width('100%').height('100%').justifyContent(FlexAlign.Center).alignItems(HorizontalAlign.Center)
    }.width(this.portrait() ? (this.widthVp - 32) / 4 : this.railWidth() - 16)
      .height(this.portrait() ? 56 : this.heightVp < 460 ? 44 : 64).padding(0).borderRadius(14)
      .backgroundColor(this.railSelected(id) ? '#201A9FFF' : this.hot(id) ? '#1A1D22' : Color.Transparent)
      .border({ width: this.focus === id ? 1.5 : 0, color: '#1A9FFF', radius: 14 })
      .focusBox({ strokeWidth: LengthMetrics.vp(0) })
      .animation({ duration: this.motion ? 220 : 0, curve: DeckMotion.ease })
      .scale({ x: this.pressed === id ? 0.95 : 1, y: this.pressed === id ? 0.95 : 1 })
      .animation({ duration: 0, curve: this.motion ? DeckMotion.button : Curve.Linear })
      .onTouch((event: TouchEvent): void => this.touch(id, event))
      .onHover((value: boolean): void => { this.hovered = value ? id : ''; })
      .onFocus((): void => { this.focus = id; }).onBlur((): void => { if (this.focus === id) this.focus = ''; })
      .onClick(callback).id(id).accessibilityText(label)
  }
  @Builder
  private Rail() {
    Scroll() {
      Column({ space: 5 }) {
      if (this.heightVp >= 460) Text('H').fontSize(22).fontWeight(FontWeight.Bold).fontColor('#F2F4F7').height(48).width('100%').textAlign(TextAlign.Center)
      this.RailItem('Steam', $r('sys.symbol.gamecontroller'), 'deck-nav-steam', this.page === 'steam', (): void => this.choose('steam'))
      this.RailItem('游戏库', $r('sys.symbol.square_grid_2x2'), 'deck-nav-library', this.page === 'library', (): void => this.choose('library'))
      this.RailItem('运行中', $r('sys.symbol.rectangle_on_rectangle'), 'deck-nav-running', this.page === 'running', (): void => this.choose('running'))
      Blank()
      if (DeviceCapabilityPolicy.supportsValueAddedFeatures()) {
        this.RailItem('会员', $r('sys.symbol.person'), 'deck-membership', false, this.onMembership)
        this.RailItem('赞助', $r('sys.symbol.heart'), 'deck-support', false, this.onSupport)
      }
      this.RailItem('设置', $r('sys.symbol.gearshape'), 'deck-nav-settings', false, (): void => this.openSettings())
      this.RailItem('经典界面', $r('sys.symbol.rectangle_grid_1x2'), 'deck-classic', false, this.onClassic)
      }.width(this.railWidth()).constraintSize({ minHeight: this.heightVp }).padding({ left: 8, right: 8, top: 4, bottom: 12 })
    }.width(this.railWidth()).height('100%').scrollBar(BarState.Off)
      .backgroundColor('#121417').border({ width: { right: 1 }, color: '#262A31' })
  }
  @Builder
  private BottomNav() {
    Row() {
      this.RailItem('Steam', $r('sys.symbol.gamecontroller'), 'deck-nav-steam', this.page === 'steam', (): void => this.choose('steam'))
      this.RailItem('游戏库', $r('sys.symbol.square_grid_2x2'), 'deck-nav-library', this.page === 'library', (): void => this.choose('library'))
      this.RailItem('运行中', $r('sys.symbol.rectangle_on_rectangle'), 'deck-nav-running', this.page === 'running', (): void => this.choose('running'))
      this.RailItem('更多', $r('sys.symbol.square_grid_2x2'), 'deck-nav-more', false, (): void => this.openSheet('more'))
    }.width('100%').height(this.layout().bottomHeight).padding({ left: 16, right: 16 })
      .justifyContent(FlexAlign.SpaceBetween).backgroundColor('#121417')
      .border({ width: { top: 1 }, color: '#262A31' }).id('deck-bottom-nav')
  }
  @Builder
  private Primary() {
    Button({ type: ButtonType.Normal }) {
      Row({ space: 10 }) {
        if (this.busy) LoadingProgress().width(21).height(21).color('#03111F')
        else SymbolGlyph($r('sys.symbol.play_fill')).fontSize(18).fontColor(['#03111F'])
        Text(this.busy ? '正在打开…' : LinuxSteamInstallService.getInstance().installed() ? '启动 Steam' : '安装 Steam')
          .fontSize(17).fontWeight(FontWeight.Bold).fontColor('#03111F').letterSpacing(0.5)
      }.padding({ left: 22, right: 28 }).height(56)
    }.height(56).padding(0).borderRadius(12).enabled(!this.busy)
      .linearGradient({ angle: 130, colors: [['#1A9FFF', 0], ['#1487DB', 1]] })
      .shadow({ radius: this.hot('deck-steam') ? 20 : 8, color: '#481A9FFF', offsetY: 5 })
      .animation({ duration: this.motion ? 300 : 0, curve: DeckMotion.ease })
      .scale({ x: this.buttonScale('deck-steam'), y: this.buttonScale('deck-steam') })
      .animation({ duration: 0, curve: this.motion ? DeckMotion.button : Curve.Linear })
      .onHover((value: boolean): void => { this.hovered = value ? 'deck-steam' : ''; })
      .onFocus((): void => { this.focus = 'deck-steam'; }).onBlur((): void => { if (this.focus === 'deck-steam') this.focus = ''; })
      .onTouch((event: TouchEvent): void => this.touch('deck-steam', event))
      .onClick((): void => { void this.open('library'); }).id('deck-steam')
  }
  @Builder
  private SteamHome() {
    Stack({ alignContent: this.tall() ? Alignment.TopStart : Alignment.BottomStart }) {
      DeckBackdrop({ games: this.games, widthVp: this.layout().contentWidth, heightVp: this.contentHeight(), portrait: this.tall(),
        artRevision: this.artRevision, moving: this.active && this.page === 'steam' && this.motion })
        .scale({ x: 1 - 0.05 * this.sheetSink, y: 1 - 0.05 * this.sheetSink })
      Text((this.session.state === 'running' ? 'Steam 已打开' : '准备好，开始游玩') + ' · ' + this.games.length + ' 款已安装')
        .fontSize(12).fontColor('#9AA3AF').maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
        .width(Math.max(100, this.layout().contentWidth - 2 * this.inset()))
        .position({ x: this.inset(), y: this.inset() })
      Column({ space: 0 }) {
        Text('你的游戏，随时继续').fontSize(13).fontColor('#9AA3AF').letterSpacing(1).margin({ bottom: 16 })
        Row({ space: 0 }) {
          Text('HMOS').fontSize(this.widthVp < 700 ? 34 : 46).fontWeight(FontWeight.Bold).fontColor('#F2F4F7').letterSpacing(-1.5)
          Text('Deck').fontSize(this.widthVp < 700 ? 34 : 46).fontWeight(FontWeight.Medium).fontColor('#F2F4F7').letterSpacing(-1.5)
        }.margin({ bottom: 24 })
        this.Primary()
        Button({ type: ButtonType.Normal }) {
          Row({ space: 7 }) {
            SymbolGlyph($r('sys.symbol.chevron_up')).fontSize(10).fontColor(['#9AA3AF'])
            Text('Steam 选项').fontSize(12).fontColor('#9AA3AF')
          }.padding({ left: 16, right: 16 }).height(32)
        }.padding(0).borderRadius({ bottomLeft: 12, bottomRight: 12 }).backgroundColor('#121417')
          .border({ width: 1, color: this.hot('deck-open-sheet') ? '#1A9FFF' : '#262A31', radius: { bottomLeft: 12, bottomRight: 12 } })
          .onFocus((): void => { this.focus = 'deck-open-sheet'; }).onClick((): void => this.openSheet()).id('deck-open-sheet')
        if (this.busy) Text(this.session.message).fontSize(12).fontColor('#9AA3AF').maxLines(2).width('100%').margin({ top: 14 }).id('deck-busy')
        if (this.error) Text(this.error).fontSize(12).fontColor('#FF8A80').maxLines(3).width('100%').margin({ top: 14 }).id('deck-error')
      }.width(Math.min(380, this.layout().contentWidth - this.inset() * 2)).alignItems(HorizontalAlign.Start)
        .margin({ left: this.inset(), top: this.tall() ? this.inset() + 52 : 0, bottom: this.tall() ? 0 : this.heightVp < 460 ? 24 : 52 })
        .translate({ y: this.sheetPull - this.sheetLift * 22 + (this.entered ? 0 : 18) })
        .opacity((this.entered ? 1 : 0) * (1 - this.sheetSink))
        .enabled(!this.sheet)
      if (!this.portrait()) Row({ space: 14 }) {
        Text('A  确认').fontSize(11).fontColor('#69727F')
        Text('B  返回').fontSize(11).fontColor('#69727F')
      }.position({ x: this.inset(), y: this.heightVp - 24 })
    }.width('100%').height('100%').id('deck-steam-home')
      .gesture(PanGesture({ direction: PanDirection.Up, distance: 60 }).onActionEnd((): void => this.openSheet()))
  }
  @Builder
  private OptionCard(label: string, detail: string, id: string, icon: Resource, callback: () => void) {
    Button({ type: ButtonType.Normal }) {
      Row({ space: 14 }) {
        Stack({ alignContent: Alignment.Center }) {
          SymbolGlyph(icon).fontSize(22).fontColor(['#8BCBFF'])
        }.width(38).height(38).borderRadius(10).backgroundColor('#201A9FFF')
        Column({ space: 6 }) {
          Text(label).fontSize(16).fontWeight(FontWeight.Medium).fontColor('#F2F4F7').maxLines(1)
          Text(detail).fontSize(12).fontColor('#9AA3AF').maxLines(2)
        }.layoutWeight(1).alignItems(HorizontalAlign.Start)
        SymbolGlyph($r('sys.symbol.chevron_right')).fontSize(11).fontColor(['#69727F'])
      }.width('100%').height('100%').padding(18)
    }.width('100%').height(104).padding(0).borderRadius(14)
      .backgroundColor(this.hot(id) ? '#1A1D22' : '#121417')
      .border({ width: 1, color: this.focus === id ? '#1A9FFF' : '#262A31', radius: 14 })
      .onHover((hot: boolean): void => { this.hovered = hot ? id : ''; })
      .onFocus((): void => { this.focus = id; }).onClick(callback).id(id)
  }
  @Builder
  private SteamOptions() {
    Scroll() {
      Column({ space: 24 }) {
        Row({ space: 12 }) {
          Button({ type: ButtonType.Normal }) {
            SymbolGlyph($r('sys.symbol.chevron_left')).fontSize(18).fontColor(['#F2F4F7'])
          }.width(44).height(44).padding(0).borderRadius(12).backgroundColor('#1A1D22')
            .onClick((): void => this.choose('steam')).id('deck-options-back').accessibilityText('返回 Steam 首页')
          Column({ space: 6 }) {
            Text('Steam 选项').fontSize(this.tall() ? 26 : 32).fontWeight(FontWeight.Bold).fontColor('#F2F4F7')
            Text('组件与客户端操作').fontSize(13).fontColor('#9AA3AF')
          }.layoutWeight(1).alignItems(HorizontalAlign.Start)
        }.width('100%')
        Column({ space: 12 }) {
          Text('运行组件').fontSize(13).fontColor('#9AA3AF').width('100%')
          this.OptionCard('Steam 组件', LinuxSteamInstallService.getInstance().installed() ?
            '已安装 · 导入、更新与维护' : '尚未安装 · 在线下载或本地导入',
            'deck-components', $r('sys.symbol.archivebox'), (): void => this.manageComponents())
          Text('管理组件不会自动启动 Steam；游戏库、账号与存档会保留。')
            .fontSize(12).fontColor('#69727F').width('100%')
        }.width('100%')
        Column({ space: 12 }) {
          Text('打开 Steam').fontSize(13).fontColor('#9AA3AF').width('100%')
          Grid() {
            GridItem() { this.OptionCard('大屏模式', '全屏展示，适合手柄操作', 'deck-big-picture',
              $r('sys.symbol.gamecontroller'), (): void => { void this.open('bigPicture'); }) }
            GridItem() { this.OptionCard('普通窗口', '使用桌面布局，退出 Steam 大屏', 'deck-desktop',
              $r('sys.symbol.rectangle_grid_1x2'), (): void => { void this.open('desktop'); }) }
            GridItem() { this.OptionCard('游戏下载', '查看游戏的下载与更新', 'deck-steam-downloads',
              $r('sys.symbol.arrow_down_circle'), (): void => { void this.open('downloads'); }) }
            GridItem() { this.OptionCard('Steam 账号', '客户端账号与偏好', 'deck-account',
              $r('sys.symbol.person'), (): void => { void this.open('account'); }) }
          }.columnsTemplate(this.tall() || this.widthVp < 850 ? '1fr' : '1fr 1fr')
            .rowsTemplate(this.tall() || this.widthVp < 850 ? '1fr 1fr 1fr 1fr' : '1fr 1fr')
            .rowsGap(12).columnsGap(12).width('100%').height(this.tall() || this.widthVp < 850 ? 452 : 220)
            .scrollBar(BarState.Off)
        }.width('100%')
        if (this.error) Text(this.error).fontSize(13).fontColor('#FF8A80').width('100%').id('deck-options-error')
      }.width('100%').padding(this.inset()).alignItems(HorizontalAlign.Start)
    }.width('100%').height('100%').align(Alignment.TopStart).id('deck-options-page')
  }
  @Builder
  private SectionHeading(title: string, subtitle: string) {
    Column({ space: 7 }) {
      Text(title).fontSize(this.portrait() ? 26 : 32).fontWeight(FontWeight.Bold).fontColor('#F2F4F7')
      Text(subtitle).fontSize(13).fontColor('#9AA3AF')
    }.alignItems(HorizontalAlign.Start).width('100%').margin({ bottom: 24 })
  }
  @Builder
  private LibraryTab(label: string, tab: string, count: number) {
    Button(label + '  ' + count).height(40).fontSize(this.portrait() ? 12 : 14).fontWeight(FontWeight.Medium).borderRadius(10)
      .padding({ left: this.portrait() ? 12 : 16, right: this.portrait() ? 12 : 16 }).flexShrink(0)
      .fontColor(this.libraryTab === tab ? '#C9E6FF' : '#9AA3AF')
      .backgroundColor(this.libraryTab === tab ? '#201A9FFF' : '#1A1D22')
      .border({ width: 1, color: this.libraryTab === tab ? '#601A9FFF' : '#262A31', radius: 10 })
      .onFocus((): void => { this.focus = 'deck-category-' + tab; })
      .onClick((): void => this.chooseLibraryTab(tab)).id('deck-category-' + tab)
  }
  @Builder
  private Library() {
    Scroll(this.scroll) {
      Column() {
        this.SectionHeading('游戏库', '已安装的游戏，一处开始。')
        Scroll() {
          Row({ space: this.portrait() ? 8 : 10 }) {
          this.LibraryTab('Steam 游戏', 'steam', this.games.length)
          this.LibraryTab('内建工具', 'tools', this.localGames.filter((g: AppDescriptor): boolean => g.source === AppSource.BUILTIN).length)
          this.LibraryTab('本地游戏', 'local', this.localGames.filter((g: AppDescriptor): boolean => g.source !== AppSource.BUILTIN).length)
          }
        }.width('100%').scrollable(ScrollDirection.Horizontal).scrollBar(BarState.Off).align(Alignment.Start).margin({ bottom: 20 }).id('deck-category-tabs')
        Row({ space: 12 }) {
          TextInput({ placeholder: '搜索游戏', text: this.query }).height(42).fontSize(14).fontColor('#F2F4F7')
            .placeholderColor('#69727F').backgroundColor('#1A1D22').borderRadius(10).layoutWeight(1)
            .onChange((text: string): void => { this.query = text; }).id('deck-search')
          Button('刷新').height(42).fontSize(13).fontColor('#9AA3AF').backgroundColor('#1A1D22').borderRadius(10)
            .onClick((): void => { LinuxSteamLibraryService.getInstance().scan(true); this.refresh(); }).id('deck-refresh')
        }.margin({ bottom: 24 })
        if (this.filtered().length + this.filteredLocal().length > 0) {
          Grid() {
            ForEach(this.filtered(), (game: LinuxSteamGame) => {
              GridItem() {
                Column({ space: 10 }) {
                  Button({ type: ButtonType.Normal }) {
                    DeckArtwork({ name: game.name, source: this.cover(game), appId: game.appId, badge: 'Steam' })
                  }.width('100%').height(this.coverHeight()).padding(0).borderRadius(14).backgroundColor('#1A1D22')
                    .border({ width: this.hot('deck-game-' + game.appId) ? 2 : 0, color: '#1A9FFF', radius: 14 })
                    .focusBox({ strokeWidth: LengthMetrics.vp(0) })
                    .shadow({ radius: this.hot('deck-game-' + game.appId) ? 18 : 2, color: this.hot('deck-game-' + game.appId) ? '#481A9FFF' : '#66000000', offsetY: 4 })
                    .animation({ duration: this.motion ? 300 : 0, curve: DeckMotion.ease })
                    .scale({ x: this.tileScale('deck-game-' + game.appId), y: this.tileScale('deck-game-' + game.appId), centerY: '90%' })
                    .animation({ duration: 0, curve: this.motion ? DeckMotion.tile : Curve.Linear })
                    .translate({ y: this.hot('deck-game-' + game.appId) ? -5 : 0 })
                    .animation({ duration: 0, curve: this.motion ? DeckMotion.tileLift : Curve.Linear })
                    .onTouch((event: TouchEvent): void => this.touch('deck-game-' + game.appId, event))
                    .onHover((hot: boolean): void => { this.hovered = hot ? 'deck-game-' + game.appId : ''; })
                    .onFocus((): void => { this.selected = game.appId; this.focus = 'deck-game-' + game.appId; })
                    .onClick((): void => { void this.open('launch', game.appId); }).id('deck-game-' + game.appId)
                  Text(game.name).fontSize(13).fontColor('#F2F4F7').maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis }).width('100%')
                  Text(game.updating ? '更新中 · 在 Steam 查看' : '已安装').fontSize(11).fontColor(game.updating ? '#1A9FFF' : '#69727F').width('100%')
                }.width('100%').height('100%')
              }.height(this.coverHeight() + 54)
            }, (game: LinuxSteamGame): string => game.appId)
            ForEach(this.filteredLocal(), (game: AppDescriptor) => {
              GridItem() {
                Column({ space: 10 }) {
                  Button({ type: ButtonType.Normal }) {
                    DeckArtwork({ name: game.title, source: this.localCover(game), appId: 'local-' + game.id,
                      libraryLabel: 'LOCAL LIBRARY', builtin: this.localBuiltin(game),
                      badge: game.source === AppSource.BUILTIN ? '工具' : '本地' })
                  }.width('100%').height(this.coverHeight()).padding(0).borderRadius(14).backgroundColor('#1A1D22')
                    .border({ width: this.hot('deck-local-' + game.id) ? 2 : 0, color: '#1A9FFF', radius: 14 })
                    .focusBox({ strokeWidth: LengthMetrics.vp(0) })
                    .shadow({ radius: this.hot('deck-local-' + game.id) ? 18 : 2, color: this.hot('deck-local-' + game.id) ? '#481A9FFF' : '#66000000', offsetY: 4 })
                    .animation({ duration: this.motion ? 300 : 0, curve: DeckMotion.ease })
                    .scale({ x: this.tileScale('deck-local-' + game.id), y: this.tileScale('deck-local-' + game.id), centerY: '90%' })
                    .animation({ duration: 0, curve: this.motion ? DeckMotion.tile : Curve.Linear })
                    .translate({ y: this.hot('deck-local-' + game.id) ? -5 : 0 })
                    .animation({ duration: 0, curve: this.motion ? DeckMotion.tileLift : Curve.Linear })
                    .onTouch((event: TouchEvent): void => this.touch('deck-local-' + game.id, event))
                    .onHover((hot: boolean): void => { this.hovered = hot ? 'deck-local-' + game.id : ''; })
                    .onFocus((): void => { this.focus = 'deck-local-' + game.id; })
                    .onClick((): void => { void this.openLocal(game); }).id('deck-local-' + game.id)
                  Text(game.title).fontSize(13).fontColor('#F2F4F7').maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis }).width('100%')
                  Row() {
                    Text(game.available ? '已添加' : '需配置启动程序').fontSize(11).fontColor('#69727F').layoutWeight(1).maxLines(1)
                    Button('设置').fontSize(11).height(24).padding({ left: 8, right: 8 }).fontColor('#9AA3AF')
                      .backgroundColor('#1A1D22').borderRadius(6)
                      .onFocus((): void => { this.focus = 'deck-local-settings-' + game.id; })
                      .onClick((): void => this.onLocalSettings(game)).id('deck-local-settings-' + game.id)
                  }.width('100%').height(18)
                }.width('100%').height('100%')
              }.height(this.coverHeight() + 54)
            }, (game: AppDescriptor): string => 'local-' + game.id)
          }.columnsTemplate('1fr '.repeat(this.columns()).trim()).columnsGap(this.layout().gap).rowsGap(22).width('100%')
            .height(Math.ceil((this.filtered().length + this.filteredLocal().length) / this.columns()) * (this.coverHeight() + 76)).scrollBar(BarState.Off)
        } else {
          Column({ space: 12 }) {
            SymbolGlyph($r('sys.symbol.gamecontroller')).fontSize(40).fontColor(['#343A43'])
            Text(this.query ? '未找到匹配的游戏' : '你的下一场冒险，从这里开始').fontSize(18).fontColor('#F2F4F7')
            Text(this.query ? '当前分类没有匹配项，请换个名称或切换分类。' : this.libraryTab === 'steam' ?
              '在 Steam 中下载后，会自动显示在这里。' : '从经典界面添加或扫描 games 目录中的游戏。').fontSize(13).fontColor('#9AA3AF')
            if (this.libraryTab === 'steam') {
              Button('打开 Steam 游戏库').height(42).fontColor('#03111F').backgroundColor('#1A9FFF')
                .onClick((): void => { void this.open('library'); }).id('deck-empty-open')
            } else {
              Button('打开经典界面').height(42).fontColor('#03111F').backgroundColor('#1A9FFF')
                .onClick(this.onClassic).id('deck-empty-classic')
            }
          }.width('100%').padding({ top: 50, bottom: 50 }).id('deck-empty')
        }
      }.width('100%').padding(this.inset()).alignItems(HorizontalAlign.Start)
    }.width('100%').height('100%').scrollBar(BarState.Auto).align(Alignment.TopStart).id('deck-library-page')
  }
  @Builder
  private LaunchFeedback() {
    Stack({ alignContent: Alignment.Center }) {
      Row({ space: 22 }) {
        DeckArtwork({ name: this.pendingTitle, source: this.pendingCover, appId: 'launch', builtin: this.pendingBuiltin,
          libraryLabel: this.pendingSource === 'Steam' ? 'STEAM LIBRARY' : 'LOCAL LIBRARY', badge: this.pendingSource })
          .width(86).height(129)
        Column({ space: 12 }) {
          Text('正在启动').fontSize(12).fontColor('#1A9FFF').id('deck-launch-status')
          Text(this.pendingTitle).fontSize(23).fontWeight(FontWeight.Bold).fontColor('#F2F4F7').maxLines(2)
          Row({ space: 10 }) {
            LoadingProgress().width(20).height(20).color('#1A9FFF')
            Text(this.pendingSource === 'Steam' && ['preparing', 'starting'].includes(this.session.state) ?
              this.session.message : this.pendingMessage).fontSize(13).fontColor('#9AA3AF').maxLines(3).layoutWeight(1)
          }.width('100%')
        }.alignItems(HorizontalAlign.Start).layoutWeight(1)
      }.padding(28).width(this.widthVp < 700 ? '90%' : 520).borderRadius(20).backgroundColor('#121417')
        .border({ width: 1, color: '#262A31', radius: 20 })
    }.width('100%').height('100%').backgroundColor('#D90A0B0D').onClick((): void => {})
      .accessibilityText('正在启动 ' + this.pendingTitle).id('deck-launch-feedback')
  }
  @Builder
  private Running() {
    Scroll() {
      Column({ space: 14 }) {
        this.SectionHeading('继续游玩', '回到正在运行的 Steam 或游戏。')
        if (!this.tasks.length) Text('还没有运行中的窗口。').fontSize(15).fontColor('#9AA3AF').padding({ top: 30 }).id('deck-no-tasks')
        ForEach(this.tasks, (task: HostWindowTask) => {
          Button({ type: ButtonType.Normal }) {
            Row({ space: 16 }) {
              SymbolGlyph($r('sys.symbol.gamecontroller')).fontSize(26).fontColor(['#1A9FFF'])
              Column({ space: 6 }) {
                Text(task.title).fontSize(16).fontColor('#F2F4F7').maxLines(1)
                Text(task.source === 'steam-linux' ? 'Linux Steam' : '正在运行').fontSize(12).fontColor('#9AA3AF')
              }.alignItems(HorizontalAlign.Start).layoutWeight(1)
              Text('继续').fontSize(14).fontColor('#1A9FFF')
            }.padding(20)
          }.width('100%').padding(0).borderRadius(12).backgroundColor('#121417').border({ width: 1, color: '#262A31', radius: 12 })
            .onFocus((): void => { this.focus = 'deck-task-' + task.key; }).onClick((): void => { void this.restoreTask(task.key); }).id('deck-task-' + task.key)
        }, (task: HostWindowTask): string => task.key)
        if (this.session.state === 'running') Button('显示 Steam').height(46).fontColor('#03111F').backgroundColor('#1A9FFF')
          .onClick((): void => { void this.open('library'); }).id('deck-running-steam')
      }.padding(this.inset()).width('100%').alignItems(HorizontalAlign.Start)
    }.width('100%').height('100%').align(Alignment.TopStart).id('deck-running-page')
  }
  @Builder
  private SheetRow(label: string, detail: string, id: string, icon: Resource, callback: () => void, beat: number) {
    Button({ type: ButtonType.Normal }) {
      Row({ space: 14 }) {
        SymbolGlyph(icon).fontSize(21).fontColor([this.hot(id) ? '#1A9FFF' : '#9AA3AF'])
        Column({ space: 5 }) {
          Text(label).fontSize(15).fontWeight(FontWeight.Medium).fontColor('#F2F4F7')
          Text(detail).fontSize(12).fontColor('#9AA3AF')
        }.alignItems(HorizontalAlign.Start).layoutWeight(1)
        SymbolGlyph($r('sys.symbol.chevron_right')).fontSize(11).fontColor(['#69727F'])
      }.padding({ left: 18, right: 18, top: 16, bottom: 16 })
    }.width('100%').padding(0).borderRadius(12).backgroundColor(this.hot(id) ? '#1A1D22' : '#121417')
      .border({ width: 1, color: this.focus === id ? '#1A9FFF' : '#262A31', radius: 12 })
      .focusBox({ strokeWidth: LengthMetrics.vp(0) })
      .onHover((hot: boolean): void => { this.hovered = hot ? id : ''; }).onFocus((): void => { this.focus = id; })
      .scale({ x: this.buttonScale(id), y: this.buttonScale(id) })
      .animation({ duration: 0, curve: this.motion ? DeckMotion.button : Curve.Linear })
      .onTouch((event: TouchEvent): void => this.touch(id, event))
      .opacity(this.sheetPieces ? 1 : 0).translate({ y: this.sheetPieces ? 0 : 6 })
      .animation({ duration: this.motion ? (this.sheetOpen ? 220 : 90) : 0,
        delay: this.motion && this.sheetOpen ? beat * 55 : 0, curve: DeckMotion.ease })
      .enabled(this.sheetOpen && this.sheetPieces)
      .onClick(callback).id(id)
  }
  @Builder
  private SettingsSheet() {
    Stack({ alignContent: Alignment.Bottom }) {
      Column().width('100%').height('100%').backgroundColor('#6B000000').opacity(this.sheetSink)
        .onClick((): void => this.closeSheet()).id('deck-sheet-scrim')
      Column({ space: 20 }) {
        Row() {
          Column({ space: 6 }) {
            Text(this.sheetKind === 'more' ? '更多' : 'Steam 选项').fontSize(27).fontWeight(FontWeight.Bold).fontColor('#F2F4F7')
            Text(this.sheetKind === 'more' ? '组件管理与应用偏好' : '显示模式与 Steam 客户端操作').fontSize(13).fontColor('#9AA3AF')
          }.alignItems(HorizontalAlign.Start).layoutWeight(1)
          Button('关闭').height(38).fontSize(13).fontColor('#9AA3AF').backgroundColor('#1A1D22')
            .onClick((): void => this.closeSheet()).id('deck-sheet-close')
        }.width('100%')
          .gesture(PanGesture({ direction: PanDirection.Down, distance: 70 }).onActionEnd((): void => this.closeSheet()))
        Scroll() {
          Column({ space: 10 }) {
            if (this.sheetKind === 'more') {
              this.SheetRow('Steam 选项', '组件管理、显示模式与客户端操作', 'deck-more-options', $r('sys.symbol.archivebox'), (): void => this.openSheet(), 0)
              this.SheetRow('系统设置', '外观、输入、声音与运行配置', 'deck-settings', $r('sys.symbol.gearshape'), (): void => this.openSettings(), 1)
              this.SheetRow('返回经典界面', '保留当前 Steam 与游戏会话', 'deck-sheet-classic', $r('sys.symbol.rectangle_on_rectangle'), this.onClassic, 2)
            }
          }.width('100%').padding({ bottom: 8 })
        }.width('100%').layoutWeight(1).scrollBar(BarState.Auto).align(Alignment.Top)
      }.padding(this.portrait() ? 16 : this.widthVp < 700 ? 22 : 32).width(this.portrait() ? '100%' : this.widthVp < 700 ? '94%' : '70%')
        .height(this.sheetHeight()).borderRadius({ topLeft: 24, topRight: 24 })
        .backgroundColor('#0E1013').border({ width: 1, color: '#262A31', radius: { topLeft: 24, topRight: 24 } })
        .translate({ y: (1 - this.sheetLift) * this.sheetHeight() }).id('deck-settings-sheet')
    }.width('100%').height('100%')
  }
  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      if (this.portrait()) {
        Column({ space: 0 }) {
          this.PageContent()
          this.BottomNav()
        }.width('100%').height('100%').id('deck-portrait-layout')
      } else {
        Row() {
          this.Rail()
          this.PageContent()
        }.width('100%').height('100%').id('deck-landscape-layout')
      }
      if (this.sheet) this.SettingsSheet()
      if (this.busy) this.LaunchFeedback()
    }.width('100%').height('100%').backgroundColor('#0A0B0D')
      .onAreaChange((_old: Area, area: Area): void => { this.widthVp = Number(area.width); this.heightVp = Number(area.height); })
      .id('hmosdeck-home')
  }
  @Builder
  private PageContent() {
    Stack() {
      ForEach([this.page], (page: string) => {
        Stack() {
          if (page === 'steam') this.SteamHome()
          else if (page === 'options') this.SteamOptions()
          else if (page === 'library') this.Library()
          else this.Running()
        }.width('100%').height('100%')
          .transition(this.motion ? TransitionEffect.asymmetric(
            TransitionEffect.OPACITY.combine(TransitionEffect.translate({ y: 10 })),
            TransitionEffect.OPACITY.combine(TransitionEffect.translate({ y: -6 }))) : TransitionEffect.IDENTITY)
      }, (page: string): string => page)
    }.width(this.layout().contentWidth).height(this.contentHeight()).clip(true)
  }
}
