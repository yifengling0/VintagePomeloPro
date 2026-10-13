import { BusinessError } from '@kit.BasicServicesKit';
import { CoffeePurchaseService } from '../service/valueadded/CoffeePurchaseService';
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
  @State private coffeeUnit: number = 8;
  @State private coffeeCurrency: string = 'CNY';
  @State private coffeeStatus: string = '通过华为应用内支付';
  @State private status: string = '';
  @State private statusArea: string = 'account';
  @State private membership: string = '未登录';
  @State private products: StoreProduct[] = [];
  @State private membershipPriceStatus: string = '正在读取华为商品信息';
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
    if (this.supported) void this.loadCoffee();
    // Registered pages are also directly addressable. Guard before any service or SDK initialization.
    if (!this.supported) return;
    this.syncListener = (): void => this.refreshAutomaticSync();
    AccountAutomaticSyncService.getInstance().subscribe(this.syncListener);
    this.refreshAutomaticSync();
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
  aboutToDisappear(): void { this.alive = false; if (this.backupListener) LocalSaveSnapshotService.getInstance().unsubscribe(this.backupListener); if (this.syncListener) AccountAutomaticSyncService.getInstance().unsubscribe(this.syncListener); if (this.supported) CloudSaveService.getInstance().cancel(); }
  private errorMessage(error: Error): string {
    if (error.message.startsWith('HUAWEI_LOGIN_')) {
      const code = error.message.slice('HUAWEI_LOGIN_'.length);
      if (code === '1001502012') return '已取消华为账号登录';
      if (code === '1001502005') return '华为账号登录网络异常，请检查网络后重试';
      if (code === '1001500001') return '华为账号未通过应用签名校验（1001500001），需核对应用配置与开发证书指纹';
      if (code === '1001502001') return '请先登录设备上的华为账号后重试';
      return '华为账号登录未完成（' + code + '），请核对应用配置后重试';
    }
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
    this.membership = '正在向服务器核验权益';
    const entitlement = await service.entitlement();
    this.membership = entitlement.active && entitlement.expiresAt > entitlement.serverTime ?
      '会员有效至 ' + new Date(entitlement.expiresAt).toLocaleString() +
        (entitlement.autoRenew === true ? ' · 自动续费已开启' : entitlement.autoRenew === false ? ' · 到期后不再续费' : '') :
      entitlement.refreshRequired ? '会员状态待重新核验，请稍后刷新' :
      entitlement.billingRetry ? '续费扣款待处理，请在华为订阅管理中检查' :
      entitlement.subscriptionStatus === '5' ? '订阅已撤销' : '当前没有有效会员';
    await this.refreshMembershipProducts();
    if (this.profilesReady && entitlement.active) this.cloudProfiles = await service.listCloudProfiles();
    if (this.cloudSavesReady) await this.refreshCloudSaves();
  }
  private async refreshMembershipProducts(): Promise<void> {
    this.membershipPriceStatus = '正在读取华为商品信息';
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
    if (this.availability === 'identity_mismatch') return '云同步的应用配置不一致，暂不可连接。华为账号登录仍由系统独立校验。';
    if (this.availability === 'service_closed') return '云同步暂未开放。本地备份和华为账号登录仍可使用。';
    if (this.availability === 'unreachable') return '暂时无法连接云同步服务，请检查网络后重试。本地备份和华为账号登录仍可使用。';
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
        this.status = '已登录，正在自动检查同步';
      } else this.status = '华为账号登录成功；云同步尚未连接';
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
      this.status = result === 'ACK_PENDING' ? '权益核验已完成，商店确认待重试，请使用恢复购买' :
        result === 'FINISHED' ? '购买已核验并确认' : '订单状态：' + result;
      await this.refreshAccount();
    });
  }
  private refreshHuaweiIdentity(): void {
    const service = ValueAddedService.getInstance(), profile = service.getHuaweiProfile();
    this.huaweiSignedIn = service.isHuaweiSignedIn(); this.huaweiNickname = profile.nickname;
    if (this.huaweiAvatar !== profile.avatarUri) this.avatarFailed = false;
    this.huaweiAvatar = profile.avatarUri;
  }
  private async loadCoffee(): Promise<void> {
    try {
      const service = CoffeePurchaseService.getInstance(); await service.initialize(this.context);
      const fallback = service.defaultPrice();
      if (this.alive) { this.coffeeUnit = fallback.amount; this.coffeeCurrency = fallback.currency; }
      const price = await service.price();
      if (this.alive) { this.coffeeUnit = price.amount; this.coffeeCurrency = price.currency; this.coffeeStatus = '通过华为应用内支付'; }
    } catch (error) {
      if (this.alive) this.coffeeStatus = this.coffeeError(error as Error);
    }
  }
  private coffeePrice(quantity: number): string {
    const amount = this.coffeeUnit * quantity;
    return (this.coffeeCurrency === 'CNY' ? '¥' : this.coffeeCurrency + ' ') +
      (Number.isInteger(amount) ? '' + amount : amount.toFixed(2));
  }
  private coffeeError(error: Error): string {
    if (error.message === 'COFFEE_PRODUCT_MISSING') return '此应用的咖啡商品尚未在华为商店配置完成';
    if (error.message === 'COFFEE_PRICE_CHANGED') return '商品价格有变化，请刷新价格后重试';
    if (error.message === 'COFFEE_QUANTITY_UNSUPPORTED') return '当前系统暂不支持中杯和大杯，可选择小杯';
    if (error.message.startsWith('COFFEE_CONFIG')) return '咖啡商品配置需要更新';
    const sdkCode = (error as BusinessError).code;
    const code = Number.isInteger(sdkCode) ? sdkCode :
      /^COFFEE_IAP_[0-9]+$/.test(error.message) ? Number(error.message.slice('COFFEE_IAP_'.length)) : 0;
    if (code === 1001860003) return '此应用的咖啡商品尚未配置完成或商品信息无效（1001860003）';
    if (code === 1001860058 || error.message === 'COFFEE_IAP_1001860058') return '此开发版本需要在华为商店配置沙盒测试账号';
    return '暂时无法获取或处理华为支付商品' + (code ? '（' + code + '）' : '') + '，请稍后重试';
  }
  private supportDeveloper(quantity: number): void {
    void this.run('membership', async (): Promise<void> => {
      const service = CoffeePurchaseService.getInstance();
      try {
        await service.initialize(this.context);
        const result = await service.purchase(quantity, { amount: this.coffeeUnit, currency: this.coffeeCurrency });
        this.status = result === 'CANCELLED' ? '已取消支付' : result === 'ACK_PENDING' ?
          '订单确认尚未完成，请使用“检查未完成订单”，无需再次付款' :
          result === 'RECOVERED' ? '已确认之前的支持订单，感谢你！本次未发起新付款' : '感谢支持，咖啡订单已完成！';
      } catch (error) { this.status = this.coffeeError(error as Error); }
    });
  }
  private recoverCoffee(): void {
    void this.run('membership', async (): Promise<void> => {
      try {
        const service = CoffeePurchaseService.getInstance(); await service.initialize(this.context);
        const result = await service.recoverPurchases();
        this.status = result === 'ACK_PENDING' ? '部分订单还未确认，请稍后再试，无需再次付款' :
          result === 'RECOVERED' ? '已确认之前的支持订单，感谢你！' : '没有待确认的咖啡订单';
      } catch (error) { this.status = this.coffeeError(error as Error); }
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
  private Overview() {
    Row({ space: 14 }) {
      Stack({ alignContent: Alignment.Center }) {
        if (this.huaweiAvatar && !this.avatarFailed) {
          Image(this.huaweiAvatar).width(48).height(48).objectFit(ImageFit.Cover).borderRadius(16)
            .id('sync-account-avatar').onError((): void => { this.avatarFailed = true; })
        } else {
          SymbolGlyph($r('sys.symbol.person')).fontSize(27).fontColor([skinColor('accent')])
        }
      }.width(48).height(48).borderRadius(16).backgroundColor(skinColor('selected_background'))
      Column({ space: 5 }) {
        Text(this.huaweiSignedIn ? this.huaweiNickname || '已登录华为账号' : '华为账号').fontSize(this.compact ? 16 : 17).fontWeight(FontWeight.Medium)
          .fontColor(skinColor('text_primary')).width('100%')
        Text(this.userId ? this.membership : this.ready ? '登录后自动同步设置与输入方案' : this.availabilityTitle()).fontSize(12)
          .fontColor(skinColor('text_secondary')).width('100%')
      }.layoutWeight(1).alignItems(HorizontalAlign.Start)
      if (!this.huaweiSignedIn) {
        this.Action('登录华为账号', 'sync-account-login', (): boolean => !this.busy, (): void => this.login(), true)
      } else if (!this.userId && this.ready) {
        this.Action('连接云同步', 'sync-account-connect', (): boolean => !this.busy, (): void => this.login(), true)
      }
      if (this.huaweiSignedIn || this.userId) {
        this.Action('退出', 'sync-account-logout', (): boolean => (!this.busy), (): void => this.logout())
      }
    }.width('100%').alignItems(VerticalAlign.Center)
    if (this.userId) {
      Row() {
        Text('账号权益由云端核验').fontSize(12).fontColor(skinColor('text_secondary')).layoutWeight(1)
        this.Action('刷新权益', 'sync-refresh-account', (): boolean => (!this.busy), (): void => {
          void this.run('account', async (): Promise<void> => { await this.refreshAccount(); });
        })
      }.width('100%')
    }
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
    this.Heading('自动同步', '登录后按此账号的开关同步。平板与桌面分别保存；文件路径和运行环境设置不上传。')
    Row({ space: 12 }) {
      Column({ space: 5 }) {
        Text(this.huaweiSignedIn && !this.userId ? '华为账号已登录 · 等待云同步连接' : this.autoSyncStatus || '等待登录').fontSize(15).fontWeight(FontWeight.Medium)
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
              void this.run('automatic', async (): Promise<void> => { await AccountAutomaticSyncService.getInstance().resolvePreferences(false); this.refreshAutomaticSync(); });
            })
            this.Action('采用云端设置', 'sync-preferences-use', (): boolean => !this.busy, (): void => {
              void this.run('automatic', async (): Promise<void> => { await AccountAutomaticSyncService.getInstance().resolvePreferences(true); this.refreshAutomaticSync(); });
            })
          }.width('100%')
        }
      }.width('100%').padding(14).borderRadius(14).backgroundColor(skinColor('search_background'))
    }, (item: SyncItemStatus): string => item.id)
  }
  @Builder
  private LocalSaves() {
    this.Heading('存档备份', '在游戏设置中开启后，自动保留一份最新备份。移除游戏后，备份仍会保留。')
    if (this.saveSummaries.length > 0) {
      TextInput({ placeholder: '搜索已备份的游戏', text: this.saveSearch }).height(44).fontSize(14)
        .fontColor(skinColor('text_primary')).backgroundColor(skinColor('search_background')).borderRadius(12)
        .id('sync-save-search').onChange((value: string): void => { this.saveSearch = value; })
    }
    if (!this.saveSummaries.length) {
      Text('还没有游戏开启备份。打开游戏卡片的设置，开启“存档自动备份”；无需选择目录。')
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
  private CoffeeCup(title: string, quantity: number) {
    Button() {
      Column({ space: 10 }) {
        Text('☕').fontSize(quantity === 10 ? 32 : quantity === 4 ? 28 : 24).height(38)
        Text(title).fontSize(14).fontWeight(FontWeight.Medium).fontColor(skinColor('text_primary'))
        Text(this.coffeePrice(quantity)).fontSize(20).fontWeight(FontWeight.Bold).fontColor(skinColor('accent'))
      }.width('100%').padding({ top: 16, bottom: 16 })
    }.type(ButtonType.Normal).width(this.compact ? 100 : 140).margin({ right: 12, bottom: 8 })
      .borderRadius(16).backgroundColor(skinColor('search_background')).enabled(!this.busy)
      .id('sync-coffee-' + quantity).onClick((): void => this.supportDeveloper(quantity))
  }
  @Builder
  private Membership() {
    this.Heading('同步会员', '按月自动续费，价格与优惠由华为提供。')
    Text('开通后自动同步设置与输入方案。可随时在华为订阅管理中取消续费，取消后本期权益保留至到期；付款以华为收银台显示为准。')
      .fontSize(12).fontColor(skinColor('text_secondary')).width('100%').id('sync-membership-terms')
    if (!this.commerceReady) {
      Text('会员服务正在准备，暂不可开通。').fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
    }
    if (this.membershipPriceStatus) {
      Text(this.membershipPriceStatus).fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
        .id('sync-membership-price-status')
    }
    ForEach(this.products.filter((product: StoreProduct): boolean => product.purpose === 'membership'), (product: StoreProduct) => {
      Flex({ direction: this.compact ? FlexDirection.Column : FlexDirection.Row,
        alignItems: this.compact ? ItemAlign.Start : ItemAlign.Center }) {
        Column({ space: 6 }) {
          Flex({ wrap: FlexWrap.Wrap, alignItems: ItemAlign.Center }) {
            Text(product.title).fontSize(14).fontWeight(FontWeight.Medium).fontColor(skinColor('text_primary'))
            if (product.promotion) {
              Text('限时优惠').fontSize(10).fontColor(skinColor('accent')).padding({ left: 6, right: 6, top: 3, bottom: 3 }).margin({ left: 8, top: 3, bottom: 3 })
                .backgroundColor(skinColor('card_background')).borderRadius(6).id('sync-membership-offer-badge')
            }
          }.width('100%')
          if (product.regularPrice) {
            Text('标准价 ' + product.regularPrice + '／月').fontSize(12).fontColor(skinColor('text_secondary'))
              .id('sync-membership-standard-price').width('100%')
          }
          if (product.priceDescription) {
            Text(product.priceDescription).fontSize(12).fontColor(skinColor('text_secondary'))
              .id('sync-membership-price-terms').width('100%')
          }
        }.layoutWeight(this.compact ? 0 : 1).width(this.compact ? '100%' : undefined).alignItems(HorizontalAlign.Start)
        Row() {
          this.Action(product.price + '／月', 'sync-product-' + product.id,
            (): boolean => (!this.busy && this.commerceReady && !!this.userId), (): void => this.buy(product), true)
        }.margin({ left: this.compact ? 0 : 12, top: this.compact ? 12 : 0 })
      }.width('100%').padding(14).borderRadius(14).backgroundColor(skinColor('search_background'))
    }, (product: StoreProduct): string => product.id)
    Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.End }) {
      Row() {
        this.Action('刷新会员价格', 'sync-membership-refresh', (): boolean => !this.busy, (): void => {
          void this.run('membership', async (): Promise<void> => { await this.refreshMembershipProducts(); });
        })
      }.margin({ left: 8, bottom: 8 })
      Row() {
        this.Action('管理订阅', 'sync-manage-subscriptions', (): boolean => !this.busy && this.huaweiSignedIn, (): void => {
          void this.run('membership', async (): Promise<void> => {
            await ValueAddedService.getInstance().manageSubscriptions();
            if (this.userId) await this.refreshAccount();
          });
        })
      }.margin({ left: 8, bottom: 8 })
    }.width('100%').id('sync-membership-actions')
    Column({ space: 14 }) {
      Text('请开发者喝杯咖啡').fontSize(16).fontWeight(FontWeight.Medium).fontColor(skinColor('text_primary'))
        .width('100%')
      Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.Start }) {
        this.CoffeeCup('小杯', 1)
        this.CoffeeCup('中杯', 4)
        this.CoffeeCup('大杯', 10)
      }.width('100%')
      Text(this.coffeeStatus).fontSize(12).fontColor(skinColor('text_secondary')).width('100%').id('sync-coffee-status')
      Text('自愿支持，不包含会员权益；付款金额以华为收银台为准。')
        .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
      Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.End }) {
        this.Action('刷新价格', 'sync-coffee-refresh', (): boolean => !this.busy, (): void => { void this.loadCoffee(); })
        this.Action('检查未完成订单', 'sync-coffee-recover', (): boolean => !this.busy, (): void => this.recoverCoffee())
      }.width('100%')
    }.width('100%').alignItems(HorizontalAlign.Start).id('sync-support-development')
    if (this.commerceReady) {
      this.Action('恢复购买', 'sync-restore-purchases', (): boolean => (this.commerceReady && !!this.userId && !this.busy), (): void => {
      void this.run('membership', async (): Promise<void> => {
        const result = await ValueAddedService.getInstance().restorePurchases(); await this.refreshAccount();
        this.status = result === 'ACK_PENDING' ? '部分订单确认仍待重试' : '恢复购买核验完成';
      });
      })
    }
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
        }.width('100%').padding(this.groupPadding).borderRadius(20)
          .backgroundColor(this.surfaceColor || skinColor('card_background'))
          .backgroundBlurStyle(this.glass ? BlurStyle.COMPONENT_ULTRA_THICK : BlurStyle.NONE)
          .border({ width: 1, color: this.cardBorderColor }).id('sync-group-account')
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
        Column({ space: 16 }) {
          this.Membership()
          this.OperationStatus('membership')
        }.width('100%').padding(this.groupPadding).borderRadius(20)
          .backgroundColor(this.surfaceColor || skinColor('card_background'))
          .backgroundBlurStyle(this.glass ? BlurStyle.COMPONENT_ULTRA_THICK : BlurStyle.NONE)
          .border({ width: 1, color: this.cardBorderColor }).id('sync-group-membership')
      }.width('100%').alignItems(HorizontalAlign.Start)
    }
  }
}
