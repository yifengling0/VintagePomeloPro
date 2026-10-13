import { AccountAutomaticSyncService } from '../service/valueadded/AccountAutomaticSyncService';
import { SyncItemStatus } from '../model/AccountSyncModels';
import { common } from '@kit.AbilityKit';
import router from '@ohos.router';
import { BackupGameSummary } from '../model/SaveBackupModels';
import promptAction from '@ohos.promptAction';
import { DeviceCapabilityPolicy } from '../common/DeviceCapabilityPolicy';
import { AppDescriptor, InputProfile } from '../model/AppModels';
import { ProfileSummary, SaveManifest, SaveManifestEntry } from '../model/ValueAddedModels';
import { InputProfileService } from '../service/InputProfileService';
import { StoreProduct, ValueAddedService } from '../service/valueadded/ValueAddedService';
import { LocalSaveSnapshotService } from '../service/valueadded/LocalSaveSnapshotService';
import { CloudSaveSlot, CloudSaveVersion } from '../model/CloudSaveModels';
import { CloudSaveService } from '../service/valueadded/CloudSaveService';
import { skinColor } from '../service/SkinColors';

type SyncPane = 'overview' | 'profiles' | 'saves' | 'membership';

@Component
export struct AccountSyncSettings {
  @Prop compact: boolean = false;
  @State private pane: SyncPane = 'overview';
  @State private supported: boolean = DeviceCapabilityPolicy.supportsValueAddedFeatures();
  @State private busy: boolean = false;
  @State private ready: boolean = false;
  @State private userId: string = '';
  @State private status: string = '';
  @State private membership: string = '未登录';
  @State private products: StoreProduct[] = [];
  @State private profiles: InputProfile[] = [];
  @State private cloudProfiles: ProfileSummary[] = [];
  @State private conflicts: string[] = [];
  @State private games: AppDescriptor[] = [];
  @State private selectedGame: string = '';
  @State private saveSearch: string = '';
  @State private saveSummaries: BackupGameSummary[] = [];
  @State private backupEnabled: boolean = false;
  @State private backupSupported: boolean = false;
  @State private installedGame: boolean = false;
  @State private saveReason: string = '';
  @State private interruptedRestore: boolean = false;
  @State private saveDirectory: string = '';
  @State private snapshots: SaveManifest[] = [];
  @State private commerceReady: boolean = false;
  @State private profilesReady: boolean = false;
  @State private autoSyncStatus: string = '';
  @State private syncItems: SyncItemStatus[] = [];
  @State private lastSyncTime: number = 0;
  @State private autoProfiles: boolean = true;
  @State private autoPreferences: boolean = true;
  @State private preferencesConflict: boolean = false;
  private syncListener: (() => void) | null = null;
  @State private cloudSavesReady: boolean = false;
  @State private saveSlots: CloudSaveSlot[] = [];
  @State private selectedSlot: string = '';
  @State private newSlotLabel: string = '';
  @State private cloudQuota: string = '';
  @State private cloudBinding: string = '';
  private context: common.UIAbilityContext = getContext(this) as common.UIAbilityContext;
  private alive: boolean = false;

