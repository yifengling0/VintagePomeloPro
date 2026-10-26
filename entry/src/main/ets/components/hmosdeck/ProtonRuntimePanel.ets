import { common } from '@kit.AbilityKit';
import { picker } from '@kit.CoreFileKit';
import { LinuxProtonRuntimeService, ProtonRuntimeListener } from '../../service/LinuxProtonRuntimeService';
import { ProtonRuntimeSnapshot, ProtonRuntimeSource, ProtonSteamRuntime, ProtonRuntimeVersion, ProtonRuntimeRelease } from '../../model/LinuxProtonRuntimeModels';
import { formatLinuxSteamBytes } from '../../model/LinuxSteamDownloadModels';

@Component
export struct ProtonRuntimePanel {
  @Prop blocked: boolean = false;
  private context: common.UIAbilityContext = getContext(this) as common.UIAbilityContext;
  private service: LinuxProtonRuntimeService = LinuxProtonRuntimeService.getInstance();
  private listener: ProtonRuntimeListener | null = null;
  @State private state: ProtonRuntimeSnapshot = LinuxProtonRuntimeService.getInstance().snapshot();
  @State private steamVersions: ProtonSteamRuntime[] = [];
  @State private error: string = '';
  @State private scanning: boolean = false;
  aboutToAppear(): void {
    this.listener = (value: ProtonRuntimeSnapshot): void => { this.state = value; };
    this.service.subscribe(this.listener);
    void this.service.initialize(this.context).catch((error: Error): void => { this.error = error.message; });
  }
  aboutToDisappear(): void { if (this.listener) this.service.unsubscribe(this.listener); }
  private canMaintain(): boolean { return !this.blocked && !this.state.busy && !this.scanning; }
  private source(value: ProtonRuntimeSource): string {
    if (value === 'embedded') return '随应用提供';
    if (value === 'steam') return '从 Steam 导入';
    return value === 'local' ? '本地导入' : '在线安装';
  }
  private run(operation: Promise<void>): void {
    this.error = '';
    void operation.catch((error: Error): void => { this.error = error.message; });
  }
  private async chooseLocal(id: string): Promise<void> {
    if (!this.canMaintain()) return;
    this.scanning = true; this.error = ''; let selected = '';
    try {
      const result = await new picker.DocumentViewPicker(this.context).select({ maxSelectNumber: 1,
        fileSuffixFilters: ['HmosDeck Proton 运行包|.zip'] });
      selected = result[0] ?? '';
    } catch (error) { this.error = (error as Error).message; }
    finally { this.scanning = false; }
    if (selected) this.run(this.service.install(this.context, id, selected));
  }
  private async scanSteam(): Promise<void> {
    if (!this.canMaintain()) return;
    this.scanning = true; this.error = ''; this.steamVersions = [];
    try {
      this.steamVersions = await this.service.discoverSteam(this.context);
      if (this.steamVersions.length === 0) this.error = 'Steam 游戏库中尚未发现 Proton；可使用下方受支持版本。';
    } catch (error) { this.error = (error as Error).message; }
    finally { this.scanning = false; }
  }
  private remove(id: string): void {
    AlertDialog.show({ title: '移除未使用的 Proton 版本？', message: '仅移除该运行包，游戏、存档和 Steam 原安装保持不变。',
      primaryButton: { value: '取消', action: (): void => {} },
      secondaryButton: { value: '移除', action: (): void => { this.run(this.service.remove(this.context, id)); } } });
  }
  build() {
    Column({ space: 14 }) {
      Text('Proton 运行版本').fontSize(20).fontWeight(FontWeight.Bold).fontColor('#F2F4F7').width('100%')
      Text('运行包可独立更新，鸿蒙适配随应用更新。Steam 中保留 GNU Proton 与 HmosDeck Wine 的兼容性选择。')
        .fontSize(13).fontColor('#929DAB').width('100%')
      Text(this.state.activeId ? this.state.message : '当前使用随应用提供的版本；首次准备 Steam 时自动登记。')
        .fontSize(14).fontColor('#B7CDE5').width('100%').id('proton-runtime-status')
      if (this.blocked && !this.state.busy) Text('请先退出 Steam 和游戏，再维护运行版本。').fontSize(13).fontColor('#E7BE87').width('100%')
      if (this.state.busy) {
        LoadingProgress().width(24).height(24).color('#1A9FFF')
        Text(this.state.message).fontSize(13).fontColor('#B7CDE5').width('100%')
        if (this.state.totalBytes > 0) {
          Progress({ value: this.state.receivedBytes, total: this.state.totalBytes, type: ProgressType.Linear }).width('100%').color('#1A9FFF')
          Text(`${formatLinuxSteamBytes(this.state.receivedBytes)} / ${formatLinuxSteamBytes(this.state.totalBytes)}`)
            .fontSize(12).fontColor('#929DAB').width('100%')
          Button('暂停下载').fontSize(13).backgroundColor('#263343')
            .onClick((): void => this.service.pauseDownload()).id('proton-runtime-pause')
        }
      }
      if (this.error || this.state.error) Text(this.error || this.state.error).fontSize(13).fontColor('#FFB3AC').width('100%').id('proton-runtime-error')
      Row({ space: 10 }) {
        Button('检查更新').fontSize(13).backgroundColor('#263343').layoutWeight(1).enabled(this.canMaintain())
          .onClick((): void => this.run(this.service.checkUpdates(this.context))).id('proton-runtime-check')
        Button('查找 Steam 中的 Proton').fontSize(13).backgroundColor('#263343').layoutWeight(1).enabled(this.canMaintain())
          .onClick((): void => { void this.scanSteam(); }).id('proton-runtime-scan')
      }.width('100%')
      ForEach(this.state.versions, (version: ProtonRuntimeVersion) => {
        Column({ space: 8 }) {
          Text(`${version.name}${version.active ? ' · 当前使用' : ''}`).fontSize(15).fontColor('#F2F4F7').width('100%')
          Text(this.source(version.source)).fontSize(12).fontColor('#929DAB').width('100%')
          if (!version.active) Row({ space: 10 }) {
            Button(version.id === this.state.previousId ? '回退到此版本' : '使用此版本').fontSize(13).backgroundColor('#263343')
              .enabled(this.canMaintain()).onClick((): void => this.run(this.service.select(this.context, version.id)))
            if (version.id !== this.state.previousId) Button('移除').fontSize(13).backgroundColor('#263343')
              .enabled(this.canMaintain()).onClick((): void => this.remove(version.id))
          }.width('100%')
        }.width('100%').alignItems(HorizontalAlign.Start).padding(14).borderRadius(10).backgroundColor('#10151D')
      }, (version: ProtonRuntimeVersion): string => version.id)
      ForEach(this.state.available, (release: ProtonRuntimeRelease) => {
        Column({ space: 8 }) {
          Text(release.name).fontSize(15).fontColor('#F2F4F7').width('100%')
          Text(`下载 ${formatLinuxSteamBytes(release.archive.bytes)} · 运行包 ${formatLinuxSteamBytes(release.installedBytes)}`)
            .fontSize(12).fontColor('#929DAB').width('100%')
          Row({ space: 10 }) {
            Button('在线安装／修复').fontSize(13).backgroundColor('#1A9FFF').layoutWeight(1).enabled(this.canMaintain())
              .onClick((): void => this.run(this.service.install(this.context, release.id)))
            Button('导入此版本 ZIP').fontSize(13).backgroundColor('#263343').layoutWeight(1).enabled(this.canMaintain())
              .onClick((): void => { void this.chooseLocal(release.id); })
          }.width('100%')
        }.width('100%').alignItems(HorizontalAlign.Start)
      }, (release: ProtonRuntimeRelease): string => release.id)
      ForEach(this.steamVersions, (candidate: ProtonSteamRuntime) => {
        Column({ space: 8 }) {
          Text(candidate.name).fontSize(14).fontColor('#F2F4F7').width('100%')
          if (candidate.supported) Button('导入并使用').fontSize(13).backgroundColor('#263343').enabled(this.canMaintain())
            .onClick((): void => this.run(this.service.importSteam(this.context, candidate)))
          else Text('此架构或版本尚未适配，暂不可启用。').fontSize(12).fontColor('#929DAB').width('100%')
        }.width('100%').alignItems(HorizontalAlign.Start)
      }, (candidate: ProtonSteamRuntime): string => candidate.path)
      Text('仅启用经过校验的 ARM64 版本。切换仅更换运行包，保留游戏与存档；Steam 下载的原包不会被修改。')
        .fontSize(12).fontColor('#929DAB').width('100%')
    }.width('100%').alignItems(HorizontalAlign.Start).padding(22).backgroundColor('#161B22').borderRadius(14)
  }
}
