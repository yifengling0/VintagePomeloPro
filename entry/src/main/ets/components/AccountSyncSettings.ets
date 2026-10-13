import { BusinessError } from '@kit.BasicServicesKit';
import { AccountAutomaticSyncService } from '../service/valueadded/AccountAutomaticSyncService';
import { SyncItemStatus } from '../model/AccountSyncModels';
import { common } from '@kit.AbilityKit';
import router from '@ohos.router';
import { BackupGameSummary } from '../model/SaveBackupModels';
import promptAction from '@ohos.promptAction';
import { DeviceCapabilityPolicy } from '../common/DeviceCapabilityPolicy';
import { AppDescriptor, InputProfile } from '../model/AppModels';
import { MemberEntitlement, ProfileSummary, SaveManifest, SaveManifestEntry } from '../model/ValueAddedModels';
import { InputProfileService } from '../service/InputProfileService';
import { StoreProduct, ValueAddedService } from '../service/valueadded/ValueAddedService';
import { LocalSaveSnapshotService } from '../service/valueadded/LocalSaveSnapshotService';
import { CloudSaveSlot, CloudSaveVersion } from '../model/CloudSaveModels';
import { CloudSaveService } from '../service/valueadded/CloudSaveService';
import { skinColor } from '../service/SkinColors';

