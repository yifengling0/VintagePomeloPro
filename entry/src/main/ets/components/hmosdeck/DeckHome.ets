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
import { deckLayout, DeckLayout } from '../../model/DeckLayout';

// Native ArkUI shell; the official client continues to own Steam account and commerce UI.
@Component
export struct DeckHome {
  @Prop @Watch('visibilityChanged') active: boolean = false;
  @Prop localGames: AppDescriptor[] = [];
  onClassic: () => void = (): void => {};
  onSettings: () => void = (): void => {};
  onResources: () => void = (): void => {};
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
  @State private sheetTab: string = 'client';
  @State private focus: string = '';
  @State private hovered: string = '';
  @State private pressed: string = '';
  @State private entered: boolean = false;
  @State private motion: boolean = AppStorage.get<boolean>('hmosdeck.motion') ?? true;
  private timer: number = -1;
  private sheetTimer: number = -1;
  private listener: LinuxSteamSessionListener | null = null;
  private artworkListener: LinuxSteamArtworkListener | null = null;
  private navigation: GamepadNavigationListener | null = null;
  private navigationActive: boolean = false;
  private lastNavigation: number = 0;
  private scroll: Scroller = new Scroller();

  aboutToAppear(): void {
    this.listener = (value: LinuxSteamSnapshot): void => { this.session = value; };
    LinuxSteamSessionService.getInstance().subscribe(this.listener);
    this.artworkListener = (): void => { this.artRevision++; };
    LinuxSteamArtworkService.getInstance().subscribe(this.artworkListener);
    this.navigation = (code: number): boolean => this.navigate(code);
    this.visibilityChanged();
    this.timer = setInterval((): void => { if (this.active && !this.busy) this.refresh(); }, 5000);
  }
  aboutToDisappear(): void {
    if (this.timer >= 0) clearInterval(this.timer);
    if (this.sheetTimer >= 0) clearTimeout(this.sheetTimer);
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
      this.entered = false;
      this.getUIContext().animateTo({ duration: this.motion ? 420 : 0, curve: Curve.EaseOut }, (): void => { this.entered = true; });
    } else { this.detachNavigation(); this.pressed = ''; }
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
  private columns(): number { return this.layout().columns; }
  private coverHeight(): number { return this.layout().coverHeight; }
  private railWidth(): number { return this.layout().railWidth; }
  private inset(): number { return this.layout().inset; }
  private contentHeight(): number { return this.layout().contentHeight; }
  private railSelected(id: string): boolean { return id === 'deck-nav-' + this.page; }
  private hot(id: string): boolean { return this.focus === id || this.hovered === id; }
  private buttonScale(id: string): number { return this.pressed === id ? 0.955 : this.hot(id) ? 1.02 : 1; }
  private choose(page: string): void {
    this.page = page; this.error = ''; this.closeSheet();
    this.getUIContext().animateTo({ duration: this.motion ? 300 : 0, curve: Curve.EaseOut }, (): void => { this.entered = true; });
  }
  private openSheet(): void {
    if (this.sheetTimer >= 0) clearTimeout(this.sheetTimer);
    this.sheet = true;
    this.getUIContext().animateTo({ duration: this.motion ? 380 : 0, curve: Curve.EaseOut }, (): void => { this.sheetLift = 1; });
    this.focus = 'deck-tab-client';
    setTimeout((): void => { if (this.active && this.sheet) focusControl.requestFocus('deck-tab-client'); }, this.motion ? 400 : 0);
  }
  private closeSheet(): void {
    if (!this.sheet) return;
    this.getUIContext().animateTo({ duration: this.motion ? 300 : 0, curve: Curve.EaseOut }, (): void => { this.sheetLift = 0; });
    this.sheetTimer = setTimeout((): void => { this.sheet = false; }, this.motion ? 310 : 0);
    this.focus = 'deck-steam';
  }
  private focusIds(): string[] {
    if (this.sheet) return ['deck-sheet-close', 'deck-tab-client', 'deck-tab-display', 'deck-tab-system',
      ...(this.sheetTab === 'client' ? ['deck-library', 'deck-downloads', 'deck-store', 'deck-account'] :
        this.sheetTab === 'display' ? ['deck-big-picture', 'deck-desktop', 'deck-motion'] : ['deck-settings', 'deck-sheet-classic'])];
    const ids = this.portrait() ? ['deck-nav-steam', 'deck-nav-library', 'deck-nav-running', 'deck-nav-options'] :
      ['deck-nav-steam', 'deck-nav-library', 'deck-nav-running', 'deck-nav-downloads', 'deck-nav-settings', 'deck-classic'];
    if (this.page === 'steam') ids.push('deck-steam', 'deck-open-sheet');
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
      if (this.sheet) this.closeSheet(); else if (this.page !== 'steam') this.choose('steam'); else this.onClassic();
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
    else if (id === 'deck-settings' || id === 'deck-nav-settings') this.onSettings();
    else if (id === 'deck-nav-components' || id === 'deck-client-status' ||
      id === 'deck-downloads' || id === 'deck-nav-downloads') this.manageComponents();
    else if (id === 'deck-nav-steam') this.choose('steam');
    else if (id === 'deck-nav-library') this.choose('library');
    else if (id === 'deck-nav-running') this.choose('running');
    else if (id === 'deck-open-sheet' || id === 'deck-nav-options') this.openSheet();
    else if (id === 'deck-sheet-close') this.closeSheet();
    else if (id.startsWith('deck-tab-')) this.sheetTab = id.substring(9);
    else if (id === 'deck-motion') this.toggleMotion();
    else if (id === 'deck-store') void this.open('store');
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
  private manageComponents(): void {
    this.closeSheet();
    this.onResources();
  }
  private toggleMotion(): void { this.motion = !this.motion; AppStorage.setOrCreate('hmosdeck.motion', this.motion); }
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
        SymbolGlyph(icon).fontSize(23).fontColor([this.railSelected(id) ? '#1A9FFF' : this.hot(id) ? '#F2F4F7' : '#9AA3AF'])
        if (this.portrait() || (this.widthVp >= 700 && this.heightVp >= 460)) Text(label).fontSize(10).fontWeight(FontWeight.Medium).fontColor(this.railSelected(id) ? '#C9E6FF' : '#9AA3AF')
      }.width('100%').height('100%').justifyContent(FlexAlign.Center)
    }.width(this.portrait() ? (this.widthVp - 32) / 4 : this.railWidth() - 16)
      .height(this.portrait() ? 56 : this.heightVp < 460 ? 44 : 64).padding(0).borderRadius(14)
      .backgroundColor(this.railSelected(id) ? '#201A9FFF' : this.hot(id) ? '#1A1D22' : Color.Transparent)
      .border({ width: this.focus === id ? 1.5 : 0, color: '#1A9FFF', radius: 14 })
      .focusBox({ strokeWidth: LengthMetrics.vp(0) })
      .scale({ x: this.buttonScale(id), y: this.buttonScale(id) })
      .animation({ duration: this.motion ? 180 : 0, curve: Curve.EaseOut })
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
      this.RailItem('下载', $r('sys.symbol.arrow_down_circle'), 'deck-nav-downloads', false, (): void => this.manageComponents())
      Blank()
      this.RailItem('设置', $r('sys.symbol.gearshape'), 'deck-nav-settings', false, this.onSettings)
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
      this.RailItem('选项', $r('sys.symbol.gearshape'), 'deck-nav-options', false, (): void => this.openSheet())
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
      .scale({ x: this.buttonScale('deck-steam'), y: this.buttonScale('deck-steam') })
      .animation({ duration: this.motion ? 200 : 0, curve: Curve.EaseOut })
      .onHover((value: boolean): void => { this.hovered = value ? 'deck-steam' : ''; })
      .onFocus((): void => { this.focus = 'deck-steam'; }).onBlur((): void => { this.focus = ''; })
      .onTouch((event: TouchEvent): void => { this.pressed = event.type === TouchType.Down ? 'deck-steam' : ''; })
      .onClick((): void => { void this.open('library'); }).id('deck-steam')
  }
  @Builder
  private SteamHome() {
    Stack({ alignContent: Alignment.BottomStart }) {
      DeckBackdrop({ games: this.games, widthVp: this.layout().contentWidth, heightVp: this.contentHeight(), portrait: this.portrait(),
        artRevision: this.artRevision, moving: this.active && this.page === 'steam' && !this.sheet && this.motion })
      Row({ space: 8 }) {
        Circle().width(6).height(6).fill(this.session.state === 'running' ? '#4CD37F' : '#6C7480')
        Text(this.session.state === 'running' ? 'Steam 已打开' : '准备好，开始游玩').fontSize(12).fontColor('#9AA3AF')
        Text('·').fontSize(12).fontColor('#505762')
        Text(this.games.length + ' 款已安装').fontSize(12).fontColor('#9AA3AF')
      }.position({ x: this.inset(), y: this.portrait() ? 20 : 28 })
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
        .margin({ left: this.inset(), bottom: this.portrait() ? 28 : this.heightVp < 460 ? 24 : 52 })
        .translate({ y: this.sheetLift * -22 + (this.entered ? 0 : 18) }).opacity((this.entered ? 1 : 0) * (1 - this.sheetLift * 0.65))
      if (!this.portrait()) Row({ space: 14 }) {
        Text('A  确认').fontSize(11).fontColor('#69727F')
        Text('B  返回').fontSize(11).fontColor('#69727F')
      }.position({ x: this.inset(), y: this.heightVp - 24 })
    }.width('100%').height('100%').id('deck-steam-home')
      .gesture(PanGesture({ direction: PanDirection.Up, distance: 60 }).onActionEnd((): void => this.openSheet()))
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
        }.width('100%').scrollable(ScrollDirection.Horizontal).scrollBar(BarState.Off).margin({ bottom: 20 }).id('deck-category-tabs')
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
                    .scale({ x: this.hot('deck-game-' + game.appId) ? 1.025 : 1, y: this.hot('deck-game-' + game.appId) ? 1.025 : 1 })
                    .animation({ duration: this.motion ? 180 : 0, curve: Curve.EaseOut })
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
                    .scale({ x: this.hot('deck-local-' + game.id) ? 1.025 : 1, y: this.hot('deck-local-' + game.id) ? 1.025 : 1 })
                    .animation({ duration: this.motion ? 180 : 0, curve: Curve.EaseOut })
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
    }.width('100%').height('100%').scrollBar(BarState.Auto).id('deck-library-page')
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
    }.width('100%').height('100%')
  }
  @Builder
  private SheetRow(label: string, detail: string, id: string, icon: Resource, callback: () => void) {
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
      .onClick(callback).id(id)
  }
  @Builder
  private SettingsSheet() {
    Stack({ alignContent: Alignment.Bottom }) {
      Column().width('100%').height('100%').backgroundColor('#99000000').opacity(this.sheetLift)
        .onClick((): void => this.closeSheet()).id('deck-sheet-scrim')
      Column({ space: 20 }) {
        Row() {
          Column({ space: 6 }) {
            Text('Steam').fontSize(27).fontWeight(FontWeight.Bold).fontColor('#F2F4F7')
            Text('游戏、下载与大屏体验').fontSize(13).fontColor('#9AA3AF')
          }.alignItems(HorizontalAlign.Start).layoutWeight(1)
          Button('关闭').height(38).fontSize(13).fontColor('#9AA3AF').backgroundColor('#1A1D22')
            .onClick((): void => this.closeSheet()).id('deck-sheet-close')
        }.width('100%')
          .gesture(PanGesture({ direction: PanDirection.Down, distance: 70 }).onActionEnd((): void => this.closeSheet()))
        Row({ space: 4 }) {
          ForEach(['client', 'display', 'system'], (tab: string, index: number) => {
            Button(['客户端', '显示与动画', '系统'][index]).height(38).fontSize(this.portrait() ? 12 : 13).borderRadius(10)
              .padding({ left: 8, right: 8 }).layoutWeight(1)
              .fontColor(this.sheetTab === tab ? '#C9E6FF' : '#9AA3AF').backgroundColor(this.sheetTab === tab ? '#201A9FFF' : Color.Transparent)
              .onFocus((): void => { this.focus = 'deck-tab-' + tab; }).onClick((): void => { this.sheetTab = tab; }).id('deck-tab-' + tab)
          }, (tab: string): string => tab)
        }.width('100%')
        Scroll() {
          Column({ space: 10 }) {
            if (this.sheetTab === 'client') {
              this.SheetRow('游戏库', '打开官方 Steam 客户端', 'deck-library', $r('sys.symbol.square_grid_2x2'), (): void => { void this.open('library'); })
              this.SheetRow('下载与组件', '查看组件状态、安装与更新', 'deck-downloads', $r('sys.symbol.arrow_down_circle'), (): void => this.manageComponents())
              this.SheetRow('商店', '探索 Steam 游戏', 'deck-store', $r('sys.symbol.bag'), (): void => { void this.open('store'); })
              this.SheetRow('账号与设置', '登录、Steam Cloud 与偏好', 'deck-account', $r('sys.symbol.person'), (): void => { void this.open('account'); })
            } else if (this.sheetTab === 'display') {
              this.SheetRow('Steam 大屏模式', '为手柄与大屏幕设计', 'deck-big-picture', $r('sys.symbol.gamecontroller'), (): void => { void this.open('bigPicture'); })
              this.SheetRow('Steam 普通窗口', '退出大屏，保留 Deck 主题', 'deck-desktop', $r('sys.symbol.rectangle_grid_1x2'), (): void => { void this.open('desktop'); })
              this.SheetRow('界面动画', this.motion ? '已开启 · 封面漂浮与过渡' : '已关闭 · 静态显示', 'deck-motion', $r('sys.symbol.clock'), (): void => this.toggleMotion())
            } else {
              this.SheetRow('应用设置', '会员、输入、声音与运行配置', 'deck-settings', $r('sys.symbol.gearshape'), this.onSettings)
              this.SheetRow('返回经典界面', '保留当前 Steam 与游戏会话', 'deck-sheet-classic', $r('sys.symbol.rectangle_on_rectangle'), this.onClassic)
            }
          }.width('100%').padding({ bottom: 20 })
        }.width('100%').layoutWeight(1).scrollBar(BarState.Auto)
      }.padding(this.portrait() ? 16 : this.widthVp < 700 ? 22 : 32).width(this.portrait() ? '100%' : this.widthVp < 700 ? '94%' : '70%')
        .height(Math.min(570, this.heightVp - 30)).borderRadius({ topLeft: 24, topRight: 24 })
        .backgroundColor('#0E1013').border({ width: 1, color: '#262A31', radius: { topLeft: 24, topRight: 24 } })
        .translate({ y: (1 - this.sheetLift) * this.heightVp }).id('deck-settings-sheet')
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
      if (this.page === 'steam') this.SteamHome()
      else if (this.page === 'library') this.Library()
      else this.Running()
    }.width(this.layout().contentWidth).height(this.contentHeight()).clip(true)
  }
}
