import { BusinessError } from '@kit.BasicServicesKit';
import { AccountAutomaticSyncService } from '../service/valueadded/AccountAutomaticSyncService';
import { SyncItemStatus } from '../model/AccountSyncModels';
import { common } from '@kit.AbilityKit';
import { DeviceCapabilityPolicy } from '../common/DeviceCapabilityPolicy';
import { InputProfile } from '../model/AppModels';
import { MemberEntitlement, ProfileSummary } from '../model/ValueAddedModels';
import { InputProfileService } from '../service/InputProfileService';
import { StoreProduct, StoreProductIssue, ValueAddedService } from '../service/valueadded/ValueAddedService';
import { skinColor } from '../service/SkinColors';
import { router } from '@kit.ArkUI';
import { MembershipSupport } from './MembershipSupport';
import { MembershipConfirmation, MEMBERSHIP_TERMS_VERSION } from '../model/MembershipCommerce';
import { membershipLegalText } from '../model/MembershipLegal';

interface MembershipRouteParams { membership?: string; deliveryId?: string; }

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
  @State private membershipUncertain: boolean = false;
  @State private renewalKnown: boolean = false;
  @State private renewalEnabled: boolean = true;
  @State private membershipExpanded: boolean = false;
  @State private selectedProductId: string = '';
  @State private membershipAccepted: boolean = false;
  @State private autoRenewAccepted: boolean = false;
  @State private showCheckout: boolean = false;
  @State private checkoutLegalExpanded: boolean = false;
  private checkoutScroller: Scroller = new Scroller();
  @State private reminderReady: boolean = false;
  @State private reminderStatus: string = '支付前将检查续费提醒，并在需要时申请通知权限。';
  @State private renewalDetail: string = '';
  private notificationTimer: number = -1;
  private vipOwner: string = '';
  private vipDeadline: number = 0;
  private vipTimer: number = -1;
  @State private products: StoreProduct[] = [];
  @State private productIssues: StoreProductIssue[] = [];
  private productRequest: number = 0;
  @State private membershipPriceStatus: string = '正在加载价格';
  @State private profiles: InputProfile[] = [];
  @State private cloudProfiles: ProfileSummary[] = [];
  @State private conflicts: string[] = [];
  @State private commerceReady: boolean = false;
  @State private profilesReady: boolean = false;
  @State private autoSyncStatus: string = '';
  @State private syncItems: SyncItemStatus[] = [];
  @State private profileDetailsExpanded: boolean = false;
  @State private lastSyncTime: number = 0;
  @State private autoProfiles: boolean = true;
  @State private autoPreferences: boolean = true;
  @State private preferencesConflict: boolean = false;
  @State private configurationConflict: boolean = false;
  @State private configurationBytes: number = -1;
  private syncListener: (() => void) | null = null;
  private entitlementListener: (() => void) | null = null;
  private accountListener: ((userId: string) => void) | null = null;
  private membershipRequest: number = 0;
  private context: common.UIAbilityContext = getContext(this) as common.UIAbilityContext;
  private alive: boolean = false;

  aboutToAppear(): void {
    this.alive = true;
    const params = router.getParams() as MembershipRouteParams;
    this.membershipExpanded = params?.membership === 'expanded';
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
    this.accountListener = (userId: string): void => {
      if (!this.alive) return;
      this.membershipRequest++;
      this.clearConfirmation(); this.reminderReady = false;
      this.reminderStatus = '支付前将检查续费提醒，并在需要时申请通知权限。';
      this.userId = userId; this.refreshHuaweiIdentity(); this.resetVip();
      this.membership = userId ? '正在更新会员状态' : '等待登录';
      if (userId && params?.deliveryId) void ValueAddedService.getInstance().recordReminderOpened(params.deliveryId).catch((): void => {});
      if (userId) void this.refreshMemberState().catch((): void => {});
    };
    ValueAddedService.getInstance().subscribeAccount(this.accountListener);
    void this.run('account', async (): Promise<void> => {
      const service = ValueAddedService.getInstance();
      await service.initialize(this.context);
      await InputProfileService.getInstance().initialize(this.context);
      this.profiles = InputProfileService.getInstance().getProfiles();
      this.ready = service.getCapabilities().account;
      this.commerceReady = service.getCapabilities().commerce && service.getCapabilities().membershipPurchases !== false && !!service.getMembershipOperator();
      this.profilesReady = service.getCapabilities().profiles;
      this.userId = service.getUserId();
      this.refreshHuaweiIdentity();
      this.availability = service.accountAvailability();
      if (this.userId) await this.refreshAccount(); else await this.refreshMembershipProducts();
      await this.refreshReminder(false);
      if (params?.deliveryId) await service.recordReminderOpened(params.deliveryId);
    });
    this.notificationTimer = setInterval((): void => {
      if (this.alive && this.membershipExpanded && !this.busy) void this.refreshReminder(false);
    }, 30000);
  }
  aboutToDisappear(): void {
    this.alive = false; this.membershipRequest++; this.resetVip();
    if (this.notificationTimer >= 0) clearInterval(this.notificationTimer);
    if (this.syncListener) AccountAutomaticSyncService.getInstance().unsubscribe(this.syncListener);
    if (this.entitlementListener) ValueAddedService.getInstance().unsubscribeEntitlement(this.entitlementListener);
    if (this.accountListener) ValueAddedService.getInstance().unsubscribeAccount(this.accountListener);
  }
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
    if (error.message === 'MEMBERSHIP_CONSENT_REQUIRED') return '请选择商品，并主动阅读和勾选相应协议';
    if (error.message === 'MEMBERSHIP_STATUS_UNKNOWN') return '本期会员状态待核验，请刷新或恢复购买，无需再次付款';
    if (error.message === 'MEMBERSHIP_NOT_READY' || error.message === 'MEMBERSHIP_NOT_OPEN') return '会员购买尚未开放，管理订阅和恢复购买仍可使用';
    if (error.message === 'RENEWAL_NOTIFICATION_REQUIRED') return '请允许应用发送续费通知后重试';
    if (error.message === 'REMINDERS_UNAVAILABLE') return '续费提醒服务暂不可用，连续包月暂时无法开通，请稍后检查；已有订阅仍可管理';
    if (error.message === 'PRODUCT_MISSING') return '该商品价格暂不可用，请重新获取价格后重试';
    if (error.message === 'INVALID_OFFER') return '优惠信息暂不可用，请刷新会员价格后重试';
    if (error.message === 'AUTH_STATE_MISMATCH') return '登录响应校验未通过，请重新登录';
    if (error.message === 'NOT_CONFIGURED') return '云服务暂未接通，本地设置与输入方案仍可使用';
    if (error.message === 'SIGN_IN_REQUIRED') return '登录已过期，请重新登录';
    if (error.message === 'ACCOUNT_CHANGED') return '账号已变更，本次操作已取消';
    if (error.message === 'PREFERENCES_PROFILE_PENDING') return '默认输入方案尚未就绪，请先处理对应方案的同步状态';
    if (error.message === 'BUSY') return '另一个备份操作正在进行，请稍后重试';
    if (['INVALID_JWS', 'INVALID_SUBSCRIPTION', 'INVALID_ORDER', 'ORDER_NOT_VERIFIED', 'PROVIDER_UNAVAILABLE', 'SERVICE_ERROR'].includes(error.message)) return '会员确认暂未完成，请点击刷新状态重试，无需重复购买';
    if (error.message === 'MEMBERSHIP_REQUIRED') return '云同步需要有效会员，请刷新权益或恢复购买';
    return '操作未完成，请稍后重试';
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
        if (!this.userId) { this.membership = '未登录'; this.cloudProfiles = []; this.conflicts = []; }
      }
    } finally { if (this.alive) this.busy = false; }
  }
  private refreshAutomaticSync(): void {
    if (!this.alive) return;
    const sync = AccountAutomaticSyncService.getInstance();
    this.autoSyncStatus = sync.status(); this.syncItems = sync.itemStates();
    this.autoProfiles = sync.profilesEnabled(); this.autoPreferences = sync.preferencesEnabled();
    this.configurationConflict = sync.configurationConflict(); this.configurationBytes = sync.configurationBytes();
    this.preferencesConflict = sync.hasPreferencesConflict(); this.lastSyncTime = sync.lastSyncedAt();
    this.userId = ValueAddedService.getInstance().getUserId();
    this.refreshHuaweiIdentity();
    this.profiles = InputProfileService.getInstance().getProfiles();
    this.conflicts = this.profiles.filter((profile: InputProfile): boolean => !!ValueAddedService.getInstance().getConflict(profile.id))
      .map((profile: InputProfile): string => profile.id);
  }
  private configurationSizeText(): string {
    if (this.configurationBytes < 0) return '云端大小待检查';
    if (this.configurationBytes === 0) return '云端暂无备份';
    if (this.configurationBytes < 1048576) return '云端 ' + (this.configurationBytes / 1024).toFixed(1) + ' KB';
    return '云端 ' + (this.configurationBytes / 1048576).toFixed(1) + ' MB';
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
  private inputSyncItems(): SyncItemStatus[] {
    const items = this.syncItems.filter((item: SyncItemStatus): boolean => item.id !== 'preferences' && item.id !== 'profiles-status');
    this.profiles.forEach((profile: InputProfile): void => {
      if (!items.some((item: SyncItemStatus): boolean => item.id === profile.id)) {
        items.push({ id: profile.id, name: profile.name,
          state: this.huaweiSignedIn && !this.userId ? '已登录 · 同步尚未连接' : this.autoSyncStatus || '等待登录', lastSyncedAt: 0 });
      }
    });
    return items;
  }
  private profileNeedsAttention(item: SyncItemStatus): boolean {
    return this.conflicts.indexOf(item.id) >= 0 || this.syncItems.some((value: SyncItemStatus): boolean => value.id === item.id) &&
      ['冲突', '等待', '失败', '异常', '未上传'].some((text: string): boolean => item.state.indexOf(text) >= 0);
  }
  private visibleProfileItems(): SyncItemStatus[] {
    const items = this.inputSyncItems();
    // Collapsing ordinary details must not hide conflicts or recovery that needs attention.
    return this.profileDetailsExpanded ? items : items.filter((item: SyncItemStatus): boolean => this.profileNeedsAttention(item));
  }
  private profileDetailsSummary(): string {
    const ids = new Set<string>();
    this.profiles.forEach((profile: InputProfile): void => { ids.add(profile.id); });
    const items = this.inputSyncItems();
    items.forEach((item: SyncItemStatus): void => { ids.add(item.id); });
    const attention = items.filter((item: SyncItemStatus): boolean => this.profileNeedsAttention(item)).length;
    return '共 ' + ids.size + ' 个方案' + (attention > 0 ? ' · ' + attention + ' 项待处理' :
      this.profileDetailsExpanded ? '' : ' · 点击查看备份明细');
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
    this.commerceReady = service.getCapabilities().commerce && service.getCapabilities().membershipPurchases !== false && !!service.getMembershipOperator();
    this.profilesReady = service.getCapabilities().profiles;
    const entitlement = await this.refreshMemberState();
    if (!this.alive || owner !== service.getUserId()) return;
    if (!this.vipActive) await this.refreshMembershipProducts(); else this.products = [];
    if (this.profilesReady && entitlement.active) this.cloudProfiles = await service.listCloudProfiles();
  }
  private async refreshMemberState(): Promise<MemberEntitlement> {
    const service = ValueAddedService.getInstance(), owner = service.getUserId();
    const request = ++this.membershipRequest;
    const entitlement = await service.entitlement();
    if (!this.alive || request !== this.membershipRequest || owner !== service.getUserId()) return entitlement;
    this.applyVip(entitlement, owner);
    this.membershipUncertain = entitlement.refreshRequired === true;
    this.renewalDetail = entitlement.autoRenew === false ? '自动续费已关闭，权益保留至 ' + this.expiryDate(entitlement.expiresAt) :
      entitlement.nextRenewalAt && entitlement.nextRenewalPrice ? '预计续费 ' + this.expiryDate(entitlement.nextRenewalAt) + '，' +
        (entitlement.renewalCurrency || '') + ' ' + (entitlement.nextRenewalPrice / 100).toFixed(2) + '，延长一个月' : '';
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
    this.renewalKnown = false; this.renewalEnabled = true; this.membershipUncertain = false;
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
      this.resetVip(); this.membership = '正在更新会员状态';
      // Check the next paid term before offering another purchase. A renewal may have happened while this page was open.
      void this.refreshMemberState().then((): void => {
        if (this.alive && !this.vipActive) void this.refreshMembershipProducts();
      }).catch((): void => { if (this.alive) this.membership = '会员状态待重新核验'; });
      return;
    }
    this.vipTimer = setTimeout((): void => this.updateVipExpiry(), Math.min(remaining, 60000));
  }
  private async refreshMembershipProducts(): Promise<void> {
    const service = ValueAddedService.getInstance(), owner = service.getUserId(), request = ++this.productRequest;
    this.clearConfirmation();
    this.membershipPriceStatus = '正在加载价格';
    try {
      const products = await service.products();
      if (this.alive && owner === service.getUserId() && request === this.productRequest) {
        this.products = products;
        this.productIssues = service.getMembershipProductIssues();
        this.membershipPriceStatus = products.some((product: StoreProduct): boolean => product.purpose === 'membership') ? '' :
          '暂未查到会员商品，请稍后刷新。';
      }
    } catch (_) {
      if (this.alive && owner === service.getUserId() && request === this.productRequest) {
        this.products = []; this.productIssues = service.getMembershipProductIssues();
        this.membershipPriceStatus = '价格暂时无法获取，请稍后刷新。';
      }
    }
  }
  private availabilityDetail(): string {
    if (this.availability === 'configuration_missing') return '云同步暂不可用，本地设置与输入方案可正常使用。';
    if (this.availability === 'identity_mismatch') return '同步暂不可用，请更新应用后重试。';
    if (this.availability === 'service_closed') return '同步暂未开放，本地设置与输入方案可正常使用。';
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
      this.commerceReady = service.getCapabilities().commerce && service.getCapabilities().membershipPurchases !== false && !!service.getMembershipOperator();
      this.profilesReady = service.getCapabilities().profiles;
      this.availability = service.accountAvailability();
      await service.signIn();
      this.refreshHuaweiIdentity();
      this.userId = service.getUserId();
      if (this.userId) {
        await this.refreshAccount();
        AccountAutomaticSyncService.getInstance().requestSync(0, true);
        this.status = '已登录，正在检查云端备份';
      } else this.status = '已登录，同步尚未连接';
    });
  }
  private logout(): void {
    void this.run('account', async (): Promise<void> => {
      await ValueAddedService.getInstance().signOut();
      this.huaweiSignedIn = false; this.huaweiAvatar = ''; this.huaweiNickname = ''; this.avatarFailed = false; this.userId = ''; this.membership = '未登录'; this.cloudProfiles = []; this.conflicts = [];
      await this.refreshMembershipProducts();
      this.status = '已退出，当前账号的会话和云端状态已清除';
    });
  }
  private async showPurchaseMessage(title: string, message: string): Promise<void> {
    if (!this.alive) return;
    this.status = message; this.statusArea = 'membership';
    const prompt = this.getUIContext().getPromptAction();
    try {
      await prompt.showDialog({ title: title, message: message,
        buttons: [{ text: '知道了', color: skinColor('accent') }] });
    } catch (_) {
      // Keep the outcome in the page even if the window closes during presentation.
      if (this.alive) {
        try { prompt.showToast({ message: message }); } catch (_) {}
      }
    }
  }
  private buy(product: StoreProduct): void {
    if (!this.canBuyMembership(product)) return;
    const service = ValueAddedService.getInstance();
    const confirmation: MembershipConfirmation = { userId: this.userId, quote: service.quoteKey(product),
      termsVersion: MEMBERSHIP_TERMS_VERSION, membershipAccepted: this.membershipAccepted, autoRenewAccepted: this.autoRenewAccepted };
    void this.run('membership', async (): Promise<void> => {
      // This explicit payment action can request permission. A background readiness check must not lock the button forever.
      if (product.type === 2 && !await this.refreshReminder(true)) {
        if (this.alive && confirmation.userId === service.getUserId()) {
          await this.showPurchaseMessage('暂时无法开通', this.reminderStatus + '\n\n本次未发起支付，没有扣款。');
        }
        return;
      }
      let result: string;
      try { result = await service.purchase(product, confirmation); }
      catch (error) {
        this.clearConfirmation();
        const failure = error as Error, code = (error as BusinessError).code;
        const detail = this.errorMessage(failure) + (Number.isInteger(code) ? '\n错误代码：' + code : '');
        await this.showPurchaseMessage('购买未完成', detail + '\n\n若已完成付款，请先刷新状态或恢复购买，不要重复支付。');
        if (this.alive) await this.refreshMembershipProducts();
        return;
      }
      if (!this.alive || confirmation.userId !== service.getUserId()) return;
      this.clearConfirmation();
      this.status = result === 'CANCELLED' ? '已取消购买' :
        result === 'VERIFICATION_PENDING' ? '购买已完成，会员确认暂未完成；会自动重试，无需再次付款' :
        result === 'ACK_PENDING' ? '会员已生效，订单确认稍后自动重试' :
        result === 'ALREADY_ACTIVE' ? '本期会员已生效，无需重复购买' :
        result === 'FINISHED' || result === 'RESTORED' || result === 'ALREADY_FINISHED' ? '会员状态已更新' : '订阅状态已更新';
      if (result === 'CANCELLED') return;
      if (result === 'VERIFICATION_PENDING') await this.showPurchaseMessage('付款已完成，会员待确认', this.status);
      AccountAutomaticSyncService.getInstance().requestSync(0);
      try { await this.refreshAccount(); }
      catch (_) {
        await this.showPurchaseMessage('会员状态待确认', '暂时无法刷新会员状态，请稍后刷新或恢复购买，无需再次付款。');
      }
    });
  }
  private canBuyMembership(product: StoreProduct): boolean {
    return this.showCheckout && !this.busy && !this.vipActive && !this.membershipUncertain && this.commerceReady && !!this.userId && this.selectedProductId === product.id &&
      this.membershipAccepted && (product.type !== 2 || this.autoRenewAccepted);
  }
  private clearConfirmation(): void {
    this.showCheckout = false; this.checkoutLegalExpanded = false;
    this.selectedProductId = ''; this.membershipAccepted = false; this.autoRenewAccepted = false;
  }
  private selectProduct(product: StoreProduct): void {
    this.clearConfirmation(); this.selectedProductId = product.id;
  }
  private openCheckout(): void {
    const product = this.checkoutProduct();
    if (this.busy || !this.commerceReady || !this.userId || this.vipActive || this.membershipUncertain ||
      !product) return;
    this.selectedProductId = product.id;
    this.membershipAccepted = false; this.autoRenewAccepted = false; this.checkoutLegalExpanded = false;
    this.status = ''; this.statusArea = 'membership'; this.showCheckout = true;
  }
  private checkoutProduct(): StoreProduct | undefined {
    const products = this.products.filter((product: StoreProduct): boolean => product.purpose === 'membership');
    return products.find((product: StoreProduct): boolean => product.id === this.selectedProductId) ||
      (products.length === 1 ? products[0] : undefined);
  }
  private checkoutButtonLabel(): string {
    if (!this.userId) return '登录后开通';
    if (!this.commerceReady) return '购买暂未开放';
    const product = this.checkoutProduct();
    return product ? '开通' + (product.type === 2 ? '连续包月 · ' : '单月会员 · ') + product.price : '请选择套餐';
  }
  private openLegal(document: string): void {
    void router.pushUrl({ url: 'pages/MembershipLegal', params: { document: document } });
  }
  private async refreshReminder(requestPermission: boolean): Promise<boolean> {
    const service = ValueAddedService.getInstance(), owner = service.getUserId();
    try {
      if (!owner) {
        this.reminderReady = false; this.reminderStatus = '登录后可检查续费提醒。'; return false;
      }
      if (requestPermission) {
        await service.refreshCapabilities();
        if (!this.alive || owner !== service.getUserId()) return false;
        this.commerceReady = service.getCapabilities().commerce && service.getCapabilities().membershipPurchases !== false && !!service.getMembershipOperator();
      }
      if (!service.getCapabilities().renewalReminders) throw new Error('REMINDERS_UNAVAILABLE');
      const ready = await service.prepareRenewalReminders(requestPermission);
      const enabled = ready || await service.notificationsEnabled();
      if (!this.alive || owner !== service.getUserId()) return false;
      this.reminderReady = ready;
      this.reminderStatus = ready ? '续费提醒已连接' :
        enabled ? '续费提醒登记未完成，请稍后检查重试。' : requestPermission ?
          '通知权限尚未开启，请在系统设置中允许本应用发送通知后重试。' : '通知尚未开启，点击支付将申请通知权限；允许后继续支付。';
      return ready;
    } catch (error) {
      if (this.alive && owner === service.getUserId()) {
        this.reminderReady = false;
        this.reminderStatus = (error as Error).message === 'REMINDERS_UNAVAILABLE' ? this.errorMessage(error as Error) :
          '续费提醒检查未完成，请检查网络和通知权限后重试。';
      }
      return false;
    }
  }
  private manageMembership(): void {
    void this.run('membership', async (): Promise<void> => {
      const service = ValueAddedService.getInstance();
      await service.manageSubscriptions(this.context);
      this.status = '状态待确认';
      if (!service.getUserId()) return;
      try {
        await service.restorePurchases();
        const value = await service.confirmSubscriptionState();
        this.applyVip(value, service.getUserId());
        this.status = value.refreshRequired ? '状态待确认，请稍后重试查询' : value.autoRenew === false ?
          '自动续费已关闭，权益保留至 ' + this.expiryDate(value.expiresAt) :
          value.autoRenew === true ? '自动续费仍然开启，可在华为订阅详情取消' : '状态待确认，请在华为订阅详情核实';
      } catch (_) { this.status = '状态待确认，请稍后重试查询；仅关闭弹窗不会取消自动续费'; }
    });
  }
  private expiryDate(time: number): string {
    const date = new Date(time);
    return date.getFullYear() + '-' + (date.getMonth() + 1).toString().padStart(2, '0') + '-' + date.getDate().toString().padStart(2, '0');
  }
  private refreshHuaweiIdentity(): void {
    const service = ValueAddedService.getInstance(), profile = service.getHuaweiProfile();
    if (!this.userId || this.vipOwner && this.vipOwner !== this.userId) this.resetVip();
    this.huaweiSignedIn = service.isHuaweiSignedIn(); this.huaweiNickname = profile.nickname;
    if (this.huaweiAvatar !== profile.avatarUri) this.avatarFailed = false;
    this.huaweiAvatar = profile.avatarUri;
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
          .onClick((): void => { this.membershipExpanded = !this.membershipExpanded; if (this.membershipExpanded) void this.refreshReminder(false); })
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
  private SyncItem(item: SyncItemStatus) {
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
        .id('sync-item-' + item.id)
  }
  @Builder
  private Profiles() {
    this.Heading('应用配置备份', '自动保存系统设置、输入方案和游戏配置。游戏图片留在游戏目录，游戏存档不上传。')
    Row({ space: 12 }) {
      Column({ space: 5 }) {
        Text(this.huaweiSignedIn && !this.userId ? '已登录 · 备份尚未连接' : this.backupStatus(this.autoSyncStatus || '等待登录'))
          .fontSize(15).fontWeight(FontWeight.Medium).fontColor(skinColor('text_secondary')).width('100%').id('sync-automatic-status')
        if (this.lastSyncTime > 0) {
          Text('最近备份 ' + new Date(this.lastSyncTime).toLocaleString()).fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
        }
      }.layoutWeight(1).alignItems(HorizontalAlign.Start)
      this.Action('重新检查', 'sync-automatic-check', (): boolean => !!this.userId && !this.busy, (): void => {
        AccountAutomaticSyncService.getInstance().requestSync(0, true);
      })
    }.width('100%')
    this.SyncSwitch('自动备份应用配置', '修改后自动备份；重装后恢复当前配置', 'configuration', (): boolean => this.autoProfiles && this.autoPreferences)
    if (this.configurationConflict) {
      Text('本机与云端均有配置，已保留双方内容。选择一次本次使用的配置。').fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
      Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.End }) {
        this.Action('保留本机', 'sync-configuration-keep', (): boolean => !this.busy, (): void => {
          void this.run('automatic', async (): Promise<void> => { await AccountAutomaticSyncService.getInstance().resolveConfiguration(false); this.refreshAutomaticSync(); });
        })
        this.Action('恢复云端', 'sync-configuration-restore', (): boolean => !this.busy, (): void => {
          void this.run('automatic', async (): Promise<void> => { await AccountAutomaticSyncService.getInstance().resolveConfiguration(true); this.refreshAutomaticSync(); });
        })
      }.width('100%')
    }
    Button() {
      Row({ space: 12 }) {
        Text('备份明细').fontSize(14).fontWeight(FontWeight.Medium).fontColor(skinColor('text_primary')).layoutWeight(1)
        SymbolGlyph(this.profileDetailsExpanded ? $r('sys.symbol.chevron_up') : $r('sys.symbol.chevron_down'))
          .fontSize(16).fontColor([skinColor('text_secondary')])
      }.width('100%')
    }.type(ButtonType.Normal).width('100%').padding(14).borderRadius(14)
      .backgroundColor(skinColor('search_background')).id('sync-profile-details-expand')
      .onClick((): void => { this.profileDetailsExpanded = !this.profileDetailsExpanded; })
    if (this.profileDetailsExpanded) {
      Text('完整输入方案 ' + this.profiles.length + ' 个 · ' + this.configurationSizeText())
        .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
      Text('游戏专用按键与兼容设置保存在游戏目录。系统背景和用户导入字体随应用配置备份。平板与桌面分别保存。')
        .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
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
  private CheckoutDetails(product: StoreProduct) {
    Column({ space: 16 }) {
      Scroll(this.checkoutScroller) {
        Column({ space: 16 }) {
          if (!this.checkoutLegalExpanded) {
          Column({ space: 6 }) {
            Text(product.type === 2 ? '连续包月' : '单月会员').fontSize(18).fontWeight(FontWeight.Medium)
              .fontColor(skinColor('text_primary'))
            Text('本次支付 ' + product.price).fontSize(28).fontWeight(FontWeight.Bold)
              .fontColor(skinColor('text_primary')).id('sync-checkout-amount')
            Text(product.priceDescription || '开通一个日历月，到期不自动扣款')
              .fontSize(14).fontColor(skinColor('text_primary')).textAlign(TextAlign.Center)
          }.width('100%').padding(16).borderRadius(16).backgroundColor(skinColor('selected_background'))
          Text('会员权益：自动备份和恢复应用配置、输入方案。游戏存档不上传。')
            .fontSize(14).fontColor(skinColor('text_primary')).width('100%')
          if (product.type === 2) {
            Text('按月自动扣款，优惠结束后 ' + (product.regularPrice || product.price) + '/月。华为可能提前24小时扣款，失败后可能重试。')
              .fontSize(14).fontColor(skinColor('text_primary')).width('100%')
            Text('预计续费前7天推送提醒，正常情况下至少提前5日告知金额、扣款时间和延长期限；支付前需开启通知。')
              .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
            Text('随时取消：账号与同步 → 管理订阅／取消自动续费 → 华为订阅详情 → 取消订阅 → 确认。本期权益保留，取消不等于退款。')
              .fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
          }
          }
          if (this.checkoutLegalExpanded) {
            Text(membershipLegalText('membership', ValueAddedService.getInstance().getMembershipOperator()))
              .fontSize(13).lineHeight(22).fontColor(skinColor('text_secondary')).width('100%').copyOption(CopyOptions.LocalDevice)
          }
        }.width('100%').alignItems(HorizontalAlign.Start).padding({ left: 22, right: 22, bottom: 12 })
      }.layoutWeight(1).width('100%').scrollBar(BarState.Off).edgeEffect(EdgeEffect.Spring)
      Column({ space: 8 }) {
        Divider().color(this.cardBorderColor)
        Button(this.checkoutLegalExpanded ? '收起完整服务说明' : '查看会员、续费与取消说明 ›')
          .fontSize(13).height(32).fontColor(skinColor('accent')).backgroundColor(Color.Transparent)
          .onClick((): void => {
            this.checkoutLegalExpanded = !this.checkoutLegalExpanded;
            this.checkoutScroller.scrollTo({ xOffset: 0, yOffset: 0 });
          }).id('sync-checkout-terms')
        Row({ space: 6 }) {
            Checkbox({ name: 'membership', group: 'membership-consent' }).select(this.membershipAccepted)
              .enabled(!this.busy).id('sync-consent-membership')
              .onChange((checked: boolean): void => { this.membershipAccepted = checked; })
            Text('我已阅读并同意《会员服务协议》').fontSize(13).fontColor(skinColor('text_primary')).layoutWeight(1)
          }.width('100%')
          if (product.type === 2) {
            Row({ space: 6 }) {
              Checkbox({ name: 'renewal', group: 'renewal-consent' }).select(this.autoRenewAccepted)
                .enabled(!this.busy).id('sync-consent-renewal')
                .onChange((checked: boolean): void => { this.autoRenewAccepted = checked; })
              Text('我同意《自动续费服务协议》，授权按月自动扣款').fontSize(13)
                .fontColor(skinColor('text_primary')).layoutWeight(1)
            }.width('100%')
          }
        if (this.status && this.statusArea === 'membership') {
          Text(this.status).fontSize(13).fontColor(skinColor('text_primary')).width('100%').id('sync-checkout-status')
        } else if (!this.membershipAccepted || product.type === 2 && !this.autoRenewAccepted) {
          Text(product.type === 2 ? '请分别勾选上方两项协议后支付' : '请勾选会员协议后支付')
            .fontSize(12).fontColor(skinColor('text_secondary')).width('100%').id('sync-consent-status')
        } else if (product.type === 2 && !this.reminderReady) {
          Text(this.reminderStatus).fontSize(12).fontColor(skinColor('text_secondary')).width('100%').id('sync-reminder-status')
        }
        Button(this.busy ? '正在检查，请稍候…' : '确认并支付 ' + product.price + (product.type === 2 ? ' · 连续包月' : ' · 单月会员'))
          .width('100%').height(48).fontSize(16).fontWeight(FontWeight.Medium)
          .backgroundColor(skinColor('accent')).fontColor('#FFFFFFFF').enabled(this.canBuyMembership(product))
          .onClick((): void => this.buy(product)).id('sync-product-' + product.id)
      }.width('100%').padding({ left: 22, right: 22, top: 8, bottom: 22 })
    }.width('100%').height('100%')
  }
  @Builder
  private CheckoutSheet() {
    ForEach(this.products.filter((product: StoreProduct): boolean => product.purpose === 'membership' && product.id === this.selectedProductId),
      (product: StoreProduct) => { this.CheckoutDetails(product) }, (product: StoreProduct): string => ValueAddedService.getInstance().quoteKey(product))
  }
  @Builder
  private Membership() {
    this.Heading('VIP 会员', '自动备份和恢复应用配置、输入方案；游戏存档不上传。')
    if (this.vipActive || this.membershipUncertain) {
      Text(this.membership).fontSize(14).fontColor(skinColor('text_primary')).width('100%')
      if (this.renewalDetail) { Text(this.renewalDetail).fontSize(13).fontColor(skinColor('text_secondary')).width('100%') }
      if (this.vipActive && this.renewalEnabled && !this.reminderReady) {
        Text('续费提醒未连接，请检查通知；也可随时管理订阅。').fontSize(13).fontColor(skinColor('text_secondary'))
        this.Action('检查续费提醒', 'sync-restore-reminders', (): boolean => !this.busy && !!this.userId,
          (): void => { void this.run('membership', async (): Promise<void> => { await this.refreshReminder(true); }); })
      }
    } else {
      ForEach(this.products.filter((product: StoreProduct): boolean => product.purpose === 'membership'), (product: StoreProduct) => {
        Button() {
          Row({ space: 12 }) {
            Column({ space: 6 }) {
              Text(product.type === 2 ? '连续包月' : '单月会员').fontSize(16).fontWeight(FontWeight.Medium)
                .fontColor(skinColor('text_primary'))
              Text(product.priceDescription || '一个日历月，到期不自动扣款').fontSize(13)
                .fontColor(skinColor('text_secondary')).textAlign(TextAlign.Start)
            }.layoutWeight(1).alignItems(HorizontalAlign.Start)
            Text(product.price).fontSize(22).fontWeight(FontWeight.Bold).fontColor(skinColor('text_primary'))
            SymbolGlyph(this.selectedProductId === product.id ? $r('sys.symbol.checkmark_circle_fill') : $r('sys.symbol.circle'))
              .fontSize(22).fontColor([this.selectedProductId === product.id ? skinColor('accent') : skinColor('text_secondary')])
          }.width('100%').padding(16)
        }.width('100%').type(ButtonType.Normal).borderRadius(16)
          .backgroundColor(this.selectedProductId === product.id ? skinColor('selected_background') : skinColor('search_background'))
          .border({ width: this.selectedProductId === product.id ? 1.5 : 1,
            color: this.selectedProductId === product.id ? skinColor('accent') : this.cardBorderColor })
          .enabled(!this.busy).onClick((): void => this.selectProduct(product)).id('sync-select-' + product.id)
      }, (product: StoreProduct): string => ValueAddedService.getInstance().quoteKey(product))
      ForEach(this.productIssues, (issue: StoreProductIssue) => {
        Row({ space: 12 }) {
          Column({ space: 4 }) {
            Text(issue.type === 2 ? '连续包月' : '单月会员').fontSize(15).fontColor(skinColor('text_secondary'))
            Text('暂未获取到价格').fontSize(12).fontColor(skinColor('text_secondary'))
          }.layoutWeight(1).alignItems(HorizontalAlign.Start)
          Button('重试').fontSize(13).fontColor(skinColor('accent')).backgroundColor(Color.Transparent).enabled(!this.busy)
            .onClick((): void => { void this.run('membership', async (): Promise<void> => { await this.refreshMembershipProducts(); }); })
            .id('sync-retry-product-' + issue.type)
        }.width('100%').padding(16).borderRadius(16).backgroundColor(skinColor('search_background'))
          .id('sync-unavailable-product-' + issue.type)
      }, (issue: StoreProductIssue): string => issue.id + ':' + issue.type + ':' + issue.code)
      if (this.membershipPriceStatus) {
        Text(this.membershipPriceStatus).fontSize(13).fontColor(skinColor('text_secondary')).width('100%')
      }
      Button(this.checkoutButtonLabel()).width('100%').height(48).fontSize(16).fontWeight(FontWeight.Medium)
        .backgroundColor(skinColor('accent')).fontColor('#FFFFFFFF')
        .enabled(!this.busy && (!this.userId || this.commerceReady && !!this.checkoutProduct()))
        .onClick((): void => { if (!this.userId) this.login(); else this.openCheckout(); }).id('sync-checkout-open')
        .bindSheet($$this.showCheckout, this.CheckoutSheet(), { height: '90%', width: this.compact ? '100%' : 560,
          preferType: SheetType.CENTER, showClose: true, dragBar: false, title: { title: '确认开通 VIP' },
          backgroundColor: skinColor('card_background'), blurStyle: this.glass ? BlurStyle.COMPONENT_ULTRA_THICK : BlurStyle.NONE,
          onDisappear: (): void => { this.membershipAccepted = false; this.autoRenewAccepted = false; this.checkoutLegalExpanded = false; } })
      if (!this.commerceReady) {
        Text('购买暂未开放，已有会员仍可管理订阅或恢复购买。').fontSize(12).fontColor(skinColor('text_secondary'))
      }
      if (this.products.length === 0 && this.productIssues.length === 0) {
        this.Action('重新获取价格', 'sync-membership-refresh', (): boolean => !this.busy, (): void => {
          void this.run('membership', async (): Promise<void> => { await this.refreshMembershipProducts(); });
        })
      }
    }
    Button('会员服务、自动续费与取消说明 ›').fontSize(13).fontColor(skinColor('accent')).backgroundColor(Color.Transparent)
      .onClick((): void => this.openLegal('membership')).id('sync-terms-read')
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
          Flex({ wrap: FlexWrap.Wrap }) {
            this.Action('管理订阅／取消自动续费', 'sync-manage-subscriptions', (): boolean => !this.busy,
              (): void => this.manageMembership())
            this.Action('恢复购买', 'sync-restore-purchases', (): boolean => !this.busy && !!this.userId, (): void => {
              void this.run('membership', async (): Promise<void> => {
                await ValueAddedService.getInstance().restorePurchases(); await this.refreshAccount(); this.status = '恢复购买核验完成';
              });
            })
            this.Action('客服与帮助', 'membership-support', (): boolean => true, (): void => this.openLegal('support'))
          }.width('100%')
          MembershipSupport({ compact: true, showHelpLink: false })
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



      }.width('100%').alignItems(HorizontalAlign.Start)
    }
  }
}
