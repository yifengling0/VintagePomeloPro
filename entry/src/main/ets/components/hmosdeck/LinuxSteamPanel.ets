import { ProtonRuntimePanel } from './ProtonRuntimePanel';
import { LinuxProtonRuntimeService, ProtonRuntimeListener } from '../../service/LinuxProtonRuntimeService';
import { ProtonRuntimeSnapshot } from '../../model/LinuxProtonRuntimeModels';
import { linuxSteamDownloadHttp1, setLinuxSteamDownloadHttp1 } from '../../service/LinuxSteamDownloadPolicy';
import { LinuxSteamStorageLocation } from '../../service/LinuxSteamPaths';
import { hilog } from '../../service/LogService';
import { common } from '@kit.AbilityKit';
import { router, window } from '@kit.ArkUI';
import { picker } from '@kit.CoreFileKit';
import { LinuxSteamSessionService, LinuxSteamSessionListener } from '../../service/LinuxSteamSessionService';
import { LinuxSteamInstallService, LinuxSteamInstallListener } from '../../service/LinuxSteamInstallService';
import { LinuxSteamInstallSnapshot, linuxSteamInstallBusy, formatLinuxSteamBytes } from '../../model/LinuxSteamDownloadModels';
import { LinuxSteamAction, LinuxSteamSnapshot } from '../../model/LinuxSteamModels';
import { SteamClientPanel } from '../SteamClientPanel';
import { SteamLocalService, SteamImportListener, SteamImportProgress } from '../../steam/SteamLocalService';
import { SteamClientService } from '../../service/SteamClientService';
import { AppCatalogService } from '../../service/AppCatalogService';
import { applyStatusOnlyImmersiveShell, getStatusOnlyWindowInsets, observeStatusOnlyWindowInsets } from '../../common/StatusOnlyImmersiveShell';

type ComponentOperation = 'online' | 'local' | 'reset';

