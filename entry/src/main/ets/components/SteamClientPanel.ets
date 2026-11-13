import { promptAction } from '@kit.ArkUI';
import { SteamLocalService, SteamImportProgress, SteamImportListener } from '../steam/SteamLocalService';
import { AppCatalogService } from '../service/AppCatalogService';
import { SteamClientService } from '../service/SteamClientService';
import { common } from '@kit.AbilityKit';
import { router } from '@kit.ArkUI';
import { skinColor } from '../service/SkinColors';

/** One client control surface shared by both launcher skins. */
@Component
export struct SteamClientPanel {
  private context: common.UIAbilityContext = getContext(this) as common.UIAbilityContext;
  onChanged: () => void = (): void => {};
  onOpen: () => void = (): void => {};
  managementOnly: boolean = false;
  showManagement: boolean = true;
  dark: boolean = false;
  @State private installed: boolean = false;
  @State private busy: boolean = false;
  @State private launching: boolean = false;
  @State private scanning: boolean = false;
  @State private compatibility: boolean = false;
  @State private message: string = '';
  @State private percent: number = 0;
  private listener: SteamImportListener | null = null;

  aboutToAppear(): void {
    const service = SteamLocalService.getInstance();
    this.installed = service.isInstalled();
    this.compatibility = service.isCompatibility();
    this.listener = (progress: SteamImportProgress): void => {
      this.busy = progress.busy;
      this.message = progress.message;
      this.percent = progress.percent;
      this.installed = service.isInstalled();
    };
    service.subscribe(this.listener);
  }
  aboutToDisappear(): void {
    if (this.listener) SteamLocalService.getInstance().unsubscribe(this.listener);
  }
  private foreground(): ResourceColor { return this.dark ? '#F2F4F7' : skinColor('text_primary'); }
  private secondary(): ResourceColor { return this.dark ? '#929DAB' : skinColor('text_secondary'); }
  private buttonBackground(): ResourceColor { return this.dark ? '#263343' : skinColor('search_background'); }
  private accent(): ResourceColor { return this.dark ? '#1A9FFF' : skinColor('accent'); }
  private async importClient(): Promise<void> {
    if (this.busy || this.launching || this.scanning) return;
    try {
      if (await SteamLocalService.getInstance().importZip()) {
        this.installed = true;
        await AppCatalogService.getInstance().scan();
        this.onChanged();
      }
    } catch (error) { this.message = (error as Error).message; }
  }
  private async openClient(): Promise<void> {
    if (this.busy || this.launching || this.scanning) return;
    this.launching = true;
    try {
      const service = SteamClientService.getInstance();
      this.onOpen();
      await service.open();
    } catch (error) { promptAction.showToast({ message: (error as Error).message }); }
    finally { this.launching = false; }
  }
  private async scanGames(): Promise<void> {
    if (this.busy || this.launching || this.scanning) return;
    this.scanning = true;
    try {
      await AppCatalogService.getInstance().scan();
      this.message = '已刷新 Windows Steam 游戏库';
      this.onChanged();
    } catch (error) { this.message = (error as Error).message; }
    finally { this.scanning = false; }
  }
  private openComponents(): void {
    this.onOpen();
    void router.pushUrl({ url: 'pages/SteamComponents' }).catch((error: Error): void => {
      promptAction.showToast({ message: error.message });
    });
  }
  build() {
    Column({ space: 8 }) {
      Text(this.managementOnly ? 'Windows Steam' : 'Steam 客户端').fontSize(15).fontWeight(FontWeight.Medium)
        .fontColor(this.foreground()).width('100%')
      if (!this.managementOnly && this.installed) {
        Button(this.launching ? '正在打开…' : '启动 Steam').width('100%').height(40).borderRadius(10)
          .backgroundColor(this.accent()).fontColor('#FFFFFFFF').enabled(!this.busy && !this.launching && !this.scanning)
          .onClick((): void => { void this.openClient(); }).id('steam-client-open')
      }
      Text(this.installed ? 'Windows 版已导入' : '导入原来的 Windows Steam ZIP，通过 Wine 运行。')
        .fontSize(12).fontColor(this.secondary()).width('100%')
      Button(this.installed ? '更新 Windows Steam ZIP' : '导入 Windows Steam ZIP')
        .width('100%').height(40).borderRadius(10)
        .backgroundColor(this.buttonBackground()).fontColor(this.foreground()).enabled(!this.busy && !this.launching && !this.scanning)
        .onClick((): void => { void this.importClient(); })
        .id('steam-client-primary')
      if (!this.managementOnly && this.showManagement) Button('Steam 组件管理').width('100%').height(38).borderRadius(10)
        .fontColor(this.foreground()).backgroundColor(this.buttonBackground()).enabled(!this.busy && !this.launching && !this.scanning)
        .onClick((): void => this.openComponents()).id('steam-client-components')
      if (this.installed) {
          Button('扫描 Windows Steam 游戏').width('100%').height(38).borderRadius(10)
            .fontColor(this.foreground()).backgroundColor(this.buttonBackground())
            .enabled(!this.busy && !this.launching && !this.scanning).onClick((): void => { void this.scanGames(); }).id('steam-client-scan')
        Row({ space: 6 }) {
          Text('Windows 版兼容模式（关闭 GPU）').fontSize(12).fontColor(this.secondary()).layoutWeight(1)
          Toggle({ type: ToggleType.Switch, isOn: this.compatibility })
            .enabled(!this.busy).onChange((value: boolean): void => {
              this.compatibility = value;
              void SteamLocalService.getInstance().setCompatibility(value);
              this.message = '将在下次启动 Steam 时生效';
            }).id('steam-client-compatibility')
        }.width('100%')
      }
      if (this.busy) {
        Progress({ value: this.percent, total: 100, type: ProgressType.Linear }).width('100%').color(this.accent())
      }
      if (this.message) Text(this.message).fontSize(12).fontColor(this.secondary()).width('100%')
    }.width('100%').alignItems(HorizontalAlign.Start).id('steam-client-panel')
  }
}
