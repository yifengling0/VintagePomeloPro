import { common } from '@kit.AbilityKit';
import router from '@ohos.router';
import { LocalSaveSnapshotService } from '../service/valueadded/LocalSaveSnapshotService';
import { skinColor } from '../service/SkinColors';

/** A per-game opt-in. Directory detection is the application's responsibility. */
@Component
export struct GameBackupSetting {
  @Prop appId: string = '';
  @State private supported: boolean = false;
  @State private backupOn: boolean = false;
  @State private pending: boolean = false;
  @State private detail: string = '';
  private context: common.UIAbilityContext = getContext(this) as common.UIAbilityContext;
  private alive: boolean = false;
  aboutToAppear(): void { this.alive = true; this.refresh(); }
  aboutToDisappear(): void { this.alive = false; }
  private refresh(): void {
    try {
      const state = LocalSaveSnapshotService.getInstance().backupState(this.context, this.appId);
      this.supported = state.supported; this.backupOn = state.enabled;
      this.detail = state.supported ? '已识别存档 · ' + state.selectedPath +
        (state.reason ? '\n' + state.reason : '') : state.reason;
    } catch (_) { this.supported = false; this.detail = '存档检测暂未完成，稍后可重新检测'; }
  }
  private async change(enabled: boolean): Promise<void> {
    if (this.pending) return;
    this.pending = true;
    try {
      await LocalSaveSnapshotService.getInstance().setBackupEnabled(this.context, this.appId, enabled);
      if (this.alive) { this.refresh(); this.detail += enabled ? '\n已开启，运行环境退出后自动备份' : '\n已有备份仍保留'; }
      if (enabled) {
        await LocalSaveSnapshotService.getInstance().runAutomatic(this.context);
        if (this.alive) this.refresh();
      }
    } catch (error) {
      if (this.alive) { this.refresh(); this.detail += '\n设置未完成，请稍后重试'; }
    } finally { if (this.alive) this.pending = false; }
  }
  build() {
    Column({ space: 12 }) {
      Row({ space: 12 }) {
        Column({ space: 5 }) {
          Text('存档自动备份').fontSize(16).fontWeight(FontWeight.Medium).fontColor(skinColor('text_primary')).width('100%')
          Text('默认关闭，只备份这款游戏的存档').fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
        }.layoutWeight(1).alignItems(HorizontalAlign.Start)
        Toggle({ type: ToggleType.Switch, isOn: this.backupOn }).enabled(this.supported && !this.pending)
          .id('game-save-backup-toggle').onChange((enabled: boolean): void => { void this.change(enabled); })
      }.width('100%')
      Text(this.detail).fontSize(12).fontColor(skinColor('text_secondary')).width('100%').id('game-save-backup-status')
      Text('相同内容不会重复保存；关闭备份或移除游戏后，已有版本仍会保留。').fontSize(12)
        .fontColor(skinColor('text_secondary')).width('100%')
      Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.End }) {
        if (!this.supported) {
          Button('重新检测').height(40).fontSize(13).borderRadius(12).enabled(!this.pending)
            .backgroundColor(skinColor('search_background')).fontColor(skinColor('text_primary'))
            .id('game-save-detect').onClick((): void => this.refresh())
        }
        Button('查看备份').height(40).fontSize(13).borderRadius(12).margin({ left: 8 })
          .backgroundColor(skinColor('search_background')).fontColor(skinColor('text_primary'))
          .id('game-save-open-backups').onClick((): void => {
            void router.pushUrl({ url: 'pages/SystemSettings', params: { section: 'sync', saveAppId: this.appId } });
          })
      }.width('100%')
    }.width('100%').padding(16).backgroundColor(skinColor('card_background')).borderRadius(16)
      .alignItems(HorizontalAlign.Start)
  }
}