@Component
export struct LinuxSteamPanel {
  private context: common.UIAbilityContext = getContext(this) as common.UIAbilityContext;
  @State private state: LinuxSteamSnapshot = LinuxSteamSessionService.getInstance().snapshot();
  @State private installState: LinuxSteamInstallSnapshot = LinuxSteamInstallService.getInstance().snapshot();
  @State private ready: boolean = LinuxSteamInstallService.getInstance().installed();
  @State private windowsReady: boolean = SteamLocalService.getInstance().isInstalled();
  @State private windowsBusy: boolean = false;
  @State private downloadHttp1: boolean = linuxSteamDownloadHttp1();
  @State private location: LinuxSteamStorageLocation = LinuxSteamInstallService.getInstance().location();
  @State private section: number = 0;
  @State private busy: boolean = false;
  @State private protonBusy: boolean = false;
  private protonListener: ProtonRuntimeListener | null = null;
  @State private error: string = '';
  @State private top: number = 32;
  @State private bottom: number = 16;
  @State private wide: boolean = true;
  private lifecycle: number = 0;
  private listener: LinuxSteamSessionListener | null = null;
  private installListener: LinuxSteamInstallListener | null = null;
  private windowsListener: SteamImportListener | null = null;
  private disposeInsets: (() => void) | null = null;
  aboutToAppear(): void {
    const lifecycle = ++this.lifecycle;
    this.protonListener = (value: ProtonRuntimeSnapshot): void => { this.protonBusy = value.busy; };
    LinuxProtonRuntimeService.getInstance().subscribe(this.protonListener);
    const session = LinuxSteamSessionService.getInstance(); session.initialize(this.context);
    this.listener = (value: LinuxSteamSnapshot): void => { this.state = value; }; session.subscribe(this.listener);
    const installer = LinuxSteamInstallService.getInstance();
    this.installListener = (value: LinuxSteamInstallSnapshot): void => { this.installState = value; this.ready = installer.installed(); this.location = installer.location(); };
    installer.subscribe(this.installListener);
    void installer.initialize(this.context).catch((error: Error): void => { this.error = error.message; });
    this.windowsListener = (value: SteamImportProgress): void => {
      this.windowsReady = SteamLocalService.getInstance().isInstalled(); this.windowsBusy = value.busy;
    };
    SteamLocalService.getInstance().subscribe(this.windowsListener);
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
    if (this.protonListener) LinuxProtonRuntimeService.getInstance().unsubscribe(this.protonListener);
    if (this.listener) LinuxSteamSessionService.getInstance().unsubscribe(this.listener);
    if (this.installListener) LinuxSteamInstallService.getInstance().unsubscribe(this.installListener);
    if (this.windowsListener) SteamLocalService.getInstance().unsubscribe(this.windowsListener);
    if (this.disposeInsets) this.disposeInsets();
  }
  private available(): boolean { return (this.ready || this.windowsReady) && !this.locked(); }
  private errorMessage(): string {
    return this.error || (this.installState.phase === 'failed' ? `${this.installState.errorCode} · ${this.installState.error}` : '');
  }
  private locked(): boolean { return this.busy || this.windowsBusy || this.protonBusy || linuxSteamInstallBusy(this.installState.phase); }
  private locationLocked(): boolean {
    return this.locked() || !['stopped', 'failed'].includes(this.state.state);
  }
  private selectLocation(location: LinuxSteamStorageLocation): void {
    if (this.location === location || this.locationLocked()) return;
    AlertDialog.show({ title: location === 'private' ? '使用 C 盘私有位置' : '使用 Download 位置',
      message: '切换位置不会移动或删除原位置的组件、游戏和存档，可随时切回。新位置需要单独安装组件和登录 Steam。',
      primaryButton: { value: '取消', action: (): void => {} },
      secondaryButton: { value: '切换位置', action: (): void => {
        this.busy = true; this.error = '';
        void LinuxSteamInstallService.getInstance().selectLocation(this.context, location)
          .catch((error: Error): void => { this.error = error.message; })
          .finally((): void => { this.busy = false; });
      } }
    });
  }
  private async open(action: LinuxSteamAction): Promise<void> {
    if (!this.available()) return;
    this.busy = true; this.error = '';
    // The session restores its own Wine window/ability. Popping this router
    // afterwards can bring the management page back over that desktop.
    try {
      if (action === 'shutdown') await LinuxSteamSessionService.getInstance().open(action);
      else {
        const service = SteamClientService.getInstance();
        const backend = await service.choose(this.getUIContext());
        if (backend) await service.open(this.context, backend, action);
      }
    }
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
      const windows = SteamLocalService.getInstance();
      if (windows.hasRunningProcesses() || windows.hasForeignClient()) throw new Error('请先正常退出 Windows Steam 及其游戏，再维护 Linux Steam 组件');
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
  private StorageOption(location: LinuxSteamStorageLocation, title: string, detail: string) {
    Button() {
      Row({ space: 12 }) {
        SymbolGlyph(this.location === location ? $r('sys.symbol.checkmark_circle_fill') : $r('sys.symbol.circle'))
          .fontSize(20).fontColor([this.location === location ? '#66B8FF' : '#677484'])
        Column({ space: 5 }) {
          Text(title).fontSize(15).fontColor('#F2F4F7').width('100%')
          Text(detail).fontSize(12).fontColor('#929DAB').width('100%')
        }.layoutWeight(1).alignItems(HorizontalAlign.Start)
      }.width('100%').padding(14)
    }.width('100%').borderRadius(12).backgroundColor(this.location === location ? '#172B40' : '#161B22')
      .enabled(!this.locationLocked()).onClick((): void => this.selectLocation(location)).id('linux-steam-storage-' + location)
  }
  @Builder
  private Components() {
    Column({ space: 18 }) {
      Text('可只安装一个版本，也可同时保留两个。只安装一个时直接启动；两个都有时，启动前选择版本。游戏的下载与更新在各自 Steam 中管理。')
        .fontSize(13).fontColor('#929DAB').width('100%')
      Column() {
        SteamClientPanel({ managementOnly: true, dark: true,
          onChanged: (): void => {
            void AppCatalogService.getInstance().scan().catch((error: Error): void => { this.error = error.message; });
          } })
      }.width('100%').padding(22).backgroundColor('#161B22').borderRadius(14)
        .enabled(!this.busy && !linuxSteamInstallBusy(this.installState.phase))
      Text('Linux ARM64 Steam').fontSize(20).fontWeight(FontWeight.Bold).fontColor('#F2F4F7').width('100%')
      Column({ space: 10 }) {
        Text('安装位置').fontSize(17).fontWeight(FontWeight.Bold).fontColor('#F2F4F7').width('100%')
        this.StorageOption('private', 'C 盘 · 应用私有', '组件和游戏仅在应用内访问；重置 Wine 会保留，卸载应用会删除。')
        this.StorageOption('download', 'Download · 公共下载目录', '可在系统文件管理器中访问，便于管理或备份游戏文件。')
        Text(this.locationLocked() && !this.locked() ? '退出 Steam 和游戏后可切换位置。' : '两处位置独立保存；切换不会搬移原有游戏和存档。')
          .fontSize(12).fontColor('#929DAB').width('100%')
      }.width('100%').alignItems(HorizontalAlign.Start)
      ProtonRuntimePanel({ blocked: this.busy || this.windowsBusy || linuxSteamInstallBusy(this.installState.phase) || !['stopped', 'failed'].includes(this.state.state) })
      this.ProgressCard()
      Column({ space: 14 }) {
        Text(this.ready ? '更新与修复' : '选择安装方式').fontSize(20).fontWeight(FontWeight.Bold).fontColor('#F2F4F7').width('100%')
        Text('在线获取官方组件，或从本地包快速导入。两种方式都安装到上方选定的位置。').fontSize(14).fontColor('#929DAB').width('100%')
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
      Column({ space: 14 }) {
        Text('游戏下载连接').fontSize(20).fontWeight(FontWeight.Bold).fontColor('#F2F4F7').width('100%')
        Row({ space: 12 }) {
          Text('使用 HTTP/1.1 兼容模式').fontSize(15).fontColor('#D8E5F3').layoutWeight(1)
          Toggle({ type: ToggleType.Switch, isOn: this.downloadHttp1 }).selectedColor('#1A9FFF')
            .enabled(!this.locationLocked()).onChange((enabled: boolean): void => {
              try { setLinuxSteamDownloadHttp1(enabled); this.downloadHttp1 = enabled; }
              catch (error) { this.error = (error as Error).message; }
            }).id('linux-steam-download-http1')
        }.width('100%')
        Text('遇到游戏下载慢或频繁停顿时可尝试。只调整 Linux Steam 的内容下载连接，不改变下载地区或限速；退出 Steam 后可修改，下次启动生效。')
          .fontSize(13).fontColor('#929DAB').width('100%')
      }.width('100%').alignItems(HorizontalAlign.Start).padding(22).backgroundColor('#161B22').borderRadius(14)
      if (this.state.state === 'running') this.Action('退出 Steam', '正常结束客户端会话', $r('sys.symbol.person'), 'shutdown')
      Column({ space: 14 }) {
        Text('重置 Linux Steam 运行组件').fontSize(20).fontWeight(FontWeight.Bold).fontColor('#F2F4F7').width('100%')
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
        Text(this.ready && this.windowsReady ? '两个版本已安装' : this.ready ? 'Linux 已安装' : this.windowsReady ? 'Windows 已导入' : '待安装')
          .fontSize(12).fontColor(this.ready || this.windowsReady ? '#9FCBB8' : '#9AAFC7')
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
              Text(this.ready || this.windowsReady ? '可按需启动，两个版本的客户端与游戏库独立保存' : '在线安装 Linux 版，或导入 Linux／Windows 本地包')
                .fontSize(14).fontColor('#929DAB').width('100%').id('linux-steam-status')
            }.layoutWeight(1)
          }.width('100%').padding({ top: 8, bottom: 6 })
          if (this.ready || this.windowsReady) Button(this.ready && this.windowsReady ? '选择并启动 Steam' : '启动 Steam').width('100%').height(50).fontSize(17).fontWeight(FontWeight.Bold).backgroundColor('#1A9FFF')
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
