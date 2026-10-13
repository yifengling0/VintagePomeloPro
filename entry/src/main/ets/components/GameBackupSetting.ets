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
  @State private hasBackup: boolean = false;
  private context: common.UIAbilityContext = getContext(this) as common.UIAbilityContext;
  private alive: boolean = false;
  private listener: (() => void) | null = null;
  aboutToAppear(): void {
    this.alive = true; this.refresh();
    this.listener = (): void => { if (this.alive) this.refresh(); };
    LocalSaveSnapshotService.getInstance().subscribe(this.listener);
  }
  aboutToDisappear(): void {
    this.alive = false;
    if (this.listener) LocalSaveSnapshotService.getInstance().unsubscribe(this.listener);
  }
  private refresh(): void {
    try {
      const state = LocalSaveSnapshotService.getInstance().backupState(this.context, this.appId);
      this.supported = state.supported; this.backupOn = state.enabled;
      this.hasBackup = LocalSaveSnapshotService.getInstance().list(this.context, this.appId).length > 0;
      this.detail = state.supported ? (state.enabled ? '游戏退出后自动更新备份' : '开启后自动保留最新存档') +
        (state.reason ? '\n' + state.reason : '') :
        '暂未可靠识别此游戏的存档。保存游戏后返回这里会自动检测。';
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
          Text(this.supported ? '只备份这款游戏，无需选择文件或目录' : '此游戏暂不支持自动备份')
            .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
        }.layoutWeight(1).alignItems(HorizontalAlign.Start)
        if (this.supported) {
          Toggle({ type: ToggleType.Switch, isOn: this.backupOn }).enabled(!this.pending)
            .id('game-save-backup-toggle').onChange((enabled: boolean): void => { void this.change(enabled); })
        }
      }.width('100%')
      Text(this.detail).fontSize(12).fontColor(skinColor('text_secondary')).width('100%').id('game-save-backup-status')
      if (this.supported || this.hasBackup) {
        Text('仅保留最新备份。关闭备份或移除游戏后，已有备份仍保留。').fontSize(12)
          .fontColor(skinColor('text_secondary')).width('100%')
      }
      if (this.hasBackup) {
        Row() {
          Blank()
          Button('查看备份').height(40).fontSize(13).borderRadius(12)
          .backgroundColor(skinColor('search_background')).fontColor(skinColor('text_primary'))
          .id('game-save-open-backups').onClick((): void => {
            void router.pushUrl({ url: 'pages/SystemSettings', params: { section: 'sync', saveAppId: this.appId } });
          })
        }.width('100%')
      }
    }.width('100%').padding(16).backgroundColor(skinColor('card_background')).borderRadius(16)
      .alignItems(HorizontalAlign.Start)
  }
}
