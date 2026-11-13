import { common } from '@kit.AbilityKit';
import { router, window } from '@kit.ArkUI';
import { SteamClientPanel } from './SteamClientPanel';
import { applyStatusOnlyImmersiveShell, getStatusOnlyWindowInsets, observeStatusOnlyWindowInsets } from '../common/StatusOnlyImmersiveShell';
import { hilog } from '../service/LogService';

@Component
export struct SteamComponentsPanel {
  legacyLinux: boolean = false;
  @State private top: number = 12;
  @State private bottom: number = 16;
  private lifecycle: number = 0;
  private disposeInsets: (() => void) | null = null;
  aboutToAppear(): void {
    const lifecycle = ++this.lifecycle;
    const context = getContext(this) as common.UIAbilityContext;
    void window.getLastWindow(context).then(async (w: window.Window): Promise<void> => {
      if (lifecycle !== this.lifecycle) return;
      await applyStatusOnlyImmersiveShell(w);
      if (lifecycle !== this.lifecycle) return;
      const update = (): void => {
        const insets = getStatusOnlyWindowInsets(w);
        this.top = insets.top + 12; this.bottom = insets.bottom + 16;
      };
      update(); this.disposeInsets = observeStatusOnlyWindowInsets(w, update);
    }).catch((error: Error): void => { hilog.warn(0, 'SteamComponents', '%{public}s', error.message); });
  }
  aboutToDisappear(): void {
    ++this.lifecycle;
    if (this.disposeInsets) this.disposeInsets();
    this.disposeInsets = null;
  }
  build() {
    Column({ space: 12 }) {
      Row({ space: 12 }) {
        Button('返回').height(40).fontColor('#F2F4F7').backgroundColor('#263343')
          .onClick((): void => router.back()).id('steam-components-back')
        Text('Steam 组件管理').fontSize(20).fontWeight(FontWeight.Bold).fontColor('#F2F4F7').layoutWeight(1)
      }.width('100%')
      Scroll() {
        Column({ space: 16 }) {
          if (this.legacyLinux) Text('2.0 已结束 Linux Steam 支持。原有组件、游戏和存档均保留，未迁移或删除。Linux 游戏请使用保留的 1.5.8 环境或 HSL。')
            .fontSize(14).fontColor('#F5D28A').width('100%').id('steam-linux-retired')
          Text('通过 OHOS Wine 运行 Windows Steam，账号登录、游戏下载和更新由 Steam 客户端管理。')
            .fontSize(14).fontColor('#9AA3AF').width('100%')
          SteamClientPanel({ dark: true, showManagement: false })
          Text('请选择包含 steam.exe 的 Windows Steam ZIP。旧版 Linux Steam／GNU Proton 组件包不适用于此版本。导入会保留现有游戏库、账号配置和存档。')
            .fontSize(12).fontColor('#9AA3AF').width('100%').id('steam-components-format')
        }.width('100%').alignItems(HorizontalAlign.Start).constraintSize({ maxWidth: 680 })
      }.layoutWeight(1).width('100%').scrollBar(BarState.Auto)
    }.padding({ top: this.top, bottom: this.bottom, left: 16, right: 16 })
      .width('100%').height('100%').backgroundColor('#0F1115').id('steam-components-page')
  }
}
