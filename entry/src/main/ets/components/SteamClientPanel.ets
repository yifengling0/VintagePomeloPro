import { promptAction } from '@kit.ArkUI';
import { SteamLocalService, SteamImportProgress, SteamImportListener } from '../steam/SteamLocalService';
import { AppCatalogService } from '../service/AppCatalogService';
import { AppSettingsStore } from '../service/AppSettingsStore';
import { AppSessionService } from '../service/AppSessionService';
import { skinColor } from '../service/SkinColors';

/** One client control surface shared by both launcher skins. */
@Component
export struct SteamClientPanel {
  onChanged: () => void = (): void => {};
  onOpen: () => void = (): void => {};
  @State private installed: boolean = false;
  @State private busy: boolean = false;
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
  private async importClient(): Promise<void> {
    try {
      if (await SteamLocalService.getInstance().importZip()) {
        this.installed = true;
        await AppCatalogService.getInstance().scan();
        this.onChanged();
      }
    } catch (error) { this.message = (error as Error).message; }
  }
  private async openClient(): Promise<void> {
    try {
      this.onOpen();
      const catalog = AppCatalogService.getInstance();
      const result = await AppSessionService.getInstance().launch(SteamLocalService.getInstance().clientCard(),
        AppSettingsStore.getInstance().getGlobalSettings(), catalog.getWineHomeDirectory());
      if (!result) throw new Error('Steam 启动失败，请重试');
    } catch (error) { promptAction.showToast({ message: (error as Error).message }); }
  }
  build() {
    Column({ space: 8 }) {
      Text('Steam 客户端').fontSize(15).fontWeight(FontWeight.Medium)
        .fontColor(skinColor('text_primary')).width('100%')
      Button(this.installed ? '打开 Steam' : '导入 Steam ZIP')
        .width('100%').height(40).borderRadius(10)
        .backgroundColor(skinColor('accent')).fontColor('#FFFFFFFF').enabled(!this.busy)
        .onClick((): void => { if (this.installed) void this.openClient(); else void this.importClient(); })
        .id('steam-client-primary')
      if (this.installed) {
        Row({ space: 6 }) {
          Button('更新客户端').layoutWeight(1).height(38).borderRadius(10)
            .fontColor(skinColor('text_primary')).backgroundColor(skinColor('search_background'))
            .enabled(!this.busy).onClick((): void => { void this.importClient(); }).id('steam-client-update')
          Button('扫描游戏').layoutWeight(1).height(38).borderRadius(10)
            .fontColor(skinColor('text_primary')).backgroundColor(skinColor('search_background'))
            .enabled(!this.busy).onClick((): void => { this.onChanged(); }).id('steam-client-scan')
        }.width('100%')
        Row({ space: 6 }) {
          Text('兼容模式（关闭 GPU）').fontSize(12).fontColor(skinColor('text_secondary')).layoutWeight(1)
          Toggle({ type: ToggleType.Switch, isOn: this.compatibility })
            .enabled(!this.busy).onChange((value: boolean): void => {
              this.compatibility = value;
              void SteamLocalService.getInstance().setCompatibility(value);
              this.message = '将在下次启动 Steam 时生效';
            }).id('steam-client-compatibility')
        }.width('100%')
      }
      if (this.busy) {
        Progress({ value: this.percent, total: 100, type: ProgressType.Linear }).width('100%').color(skinColor('accent'))
      }
      if (this.message) Text(this.message).fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
    }.width('100%').alignItems(HorizontalAlign.Start).id('steam-client-panel')
  }
}