@Component
export struct AccountSyncSettings {
  @Prop compact: boolean = false;
  @Prop surfaceColor: string | Resource = '';
  @Prop cardBorderColor: string = '#14000000';
  @Prop glass: boolean = false;
  @Prop groupPadding: number = 22;
  @State private availability: string = 'checking';
  @State private supported: boolean = DeviceCapabilityPolicy.supportsValueAddedFeatures();
  @State private busy: boolean = false;
  @State private ready: boolean = false;
  @State private userId: string = '';
  @State private huaweiSignedIn: boolean = false;
  @State private huaweiNickname: string = '';
  @State private huaweiAvatar: string = '';
  @State private avatarFailed: boolean = false;
  @State private status: string = '';
  @State private statusArea: string = 'account';
  @State private membership: string = '未登录';
  @State private vipActive: boolean = false;
  @State private renewalKnown: boolean = false;
  @State private renewalEnabled: boolean = true;
  @State private membershipExpanded: boolean = false;
  private vipOwner: string = '';
  private vipDeadline: number = 0;
  private vipTimer: number = -1;
  @State private products: StoreProduct[] = [];
  @State private membershipPriceStatus: string = '正在加载价格';
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
  private backupListener: (() => void) | null = null;
  private syncListener: (() => void) | null = null;
  private entitlementListener: (() => void) | null = null;
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
    this.entitlementListener = (): void => {
      if (this.alive && ValueAddedService.getInstance().getUserId()) {
        void this.refreshMemberState().catch((): void => {});
      }
    };
    ValueAddedService.getInstance().subscribeEntitlement(this.entitlementListener);
    this.backupListener = (): void => {
      if (!this.alive) return;
      if (this.selectedGame) this.selectSaveGame(this.selectedGame); else this.refreshBackups();
    };
    LocalSaveSnapshotService.getInstance().subscribe(this.backupListener);
    void this.run('account', async (): Promise<void> => {
      await InputProfileService.getInstance().initialize(this.context);
      this.profiles = InputProfileService.getInstance().getProfiles();
      this.games = LocalSaveSnapshotService.getInstance().eligibleGames();
      this.refreshBackups();
      const params = router.getParams() as Record<string, string> | undefined;
      if (params?.saveAppId) this.selectSaveGame(params.saveAppId);
      const service = ValueAddedService.getInstance();
      await service.initialize(this.context);
      this.ready = service.getCapabilities().account;
      this.commerceReady = service.getCapabilities().commerce;
      this.profilesReady = service.getCapabilities().profiles;
      this.cloudSavesReady = service.getCapabilities().cloudSaves;
      this.userId = service.getUserId();
      this.refreshHuaweiIdentity();
      this.availability = service.accountAvailability();
      if (this.userId) await this.refreshAccount(); else await this.refreshMembershipProducts();
    });
  }
  aboutToDisappear(): void { this.alive = false; this.resetVip(); if (this.backupListener) LocalSaveSnapshotService.getInstance().unsubscribe(this.backupListener); if (this.syncListener) AccountAutomaticSyncService.getInstance().unsubscribe(this.syncListener); if (this.entitlementListener) ValueAddedService.getInstance().unsubscribeEntitlement(this.entitlementListener); if (this.supported) CloudSaveService.getInstance().cancel(); }
  private errorMessage(error: Error): string {
    const sdkCode = (error as BusinessError).code;
    const diagnostic = Number.isInteger(sdkCode) ? '' + sdkCode :
      (/^[A-Z][A-Z0-9_]{1,63}$/.test(error.message) ? error.message : 'FAILED');
    console.warn('[AccountSyncSettings] operation failed: ' + diagnostic);
    if (error.message.startsWith('HUAWEI_LOGIN_')) {
      const code = error.message.slice('HUAWEI_LOGIN_'.length);
      if (code === '1001502012') return '已取消登录';
      if (code === '1001502005') return '登录连接失败，请稍后重试';
      if (code === '1001500001') return '当前版本暂时无法登录，请更新后重试';
      if (code === '1001502001') return '请先登录设备账号';
      return '登录未完成（' + code + '），请稍后重试';
    }
    if (error.message === 'SANDBOX_REQUIRED') return '当前账号无法进行测试订阅，请检查测试账号配置';
    if (error.message === 'PRICE_CHANGED') return '商品优惠已变化，请刷新会员价格后重试';
    if (error.message === 'INVALID_OFFER') return '优惠信息暂不可用，请刷新会员价格后重试';
    if (error.message === 'AUTH_STATE_MISMATCH') return '登录响应校验未通过，请重新登录';
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
    if (['INVALID_JWS', 'INVALID_SUBSCRIPTION', 'INVALID_ORDER', 'ORDER_NOT_VERIFIED', 'PROVIDER_UNAVAILABLE', 'SERVICE_ERROR'].includes(error.message)) return '会员确认暂未完成，请点击刷新状态重试，无需重复购买';
    if (error.message === 'MEMBERSHIP_REQUIRED') return '云同步需要有效会员，请刷新权益或恢复购买';
    if (error.message === 'SAVE_BINDING_REQUIRED') return '请先把所选云存档位绑定到本机游戏和存档子目录';
    if (error.message === 'SAVE_RECONCILIATION_REQUIRED') return '对象写入结果不明，服务器保留额度等待管理员核对；不会假报已清理';
    if (error.message === 'SAVE_CANCELLED') return '已取消后续传输。请刷新查看已完成的版本和待清理额度';
    if (error.message === 'UNSAFE_SAVE_PATH') return '请选择游戏目录内的存档子目录，不能使用上层目录或快捷链接';
    if (error.message === 'SAVE_STORAGE_FULL') return '本地备份空间不足，请清理不需要的游戏备份；已有备份与当前存档仍保留';
    if (error.message === 'SAVE_QUOTA') return '所选存档超出备份大小或文件数量限制';
    return '操作未完成，请稍后重试；已有存档和备份仍保留';
  }
  private async run(area: string, action: () => Promise<void>): Promise<void> {
    if (!this.supported || this.busy) return;
    this.busy = true; this.status = ''; this.statusArea = area;
    try { await action(); }
    catch (error) {
      if (this.alive) {
        this.status = (error as Error).message === 'NOT_CONFIGURED' ? '' : this.errorMessage(error as Error);
        this.availability = ValueAddedService.getInstance().accountAvailability();
        this.userId = ValueAddedService.getInstance().getUserId();
    this.refreshHuaweiIdentity();
        if (!this.userId) { this.membership = '未登录'; this.cloudProfiles = []; this.conflicts = []; this.saveSlots = []; this.selectedSlot = ''; this.cloudBinding = ''; this.cloudQuota = ''; }
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
    this.refreshHuaweiIdentity();
    this.profiles = InputProfileService.getInstance().getProfiles();
    this.conflicts = this.profiles.filter((profile: InputProfile): boolean => !!ValueAddedService.getInstance().getConflict(profile.id))
      .map((profile: InputProfile): string => profile.id);
  }
  private backupStatus(value: string): string {
    if (value === '已同步') return '云端备份已更新';
    if (value === '已恢复') return '已从云端恢复';
    if (value === '正在同步') return '正在检查云端备份';
    if (value === '等待同步') return '等待备份';
    if (value === '返回应用后继续同步') return '返回应用后继续备份';
    if (value === '已开启的云同步暂未开放' || value === '云同步暂未开放') return '云端备份暂未开放';
    if (value === '自动同步已关闭') return '自动备份已关闭';
    return value;
  }
  private setAutomaticSync(family: string, enabled: boolean): void {
    void this.run('automatic', async (): Promise<void> => {
      await AccountAutomaticSyncService.getInstance().setEnabled(family, enabled); this.refreshAutomaticSync();
    });
  }
  private resolveInputConflict(id: string, useCloud: boolean): void {
    void this.run('automatic', async (): Promise<void> => {
      await AccountAutomaticSyncService.getInstance().resolveProfile(id, useCloud); this.refreshAutomaticSync();
    });
  }
  private async refreshAccount(): Promise<void> {
    const service = ValueAddedService.getInstance();
    this.userId = service.getUserId();
    this.refreshHuaweiIdentity();
    const owner = this.userId;
    this.membership = '正在更新会员状态';
    await service.refreshCapabilities();
    this.commerceReady = service.getCapabilities().commerce;
    this.profilesReady = service.getCapabilities().profiles;
    this.cloudSavesReady = service.getCapabilities().cloudSaves;
    const entitlement = await this.refreshMemberState();
    if (!this.alive || owner !== service.getUserId()) return;
    if (!this.vipActive) await this.refreshMembershipProducts(); else this.products = [];
    if (this.profilesReady && entitlement.active) this.cloudProfiles = await service.listCloudProfiles();
    if (this.cloudSavesReady) await this.refreshCloudSaves();
  }
  private async refreshMemberState(): Promise<MemberEntitlement> {
    const service = ValueAddedService.getInstance(), owner = service.getUserId();
    const entitlement = await service.entitlement();
    if (!this.alive || owner !== service.getUserId()) return entitlement;
    this.applyVip(entitlement, owner);
    this.membership = this.vipActive ?
      '有效期至 ' + new Date(entitlement.expiresAt).toLocaleDateString() +
        (entitlement.autoRenew === true ? ' · 自动续费已开启' : entitlement.autoRenew === false ? ' · 到期后不再续费' : '') :
      entitlement.refreshRequired ? '会员状态待重新核验，请稍后刷新' :
      entitlement.billingRetry ? '续费未完成，请检查订阅' :
      entitlement.subscriptionStatus === '5' ? '订阅已撤销' :
      entitlement.subscriptionStatus === '2' ? '会员已到期，可重新开通' :
      service.getPurchaseRecoveryStatus() === 'CHECKING' ? '正在确认订阅' :
      service.getPurchaseRecoveryStatus() && !['RESTORED', 'FINISHED', 'ALREADY_FINISHED'].includes(service.getPurchaseRecoveryStatus()) ?
        '订阅确认待重试，无需重复购买' : '普通账号';
    return entitlement;
  }
  private resetVip(): void {
    if (this.vipTimer >= 0) clearTimeout(this.vipTimer);
    this.vipTimer = -1; this.vipActive = false; this.vipOwner = ''; this.vipDeadline = 0;
    this.renewalKnown = false; this.renewalEnabled = true;
  }
  private applyVip(entitlement: MemberEntitlement, owner: string): void {
    this.resetVip();
    if (!this.alive || !owner || owner !== ValueAddedService.getInstance().getUserId() ||
      !entitlement.active || entitlement.refreshRequired || entitlement.expiresAt <= entitlement.serverTime) return;
    this.vipOwner = owner;
    this.vipDeadline = Date.now() + entitlement.expiresAt - entitlement.serverTime;
    this.vipActive = true;
    this.renewalKnown = typeof entitlement.autoRenew === 'boolean';
    this.renewalEnabled = entitlement.autoRenew === true;
    this.updateVipExpiry();
  }
  private updateVipExpiry(): void {
    if (this.vipTimer >= 0) clearTimeout(this.vipTimer);
    this.vipTimer = -1;
    if (!this.alive || !this.vipActive) return;
    if (this.vipOwner !== ValueAddedService.getInstance().getUserId()) { this.resetVip(); return; }
    const remaining = this.vipDeadline - Date.now();
    if (remaining <= 0) {
      this.resetVip(); this.membership = '会员已到期';
      void this.refreshMembershipProducts();
      return;
    }
    this.vipTimer = setTimeout((): void => this.updateVipExpiry(), Math.min(remaining, 60000));
  }
  private async refreshMembershipProducts(): Promise<void> {
    this.membershipPriceStatus = '正在加载价格';
    try {
      const products = await ValueAddedService.getInstance().products();
      if (this.alive) {
        this.products = products;
        this.membershipPriceStatus = products.some((product: StoreProduct): boolean => product.purpose === 'membership') ? '' :
          '暂未查到会员商品，请稍后刷新。';
      }
    } catch (_) { if (this.alive) { this.products = []; this.membershipPriceStatus = '价格暂时无法获取，请稍后刷新。'; } }
  }
  private availabilityDetail(): string {
    if (this.availability === 'configuration_missing') return '云同步暂不可用，本地设置与存档备份可正常使用。';
    if (this.availability === 'identity_mismatch') return '同步暂不可用，请更新应用后重试。';
    if (this.availability === 'service_closed') return '同步暂未开放，本地备份可正常使用。';
    if (this.availability === 'unreachable') return '同步连接失败，请稍后重试。';
    return '正在检查云同步服务，请稍后重试。';
  }
  private availabilityTitle(): string {
    if (this.availability === 'configuration_missing' || this.availability === 'identity_mismatch') return '云同步尚未接通';
    if (this.availability === 'unreachable') return '云同步连接失败';
    if (this.availability === 'service_closed') return '云同步暂未开放';
    return '正在检查云同步服务';
  }
  private login(): void {
    void this.run('account', async (): Promise<void> => {
      const service = ValueAddedService.getInstance();
      await service.initialize(this.context);
      this.ready = service.getCapabilities().account;
      this.commerceReady = service.getCapabilities().commerce;
      this.profilesReady = service.getCapabilities().profiles;
      this.cloudSavesReady = service.getCapabilities().cloudSaves;
      this.availability = service.accountAvailability();
      await service.signIn();
      this.refreshHuaweiIdentity();
      this.userId = service.getUserId();
      if (this.userId) {
        await this.refreshAccount();
        AccountAutomaticSyncService.getInstance().requestSync(0);
        this.status = '已登录，正在检查云端备份';
      } else this.status = '已登录，同步尚未连接';
    });
  }
  private logout(): void {
    void this.run('account', async (): Promise<void> => {
      await ValueAddedService.getInstance().signOut();
      this.huaweiSignedIn = false; this.huaweiAvatar = ''; this.huaweiNickname = ''; this.avatarFailed = false; this.userId = ''; this.membership = '未登录'; this.cloudProfiles = []; this.conflicts = [];
      this.saveSlots = []; this.selectedSlot = ''; this.cloudBinding = ''; this.cloudQuota = '';
      await this.refreshMembershipProducts();
      this.status = '已退出，当前账号的会话和云端状态已清除';
    });
  }
  private buy(product: StoreProduct): void {
    void this.run('membership', async (): Promise<void> => {
      const result = await ValueAddedService.getInstance().purchase(product);
      this.status = result === 'CANCELLED' ? '已取消购买' :
        result === 'VERIFICATION_PENDING' ? '购买已完成，会员确认暂未完成；会自动重试，无需再次付款' :
        result === 'ACK_PENDING' ? '会员已生效，订单确认稍后自动重试' :
        result === 'ALREADY_ACTIVE' ? '本期会员已生效，无需重复购买' :
        result === 'FINISHED' || result === 'RESTORED' || result === 'ALREADY_FINISHED' ? '会员状态已更新' : '订阅状态已更新';
      if (result !== 'CANCELLED') AccountAutomaticSyncService.getInstance().requestSync(0);
      await this.refreshAccount();
    });
  }
  private refreshHuaweiIdentity(): void {
    const service = ValueAddedService.getInstance(), profile = service.getHuaweiProfile();
    if (!this.userId || this.vipOwner && this.vipOwner !== this.userId) this.resetVip();
    this.huaweiSignedIn = service.isHuaweiSignedIn(); this.huaweiNickname = profile.nickname;
    if (this.huaweiAvatar !== profile.avatarUri) this.avatarFailed = false;
    this.huaweiAvatar = profile.avatarUri;
  }
  private refreshBackups(): void {
    const local = LocalSaveSnapshotService.getInstance();
    this.games = local.eligibleGames();
    this.saveSummaries = local.summaries(this.context).filter((game: BackupGameSummary): boolean => game.enabled || game.count > 0);
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
    return bytes < 1024 ? bytes + ' B' : bytes < 1048576 ? (bytes / 1024).toFixed(1) + ' KB' : (bytes / 1048576).toFixed(1) + ' MB';
  }
  private snapshotBytes(snapshot: SaveManifest): number {
    return snapshot.files.reduce((total: number, file: SaveManifestEntry): number => total + file.size, 0);
  }
  private filesDescription(snapshot: SaveManifest): string {
    return snapshot.files.slice(0, 8).map((file: SaveManifestEntry): string => file.relativePath + ' · ' + this.bytesLabel(file.size)).join('\n') +
      (snapshot.files.length > 8 ? '\n另有 ' + (snapshot.files.length - 8) + ' 个文件' : '');
  }
  private backupSave(): void {
    void this.run('backups', async (): Promise<void> => {
      const local = LocalSaveSnapshotService.getInstance(), gameId = this.selectedGame, path = this.saveDirectory;
      const preview = await local.preview(this.context, gameId, path);
      const answer = await promptAction.showDialog({ title: '备份 ' + this.saveTitle(),
        message: preview.files.length + ' 个存档文件 · ' + this.bytesLabel(preview.totalBytes) +
          '\n只保存在此设备。每个游戏保留一份最新备份，新备份校验完成后替换原备份。',
        buttons: [{ text: '取消', color: '#666666' }, { text: '立即备份', color: '#356BE8' }] });
      if (answer.index !== 1) return;
      const previous = local.list(this.context, gameId).map((entry: SaveManifest): string => entry.snapshotId);
      const snapshot = await local.create(this.context, gameId, path, preview);
      this.refreshBackups();
      this.status = previous.indexOf(snapshot.snapshotId) >= 0 ? '存档未变化，已有备份可用' : '本地备份完成，已保存 ' + snapshot.files.length + ' 个文件';
    });
  }
  private restoreSave(snapshot: SaveManifest): void {
    void this.run('backups', async (): Promise<void> => {
      const local = LocalSaveSnapshotService.getInstance();
      const preview = await local.preview(this.context, snapshot.appId, snapshot.selectedPath, snapshot.snapshotId);
      const answer = await promptAction.showDialog({ title: '恢复 ' + this.saveTitle(),
        message: new Date(snapshot.createdAt).toLocaleString() + ' · ' + this.bytesLabel(preview.totalBytes) +
          '\n新增 ' + preview.added + ' · 替换 ' + preview.changed + ' · 移出 ' + preview.removed +
          '\n恢复失败会回滚当前存档；成功后清理临时副本。目标位置：' + snapshot.selectedPath + '\n\n' + this.filesDescription(snapshot),
        buttons: [{ text: '取消', color: '#666666' }, { text: '恢复存档', color: '#356BE8' }] });
      if (answer.index !== 1) return;
      await local.restore(this.context, snapshot.snapshotId, snapshot.appId, preview);
      this.refreshBackups(); this.status = '存档已恢复，临时副本已清理';
    });
  }
  private deleteLocalSnapshot(snapshot: SaveManifest): void {
    void this.run('backups', async (): Promise<void> => {
      const answer = await promptAction.showDialog({ title: '删除这个游戏的备份？',
        message: this.saveTitle() + ' · ' + new Date(snapshot.createdAt).toLocaleString() +
          '\n删除后无法找回这份备份，游戏当前存档不会删除。',
        buttons: [{ text: '取消', color: '#666666' }, { text: '删除备份', color: '#C43838' }] });
      if (answer.index !== 1) return;
      LocalSaveSnapshotService.getInstance().deleteSnapshot(this.context, snapshot.snapshotId, snapshot.appId);
      this.refreshBackups(); this.status = '备份已删除，游戏当前存档仍保留';
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
    void this.run('backups', async (): Promise<void> => {
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
    void this.run('backups', async (): Promise<void> => {
      const slot = this.currentSlot(); if (!slot) return;
      const answer = await promptAction.showDialog({ title: '上传此存档快照？',
        message: '将此快照的游戏存档文件发送到应用配置的私有云存档服务，存入“' + slot.label +
          '”。不会上传游戏目录路径。成功后替换云端备份；冲突时不覆盖另一设备的备份。',
        buttons: [{ text: '取消', color: '#666666' }, { text: '上传到云端', color: '#356BE8' }] });
      if (answer.index !== 1) return;
      const result = await CloudSaveService.getInstance().upload(this.context, slot, snapshot);
      await this.refreshCloudSaves();
      this.status = result === 'CONFLICT' ? '云端已有新版本，本地快照仍保留。请先下载检查，再明确选择重新上传或新建云存档位' :
        '云存档已校验并发布；其他设备绑定相同云存档位后可下载';
    });
  }
  private downloadSave(slot: CloudSaveSlot, version: CloudSaveVersion): void {
    void this.run('backups', async (): Promise<void> => {
      const snapshot = await CloudSaveService.getInstance().download(this.context, slot, version.versionId);
      this.refreshBackups(); this.selectedGame = snapshot.appId; this.saveDirectory = snapshot.selectedPath;
      this.snapshots = LocalSaveSnapshotService.getInstance().list(this.context, snapshot.appId);
      this.status = '云存档已校验为本地快照，游戏文件尚未变动。退出 Wine 引擎后可选择“恢复”';
    });
  }
  private deleteCloudVersion(slot: CloudSaveSlot, version: CloudSaveVersion): void {
    void this.run('backups', async (): Promise<void> => {
      const answer = await promptAction.showDialog({ title: '永久删除此云端版本？',
        message: slot.label + ' · ' + new Date(version.createdAt).toLocaleString() +
          '。删除后云端无法找回；不会删除已下载的本地快照。请先确认保留了需要的副本。',
        buttons: [{ text: '取消', color: '#666666' }, { text: '永久删除备份', color: '#C43838' }] });
      if (answer.index !== 1) return;
      await CloudSaveService.getInstance().deleteVersion(slot, version.versionId);
      await this.refreshCloudSaves(); this.status = '此云端版本已删除，本地文件未变更';
    });
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
  private AccountAvatar() {
    Stack({ alignContent: Alignment.Center }) {
      if (this.huaweiAvatar && !this.avatarFailed) {
        Image(this.huaweiAvatar).width(52).height(52).objectFit(ImageFit.Cover).borderRadius(26)
          .id('sync-account-avatar').onError((): void => { this.avatarFailed = true; })
      } else {
        SymbolGlyph($r('sys.symbol.person')).fontSize(28)
          .fontColor([this.vipActive ? '#B8904B' : skinColor('text_secondary')])
      }
      if (this.vipActive) {
        Text('VIP').fontSize(9).fontWeight(FontWeight.Bold).letterSpacing(1)
          .fontColor('#352611').padding({ left: 7, right: 7, top: 3, bottom: 3 })
          .linearGradient({ angle: 120, colors: [['#F4E1B8', 0], ['#D6AE62', 1]] })
          .borderRadius(6).position({ x: 15, y: 48 }).id('sync-avatar-vip')
      }
    }.width(64).height(64).borderRadius(32).backgroundColor(skinColor('search_background'))
      .border({ width: this.vipActive ? 2 : 1, color: this.vipActive ? '#D1AA65' : this.cardBorderColor })
      .id('sync-account-avatar-frame')
  }
  @Builder
  private Overview() {
    Flex({ direction: this.compact ? FlexDirection.Column : FlexDirection.Row,
      alignItems: this.compact ? ItemAlign.Start : ItemAlign.Center }) {
      Row({ space: 14 }) {
        this.AccountAvatar()
        Column({ space: 7 }) {
          Flex({ wrap: FlexWrap.Wrap, alignItems: ItemAlign.Center }) {
            Text(this.huaweiSignedIn ? this.huaweiNickname || '已登录' : '未登录')
              .fontSize(this.compact ? 18 : 20).fontWeight(FontWeight.Medium)
              .fontColor(skinColor('text_primary')).margin({ right: 10 }).id('sync-account-name')
            if (this.huaweiSignedIn) {
              Row({ space: 4 }) {
                if (this.vipActive) {
                  Image($r('app.media.vip_crown')).width(12).height(12).fillColor('#996B24')
                }
                Text(this.vipActive ? 'VIP 会员' : '普通账号').fontSize(11).fontWeight(FontWeight.Medium)
                  .fontColor(this.vipActive ? '#996B24' : skinColor('text_secondary'))
              }.padding({ left: 9, right: 9, top: 4, bottom: 4 }).borderRadius(7)
                .backgroundColor(this.vipActive ? '#F6EBD4' : skinColor('search_background'))
                .id('sync-account-membership-badge')
            }
          }.width('100%')
          Text(this.userId ? (this.membership === '普通账号' ? '开通 VIP，自动备份设置与输入方案' : this.membership) :
            this.huaweiSignedIn ? '同步尚未连接' : '登录后自动备份设置与输入方案')
            .fontSize(12).fontColor(skinColor('text_secondary')).width('100%').id('sync-account-detail')
        }.layoutWeight(1).alignItems(HorizontalAlign.Start)
      }.layoutWeight(this.compact ? 0 : 1).width(this.compact ? '100%' : undefined)
      Row({ space: 8 }) {
        Button(this.membershipExpanded ? '收起' : this.vipActive ? '会员详情' : '开通 VIP')
          .height(36).fontSize(13).fontWeight(FontWeight.Medium).padding({ left: 16, right: 16 }).borderRadius(12)
          .fontColor('#493413').linearGradient({ angle: 120, colors: [['#F5E5C4', 0], ['#DDC18B', 1]] })
          .enabled(!this.busy).id('sync-membership-entry')
          .onClick((): void => { this.membershipExpanded = !this.membershipExpanded; })
        if (!this.huaweiSignedIn) {
          this.Action('登录', 'sync-account-login', (): boolean => !this.busy, (): void => this.login())
        } else if (!this.userId && this.ready) {
          this.Action('连接同步', 'sync-account-connect', (): boolean => !this.busy, (): void => this.login())
        }
        if (this.userId) {
          this.Action('刷新状态', 'sync-refresh-account', (): boolean => !this.busy, (): void => {
            void this.run('account', async (): Promise<void> => { await ValueAddedService.getInstance().restorePurchases(); await this.refreshAccount(); AccountAutomaticSyncService.getInstance().requestSync(0); });
          })
        }
        if (this.huaweiSignedIn || this.userId) {
          this.Action('退出', 'sync-account-logout', (): boolean => !this.busy, (): void => this.logout())
        }
      }.margin({ left: this.compact ? 0 : 14, top: this.compact ? 12 : 0 })
    }.width('100%')
    if (!this.ready) {
      Text(this.availabilityDetail()).fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
        .id('sync-account-availability')
    }
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
    this.Heading('云端备份', '登录后自动备份当前设备的设置与输入方案，重装应用后登录同一账号恢复。平板与桌面分别保存。')
    Row({ space: 12 }) {
      Column({ space: 5 }) {
        Text(this.huaweiSignedIn && !this.userId ? '已登录 · 同步尚未连接' : this.backupStatus(this.autoSyncStatus || '等待登录')).fontSize(15).fontWeight(FontWeight.Medium)
          .fontColor(skinColor('text_secondary')).width('100%').id('sync-automatic-status')
        if (this.lastSyncTime > 0) {
          Text('最近备份 ' + new Date(this.lastSyncTime).toLocaleString())
            .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
        }
      }.layoutWeight(1).alignItems(HorizontalAlign.Start)
      this.Action('重新检查', 'sync-automatic-check', (): boolean => !!this.userId && !this.busy, (): void => {
        AccountAutomaticSyncService.getInstance().requestSync(0);
      })
    }.width('100%')
    this.SyncSwitch('设置同步', '自动备份外观、卡片布局与默认输入方案', 'preferences', (): boolean => this.autoPreferences)
    this.SyncSwitch('输入方案同步', '自动备份按键布局、鼠标参数和手柄映射', 'profiles', (): boolean => this.autoProfiles)
    if (!this.profilesReady) {
      Text('云同步暂未接通。未登录或离线时，本地设置与方案仍可正常使用。')
        .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
    }
    ForEach(this.syncItems, (item: SyncItemStatus) => {
      Column({ space: 8 }) {
        Row({ space: 12 }) {
          Text(item.name).fontSize(14).fontColor(skinColor('text_primary')).layoutWeight(1)
          Text(this.backupStatus(item.state)).fontSize(12).fontColor(skinColor('text_secondary')).maxLines(2)
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
              void this.run('automatic', async (): Promise<void> => { await AccountAutomaticSyncService.getInstance().resolvePreferences(false); this.refreshAutomaticSync(); });
            })
            this.Action('采用云端设置', 'sync-preferences-use', (): boolean => !this.busy, (): void => {
              void this.run('automatic', async (): Promise<void> => { await AccountAutomaticSyncService.getInstance().resolvePreferences(true); this.refreshAutomaticSync(); });
            })
          }.width('100%')
        }
      }.width('100%').padding(14).borderRadius(14).backgroundColor(skinColor('search_background'))
    }, (item: SyncItemStatus): string => item.id + ':' + item.state + ':' + item.lastSyncedAt)
  }
  @Builder
  private LocalSaves() {
    this.Heading('本地存档备份', '在游戏设置中开启后，自动保留一份最新备份。移除游戏后，备份仍会保留。')
    if (this.saveSummaries.length > 0) {
      TextInput({ placeholder: '搜索已备份的游戏', text: this.saveSearch }).height(44).fontSize(14)
        .fontColor(skinColor('text_primary')).backgroundColor(skinColor('search_background')).borderRadius(12)
        .id('sync-save-search').onChange((value: string): void => { this.saveSearch = value; })
    }
    if (!this.saveSummaries.length) {
      Text('还没有存档备份。支持备份的本地游戏会在游戏设置中显示开关，开启后自动备份。')
        .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
    }
      ForEach(this.saveSummaries.filter((game: BackupGameSummary): boolean =>
        game.title.toLocaleLowerCase().includes(this.saveSearch.trim().toLocaleLowerCase())), (game: BackupGameSummary) => {
        Button() {
          Row({ space: 12 }) {
            SymbolGlyph($r('sys.symbol.folder')).fontSize(24).fontColor([skinColor('accent')]).width(28)
            Column({ space: 5 }) {
              Text(game.title).fontSize(15).fontWeight(FontWeight.Medium).fontColor(skinColor('text_primary')).width('100%').maxLines(2)
              Text((game.installed ? (game.enabled ? '自动备份已开启' : '自动备份已关闭') : '游戏已移除 · 备份保留') +
                (game.count ? ' · 已有备份' : ' · 等待备份')).fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
              if (game.latestAt > 0) {
                Text('最近备份 ' + new Date(game.latestAt).toLocaleString()).fontSize(11)
                  .fontColor(skinColor('text_secondary')).width('100%')
              }
            }.layoutWeight(1).alignItems(HorizontalAlign.Start)
            SymbolGlyph($r('sys.symbol.chevron_right')).fontSize(14).fontColor([skinColor('text_secondary')])
          }.width('100%')
        }.type(ButtonType.Normal).width('100%').constraintSize({ minHeight: 80 }).padding(14).borderRadius(14)
          .backgroundColor(this.selectedGame === game.appId ? skinColor('selected_background') : skinColor('search_background'))
          .enabled(!this.busy).id('sync-save-game-' + game.appId)
          .onClick((): void => {
            if (this.selectedGame === game.appId) this.selectedGame = '';
            else this.selectSaveGame(game.appId);
          })
      }, (game: BackupGameSummary): string => game.appId)
      Text('每游戏保留一份最新备份，总计最多 1 GB。新备份验证成功后替换原备份；失败时保留原备份。')
        .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
    if (this.selectedGame) {
      Divider().color(skinColor('selected_background')).margin({ top: 4, bottom: 4 })
      Row() {
        Text('已展开 ' + this.saveTitle()).fontSize(13).fontColor(skinColor('text_secondary')).layoutWeight(1)
        this.Action('收起', 'sync-save-collapse', (): boolean => !this.busy, (): void => { this.selectedGame = ''; })
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
      Text('最新备份').fontSize(14).fontWeight(FontWeight.Medium).fontColor(skinColor('text_primary')).width('100%')
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
            this.Action('删除备份', 'sync-save-delete-' + snapshot.snapshotId, (): boolean => !this.busy,
              (): void => this.deleteLocalSnapshot(snapshot))
          }.width('100%')
          if (this.cloudSavesReady && this.selectedSlot) {
            Row() {
              Blank()
              this.Action('上传备份', 'sync-save-upload-' + snapshot.snapshotId,
                (): boolean => !!this.userId && !this.busy && this.installedGame,
                (): void => this.uploadSave(snapshot))
            }.width('100%')
          }
        }.width('100%').padding(14).borderRadius(14).backgroundColor(skinColor('search_background'))
      }, (snapshot: SaveManifest): string => snapshot.snapshotId)
    }
    if (this.interruptedRestore) {
      this.Action('检查中断的恢复', 'sync-save-recover', (): boolean => !this.busy, (): void => {
        void this.run('backups', async (): Promise<void> => {
          LocalSaveSnapshotService.getInstance().recoverInterrupted(this.context);
          this.refreshBackups(); this.status = '恢复检查完成，原有文件已保留';
        });
      })
    }
  }
  @Builder
  private CloudSaves() {
    this.Heading('云端副本', '本地备份独立可用。云端版本先下载到最新备份，确认恢复后才写入游戏。')
    if (!this.cloudSavesReady) {
      Text('云服务暂未接通，本地备份不受影响。').fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
    } else if (this.selectedGame && this.installedGame) {
      if (this.cloudQuota) { Text(this.cloudQuota).fontSize(12).fontColor(skinColor('text_secondary')).width('100%') }
      Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.End }) {
        this.Action('为此游戏创建云端副本', 'sync-save-new-slot',
          (): boolean => !!this.userId && !this.busy && this.backupSupported && !this.selectedSlot, (): void => {
          void this.run('backups', async (): Promise<void> => {
            const slot = await CloudSaveService.getInstance().createSlot(this.saveTitle());
            CloudSaveService.getInstance().bind(this.context, slot, this.selectedGame, this.saveDirectory);
            this.selectedSlot = slot.slotId; await this.refreshCloudSaves(); this.status = '已关联此游戏，可以上传备份版本';
          });
        }, true)
        this.Action('刷新', 'sync-save-refresh', (): boolean => !!this.userId && !this.busy, (): void => {
          void this.run('backups', async (): Promise<void> => { await this.refreshCloudSaves(); });
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
                  this.Action('下载到最新备份', 'sync-cloud-save-download-' + version.versionId, (): boolean => !this.busy,
                    (): void => this.downloadSave(slot, version), true)
                  this.Action('删除云端版本', 'sync-cloud-save-delete-' + version.versionId, (): boolean => !this.busy,
                    (): void => this.deleteCloudVersion(slot, version))
                }.width('100%')
              }.width('100%')
            }, (version: CloudSaveVersion): string => version.versionId)
          }
        }.width('100%').padding(14).borderRadius(14).backgroundColor(skinColor('search_background'))
      }, (slot: CloudSaveSlot): string => slot.slotId)
      Text('云端每款游戏保留一份最新备份，单份最大 256 MB；上传失败或冲突时保留云端原备份。')
        .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
    } else if (!this.selectedGame) {
      Text('选择上方游戏后，查看与它关联的云端副本。').fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
    }
  }
  @Builder
  private MembershipBenefit(title: string, detail: string) {
    Row({ space: 10 }) {
      Text('✓').fontSize(15).fontWeight(FontWeight.Medium)
        .fontColor(this.vipActive ? '#B8904B' : skinColor('text_secondary'))
      Column({ space: 5 }) {
        Text(title).fontSize(14).fontWeight(FontWeight.Medium).fontColor(skinColor('text_primary')).width('100%')
        Text(detail).fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
      }.layoutWeight(1).alignItems(HorizontalAlign.Start)
    }.width(this.compact ? '100%' : '48%').margin({ bottom: this.compact ? 12 : 0 })
  }
  @Builder
  private Membership() {
    Text('会员权益').fontSize(15).fontWeight(FontWeight.Medium).fontColor(skinColor('text_primary'))
      .width('100%').id('sync-membership-title')
    Flex({ direction: this.compact ? FlexDirection.Column : FlexDirection.Row, justifyContent: FlexAlign.SpaceBetween }) {
      this.MembershipBenefit('设置备份', '外观与常用设置，重装后自动恢复')
      this.MembershipBenefit('输入方案备份', '按键布局、鼠标参数与手柄映射')
    }.width('100%').padding({ top: 4, bottom: 4 }).id('sync-membership-benefits')
    if (this.vipActive) {
      Text(this.renewalKnown && !this.renewalEnabled ?
        '自动续费已关闭，本期权益保留至到期。恢复续订只恢复后续续费。' :
        '会员权益已生效，可在订阅管理中调整自动续费。')
        .fontSize(12).fontColor(skinColor('text_secondary')).width('100%').id('sync-member-renewal-status')
    } else {
      if (this.membershipPriceStatus) {
        Text(this.membershipPriceStatus).fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
          .id('sync-membership-price-status')
      }
      ForEach(this.products.filter((product: StoreProduct): boolean => product.purpose === 'membership'), (product: StoreProduct) => {
        Flex({ direction: this.compact ? FlexDirection.Column : FlexDirection.Row,
          alignItems: this.compact ? ItemAlign.Start : ItemAlign.Center }) {
          Column({ space: 8 }) {
            Row({ space: 8 }) {
              Text('连续包月').fontSize(13).fontColor(skinColor('text_secondary'))
              if (product.promotion) {
                Text('限时优惠').fontSize(10).fontColor(skinColor('text_secondary'))
                  .padding({ left: 6, right: 6, top: 3, bottom: 3 }).borderRadius(6)
                  .backgroundColor(skinColor('search_background')).id('sync-membership-offer-badge')
              }
            }
            Row({ space: 5 }) {
              Text(product.price).fontSize(28).fontWeight(FontWeight.Bold).fontColor(skinColor('text_primary'))
                .id('sync-membership-current-price')
              Text('／月').fontSize(12).fontColor(skinColor('text_secondary')).margin({ top: 10 })
              if (product.regularPrice) {
                Text('标准价 ' + product.regularPrice + '／月').fontSize(12).fontColor(skinColor('text_secondary'))
                  .margin({ left: 10, top: 10 }).id('sync-membership-standard-price')
              }
            }
            if (product.priceDescription) {
              Text(product.priceDescription).fontSize(12).fontColor(skinColor('text_secondary'))
                .id('sync-membership-price-terms').width('100%')
            }
          }.layoutWeight(this.compact ? 0 : 1).width(this.compact ? '100%' : undefined).alignItems(HorizontalAlign.Start)
          Row() {
            this.Action('开通 VIP', 'sync-product-' + product.id,
              (): boolean => (!this.busy && (this.commerceReady)), (): void => {
                if (!this.userId) { this.login(); } else { this.buy(product); }
              }, true)
          }.margin({ left: this.compact ? 0 : 12, top: this.compact ? 12 : 0 })
        }.width('100%').padding({ top: 8, bottom: 4 }).id('sync-membership-product')
      }, (product: StoreProduct): string => product.id + ':' + product.price + ':' + (product.priceDescription ?? ''))
      Text('按月自动续费，可随时取消。取消后本期权益保留至到期。')
        .fontSize(11).fontColor(skinColor('text_secondary')).width('100%').id('sync-membership-terms')
      if (!this.commerceReady && !this.vipActive) {
        Text('暂未开放购买').fontSize(11).fontColor(skinColor('text_secondary')).width('100%').id('sync-membership-unavailable')
      }
    }
    Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.End }) {
      if (!this.vipActive) {
        Row() {
        this.Action('刷新价格', 'sync-membership-refresh', (): boolean => !this.busy, (): void => {
          void this.run('membership', async (): Promise<void> => { await this.refreshMembershipProducts(); });
        })
        }.margin({ left: 8, bottom: 8 })
      }
      Row() {
        this.Action(this.vipActive && this.renewalKnown && !this.renewalEnabled ? '恢复续订' : '管理订阅', 'sync-manage-subscriptions', (): boolean => !this.busy && this.huaweiSignedIn, (): void => {
          void this.run('membership', async (): Promise<void> => {
            await ValueAddedService.getInstance().manageSubscriptions();
            if (this.userId) {
              await ValueAddedService.getInstance().restorePurchases();
              await this.refreshAccount();
              AccountAutomaticSyncService.getInstance().requestSync(0);
            }
          });
        })
      }.margin({ left: 8, bottom: 8 })
      if (this.commerceReady && !this.vipActive) {
        Row() {
      this.Action('恢复购买', 'sync-restore-purchases', (): boolean => (this.commerceReady && !!this.userId && !this.busy), (): void => {
      void this.run('membership', async (): Promise<void> => {
        const result = await ValueAddedService.getInstance().restorePurchases(); await this.refreshAccount();
        this.status = result === 'ACK_PENDING' ? '部分订单确认仍待重试' : '恢复购买核验完成';
      });
      })
        }.margin({ left: 8, bottom: 8 })
      }
    }.width('100%').id('sync-membership-actions')
  }
  @Builder
  private OperationStatus(area: string) {
    if (this.statusArea === area) {
      if (this.busy) {
        Row({ space: 8 }) {
          LoadingProgress().width(18).height(18).color(skinColor('accent'))
          Text('正在处理…').fontSize(12).fontColor(skinColor('text_secondary'))
        }.width('100%')
      }
      if (this.status) {
        Text(this.status).fontSize(12).fontColor(skinColor('text_secondary')).width('100%').id('account-cloud-status')
      }
    }
  }
  build() {
    if (this.supported) {
      Column({ space: 14 }) {
        Column({ space: 16 }) {
          this.Overview()
          this.OperationStatus('account')
          if (this.membershipExpanded) {
            Divider().color(this.cardBorderColor).margin({ top: 2, bottom: 2 })
            this.Membership()
            this.OperationStatus('membership')
          }
        }.width('100%').padding(this.groupPadding).borderRadius(20)
          .backgroundColor(this.surfaceColor || skinColor('card_background'))
          .backgroundBlurStyle(this.glass ? BlurStyle.COMPONENT_ULTRA_THICK : BlurStyle.NONE)
          .border({ width: this.vipActive ? 1.2 : 1, color: this.vipActive ? '#CBA96A' : this.cardBorderColor }).id('sync-group-account')

        Column({ space: 16 }) {
          this.Profiles()
          this.OperationStatus('automatic')
        }.width('100%').padding(this.groupPadding).borderRadius(20)
          .backgroundColor(this.surfaceColor || skinColor('card_background'))
          .backgroundBlurStyle(this.glass ? BlurStyle.COMPONENT_ULTRA_THICK : BlurStyle.NONE)
          .border({ width: 1, color: this.cardBorderColor }).id('sync-group-automatic')
        Column({ space: 16 }) {
          this.LocalSaves()
          if (this.cloudSavesReady && this.selectedGame) { this.CloudSaves() }
          this.OperationStatus('backups')
        }.width('100%').padding(this.groupPadding).borderRadius(20)
          .backgroundColor(this.surfaceColor || skinColor('card_background'))
          .backgroundBlurStyle(this.glass ? BlurStyle.COMPONENT_ULTRA_THICK : BlurStyle.NONE)
          .border({ width: 1, color: this.cardBorderColor }).id('sync-group-backups')


      }.width('100%').alignItems(HorizontalAlign.Start)
    }
  }
}
