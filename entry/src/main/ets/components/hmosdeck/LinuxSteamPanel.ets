import { hilog } from '../../service/LogService';
import { common } from '@kit.AbilityKit';
import { router, window } from '@kit.ArkUI';
import { picker } from '@kit.CoreFileKit';
import { LinuxSteamSessionService, LinuxSteamSessionListener } from '../../service/LinuxSteamSessionService';
import { LinuxSteamInstallService, LinuxSteamInstallListener } from '../../service/LinuxSteamInstallService';
import { LinuxSteamInstallSnapshot, linuxSteamInstallBusy, formatLinuxSteamBytes } from '../../model/LinuxSteamDownloadModels';
import { LinuxSteamAction, LinuxSteamSnapshot } from '../../model/LinuxSteamModels';
import { applyStatusOnlyImmersiveShell, getStatusOnlyWindowInsets, observeStatusOnlyWindowInsets } from '../../common/StatusOnlyImmersiveShell';

type ComponentOperation = 'online' | 'local' | 'reset';

@Component
export struct LinuxSteamPanel {
  private context: common.UIAbilityContext = getContext(this) as common.UIAbilityContext;
  @State private state: LinuxSteamSnapshot = LinuxSteamSessionService.getInstance().snapshot();
  @State private installState: LinuxSteamInstallSnapshot = LinuxSteamInstallService.getInstance().snapshot();
  @State private ready: boolean = LinuxSteamInstallService.getInstance().installed();
  @State private section: number = 0;
  @State private busy: boolean = false;
  @State private error: string = '';
  @State private top: number = 32;
  @State private bottom: number = 16;
  @State private wide: boolean = true;
  private lifecycle: number = 0;
  private listener: LinuxSteamSessionListener | null = null;
  private installListener: LinuxSteamInstallListener | null = null;
  private disposeInsets: (() => void) | null = null;
  aboutToAppear(): void {
    const lifecycle = ++this.lifecycle;
    const session = LinuxSteamSessionService.getInstance(); session.initialize(this.context);
    this.listener = (value: LinuxSteamSnapshot): void => { this.state = value; }; session.subscribe(this.listener);
    const installer = LinuxSteamInstallService.getInstance();
    this.installListener = (value: LinuxSteamInstallSnapshot): void => { this.installState = value; this.ready = installer.installed(); };
    installer.subscribe(this.installListener);
    void installer.initialize(this.context).catch((error: Error): void => { this.error = error.message; });
    void window.getLastWindow(this.context).then(async (w: window.Window): Promise<void> => {
      if (lifecycle !== this.lifecycle) return;
      await applyStatusOnlyImmersiveShell(w);
      if (lifecycle !== this.lifecycle) return;
      const update = (): void => { const insets = getStatusOnlyWindowInsets(w); this.top = insets.top + 12; this.bottom = insets.bottom + 16; };
      update(); this.disposeInsets = observeStatusOnlyWindowInsets(w, update);
    }).catch((error: Error): void => {
      // A route/window can disappear before an asynchronous window transaction finishes.
      hilog.warn(0, 'DeckComponents', 'Window setup failed: %{public}s', error.message);
    });
  }
  aboutToDisappear(): void {
    this.lifecycle++;
    if (this.listener) LinuxSteamSessionService.getInstance().unsubscribe(this.listener);
    if (this.installListener) LinuxSteamInstallService.getInstance().unsubscribe(this.installListener);
    if (this.disposeInsets) this.disposeInsets();
  }
  private available(): boolean { return this.ready && !this.locked(); }
  private errorMessage(): string {
    return this.error || (this.installState.phase === 'failed' ? `${this.installState.errorCode} · ${this.installState.error}` : '');
  }
  private locked(): boolean { return this.busy || linuxSteamInstallBusy(this.installState.phase); }
  private async open(action: LinuxSteamAction): Promise<void> {
    if (!this.available()) return;
    this.busy = true; this.error = '';
    // The session restores its own Wine window/ability. Popping this router
    // afterwards can bring the management page back over that desktop.
    try { await LinuxSteamSessionService.getInstance().open(action); }
    catch (error) { this.error = (error as Error).message; }
    finally { this.busy = false; }
  }
  private async stopBeforeMaintenance(): Promise<void> {
    const service = LinuxSteamSessionService.getInstance();
    if (service.snapshot().state === 'stopped' || service.snapshot().state === 'failed') return;
    if (service.snapshot().state === 'running') await service.open('shutdown');
    const deadline = Date.now() + 20000;
    while (Date.now() < deadline) {
      const state = service.snapshot().state;
      if (state === 'stopped' || state === 'failed') return;
      await new Promise<void>((resolve: () => void): void => { setTimeout(resolve, 200); });
    }
    throw new Error('Steam 尚未退出，请先正常关闭 Steam 及其游戏后重试。组件未更改。');
  }
  private async perform(operation: ComponentOperation, uri: string = ''): Promise<void> {
    if (this.locked()) return;
    this.busy = true; this.error = ''; this.section = operation === 'reset' ? 2 : 0;
    try {
      await this.stopBeforeMaintenance();
      const service = LinuxSteamInstallService.getInstance();
      if (operation === 'reset') await service.resetComponents(this.context);
      else if (operation === 'local') await service.importLocal(this.context, uri);
      else await service.install(this.context, true);
    } catch (error) { this.error = (error as Error).message; }
    finally { this.busy = false; }
  }
  private confirm(operation: ComponentOperation, uri: string = ''): void {
    if (this.locked()) return;
    if (!this.ready && operation !== 'reset') { void this.perform(operation, uri); return; }
    AlertDialog.show({ title: operation === 'reset' ? '重置 Steam 运行组件？' : '安装／更新 Steam 组件？',
      message: operation === 'reset' ? '仅清理运行组件和组件下载缓存，保留已下载的游戏库、Steam 账号和存档。完成后需重新安装。请先正常退出正在运行的游戏。' :
        '将先退出 Steam，再更新或修复配套组件。保留游戏库、账号与存档。请先正常退出正在运行的 Steam 游戏。',
      primaryButton: { value: '取消', action: (): void => {} },
      secondaryButton: { value: operation === 'reset' ? '重置组件' : '继续', action: (): void => { void this.perform(operation, uri); } } });
  }
  private async chooseLocal(): Promise<void> {
    if (this.locked()) return;
    this.busy = true; this.error = ''; let selected = '';
    try { const result = await new picker.DocumentViewPicker(this.context).select({ maxSelectNumber: 1,
      fileSuffixFilters: ['HmosDeck Steam 组件包|.zip'] }); selected = result[0] ?? ''; }
    catch (error) { this.error = (error as Error).message; }
    finally { this.busy = false; }
    if (selected) this.confirm('local', selected);
  }
  @Builder
  private Tab(label: string, value: number) {
    Button({ type: ButtonType.Normal }) {
      Row({ space: 8 }) {
        Stack({ alignContent: Alignment.Center }) {
          SymbolGlyph(value === 2 ? $r('sys.symbol.gearshape') : $r('sys.symbol.arrow_down_circle'))
            .fontSize(17).fontColor([this.section === value ? '#F2F4F7' : '#929DAB'])
        }.width(22).height(22).id(value === 2 ? 'linux-steam-maintenance-icon' : 'linux-steam-install-icon')
        Text(label).fontSize(14)
      }.width('100%').height('100%').justifyContent(FlexAlign.Center).alignItems(VerticalAlign.Center)
    }.layoutWeight(1).height(42).padding(0).borderRadius(10)
      .backgroundColor(this.section === value ? '#263343' : Color.Transparent).fontColor(this.section === value ? '#F2F4F7' : '#929DAB')
      .onClick((): void => { this.section = value; }).id('linux-steam-tab-' + value)
  }
  @Builder
  private Action(label: string, detail: string, icon: Resource, action: LinuxSteamAction) {
    Button() {
      Row({ space: 14 }) {
        SymbolGlyph(icon).fontSize(22).fontColor(['#91ADC9']).width(32)
        Column({ space: 4 }) {
          Text(label).fontSize(16).fontColor('#F2F4F7').width('100%')
          Text(detail).fontSize(12).fontColor('#929DAB').width('100%')
        }.layoutWeight(1).alignItems(HorizontalAlign.Start)
        SymbolGlyph($r('sys.symbol.chevron_right')).fontSize(14).fontColor(['#677484'])
      }.width('100%').padding(18)
    }.width('100%').borderRadius(12).backgroundColor('#161B22').enabled(this.available())
      .onClick((): void => { void this.open(action); }).id('linux-steam-' + action)
  }
  @Builder
  private ProgressCard() {
    Column({ space: 12 }) {
      Row({ space: 8 }) {
        Text(this.installState.message).fontSize(16).fontColor(this.installState.phase === 'ready' ? '#9FCBB8' : '#F2F4F7').layoutWeight(1).id('linux-steam-install-status')
        if (linuxSteamInstallBusy(this.installState.phase)) LoadingProgress().width(22).height(22).color('#1A9FFF')
      }.width('100%')
      if (this.installState.phase !== 'idle' && this.installState.phase !== 'ready') {
        Progress({ value: this.installState.receivedBytes, total: Math.max(1, this.installState.totalBytes), type: ProgressType.Linear }).width('100%').color('#1A9FFF')
        Row() {
          Text(`${formatLinuxSteamBytes(this.installState.receivedBytes)} / ${formatLinuxSteamBytes(this.installState.totalBytes)}`)
            .fontSize(13).fontColor('#B7C4D4').layoutWeight(1).id('linux-steam-download-progress')
          if (this.installState.speedBytes > 0) Text(`${formatLinuxSteamBytes(this.installState.speedBytes)}/s`).fontSize(13).fontColor('#929DAB')
        }.width('100%')
        Text(`${this.installState.component} · 组件 ${this.installState.completedComponents}/${this.installState.totalComponents} · 文件 ${this.installState.preparedFiles}/${this.installState.totalFiles}`)
          .fontSize(12).fontColor('#929DAB').width('100%').id('linux-steam-install-files')
      }
      if (linuxSteamInstallBusy(this.installState.phase)) Button('暂停任务').height(38).fontSize(13).backgroundColor('#263343').fontColor('#D8E5F3')
        .onClick((): void => LinuxSteamInstallService.getInstance().pause()).id('linux-steam-install-pause')
    }.width('100%').alignItems(HorizontalAlign.Start).padding(20).backgroundColor('#121A24').borderRadius(14)
  }
  @Builder
  private Components() {
    Column({ space: 18 }) {
      Text('这里管理 Steam 运行组件；游戏的下载与更新请在 Steam 中查看。').fontSize(13).fontColor('#929DAB').width('100%')
      this.ProgressCard()
      Column({ space: 14 }) {
        Text(this.ready ? '更新与修复' : '选择安装方式').fontSize(20).fontWeight(FontWeight.Bold).fontColor('#F2F4F7').width('100%')
        Text('在线获取官方组件，或从本地包快速导入。两种方式共用同一个游戏库。').fontSize(14).fontColor('#929DAB').width('100%')
        Button(this.ready ? '在线更新／修复' : this.installState.phase === 'paused' ? '继续在线安装' : this.installState.phase === 'failed' ? '重试在线安装' : '在线安装')
          .width('100%').height(48).fontSize(16).fontWeight(FontWeight.Bold).borderRadius(12).backgroundColor('#1A9FFF').fontColor('#FFFFFF')
          .enabled(!this.locked()).onClick((): void => this.confirm('online')).id('linux-steam-install-start')
        Button() {
          Row({ space: 10 }) {
            SymbolGlyph($r('sys.symbol.archivebox')).fontSize(20).fontColor(['#B7CDE5'])
            Text(this.ready ? '导入本地包／更新' : '导入本地组件包').fontSize(16).fontColor('#D8E5F3')
          }
        }.width('100%').height(48).borderRadius(12).backgroundColor('#263343').enabled(!this.locked())
          .onClick((): void => { void this.chooseLocal(); }).id('linux-steam-import-local')
        Text(`在线下载约 ${formatLinuxSteamBytes(this.installState.totalBytes)}，安装预留 ${formatLinuxSteamBytes(this.installState.requiredSpaceBytes)} 空间。下载慢时可在电脑获取配套组件包，再选择本地导入。`)
          .fontSize(12).fontColor('#929DAB').width('100%')
        Text('仅支持与当前应用版本配套的 HmosDeck Steam 组件包；整包校验通过后才会安装。').fontSize(12).fontColor('#929DAB').width('100%')
        Text('安装完成后留在本页，可按需点击“启动 Steam”。').fontSize(12).fontColor('#929DAB').width('100%')
      }.width('100%').alignItems(HorizontalAlign.Start).padding(22).backgroundColor('#161B22').borderRadius(14)
    }.width('100%')
  }
  @Builder
  private Maintenance() {
    Column({ space: 18 }) {
      if (this.state.state === 'running') this.Action('退出 Steam', '正常结束客户端会话', $r('sys.symbol.person'), 'shutdown')
      Column({ space: 14 }) {
        Text('重置运行组件').fontSize(20).fontWeight(FontWeight.Bold).fontColor('#F2F4F7').width('100%')
        Text('清理 Steam 运行组件和组件下载缓存。重置后可重新在线安装，或导入本地包。').fontSize(14).fontColor('#929DAB').width('100%')
        Text('保留已下载的游戏库、账号和存档。').fontSize(14).fontColor('#9FCBB8').width('100%')
        Button('清空并重置运行组件').width('100%').height(46).fontSize(15).backgroundColor('#38262B').fontColor('#FFB5A7')
          .enabled(!this.locked()).onClick((): void => this.confirm('reset')).id('linux-steam-reset')
      }.width('100%').alignItems(HorizontalAlign.Start).padding(22).backgroundColor('#161B22').borderRadius(14)
      if (linuxSteamInstallBusy(this.installState.phase) || !this.ready) this.ProgressCard()
    }.width('100%')
  }
  build() {
    Column({ space: 18 }) {
      Row({ space: 14 }) {
        Button('返回').height(38).fontSize(14).backgroundColor('#1A2029').fontColor('#B7C4D4').onClick((): void => router.back()).id('linux-steam-back')
        Text('Steam 组件').fontSize(24).fontWeight(FontWeight.Bold).fontColor('#F2F4F7').layoutWeight(1)
        Text(this.ready ? '已安装' : '待安装').fontSize(12).fontColor(this.ready ? '#9FCBB8' : '#9AAFC7')
          .padding({ left: 12, right: 12, top: 7, bottom: 7 }).borderRadius(20).backgroundColor('#1C2933')
      }.width('100%')
      Scroll() {
        Column({ space: 20 }) {
          Row({ space: 18 }) {
            Stack({ alignContent: Alignment.Center }) {
              SymbolGlyph($r('sys.symbol.gamecontroller')).fontSize(38).fontColor(['#66B8FF'])
            }.width(72).height(72).backgroundColor('#172B40').borderRadius(18).id('linux-steam-component-icon')
            Column({ space: 7 }) {
              Text('Steam 运行组件').fontSize(this.wide ? 25 : 20).fontWeight(FontWeight.Bold).fontColor('#F2F4F7').width('100%')
              Text(this.ready ? '组件已安装，可按需启动或更新' : '在线安装或导入本地包，连接你的游戏库').fontSize(14).fontColor('#929DAB').width('100%').id('linux-steam-status')
            }.layoutWeight(1)
          }.width('100%').padding({ top: 8, bottom: 6 })
          if (this.ready) Button(this.state.state === 'running' ? '返回 Steam' : '启动 Steam').width('100%').height(50).fontSize(17).fontWeight(FontWeight.Bold).backgroundColor('#1A9FFF')
            .fontColor('#FFFFFF').borderRadius(12).enabled(this.available()).onClick((): void => { void this.open('library'); }).id('linux-steam-open')
          Row({ space: 4 }) { this.Tab('安装与更新', 0); this.Tab('维护', 2); }
            .width('100%').padding(4).backgroundColor('#14191F').borderRadius(14)
          if (this.errorMessage()) Text(this.errorMessage()).fontSize(14).fontColor('#FFB5A7').width('100%')
            .padding(16).backgroundColor('#30252A').borderRadius(12).id('linux-steam-error')
          if (this.busy && !linuxSteamInstallBusy(this.installState.phase)) Text('正在处理，请稍候…').fontSize(13).fontColor('#66B8FF').width('100%')
          if (this.section === 0) this.Components()
          else this.Maintenance()
        }.width('100%').constraintSize({ maxWidth: 760 }).padding({ bottom: 24 })
      }.layoutWeight(1).width('100%').scrollBar(BarState.Off).align(Alignment.Top)
    }.width('100%').height('100%').padding({ left: this.wide ? 32 : 20, right: this.wide ? 32 : 20, top: this.top, bottom: this.bottom })
      .backgroundColor('#0A0B0D').onAreaChange((_old: Area, area: Area): void => { this.wide = Number(area.width) > 600; }).id('linux-steam-page')
  }
}