  aboutToAppear(): void {
    this.alive = true;
    // Registered pages are also directly addressable. Guard before any service or SDK initialization.
    if (!this.supported) return;
    this.syncListener = (): void => this.refreshAutomaticSync();
    AccountAutomaticSyncService.getInstance().subscribe(this.syncListener);
    this.refreshAutomaticSync();
    void this.run(async (): Promise<void> => {
      await InputProfileService.getInstance().initialize(this.context);
      this.profiles = InputProfileService.getInstance().getProfiles();
      this.games = LocalSaveSnapshotService.getInstance().eligibleGames();
      this.refreshBackups();
      const params = router.getParams() as Record<string, string> | undefined;
      if (params?.saveAppId) { this.pane = 'saves'; this.selectSaveGame(params.saveAppId); }
      const service = ValueAddedService.getInstance();
      await service.initialize(this.context);
      this.ready = service.getCapabilities().account;
      this.commerceReady = service.getCapabilities().commerce;
      this.profilesReady = service.getCapabilities().profiles;
      this.cloudSavesReady = service.getCapabilities().cloudSaves;
      this.userId = service.getUserId();
      this.status = this.ready ? '登录后可同步输入方案和存档' : '云服务暂未接通，本地备份仍可使用';
      if (this.userId) await this.refreshAccount();
    });
  }
  aboutToDisappear(): void { this.alive = false; if (this.syncListener) AccountAutomaticSyncService.getInstance().unsubscribe(this.syncListener); if (this.supported) CloudSaveService.getInstance().cancel(); }
  private errorMessage(error: Error): string {
    if (error.message === 'NOT_CONFIGURED') return '云服务暂未接通，本地备份仍可使用';
    if (error.message === 'SIGN_IN_REQUIRED') return '登录已过期，请重新登录';
    if (error.message === 'ACCOUNT_CHANGED') return '账号已变更，本次操作已取消';
    if (error.message === 'SAVE_ENGINE_RUNNING') return '请先在现有界面退出 Wine 引擎，再备份或恢复；不会自动停止任何游戏';
    if (error.message === 'SAVE_IDENTITY_CHANGED') return '游戏主程序与备份时不同，已暂停恢复；不会覆盖当前存档';
    if (error.message === 'SAVE_HASH_MISMATCH') return '备份校验未通过，当前存档未被替换';
    if (error.message === 'SAVE_NOT_RECOGNIZED') return '尚未可靠识别存档位置，备份暂不可用';
    if (error.message === 'SAVE_DIRECTORY_MISSING') return '存档位置暂不可访问，已有备份仍保留';
    if (error.message === 'PREFERENCES_PROFILE_PENDING') return '默认输入方案尚未就绪，请先处理对应方案的同步状态';
    if (error.message === 'BUSY') return '另一个备份操作正在进行，请稍后重试';
    if (error.message === 'SAVE_CHANGED') return '存档仍在变化，本次操作已取消，请关闭游戏后重试';
    if (error.message === 'SAVE_RECOVERY_REQUIRED') return '上次恢复中断，请先点击处理恢复中断';
    if (error.message === 'MEMBERSHIP_REQUIRED') return '云同步需要有效会员，请刷新权益或恢复购买';
    if (error.message === 'SAVE_BINDING_REQUIRED') return '请先把所选云存档位绑定到本机游戏和存档子目录';
    if (error.message === 'SAVE_HISTORY_FULL') return '此存档位已保留3个版本，请先下载保留并删除不需要的云端版本';
    if (error.message === 'SAVE_RECONCILIATION_REQUIRED') return '对象写入结果不明，服务器保留额度等待管理员核对；不会假报已清理';
    if (error.message === 'SAVE_CANCELLED') return '已取消后续传输。请刷新查看已完成的版本和待清理额度';
    if (error.message === 'UNSAFE_SAVE_PATH') return '请选择游戏目录内的存档子目录，不能使用上层目录或快捷链接';
    if (error.message === 'SAVE_STORAGE_FULL') return '本地备份已达到 1 GB，请删除不需要的旧版本后重试；当前存档未改变';
    if (error.message === 'SAVE_QUOTA') return '所选存档超出备份大小或文件数量限制';
    return '操作未完成，请稍后重试；已有存档和备份仍保留';
  }
  private async run(action: () => Promise<void>): Promise<void> {
    if (!this.supported || this.busy) return;
    this.busy = true;
    try { await action(); }
    catch (error) {
      if (this.alive) {
        this.status = this.errorMessage(error as Error);
        this.userId = ValueAddedService.getInstance().getUserId();
        if (!this.userId) { this.membership = '未登录'; this.products = []; this.cloudProfiles = []; this.conflicts = []; this.saveSlots = []; this.selectedSlot = ''; this.cloudBinding = ''; this.cloudQuota = ''; }
      }
    } finally { if (this.alive) {
      this.busy = false;
      if (CloudSaveService.getInstance().hasLocalCleanupPending()) this.status += '；本地传输临时文件待清理';
    } }
  }
  private refreshAutomaticSync(): void {
    if (!this.alive) return;
    const sync = AccountAutomaticSyncService.getInstance();
    this.autoSyncStatus = sync.status(); this.syncItems = sync.itemStates();
    this.autoProfiles = sync.profilesEnabled(); this.autoPreferences = sync.preferencesEnabled();
    this.preferencesConflict = sync.hasPreferencesConflict(); this.lastSyncTime = sync.lastSyncedAt();
    this.userId = ValueAddedService.getInstance().getUserId();
    this.profiles = InputProfileService.getInstance().getProfiles();
    this.conflicts = this.profiles.filter((profile: InputProfile): boolean => !!ValueAddedService.getInstance().getConflict(profile.id))
      .map((profile: InputProfile): string => profile.id);
  }
  private setAutomaticSync(family: string, enabled: boolean): void {
    void this.run(async (): Promise<void> => {
      await AccountAutomaticSyncService.getInstance().setEnabled(family, enabled); this.refreshAutomaticSync();
    });
  }
  private resolveInputConflict(id: string, useCloud: boolean): void {
    void this.run(async (): Promise<void> => {
      await AccountAutomaticSyncService.getInstance().resolveProfile(id, useCloud); this.refreshAutomaticSync();
    });
  }
  private async refreshAccount(): Promise<void> {
    const service = ValueAddedService.getInstance();
    this.userId = service.getUserId();
    this.membership = '正在向服务器核验权益';
    const entitlement = await service.entitlement();
    this.membership = entitlement.active && entitlement.expiresAt > entitlement.serverTime ?
      '会员有效至 ' + new Date(entitlement.expiresAt).toLocaleString() :
      entitlement.refreshRequired ? '会员待重新核验，请使用恢复购买' : '当前没有有效会员';
    if (this.commerceReady) this.products = await service.products();
    if (this.profilesReady && entitlement.active) this.cloudProfiles = await service.listCloudProfiles();
    if (this.cloudSavesReady) await this.refreshCloudSaves();
  }
  private login(): void {
    void this.run(async (): Promise<void> => {
      await ValueAddedService.getInstance().signIn();
      await this.refreshAccount();
      AccountAutomaticSyncService.getInstance().requestSync(0);
      this.status = '已登录，正在自动检查同步';
    });
  }
  private logout(): void {
    void this.run(async (): Promise<void> => {
      await ValueAddedService.getInstance().signOut();
      this.userId = ''; this.membership = '未登录'; this.products = []; this.cloudProfiles = []; this.conflicts = [];
      this.saveSlots = []; this.selectedSlot = ''; this.cloudBinding = ''; this.cloudQuota = '';
      this.status = '已退出，当前账号的会话和云端状态已清除';
    });
  }
  private buy(product: StoreProduct): void {
    void this.run(async (): Promise<void> => {
      const result = await ValueAddedService.getInstance().purchase(product);
      this.status = result === 'ACK_PENDING' ? '权益核验已完成，商店确认待重试，请使用恢复购买' :
        result === 'FINISHED' ? '购买已核验并确认' : '订单状态：' + result;
      await this.refreshAccount();
    });
  }
  private refreshBackups(): void {
    const local = LocalSaveSnapshotService.getInstance();
    this.games = local.eligibleGames();
    this.saveSummaries = local.summaries(this.context);
    this.interruptedRestore = local.hasInterruptedRestore(this.context);
    if (this.selectedGame) this.snapshots = local.list(this.context, this.selectedGame);
  }
  private selectSaveGame(appId: string): void {
    this.selectedGame = appId;
    this.refreshBackups();
    const local = LocalSaveSnapshotService.getInstance(), state = local.backupState(this.context, appId);
    this.installedGame = this.games.some((game: AppDescriptor): boolean => game.id === appId);
    this.saveDirectory = state.selectedPath || local.configuredPath(this.context, appId);
    this.backupEnabled = state.enabled; this.backupSupported = state.supported; this.saveReason = state.reason;
    if (this.cloudSavesReady && this.userId) {
      const slot = this.saveSlots.find((item: CloudSaveSlot): boolean =>
        CloudSaveService.getInstance().binding(this.context, item)?.appId === appId);
      this.selectedSlot = slot?.slotId ?? ''; this.showBinding();
    }
  }
  private saveTitle(): string {
    return this.games.find((game: AppDescriptor): boolean => game.id === this.selectedGame)?.title ??
      this.saveSummaries.find((game: BackupGameSummary): boolean => game.appId === this.selectedGame)?.title ?? '游戏存档';
  }
  private bytesLabel(bytes: number): string {
    return bytes < 1048576 ? (bytes / 1024).toFixed(1) + ' KB' : (bytes / 1048576).toFixed(1) + ' MB';
  }
  private snapshotBytes(snapshot: SaveManifest): number {
    return snapshot.files.reduce((total: number, file: SaveManifestEntry): number => total + file.size, 0);
  }
  private filesDescription(snapshot: SaveManifest): string {
    return snapshot.files.slice(0, 8).map((file: SaveManifestEntry): string => file.relativePath + ' · ' + this.bytesLabel(file.size)).join('\n') +
      (snapshot.files.length > 8 ? '\n另有 ' + (snapshot.files.length - 8) + ' 个文件' : '');
  }
  private backupSave(): void {
    void this.run(async (): Promise<void> => {
      const local = LocalSaveSnapshotService.getInstance(), gameId = this.selectedGame, path = this.saveDirectory;
      const preview = await local.preview(this.context, gameId, path);
      const answer = await promptAction.showDialog({ title: '备份 ' + this.saveTitle(),
        message: preview.files.length + ' 个存档文件 · ' + this.bytesLabel(preview.totalBytes) +
          '\n只保存在此设备，内容未变化时复用已有版本。',
        buttons: [{ text: '取消', color: '#666666' }, { text: '立即备份', color: '#356BE8' }] });
      if (answer.index !== 1) return;
      const previous = local.list(this.context, gameId).map((entry: SaveManifest): string => entry.snapshotId);
      const snapshot = await local.create(this.context, gameId, path, preview);
      this.refreshBackups();
      this.status = previous.indexOf(snapshot.snapshotId) >= 0 ? '存档未变化，已有备份可用' : '本地备份完成，已保存 ' + snapshot.files.length + ' 个文件';
    });
  }
  private restoreSave(snapshot: SaveManifest): void {
    void this.run(async (): Promise<void> => {
      const local = LocalSaveSnapshotService.getInstance();
      const preview = await local.preview(this.context, snapshot.appId, snapshot.selectedPath, snapshot.snapshotId);
      const answer = await promptAction.showDialog({ title: '恢复 ' + this.saveTitle(),
        message: new Date(snapshot.createdAt).toLocaleString() + ' · ' + this.bytesLabel(preview.totalBytes) +
          '\n新增 ' + preview.added + ' · 替换 ' + preview.changed + ' · 移出 ' + preview.removed +
          '\n恢复前会保留当前存档。目标位置：' + snapshot.selectedPath + '\n\n' + this.filesDescription(snapshot),
        buttons: [{ text: '取消', color: '#666666' }, { text: '备份并恢复', color: '#356BE8' }] });
      if (answer.index !== 1) return;
      await local.restore(this.context, snapshot.snapshotId, snapshot.appId, preview);
      this.refreshBackups(); this.status = '存档已恢复，恢复前的版本已保留';
    });
  }
  private deleteLocalSnapshot(snapshot: SaveManifest): void {
    void this.run(async (): Promise<void> => {
      const answer = await promptAction.showDialog({ title: '删除这个备份版本？',
        message: this.saveTitle() + ' · ' + new Date(snapshot.createdAt).toLocaleString() +
          '\n删除后无法找回这一版。游戏当前存档和其他备份不会删除。',
        buttons: [{ text: '取消', color: '#666666' }, { text: '删除此版本', color: '#C43838' }] });
      if (answer.index !== 1) return;
      LocalSaveSnapshotService.getInstance().deleteSnapshot(this.context, snapshot.snapshotId, snapshot.appId);
      this.refreshBackups(); this.status = '此备份版本已删除，游戏存档与其他版本仍保留';
    });
  }
  private currentSlot(): CloudSaveSlot | undefined {
    return this.saveSlots.find((slot: CloudSaveSlot): boolean => slot.slotId === this.selectedSlot);
  }
  private showBinding(): void {
    const slot = this.currentSlot();
    if (!slot) { this.cloudBinding = ''; return; }
    const binding = CloudSaveService.getInstance().binding(this.context, slot);
    const game = binding ? this.games.find((item: AppDescriptor): boolean => item.id === binding.appId) : undefined;
    this.cloudBinding = binding ? '本机绑定：' + (game?.title ?? binding.appId) + ' / ' + binding.selectedPath : '此设备尚未绑定，下载不会自动猜测游戏目录';
  }
  private async refreshCloudSaves(): Promise<void> {
    const result = await CloudSaveService.getInstance().list();
    this.saveSlots = result.slots;
    if (!this.saveSlots.some((slot: CloudSaveSlot): boolean => slot.slotId === this.selectedSlot)) {
      this.selectedSlot = '';
    }
    if (this.selectedGame && !this.selectedSlot) {
      this.selectedSlot = this.saveSlots.find((slot: CloudSaveSlot): boolean =>
        CloudSaveService.getInstance().binding(this.context, slot)?.appId === this.selectedGame)?.slotId ?? '';
    }
    this.cloudQuota = '已用 ' + (result.usedBytes / 1048576).toFixed(1) + ' MiB，上传预留 ' +
      (result.reservedBytes / 1048576).toFixed(1) + ' MiB / 1024 MiB';
    this.showBinding();
  }
  private bindCloudSlot(): void {
    void this.run(async (): Promise<void> => {
      const slot = this.currentSlot(); if (!slot) return;
      const game = this.games.find((item: AppDescriptor): boolean => item.id === this.selectedGame);
      const answer = await promptAction.showDialog({ title: '绑定此设备的存档目录？',
        message: slot.label + ' → ' + (game?.title ?? '') + ' / ' + this.saveDirectory.trim() +
          '。只保存本机映射，不上传路径，也不会立即替换存档。另一台设备须手动选择相同云存档位。',
        buttons: [{ text: '取消', color: '#666666' }, { text: '确认绑定', color: '#356BE8' }] });
      if (answer.index !== 1) return;
      CloudSaveService.getInstance().bind(this.context, slot, this.selectedGame, this.saveDirectory.trim());
      this.showBinding(); this.status = '本机映射已保存';
    });
  }
  private uploadSave(snapshot: SaveManifest): void {
    void this.run(async (): Promise<void> => {
      const slot = this.currentSlot(); if (!slot) return;
      const answer = await promptAction.showDialog({ title: '上传此存档快照？',
        message: '将此快照的游戏存档文件发送到应用配置的私有云存档服务，存入“' + slot.label +
          '”。不会上传游戏目录路径。成功后保留旧版本；冲突时不覆盖另一设备版本。',
        buttons: [{ text: '取消', color: '#666666' }, { text: '上传到云端', color: '#356BE8' }] });
      if (answer.index !== 1) return;
      const result = await CloudSaveService.getInstance().upload(this.context, slot, snapshot);
      await this.refreshCloudSaves();
      this.status = result === 'CONFLICT' ? '云端已有新版本，本地快照仍保留。请先下载检查，再明确选择重新上传或新建云存档位' :
        '云存档已校验并发布；其他设备绑定相同云存档位后可下载';
    });
  }
  private downloadSave(slot: CloudSaveSlot, version: CloudSaveVersion): void {
    void this.run(async (): Promise<void> => {
      const snapshot = await CloudSaveService.getInstance().download(this.context, slot, version.versionId);
      this.refreshBackups(); this.selectedGame = snapshot.appId; this.saveDirectory = snapshot.selectedPath;
      this.snapshots = LocalSaveSnapshotService.getInstance().list(this.context, snapshot.appId);
      this.status = '云存档已校验为本地快照，游戏文件尚未变动。退出 Wine 引擎后可选择“恢复”';
    });
  }
  private deleteCloudVersion(slot: CloudSaveSlot, version: CloudSaveVersion): void {
    void this.run(async (): Promise<void> => {
      const answer = await promptAction.showDialog({ title: '永久删除此云端版本？',
        message: slot.label + ' · ' + new Date(version.createdAt).toLocaleString() +
          '。删除后云端无法找回；不会删除已下载的本地快照。请先确认保留了需要的副本。',
        buttons: [{ text: '取消', color: '#666666' }, { text: '永久删除此版本', color: '#C43838' }] });
      if (answer.index !== 1) return;
      await CloudSaveService.getInstance().deleteVersion(slot, version.versionId);
      await this.refreshCloudSaves(); this.status = '此云端版本已删除，本地文件未变更';
    });
  }
  private openPane(pane: SyncPane): void {
    if (this.busy) return;
    this.pane = pane;
    if (pane === 'saves') { this.selectedGame = ''; this.refreshBackups(); }
  }
  @Builder
  private Action(label: string, id: string, enabled: () => boolean, action: () => void, primary: boolean = false) {
    Button(label).height(36).padding({ left: 16, right: 16 }).borderRadius(12).fontSize(13)
      .backgroundColor(primary ? skinColor('accent') : skinColor('search_background'))
      .fontColor(primary ? '#FFFFFFFF' : skinColor('text_primary'))
      .enabled(enabled()).id(id).onClick(action)
  }
  @Builder
  private Heading(title: string, detail: string) {
    Column({ space: 6 }) {
      Text(title).fontSize(16).fontWeight(FontWeight.Medium).fontColor(skinColor('text_primary')).width('100%')
      Text(detail).fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
    }.width('100%').alignItems(HorizontalAlign.Start)
  }
  @Builder
  private Option(title: string, detail: string, icon: Resource, pane: SyncPane) {
    Button() {
      Row({ space: 14 }) {
        SymbolGlyph(icon).fontSize(22).fontColor([skinColor('accent')]).width(28)
        Column({ space: 4 }) {
          Text(title).fontSize(15).fontWeight(FontWeight.Medium).fontColor(skinColor('text_primary')).width('100%')
          Text(detail).fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
        }.layoutWeight(1).alignItems(HorizontalAlign.Start)
        SymbolGlyph($r('sys.symbol.chevron_right')).fontSize(14).fontColor([skinColor('text_secondary')])
      }.width('100%')
    }.type(ButtonType.Normal).width('100%').constraintSize({ minHeight: 70 })
      .padding({ left: 14, right: 14, top: 12, bottom: 12 }).borderRadius(14)
      .backgroundColor(skinColor('search_background')).enabled(!this.busy)
      .id('sync-open-' + pane).onClick((): void => this.openPane(pane))
  }
  @Builder
  private Overview() {
    Row({ space: 14 }) {
      Stack({ alignContent: Alignment.Center }) {
        SymbolGlyph($r('sys.symbol.person')).fontSize(27).fontColor([skinColor('accent')])
      }.width(48).height(48).borderRadius(16).backgroundColor(skinColor('selected_background'))
      Column({ space: 5 }) {
        Text(this.userId ? '已登录华为账号' : '华为账号').fontSize(this.compact ? 16 : 17).fontWeight(FontWeight.Medium)
          .fontColor(skinColor('text_primary')).width('100%')
        Text(this.userId ? this.membership : '登录后在设备间同步方案和存档').fontSize(12)
          .fontColor(skinColor('text_secondary')).width('100%')
      }.layoutWeight(1).alignItems(HorizontalAlign.Start)
      if (!this.userId) {
        this.Action('登录', 'sync-account-login', (): boolean => (this.ready && !this.busy), (): void => this.login(), true)
      } else {
        this.Action('退出', 'sync-account-logout', (): boolean => (!this.busy), (): void => this.logout())
      }
    }.width('100%').alignItems(VerticalAlign.Center)
    if (this.userId) {
      Row() {
        Text('账号权益由云端核验').fontSize(12).fontColor(skinColor('text_secondary')).layoutWeight(1)
        this.Action('刷新权益', 'sync-refresh-account', (): boolean => (!this.busy), (): void => {
          void this.run(async (): Promise<void> => { await this.refreshAccount(); });
        })
      }.width('100%')
    }
    this.Option('自动同步', this.autoSyncStatus || '登录后自动同步设置和输入方案',
      $r('sys.symbol.arrow_counterclockwise'), 'profiles')
    this.Option('游戏存档', '游戏设置中开启备份 · 在这里查看历史', $r('sys.symbol.doc_text'), 'saves')
    this.Option('会员与支持', this.userId ? this.membership : '查看会员权益和已购项目',
      $r('sys.symbol.info_circle'), 'membership')
  }
  @Builder
  private SyncSwitch(title: string, detail: string, family: string, value: () => boolean) {
    Row({ space: 12 }) {
      Column({ space: 5 }) {
        Text(title).fontSize(15).fontWeight(FontWeight.Medium).fontColor(skinColor('text_primary')).width('100%')
        Text(detail).fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
      }.layoutWeight(1).alignItems(HorizontalAlign.Start)
      Toggle({ type: ToggleType.Switch, isOn: value() }).enabled(!!this.userId && !this.busy)
        .id('sync-auto-' + family).onChange((enabled: boolean): void => this.setAutomaticSync(family, enabled))
    }.width('100%').padding(14).borderRadius(14).backgroundColor(skinColor('search_background'))
  }
  @Builder
  private Profiles() {
    this.Heading('自动同步', '登录后按此账号的开关同步。平板与桌面分别保存；文件路径和运行环境设置不上传。')
    Row({ space: 12 }) {
      Column({ space: 5 }) {
        Text(this.autoSyncStatus || '等待登录').fontSize(15).fontWeight(FontWeight.Medium)
          .fontColor(skinColor('accent')).width('100%').id('sync-automatic-status')
        if (this.lastSyncTime > 0) {
          Text('最近同步 ' + new Date(this.lastSyncTime).toLocaleString())
            .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
        }
      }.layoutWeight(1).alignItems(HorizontalAlign.Start)
      this.Action('重新检查', 'sync-automatic-check', (): boolean => !!this.userId && !this.busy, (): void => {
        AccountAutomaticSyncService.getInstance().requestSync(0);
      })
    }.width('100%')
    this.SyncSwitch('设置同步', '同步外观、卡片布局与默认输入方案', 'preferences', (): boolean => this.autoPreferences)
    this.SyncSwitch('输入方案同步', '自动同步按键布局、鼠标参数和手柄映射', 'profiles', (): boolean => this.autoProfiles)
    if (!this.profilesReady) {
      Text('云同步暂未接通。未登录或离线时，本地设置与方案仍可正常使用。')
        .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
    }
    ForEach(this.syncItems, (item: SyncItemStatus) => {
      Column({ space: 8 }) {
        Row({ space: 12 }) {
          Text(item.name).fontSize(14).fontColor(skinColor('text_primary')).layoutWeight(1)
          Text(item.state).fontSize(12).fontColor(skinColor('text_secondary')).maxLines(2)
        }.width('100%')
        if (this.conflicts.indexOf(item.id) >= 0) {
          Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.End }) {
            this.Action('保留本地', 'sync-profile-keep-' + item.id, (): boolean => !this.busy,
              (): void => this.resolveInputConflict(item.id, false))
            this.Action('采用云端', 'sync-profile-use-' + item.id, (): boolean => !this.busy,
              (): void => this.resolveInputConflict(item.id, true))
          }.width('100%')
        }
        if (item.id === 'preferences' && this.autoPreferences && this.preferencesConflict) {
          Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.End }) {
            this.Action('保留本机设置', 'sync-preferences-keep', (): boolean => !this.busy, (): void => {
              void this.run(async (): Promise<void> => { await AccountAutomaticSyncService.getInstance().resolvePreferences(false); this.refreshAutomaticSync(); });
            })
            this.Action('采用云端设置', 'sync-preferences-use', (): boolean => !this.busy, (): void => {
              void this.run(async (): Promise<void> => { await AccountAutomaticSyncService.getInstance().resolvePreferences(true); this.refreshAutomaticSync(); });
            })
          }.width('100%')
        }
      }.width('100%').padding(14).borderRadius(14).backgroundColor(skinColor('search_background'))
    }, (item: SyncItemStatus): string => item.id)
  }
  @Builder
  private LocalSaves() {
    if (!this.selectedGame) {
      this.Heading('存档备份', '在游戏设置中开启自动备份。这里管理已有版本；移除游戏后，备份仍会保留。')
      TextInput({ placeholder: '搜索已备份的游戏', text: this.saveSearch }).height(44).fontSize(14)
        .fontColor(skinColor('text_primary')).backgroundColor(skinColor('search_background')).borderRadius(12)
        .id('sync-save-search').onChange((value: string): void => { this.saveSearch = value; })
      if (!this.saveSummaries.length) {
        this.Heading('还没有游戏开启备份', '打开游戏卡片的设置，开启“存档自动备份”。应用会识别位置，无需选择目录。')
      }
      ForEach(this.saveSummaries.filter((game: BackupGameSummary): boolean =>
        game.title.toLocaleLowerCase().includes(this.saveSearch.trim().toLocaleLowerCase())), (game: BackupGameSummary) => {
        Button() {
          Row({ space: 12 }) {
            SymbolGlyph($r('sys.symbol.folder')).fontSize(24).fontColor([skinColor('accent')]).width(28)
            Column({ space: 5 }) {
              Text(game.title).fontSize(15).fontWeight(FontWeight.Medium).fontColor(skinColor('text_primary')).width('100%').maxLines(2)
              Text((game.installed ? (game.enabled ? '自动备份已开启' : '自动备份已关闭') : '游戏已移除 · 备份保留') +
                ' · ' + game.count + ' 个版本').fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
              if (game.latestAt > 0) {
                Text('最近备份 ' + new Date(game.latestAt).toLocaleString()).fontSize(11)
                  .fontColor(skinColor('text_secondary')).width('100%')
              }
            }.layoutWeight(1).alignItems(HorizontalAlign.Start)
            SymbolGlyph($r('sys.symbol.chevron_right')).fontSize(14).fontColor([skinColor('text_secondary')])
          }.width('100%')
        }.type(ButtonType.Normal).width('100%').constraintSize({ minHeight: 80 }).padding(14).borderRadius(14)
          .backgroundColor(skinColor('search_background')).enabled(!this.busy).id('sync-save-game-' + game.appId)
          .onClick((): void => this.selectSaveGame(game.appId))
      }, (game: BackupGameSummary): string => game.appId)
      Text('本地备份最多使用 1 GB，满后暂停并保留已有版本。相同内容不重复保存，旧版本只在确认后删除。')
        .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
    } else {
      Row() {
        this.Action('所有备份', 'sync-save-all-games', (): boolean => !this.busy, (): void => {
          this.selectedGame = ''; this.refreshBackups();
        })
        Blank()
        if (this.installedGame) {
          this.Action('游戏设置', 'sync-save-game-settings', (): boolean => !this.busy, (): void => {
            void router.pushUrl({ url: 'pages/AppSettings', params: { appId: this.selectedGame } });
          })
        }
      }.width('100%')
      this.Heading(this.saveTitle(), !this.installedGame ? '游戏已移除或不可访问。已有备份仍保留，重新导入并核对身份后可恢复。' :
        this.backupEnabled ? '自动备份已开启，运行环境退出后保存变化。' : '自动备份已关闭。可在此游戏的设置中开启。')
      if (this.saveReason) { Text(this.saveReason).fontSize(12).fontColor(skinColor('text_secondary')).width('100%') }
      if (this.backupSupported && this.installedGame) {
        Row({ space: 10 }) {
          Text('已识别存档 · ' + this.saveDirectory).fontSize(12).fontColor(skinColor('text_secondary')).layoutWeight(1)
          this.Action('立即备份', 'sync-create-local-save', (): boolean => !this.busy && this.backupEnabled,
            (): void => this.backupSave(), true)
        }.width('100%')
      }
      Text('备份历史').fontSize(14).fontWeight(FontWeight.Medium).fontColor(skinColor('text_primary')).width('100%')
      if (!this.snapshots.length) { Text('等待首次备份。').fontSize(12).fontColor(skinColor('text_secondary')).width('100%') }
      ForEach(this.snapshots, (snapshot: SaveManifest) => {
        Column({ space: 10 }) {
          Row({ space: 10 }) {
            Column({ space: 5 }) {
              Text(new Date(snapshot.createdAt).toLocaleString()).fontSize(14).fontWeight(FontWeight.Medium)
                .fontColor(skinColor('text_primary')).width('100%')
              Text((snapshot.origin === 'before-restore' ? '恢复前保护备份 · ' : snapshot.origin === 'cloud-download' ? '云端下载 · ' : '本地备份 · ') +
                snapshot.files.length + ' 个文件 · ' + this.bytesLabel(this.snapshotBytes(snapshot)))
                .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
            }.layoutWeight(1).alignItems(HorizontalAlign.Start)
            this.Action('查看与恢复', 'sync-save-restore-' + snapshot.snapshotId,
              (): boolean => !this.busy && this.installedGame && this.backupSupported,
              (): void => this.restoreSave(snapshot))
          }.width('100%')
          Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.End }) {
            this.Action('删除此版本', 'sync-save-delete-' + snapshot.snapshotId, (): boolean => !this.busy,
              (): void => this.deleteLocalSnapshot(snapshot))
          }.width('100%')
          if (this.cloudSavesReady && this.selectedSlot) {
            Row() {
              Blank()
              this.Action('上传此版本', 'sync-save-upload-' + snapshot.snapshotId,
                (): boolean => !!this.userId && !this.busy && this.installedGame,
                (): void => this.uploadSave(snapshot))
            }.width('100%')
          }
        }.width('100%').padding(14).borderRadius(14).backgroundColor(skinColor('search_background'))
      }, (snapshot: SaveManifest): string => snapshot.snapshotId)
    }
    if (this.interruptedRestore) {
      this.Action('检查中断的恢复', 'sync-save-recover', (): boolean => !this.busy, (): void => {
        void this.run(async (): Promise<void> => {
          LocalSaveSnapshotService.getInstance().recoverInterrupted(this.context);
          this.refreshBackups(); this.status = '恢复检查完成，原有文件已保留';
        });
      })
    }
  }
  @Builder
  private CloudSaves() {
    this.Heading('云端副本', '本地备份独立可用。云端版本先下载到备份历史，确认恢复后才写入游戏。')
    if (!this.cloudSavesReady) {
      Text('云服务暂未接通，本地备份不受影响。').fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
    } else if (this.selectedGame && this.installedGame) {
      if (this.cloudQuota) { Text(this.cloudQuota).fontSize(12).fontColor(skinColor('text_secondary')).width('100%') }
      Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.End }) {
        this.Action('为此游戏创建云端副本', 'sync-save-new-slot',
          (): boolean => !!this.userId && !this.busy && this.backupSupported && !this.selectedSlot, (): void => {
          void this.run(async (): Promise<void> => {
            const slot = await CloudSaveService.getInstance().createSlot(this.saveTitle());
            CloudSaveService.getInstance().bind(this.context, slot, this.selectedGame, this.saveDirectory);
            this.selectedSlot = slot.slotId; await this.refreshCloudSaves(); this.status = '已关联此游戏，可以上传备份版本';
          });
        }, true)
        this.Action('刷新', 'sync-save-refresh', (): boolean => !!this.userId && !this.busy, (): void => {
          void this.run(async (): Promise<void> => { await this.refreshCloudSaves(); });
        })
        if (this.busy) {
          this.Action('取消传输', 'sync-save-cancel', (): boolean => true, (): void => {
            CloudSaveService.getInstance().cancel(); this.status = '正在取消传输';
          })
        }
      }.width('100%')
      ForEach(this.saveSlots.filter((slot: CloudSaveSlot): boolean => {
        const binding = CloudSaveService.getInstance().binding(this.context, slot);
        return !binding || binding.appId === this.selectedGame;
      }), (slot: CloudSaveSlot) => {
        Column({ space: 10 }) {
          Text(slot.label).fontSize(14).fontWeight(FontWeight.Medium).fontColor(skinColor('text_primary')).width('100%')
          if (slot.slotId !== this.selectedSlot) {
            this.Action('关联到此游戏', 'sync-save-associate-' + slot.slotId,
              (): boolean => !this.busy && this.backupSupported, (): void => {
                this.selectedSlot = slot.slotId; this.bindCloudSlot();
              })
          } else {
            Text('已关联 ' + this.saveTitle()).fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
            ForEach(slot.versions, (version: CloudSaveVersion) => {
              Column({ space: 8 }) {
                Text(new Date(version.createdAt).toLocaleString() + ' · ' + this.bytesLabel(version.totalBytes))
                  .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
                Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.End }) {
                  this.Action('下载到备份历史', 'sync-cloud-save-download-' + version.versionId, (): boolean => !this.busy,
                    (): void => this.downloadSave(slot, version), true)
                  this.Action('删除云端版本', 'sync-cloud-save-delete-' + version.versionId, (): boolean => !this.busy,
                    (): void => this.deleteCloudVersion(slot, version))
                }.width('100%')
              }.width('100%')
            }, (version: CloudSaveVersion): string => version.versionId)
          }
        }.width('100%').padding(14).borderRadius(14).backgroundColor(skinColor('search_background'))
      }, (slot: CloudSaveSlot): string => slot.slotId)
      Text('云端每款游戏保留 3 个版本，单份最大 256 MB；冲突时保留两份，满额后由用户决定删除哪一版。')
        .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
    } else if (!this.selectedGame) {
      Text('选择上方游戏后，查看与它关联的云端副本。').fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
    }
  }
  @Builder
  private Membership() {
    this.Heading('会员与支持', '会员提供云同步权益，支持开发是独立商品。购买前请核对华为收银台的价格与期限。')
    if (!this.commerceReady) {
      Text('购买暂未开放，游戏和本地备份仍可免费使用。').fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
    }
    ForEach(this.products, (product: StoreProduct) => {
      Row({ space: 12 }) {
        Text(product.title).fontSize(14).fontColor(skinColor('text_primary')).layoutWeight(1)
        this.Action(product.price, 'sync-product-' + product.id, (): boolean => (!this.busy), (): void => this.buy(product), true)
      }.width('100%').padding(14).borderRadius(14).backgroundColor(skinColor('search_background'))
    }, (product: StoreProduct): string => product.id)
    this.Action('恢复购买', 'sync-restore-purchases', (): boolean => (this.commerceReady && !!this.userId && !this.busy), (): void => {
      void this.run(async (): Promise<void> => {
        const result = await ValueAddedService.getInstance().restorePurchases(); await this.refreshAccount();
        this.status = result === 'ACK_PENDING' ? '部分订单确认仍待重试' : '恢复购买核验完成';
      });
    })
  }
  build() {
    if (this.supported) {
      Column({ space: 16 }) {
        if (this.pane === 'overview') {
          this.Overview()
        } else {
          Row() {
            this.Action('返回同步选项', 'sync-back', (): boolean => (!this.busy), (): void => this.openPane('overview'))
            Blank()
          }.width('100%')
          if (this.pane === 'profiles') { this.Profiles() }
          else if (this.pane === 'saves') {
            this.LocalSaves()
            Divider().color(skinColor('selected_background')).margin({ top: 4, bottom: 4 })
            this.CloudSaves()
          } else { this.Membership() }
        }
        if (this.busy) {
          Row({ space: 8 }) {
            LoadingProgress().width(18).height(18).color(skinColor('accent'))
            Text('正在处理…').fontSize(12).fontColor(skinColor('text_secondary'))
          }.width('100%')
        }
        if (this.status && !(this.pane === 'saves' && !this.cloudSavesReady && this.status === '云服务暂未接通，本地备份仍可使用')) {
          Text(this.status).fontSize(12).fontColor(skinColor('text_secondary')).width('100%').id('account-cloud-status')
        }
      }.width('100%').alignItems(HorizontalAlign.Start)
    }
  }
}
