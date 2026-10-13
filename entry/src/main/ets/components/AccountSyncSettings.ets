import { BusinessError } from '@kit.BasicServicesKit';
import { AccountAutomaticSyncService } from '../service/valueadded/AccountAutomaticSyncService';
import { SyncItemStatus } from '../model/AccountSyncModels';
import { common } from '@kit.AbilityKit';
import { DeviceCapabilityPolicy } from '../common/DeviceCapabilityPolicy';
import { InputProfile } from '../model/AppModels';
import { MemberEntitlement, ProfileSummary } from '../model/ValueAddedModels';
import { InputProfileService } from '../service/InputProfileService';
import { StoreProduct, ValueAddedService } from '../service/valueadded/ValueAddedService';
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
  @State private commerceReady: boolean = false;
  @State private profilesReady: boolean = false;
  @State private autoSyncStatus: string = '';
  @State private syncItems: SyncItemStatus[] = [];
  @State private profileDetailsExpanded: boolean = false;
  @State private lastSyncTime: number = 0;
  @State private autoProfiles: boolean = true;
  @State private autoPreferences: boolean = true;
  @State private preferencesConflict: boolean = false;
  private syncListener: (() => void) | null = null;
  private entitlementListener: (() => void) | null = null;
  private accountListener: ((userId: string) => void) | null = null;
  private membershipRequest: number = 0;
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
    this.accountListener = (userId: string): void => {
      if (!this.alive) return;
      this.membershipRequest++;
      this.userId = userId; this.refreshHuaweiIdentity(); this.resetVip();
      this.membership = userId ? '正在更新会员状态' : '等待登录';
      if (userId) void this.refreshMemberState().catch((): void => {});
    };
    ValueAddedService.getInstance().subscribeAccount(this.accountListener);
    void this.run('account', async (): Promise<void> => {
      await InputProfileService.getInstance().initialize(this.context);
      this.profiles = InputProfileService.getInstance().getProfiles();
      const service = ValueAddedService.getInstance();
      await service.initialize(this.context);
      this.ready = service.getCapabilities().account;
      this.commerceReady = service.getCapabilities().commerce;
      this.profilesReady = service.getCapabilities().profiles;
      this.userId = service.getUserId();
      this.refreshHuaweiIdentity();
      this.availability = service.accountAvailability();
      if (this.userId) await this.refreshAccount(); else await this.refreshMembershipProducts();
    });
  }
  aboutToDisappear(): void {
    this.alive = false; this.membershipRequest++; this.resetVip();
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
    this.commerceReady = service.getCapabilities().commerce;
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
      this.commerceReady = service.getCapabilities().commerce;
      this.profilesReady = service.getCapabilities().profiles;
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
    ForEach(this.syncItems.filter((item: SyncItemStatus): boolean => item.id === 'preferences' || item.id === 'profiles-status'), (item: SyncItemStatus) => {
      this.SyncItem(item)
    }, (item: SyncItemStatus): string => item.id + ':' + item.state + ':' + item.lastSyncedAt)
    Button() {
      Row({ space: 12 }) {
        Column({ space: 5 }) {
          Text('方案备份明细').fontSize(14).fontWeight(FontWeight.Medium).fontColor(skinColor('text_primary')).width('100%')
          Text(this.profileDetailsSummary()).fontSize(12).fontColor(skinColor('text_secondary')).width('100%')
        }.layoutWeight(1).alignItems(HorizontalAlign.Start)
        SymbolGlyph(this.profileDetailsExpanded ? $r('sys.symbol.chevron_up') : $r('sys.symbol.chevron_down'))
          .fontSize(16).fontColor([skinColor('text_secondary')])
      }.width('100%')
    }.type(ButtonType.Normal).width('100%').padding(14).borderRadius(14)
      .backgroundColor(skinColor('search_background')).id('sync-profile-details-expand')
      .accessibilityText(this.profileDetailsExpanded ? '收起方案备份明细' : '展开方案备份明细')
      .onClick((): void => { this.profileDetailsExpanded = !this.profileDetailsExpanded; })
    ForEach(this.visibleProfileItems(), (item: SyncItemStatus) => {
      this.SyncItem(item)
    }, (item: SyncItemStatus): string => item.id + ':' + item.state + ':' + item.lastSyncedAt)
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
    Text('更多会员功能正在开发中').fontSize(12).fontColor(skinColor('text_secondary'))
      .width('100%').id('sync-membership-coming-soon')
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



      }.width('100%').alignItems(HorizontalAlign.Start)
    }
  }
}
