import { bundleManager, common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { authentication } from '@kit.AccountKit';
import { iap } from '@kit.IAPKit';
import { cloudFunction } from '@kit.CloudFoundationKit';
import deviceInfo from '@ohos.deviceInfo';
import http from '@ohos.net.http';
import util from '@ohos.util';
import { cryptoFramework } from '@kit.CryptoArchitectureKit';
import { DeviceCapabilityPolicy } from '../../common/DeviceCapabilityPolicy';
import { InputProfile } from '../../model/AppModels';
import { AccountSession, MemberEntitlement, ServiceCapabilities, ValueAddedConfig, ProfileEnvelope,
  ProfileConflict, ProfileSummary, cloudProfilePayload, profileDeviceClass, validateProfileEnvelope,
  validateValueAddedConfig } from '../../model/ValueAddedModels';
import { AutomaticSyncData, PreferencesRead, PreferencesWrite, PreferencesEnvelope, SyncedPreferences, validateSyncedPreferences } from '../../model/AccountSyncModels';
import { InputProfileService } from '../InputProfileService';
import { fileIo as fs } from '@kit.CoreFileKit';
import preferences from '@ohos.data.preferences';

interface AccountAttempt { attemptId: string; state: string; }
interface AccountExchange { attemptId: string; state: string; code: string; clientId: string; }
interface ApiError { code: string; diagnosticCode?: string; }
interface FunctionRequest { protocol: number; method: string; path: string; authorization: string; body: string; }
interface FunctionResponse { protocol: number; status: number; body: string; }
interface PromotionalPurchase { promotionalOfferId: string; applicationUserName: string; jwsRepresentation: string; }
interface PurchaseIntent { developerPayload: string; promotionalPurchase?: PromotionalPurchase; }
interface ProductRequest { productId: string; productType: number; promotionalOfferId?: string; }
interface VerifyRequest { purchaseData: string; productType: number; }
interface PurchaseVerdict {
  status: string;
  finishAllowed: boolean;
  productId: string;
  productType: number;
  purchaseOrderId: string;
  purchaseToken: string;
}
interface ProfilePut { baseRevision: number; payload: InputProfile; }
interface ProfilePutResult { status: string; current: ProfileEnvelope; }
interface ProfileIndex { profiles: ProfileSummary[]; }
interface EmptyBody { }
export interface StorePromotion {
  id: string; price: string; microPrice: number; paymentMode: number; periodUnit: number; periodCount: number;
}
export interface StoreProduct {
  id: string; title: string; price: string; type: number; purpose: 'membership' | 'coffee';
  regularPrice?: string; microPrice?: number; currency?: string; promotion?: StorePromotion; priceDescription?: string;
}
export interface HuaweiAccountProfile { nickname: string; avatarUri: string; status: string; }

/** No provider token, purchase token, or internal session is persisted on the device. */
export class ValueAddedService {
  private static instance: ValueAddedService | null = null;
  private config: ValueAddedConfig | null = null;
  private availability: string = 'checking';
  private context: common.UIAbilityContext | null = null;
  private session: AccountSession | null = null;
  private huaweiSignedIn: boolean = false;
  isHuaweiSignedIn(): boolean { this.guard(); return this.huaweiSignedIn; }
  private huaweiProfile: HuaweiAccountProfile = { nickname: '', avatarUri: '', status: '' };
  getHuaweiProfile(): HuaweiAccountProfile {
    this.guard(); return { nickname: this.huaweiProfile.nickname, avatarUri: this.huaweiProfile.avatarUri, status: this.huaweiProfile.status };
  }
  private generation: number = 0;
  private busy: boolean = false;
  private initializing: Promise<void> | null = null;
  private identityStore: preferences.Preferences | null = null;
  private resumeEnabled: boolean = false;
  private resuming: Promise<void> | null = null;
  private restoring: Promise<string> | null = null;
  private entitlementFlight: Promise<MemberEntitlement> | null = null;
  private entitlementRevision: number = 0;
  private capabilities: ServiceCapabilities = { account: false, commerce: false, profiles: false,
    cloudSaves: false, reason: 'NOT_CONFIGURED' };
  private purchaseRecovery: string = '';
  private purchaseRecoveryCheckedAt: number = 0;
  getPurchaseRecoveryStatus(): string { this.guard(); return this.purchaseRecovery; }
  getPurchaseRecoveryCheckedAt(): number { this.guard(); return this.purchaseRecoveryCheckedAt; }
  private entitlementListeners: Set<() => void> = new Set<() => void>();
  subscribeEntitlement(listener: () => void): void { this.entitlementListeners.add(listener); }
  unsubscribeEntitlement(listener: () => void): void { this.entitlementListeners.delete(listener); }
  private entitlementChanged(): void { this.entitlementListeners.forEach((listener: () => void): void => listener()); }
  private accountListeners: Set<(userId: string) => void> = new Set<(userId: string) => void>();
  subscribeAccount(listener: (userId: string) => void): void { this.accountListeners.add(listener); }
  unsubscribeAccount(listener: (userId: string) => void): void { this.accountListeners.delete(listener); }
  private profileHeads: Map<string, number> = new Map<string, number>();
  private conflicts: Map<string, ProfileConflict> = new Map<string, ProfileConflict>();

  static getInstance(): ValueAddedService {
    if (!ValueAddedService.instance) ValueAddedService.instance = new ValueAddedService();
    return ValueAddedService.instance;
  }
  private guard(): void {
    if (!DeviceCapabilityPolicy.supportsValueAddedFeatures()) throw new Error('FEATURE_HIDDEN');
  }
  async initialize(context: common.UIAbilityContext): Promise<void> {
    this.guard();
    if (this.initializing) return this.initializing;
    const work = this.initializeAccount(context);
    this.initializing = work;
    try { await work; } finally { if (this.initializing === work) this.initializing = null; }
  }
  private async initializeAccount(context: common.UIAbilityContext): Promise<void> {
    this.context = context;
    if (this.config && this.availability === 'ready') return;
    if (!this.identityStore) {
      this.identityStore = await preferences.getPreferences(context, 'account_identity');
      const remembered = this.identityStore.getSync('resume_login_v1', '') as string;
      if (remembered === '') {
        // Migrate only an existing account journal. This is intent to resume, never proof of identity or VIP.
        const journal = await preferences.getPreferences(context, 'account_automatic_sync');
        try {
          const raw = journal.getSync('state_v1', '') as string;
          this.resumeEnabled = raw.length <= 524288 && /^[a-f0-9]{64}$/.test((JSON.parse(raw) as AutomaticSyncData).lastOwner);
        } catch (_) { this.resumeEnabled = false; }
        await this.rememberLogin(this.resumeEnabled);
      } else this.resumeEnabled = remembered === 'true';
    }
    const bytes = await context.resourceManager.getRawFileContent('value_added_config.json');
    const config = JSON.parse(util.TextDecoder.create('utf-8').decodeToString(bytes)) as ValueAddedConfig;
    try { validateValueAddedConfig(config); }
    catch (_) { this.availability = 'configuration_missing'; return; }
    const info = bundleManager.getBundleInfoForSelfSync(bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_HAP_MODULE |
      bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_METADATA);
    const module = info.hapModulesInfo.find((item: bundleManager.HapModuleInfo): boolean => item.name === 'entry');
    if (!module || !module.metadata.some((item: bundleManager.Metadata): boolean =>
      item.name === 'client_id' && item.value === config.accountClientId)) {
      this.availability = 'identity_mismatch'; return;
    }
    this.context = context;
    this.config = config;
    try {
      this.availability = 'checking';
      await this.refreshCapabilities();
      this.availability = this.capabilities.account ? 'ready' : 'service_closed';
    } catch (error) {
      const sdkCode = (error as BusinessError).code;
      const message = (error as Error).message ?? '';
      const diagnostic = Number.isInteger(sdkCode) ? '' + sdkCode :
        (/^[A-Z_]{2,64}$/.test(message) ? message : 'FAILED');
      console.warn('[ValueAddedService] cloud connection failed: ' + diagnostic);
      this.config = null; this.availability = 'unreachable';
    }
    if (this.availability === 'ready') await this.resumeAccount();
  }
  private async rememberLogin(value: boolean): Promise<void> {
    this.resumeEnabled = value;
    if (this.identityStore) {
      await this.identityStore.put('resume_login_v1', value ? 'true' : 'false');
      await this.identityStore.flush();
    }
  }
  hasRememberedAccount(): boolean { this.guard(); return this.resumeEnabled; }
  async resumeAccount(): Promise<void> {
    this.guard();
    if (this.validSession() || !this.resumeEnabled || !this.context) return;
    if (!this.config || this.availability !== 'ready') { await this.initialize(this.context); return; }
    if (this.resuming) return this.resuming;
    if (this.busy) return;
    const work = this.resumeAuthorizedAccount();
    this.resuming = work;
    try { await work; }
    finally { if (this.resuming === work) this.resuming = null; }
  }
  private async resumeAuthorizedAccount(): Promise<void> {
    try { await this.authenticate(false); }
    catch (error) {
      // Silent requests never open login or consent UI. Retry transient failures in the foreground queue.
      const code = (error as BusinessError).code;
      const message = (error as Error).message;
      if (code === 1001502001 || code === 1001502002 ||
        message === 'HUAWEI_LOGIN_1001502001' || message === 'HUAWEI_LOGIN_1001502002') {
        await this.rememberLogin(false);
      }
      console.warn('[ValueAddedService] account resume failed: ' + this.safeDiagnostic(error as Error));
    }
  }
  async refreshCapabilities(): Promise<void> {
    this.guard();
    if (!this.context || !this.config) throw new Error('NOT_CONFIGURED');
    const capabilities = await this.request<ServiceCapabilities>('/v1/capabilities', http.RequestMethod.GET, '', false);
    if (capabilities.sandboxCommerce === true) {
      try { capabilities.commerce = capabilities.commerce && await iap.isSandboxActivated(this.context); }
      catch (_) { capabilities.commerce = false; }
    }
    this.capabilities = capabilities;
  }
  private recoveryFailure(error: Error): string {
    const diagnostic = this.safeDiagnostic(error);
    console.warn('[ValueAddedService] purchase recovery failed: ' + diagnostic);
    return diagnostic;
  }
  private safeDiagnostic(error: Error): string {
    const code = (error as BusinessError).code;
    const diagnostic = Number.isInteger(code) ? '' + code :
      (/^[A-Z][A-Z0-9_]{1,63}$/.test(error.message) ? error.message : 'FAILED');
    return diagnostic;
  }
  accountAvailability(): string { this.guard(); return this.availability; }
  getCapabilities(): ServiceCapabilities { this.guard(); return this.capabilities; }
  getUserId(): string { this.guard(); return this.validSession()?.userId ?? ''; }
  getConflict(id: string): ProfileConflict | undefined { this.guard(); return this.conflicts.get(id); }
  private validSession(): AccountSession | null {
    if (this.session && this.session.expiresAt <= Date.now()) this.clearSession();
    return this.session;
  }
  private clearSession(): void {
    this.generation++;
    this.session = null;
    this.purchaseRecovery = '';
    this.purchaseRecoveryCheckedAt = 0;
    this.profileHeads.clear();
    this.conflicts.clear();
    this.invalidateEntitlement();
    this.accountListeners.forEach((listener: (userId: string) => void): void => listener(''));
  }
  private async request<T>(path: string, method: http.RequestMethod, body: string | ArrayBuffer, authenticated: boolean = true): Promise<T> {
    this.guard();
    if (!this.config) throw new Error('NOT_CONFIGURED');
    const session = authenticated ? this.validSession() : null;
    if (authenticated && !session) throw new Error('SIGN_IN_REQUIRED');
    const epoch = this.generation;
    if (this.config.cloudFunctionName) {
      const desktop = DeviceCapabilityPolicy.deviceType() !== 'tablet';
      const supported = canIUse('SystemCapability.DeviceCloudGateway.CloudFoundation') && (!desktop || deviceInfo.sdkApiVersion >= 23);
      if (supported) {
        if (typeof body !== 'string') throw new Error('CLOUD_SAVE_TRANSPORT_UNAVAILABLE');
        const data: FunctionRequest = { protocol: 1, method: method, path: path,
          authorization: session ? 'Bearer ' + session.token : '', body: body };
        const encoded = new util.TextEncoder().encodeInto(JSON.stringify(data));
        if (encoded.byteLength > 768 * 1024) throw new Error('REQUEST_TOO_LARGE');
        const value = await cloudFunction.call({ name: this.config.cloudFunctionName,
          version: this.config.cloudFunctionVersion ?? '$latest', timeout: 60000, data: data });
        if (epoch !== this.generation) throw new Error('ACCOUNT_CHANGED');
        const response = (typeof value.result === 'string' ? JSON.parse(value.result) : value.result) as FunctionResponse;
        if (!response || response.protocol !== 1 || !Number.isInteger(response.status) ||
          response.status < 100 || response.status > 599 || typeof response.body !== 'string' ||
          new util.TextEncoder().encodeInto(response.body).byteLength > 1048576) throw new Error('INVALID_SERVICE_RESPONSE');
        return this.decodeResponse<T>(response.status, response.body);
      }
      // Older PC systems retain the configured HTTPS transport. Do not retry a failed
      // cloud mutation through another transport: the first request may have committed.
      if (!this.config.apiBaseUrl) throw new Error('CLOUD_FUNCTION_UNSUPPORTED');
    }
    const client = http.createHttp();
    try {
      const headers: Record<string, string> = { 'Content-Type': typeof body === 'string' ? 'application/json' : 'application/octet-stream',
        'Authorization': session ? 'Bearer ' + session.token : '' };
      if (typeof body !== 'string') headers['Content-Length'] = '' + body.byteLength;
      const response = await client.request(this.config.apiBaseUrl + path, {
        method: method, header: headers,
        extraData: body, expectDataType: http.HttpDataType.STRING,
        connectTimeout: 15000, readTimeout: typeof body === 'string' ? 30000 : 120000, usingCache: false,
        maxLimit: 1048576
      });
      if (epoch !== this.generation) throw new Error('ACCOUNT_CHANGED');
      return this.decodeResponse<T>(response.responseCode, response.result as string);
    } finally { client.destroy(); }
  }
  private decodeResponse<T>(status: number, body: string): T {
    if (status === 401) { this.clearSession(); throw new Error('SIGN_IN_REQUIRED'); }
    if (status < 200 || status >= 300) {
      const failure = JSON.parse(body) as ApiError;
      if (failure.diagnosticCode && /^(?:[A-Z][A-Z0-9_]{1,63}|[0-9]{1,12})$/.test(failure.diagnosticCode)) {
        console.warn('[ValueAddedService] server diagnostic: ' + failure.diagnosticCode);
      }
      const code = /^[A-Z_]{2,64}$/.test(failure.code) ? failure.code : 'SERVICE_ERROR';
      console.warn('[ValueAddedService] server error: ' + code);
      throw new Error(code);
    }
    return JSON.parse(body) as T;
  }
  cloudEpoch(): number { this.guard(); if (!this.validSession()) throw new Error('SIGN_IN_REQUIRED'); return this.generation; }
  assertCloudEpoch(epoch: number): void { this.guard(); this.assertGeneration(epoch); }
  async cloudRequest<T>(path: string, method: http.RequestMethod, body: string | ArrayBuffer = ''): Promise<T> {
    this.guard();
    if (!this.capabilities.cloudSaves || !path.startsWith('/v1/saves')) throw new Error('NOT_CONFIGURED');
    return this.request<T>(path, method, body);
  }
  async configurationRequest<T>(path: string, method: http.RequestMethod, body: string = ''): Promise<T> {
    this.guard();
    if (!this.capabilities.configuration || !path.startsWith('/v1/configuration/')) throw new Error('NOT_CONFIGURED');
    try {
      return await this.request<T>(path, method, body);
    } catch (error) {
      // Log only a fixed operation name: never account/job IDs, hashes or body.
      const operation = path.endsWith('/abort') ? 'abort' : path.endsWith('/finalize') ? 'finalize' :
        path.endsWith('/lease') ? 'lease' : path.endsWith('/uploads') ? 'begin' :
        path.indexOf('/chunks') >= 0 ? 'chunk' : 'current';
      console.warn('[ValueAddedService] configuration request failed: ' + operation);
      throw error as Error;
    }
  }
  async signIn(): Promise<void> {
    if (this.resuming) await this.resuming.catch((): void => {});
    await this.authenticate(true);
  }
  private async authenticate(interactive: boolean): Promise<void> {
    this.guard();
    if (!this.context) throw new Error('NOT_CONFIGURED');
    if (this.busy) throw new Error('BUSY');
    this.busy = true;
    this.clearSession();
    this.huaweiSignedIn = false;
    this.huaweiProfile = { nickname: '', avatarUri: '', status: '' };
    const epoch = this.generation;
    try {
      const request = new authentication.HuaweiIDProvider().createLoginWithHuaweiIDRequest();
      request.forceLogin = interactive;
      request.state = util.generateRandomUUID();
      const controller = new authentication.AuthenticationController(this.context);
      let response: authentication.LoginWithHuaweiIDResponse;
      try {
        response = await controller.executeRequest(request) as authentication.LoginWithHuaweiIDResponse;
      } catch (error) {
        const code = (error as BusinessError).code;
        // Preserve only the SDK error number; credentials never enter logs or error messages.
        throw new Error('HUAWEI_LOGIN_' + (Number.isInteger(code) ? code : 'FAILED'));
      }
      if (epoch !== this.generation) throw new Error('ACCOUNT_CHANGED');
      if (response.state !== request.state || !response.data?.openID) throw new Error('AUTH_STATE_MISMATCH');
      // SDK success describes the system identity only. Cloud access still requires a server-verified session.
      this.huaweiSignedIn = true;
      await this.loadHuaweiProfile(epoch, response.data.openID, interactive);
      if (this.config && this.capabilities.account && this.availability === 'ready') await this.connectCloud(epoch, response.data.openID, interactive);
      if (epoch === this.generation) await this.rememberLogin(true);
    } finally { this.busy = false; }
    // Official unfinished/current purchases survive reinstall; login reconciles them without a new charge.
    if (epoch === this.generation && this.getUserId() && this.capabilities.commerce && this.config?.membershipProductType === 2) {
      this.purchaseRecovery = 'CHECKING';
      try { await this.restorePurchases(); }
      catch (error) {
        if (epoch === this.generation) this.purchaseRecovery = this.recoveryFailure(error as Error);
      }
    }
  }
  private async loadHuaweiProfile(epoch: number, openID: string, interactive: boolean): Promise<void> {
    if (!this.context) return;
    try {
      const request = new authentication.HuaweiIDProvider().createAuthorizationWithHuaweiIDRequest();
      request.scopes = ['profile']; request.forceAuthorization = interactive; request.state = util.generateRandomUUID();
      const controller = new authentication.AuthenticationController(this.context);
      const response = await controller.executeRequest(request) as authentication.AuthorizationWithHuaweiIDResponse;
      if (epoch !== this.generation) throw new Error('ACCOUNT_CHANGED');
      if (response.state !== request.state || response.data?.openID && response.data.openID !== openID) throw new Error('AUTH_STATE_MISMATCH');
      const avatar = response.data?.avatarUri ?? '';
      this.huaweiProfile = { nickname: (response.data?.nickName ?? '').slice(0, 128),
        avatarUri: /^https:\/\//.test(avatar) ? avatar : '', status: 'ready' };
    } catch (_) {
      if (epoch !== this.generation) throw new Error('ACCOUNT_CHANGED');
      // Declining profile consent does not undo a successful identity login.
      this.huaweiProfile = { nickname: '', avatarUri: '', status: 'unavailable' };
    }
  }
  private async connectCloud(epoch: number, openID: string, interactive: boolean): Promise<void> {
    if (!this.context || !this.config) throw new Error('NOT_CONFIGURED');
    const attempt = await this.request<AccountAttempt>('/v1/auth/begin', http.RequestMethod.POST, '{}', false);
    const provider = new authentication.HuaweiIDProvider();
    const request = provider.createAuthorizationWithHuaweiIDRequest();
    request.forceAuthorization = interactive;
    request.scopes = ['profile'];
    request.permissions = ['serviceauthcode'];
    request.state = attempt.state;
    const controller = new authentication.AuthenticationController(this.context);
    const response = await controller.executeRequest(request) as authentication.AuthorizationWithHuaweiIDResponse;
    const code = response.data?.authorizationCode;
    if (!code || response.state !== attempt.state || response.data?.openID && response.data.openID !== openID || epoch !== this.generation) throw new Error('AUTH_STATE_MISMATCH');
    const exchange: AccountExchange = { attemptId: attempt.attemptId, state: attempt.state,
      code: code, clientId: this.config.accountClientId };
    const session = await this.request<AccountSession>('/v1/auth/exchange', http.RequestMethod.POST,
      JSON.stringify(exchange), false);
    if (epoch !== this.generation || !session.token || !session.userId || !Number.isFinite(session.expiresAt) ||
      session.expiresAt <= Date.now()) throw new Error('INVALID_SESSION');
    this.session = session;
    this.purchaseRecovery = this.config.membershipProductType === 2 && this.capabilities.commerce ? 'CHECKING' : '';
    this.accountListeners.forEach((listener: (userId: string) => void): void => listener(session.userId));

  }
  async signOut(): Promise<void> {
    this.guard();
    // Forget identity immediately, even if revocation cannot reach the service. The server also has a short TTL.
    const pending = this.session ? this.request<EmptyBody>('/v1/auth/logout', http.RequestMethod.POST, '{}') : null;
    this.clearSession();
    this.huaweiSignedIn = false;
    this.huaweiProfile = { nickname: '', avatarUri: '', status: '' };
    await this.rememberLogin(false);
    if (pending) { try { await pending; } catch (_) { } }
  }
  async entitlement(): Promise<MemberEntitlement> {
    if (this.entitlementFlight) return this.entitlementFlight;
    const work = this.readEntitlement(this.generation, this.entitlementRevision);
    this.entitlementFlight = work;
    try { return await work; }
    finally { if (this.entitlementFlight === work) this.entitlementFlight = null; }
  }
  private invalidateEntitlement(): void { this.entitlementRevision++; this.entitlementFlight = null; }
  private async readEntitlement(epoch: number, revision: number): Promise<MemberEntitlement> {
    const result = await this.request<MemberEntitlement>('/v1/entitlement', http.RequestMethod.GET, '');
    this.assertGeneration(epoch);
    if (revision !== this.entitlementRevision) return this.entitlement();
    if (typeof result.active !== 'boolean' || !Number.isFinite(result.expiresAt) || !Number.isFinite(result.serverTime)) {
      throw new Error('INVALID_ENTITLEMENT');
    }
    return result;
  }
  private storeProduct(product: iap.Product): StoreProduct {
    const display: StoreProduct = { id: product.id, title: product.name, price: product.localPrice ?? product.price,
      type: product.type, purpose: product.id === this.config?.coffeeProductId ? 'coffee' : 'membership',
      microPrice: product.microPrice, currency: product.currency };
    if (product.type !== iap.ProductType.AUTORENEWABLE) return display;
    const recurring = display.price + '／月';
    display.priceDescription = recurring + '，按月自动续费。';
    const configuredOffer = this.config?.membershipPromotionalOfferId;
    const offers = product.promotionalOffers?.filter((value: iap.PromotionalOffer): boolean => value.offerId === configuredOffer) ?? [];
    if (offers.length > 1) throw new Error('INVALID_OFFER');
    if (offers.length === 0) return display;
    const offer = offers[0];
    // This product is a monthly subscription. Unsupported or malformed discounts never become a full-price fallback.
    if (product.subscriptionInfo?.periodUnit !== iap.PeriodUnit.MONTH || product.subscriptionInfo.periodCount !== 1 ||
      offer.paymentMode !== iap.OfferPaymentMode.PAY_AS_YOU_GO || offer.periodUnit !== iap.PeriodUnit.MONTH ||
      !Number.isSafeInteger(offer.periodCount) || offer.periodCount === undefined || offer.periodCount < 1 || offer.periodCount > 12 ||
      !Number.isSafeInteger(offer.microPrice) || offer.microPrice <= 0 || !Number.isSafeInteger(product.microPrice) ||
      offer.microPrice >= product.microPrice || !offer.localPrice) throw new Error('INVALID_OFFER');
    display.regularPrice = display.price; display.price = offer.localPrice;
    display.promotion = { id: offer.offerId, price: offer.localPrice, microPrice: offer.microPrice,
      paymentMode: offer.paymentMode, periodUnit: offer.periodUnit, periodCount: offer.periodCount };
    display.priceDescription = '前 ' + offer.periodCount + ' 个月，每月 ' + offer.localPrice +
      '；之后 ' + recurring + '自动续费。';
    return display;
  }
  async products(): Promise<StoreProduct[]> {
    this.guard();
    // Official product information is public; displaying prices does not grant paid access.
    if (!this.context || !this.config) throw new Error('NOT_CONFIGURED');
    const config = this.config, epoch = this.generation;
    const result: StoreProduct[] = [];
    const configured: ProductRequest[] = [
      { productId: config.membershipProductId, productType: config.membershipProductType },
      { productId: config.coffeeProductId, productType: iap.ProductType.CONSUMABLE }
    ];
    for (const item of configured) {
      if (!item.productId) continue;
      const products = await iap.queryProducts(this.context, { productType: item.productType, productIds: [item.productId] });
      if (epoch !== this.generation) throw new Error('ACCOUNT_CHANGED');
      for (const product of products) {
        if (product.id === item.productId && product.type === item.productType) {
          result.push(this.storeProduct(product));
        }
      }
    }
    return result;
  }
  private async verifyAndFinish(data: string, type: number, epoch: number): Promise<string> {
    if (!this.context) throw new Error('NOT_CONFIGURED');
    if (epoch !== this.generation) throw new Error('ACCOUNT_CHANGED');
    const body: VerifyRequest = { purchaseData: data, productType: type };
    // Client deliberately does not decode a JWS to grant access or choose a finish token.
    const verdict = await this.request<PurchaseVerdict>('/v1/purchases/verify', http.RequestMethod.POST, JSON.stringify(body));
    this.invalidateEntitlement();
    if (!verdict.finishAllowed) return verdict.status;
    if (epoch !== this.generation) throw new Error('ACCOUNT_CHANGED');
    const allowed = this.config && ((verdict.productId === this.config.coffeeProductId && verdict.productType === 0) ||
      (verdict.productId === this.config.membershipProductId && verdict.productType === this.config.membershipProductType));
    if (!allowed || type !== verdict.productType || !verdict.purchaseToken || !verdict.purchaseOrderId) {
      throw new Error('INVALID_ORDER');
    }
    try {
      await iap.finishPurchase(this.context, { productType: verdict.productType, purchaseToken: verdict.purchaseToken,
        purchaseOrderId: verdict.purchaseOrderId });
      return 'FINISHED';
    } catch (_) {
      // Store query UNFINISHED retries this on next restore. Never grant a second time.
      return 'ACK_PENDING';
    }
  }
  private async checkPurchaseEnvironment(): Promise<void> {
    if (this.capabilities.sandboxCommerce === true &&
      (!this.context || !await iap.isSandboxActivated(this.context))) throw new Error('SANDBOX_REQUIRED');
  }
  async purchase(product: StoreProduct): Promise<string> {
    this.guard();
    if (!this.context || !this.config || !this.capabilities.commerce) throw new Error('NOT_CONFIGURED');
    await this.checkPurchaseEnvironment();
    if (this.busy) throw new Error('BUSY');
    if (!this.validSession()) throw new Error('SIGN_IN_REQUIRED');
    const allowed = (product.id === this.config.coffeeProductId && product.type === 0) ||
      (product.id === this.config.membershipProductId && product.type === this.config.membershipProductType);
    if (!allowed) throw new Error('UNKNOWN_PRODUCT');
    const epoch = this.generation;
    this.busy = true;
    try {
      if (product.type === iap.ProductType.AUTORENEWABLE) {
        // A cancelled renewal retains the paid current term. Never create another purchase
        // for an active member, even if the UI has an older entitlement snapshot.
        const current = await this.entitlement();
        if (epoch !== this.generation) throw new Error('ACCOUNT_CHANGED');
        if (current.active && !current.refreshRequired && current.expiresAt > current.serverTime) {
          this.purchaseRecovery = 'RESTORED';
          this.entitlementChanged();
          return 'ALREADY_ACTIVE';
        }
      }
      let selected: StoreProduct = product;
      if (product.type === iap.ProductType.AUTORENEWABLE) {
        const fresh = await iap.queryProducts(this.context, { productType: product.type, productIds: [product.id] });
        const official = fresh.find((value: iap.Product): boolean => value.id === product.id && value.type === product.type);
        if (!official) throw new Error('PRODUCT_MISSING');
        selected = this.storeProduct(official);
        if ((selected.promotion?.id ?? '') !== (product.promotion?.id ?? '') ||
          (product.microPrice !== undefined && selected.microPrice !== product.microPrice) ||
          (product.currency !== undefined && selected.currency !== product.currency) ||
          (product.promotion && (selected.promotion?.microPrice !== product.promotion.microPrice ||
            selected.promotion.periodCount !== product.promotion.periodCount))) throw new Error('PRICE_CHANGED');
        if (epoch !== this.generation) throw new Error('ACCOUNT_CHANGED');
      }
      const body: ProductRequest = { productId: product.id, productType: product.type,
        promotionalOfferId: selected.promotion?.id };
      const intent = await this.request<PurchaseIntent>('/v1/purchases/intent', http.RequestMethod.POST, JSON.stringify(body));
      if (epoch !== this.generation) throw new Error('ACCOUNT_CHANGED');
      const parameter: iap.PurchaseParameter = { productId: product.id, productType: product.type,
        developerPayload: intent.developerPayload };
      if (selected.promotion) {
        const signed = intent.promotionalPurchase;
        if (!signed || signed.promotionalOfferId !== selected.promotion.id || signed.applicationUserName !== this.getUserId() ||
          typeof signed.jwsRepresentation !== 'string' || signed.jwsRepresentation.length > 16384 ||
          !/^[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+$/.test(signed.jwsRepresentation)) throw new Error('INVALID_OFFER');
        parameter.promotionalOfferId = signed.promotionalOfferId;
        parameter.applicationUserName = signed.applicationUserName;
        parameter.jwsRepresentation = signed.jwsRepresentation;
      } else if (intent.promotionalPurchase) throw new Error('INVALID_OFFER');
      let result: iap.CreatePurchaseResult;
      try { result = await iap.createPurchase(this.context, parameter); }
      catch (error) {
        if ((error as BusinessError).code === iap.IAPErrorCode.USER_CANCELED) return 'CANCELLED';
        // The store retains already-owned and unfinished orders; do not ask for another payment.
        if ((error as BusinessError).code === iap.IAPErrorCode.PRODUCT_OWNED) {
          this.busy = false;
          return await this.restorePurchases();
        }
        throw error as Error;
      }
      this.purchaseRecovery = 'CHECKING';
      let verdict: string;
      try { verdict = await this.verifyAndFinish(result.purchaseData, product.type, epoch); }
      catch (error) {
        if (epoch !== this.generation) throw new Error('ACCOUNT_CHANGED');
        this.purchaseRecovery = this.recoveryFailure(error as Error);
        return 'VERIFICATION_PENDING';
      }
      if (epoch !== this.generation) throw new Error('ACCOUNT_CHANGED');
      this.purchaseRecovery = verdict;
      this.entitlementChanged();
      return verdict;
    } finally { this.busy = false; }
  }
  async manageSubscriptions(): Promise<void> {
    this.guard();
    if (!this.context) throw new Error('NOT_CONFIGURED');
    await iap.showManagedSubscriptions(this.context, { windowScreenMode: iap.WindowScreenMode.DIALOG_BOX });
  }
  async restorePurchases(): Promise<string> {
    if (this.restoring) return this.restoring;
    const work = this.reconcilePurchases();
    this.restoring = work;
    try { return await work; }
    finally { if (this.restoring === work) this.restoring = null; }
  }
  private async reconcilePurchases(): Promise<string> {
    this.guard();
    if (!this.context || !this.config || !this.capabilities.commerce) throw new Error('NOT_CONFIGURED');
    await this.checkPurchaseEnvironment();
    if (this.busy) throw new Error('BUSY');
    if (!this.validSession()) throw new Error('SIGN_IN_REQUIRED');
    this.busy = true;
    const epoch = this.generation;
    let pending = false;
    let failure: Error | null = null;
    try {
      // Coffee has its own independent recovery flow and must not block membership restoration.
      const types: number[] = [this.config.membershipProductType];
      for (const type of types) {
        const queries: number[] = type === 0 ? [iap.PurchaseQueryType.UNFINISHED] :
          [iap.PurchaseQueryType.UNFINISHED, type === iap.ProductType.AUTORENEWABLE ?
            iap.PurchaseQueryType.CURRENT_ENTITLEMENT : iap.PurchaseQueryType.ALL];
        for (const query of queries) {
          let cursor = '';
          const cursors = new Set<string>();
          do {
            if (epoch !== this.generation) throw new Error('ACCOUNT_CHANGED');
            const response = await iap.queryPurchases(this.context, { productType: type, queryType: query,
              continuationToken: cursor || undefined });
            for (const receipt of response.purchaseDataList) {
              try {
                const status = await this.verifyAndFinish(receipt, type, epoch);
                if (status === 'ACK_PENDING') pending = true;
              } catch (error) {
                this.assertGeneration(epoch);
                // One obsolete/failed receipt must not prevent verification of a later current subscription.
                if (!failure) failure = error as Error;
              }
            }
            cursor = response.continuationToken ?? '';
            if (cursor && cursors.has(cursor)) throw new Error('INVALID_PAGINATION');
            cursors.add(cursor);
          } while (cursor);
        }
      }
      if (epoch !== this.generation) throw new Error('ACCOUNT_CHANGED');
      if (failure) throw failure;
      this.purchaseRecovery = pending ? 'ACK_PENDING' : 'RESTORED';
      this.purchaseRecoveryCheckedAt = Date.now();
      this.entitlementChanged();
      return this.purchaseRecovery;
    } catch (error) {
      if (epoch === this.generation) { this.purchaseRecovery = this.recoveryFailure(error as Error); this.entitlementChanged(); }
      throw error as Error;
    } finally { this.busy = false; }
  }
  private profilePath(id: string): string {
    this.guard();
    return '/v1/profiles/' + profileDeviceClass(DeviceCapabilityPolicy.deviceType()) + '/' + encodeURIComponent(id);
  }
  private assertGeneration(epoch: number): void {
    if (epoch !== this.generation || !this.validSession()) throw new Error('ACCOUNT_CHANGED');
  }
  private async verifyEnvelope(envelope: ProfileEnvelope, id: string): Promise<void> {
    validateProfileEnvelope(envelope, profileDeviceClass(DeviceCapabilityPolicy.deviceType()), id);
    const md = cryptoFramework.createMd('SHA256');
    await md.update({ data: new util.TextEncoder().encodeInto(JSON.stringify(envelope.payload)) });
    const digest = await md.digest();
    const hash = Array.from(digest.data).map((value: number): string => value.toString(16).padStart(2, '0')).join('');
    if (hash !== envelope.payloadSha256) throw new Error('PROFILE_HASH_MISMATCH');
  }
  async listCloudProfiles(): Promise<ProfileSummary[]> {
    if (!this.capabilities.profiles) throw new Error('NOT_CONFIGURED');
    const epoch = this.generation;
    const result = await this.request<ProfileIndex>('/v1/profiles/' + profileDeviceClass(DeviceCapabilityPolicy.deviceType()),
      http.RequestMethod.GET, '');
    if (!Array.isArray(result.profiles) || result.profiles.length > 256) throw new Error('INVALID_PROFILE');
    for (const profile of result.profiles) {
      if (!/^[a-zA-Z0-9_-]{1,128}$/.test(profile.variant) || typeof profile.name !== 'string' || profile.name.length > 128 ||
        !Number.isSafeInteger(profile.revision) || profile.revision < 1 ||
        profile.deviceClass !== profileDeviceClass(DeviceCapabilityPolicy.deviceType())) throw new Error('INVALID_PROFILE');
    }
    this.assertGeneration(epoch);
    return result.profiles;
  }
  private async saveLocalBranch(profile: InputProfile, suffix: string): Promise<void> {
    if (!this.context) throw new Error('NOT_CONFIGURED');
    const directory = this.context.filesDir + '/profile_sync_backups';
    if (!fs.accessSync(directory)) fs.mkdirSync(directory);
    const file = fs.openSync(directory + '/' + util.generateRandomUUID() + '-' + suffix + '.json',
      fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.NOFOLLOW);
    try {
      const bytes = new util.TextEncoder().encodeInto(JSON.stringify(cloudProfilePayload(profile)));
      let written = 0;
      while (written < bytes.length) {
        const count = fs.writeSync(file.fd, bytes.buffer.slice(written));
        if (count <= 0) throw new Error('BACKUP_WRITE_FAILED');
        written += count;
      }
      fs.fsyncSync(file.fd);
    }
    finally { fs.closeSync(file); }
  }
  async uploadProfile(profile: InputProfile, resolveConflict: boolean = false): Promise<string> {
    this.guard();
    if (!this.capabilities.profiles) throw new Error('NOT_CONFIGURED');
    const epoch = this.generation;
    const payload = cloudProfilePayload(profile);
    const conflict = this.conflicts.get(profile.id);
    if (conflict && !resolveConflict) return 'CONFLICT';
    const base = resolveConflict && conflict ? conflict.remote.revision : this.profileHeads.get(profile.id) ?? 0;
    const body: ProfilePut = { baseRevision: base, payload: payload };
    const result = await this.request<ProfilePutResult>(this.profilePath(profile.id), http.RequestMethod.PUT, JSON.stringify(body));
    await this.verifyEnvelope(result.current, profile.id);
    this.assertGeneration(epoch);
    if (result.status === 'CONFLICT') {
      await this.saveLocalBranch(payload, 'local');
      this.assertGeneration(epoch);
      await this.saveLocalBranch(result.current.payload, 'remote');
      this.assertGeneration(epoch);
      const local: ProfileEnvelope = { schemaVersion: 1, profileFamily: 'input/native-v6',
        deviceClass: result.current.deviceClass, variant: profile.id, revision: 0, baseRevision: base,
        payloadSha256: '', payload: payload };
      this.conflicts.set(profile.id, { local: local, remote: result.current });
      return 'CONFLICT';
    }
    if (result.status !== 'SAVED') throw new Error('INVALID_PROFILE');
    this.profileHeads.set(profile.id, result.current.revision);
    this.conflicts.delete(profile.id);
    return 'SAVED';
  }
  async readCloudProfile(id: string): Promise<ProfileEnvelope> {
    this.guard();
    if (!this.capabilities.profiles) throw new Error('NOT_CONFIGURED');
    const epoch = this.generation;
    const value = await this.request<ProfileEnvelope>(this.profilePath(id), http.RequestMethod.GET, '');
    await this.verifyEnvelope(value, id); this.assertGeneration(epoch); return value;
  }
  profileRevision(id: string): number { this.guard(); return this.profileHeads.get(id) ?? 0; }
  setProfileBaseRevision(id: string, revision: number): void {
    this.guard();
    if (!this.validSession() || !/^[a-zA-Z0-9_-]{1,128}$/.test(id) || !Number.isSafeInteger(revision) || revision < 0) throw new Error('INVALID_PROFILE');
    this.profileHeads.set(id, revision);
  }
  private async verifyPreferences(value: PreferencesEnvelope): Promise<void> {
    if (value.schemaVersion !== 1 || value.deviceClass !== profileDeviceClass(DeviceCapabilityPolicy.deviceType()) ||
      !Number.isSafeInteger(value.revision) || value.revision < 1 || !/^[a-f0-9]{64}$/.test(value.payloadSha256)) throw new Error('INVALID_PREFERENCES');
    validateSyncedPreferences(value.payload);
    if (await this.payloadHash(JSON.stringify(value.payload)) !== value.payloadSha256) throw new Error('PROFILE_HASH_MISMATCH');
  }
  async payloadHash(json: string): Promise<string> {
    const md = cryptoFramework.createMd('SHA256');
    await md.update({ data: new util.TextEncoder().encodeInto(json) });
    const result = await md.digest();
    return Array.from(result.data).map((byte: number): string => byte.toString(16).padStart(2, '0')).join('');
  }
  async readPreferences(): Promise<PreferencesEnvelope | null> {
    this.guard(); if (!this.capabilities.preferences) throw new Error('NOT_CONFIGURED');
    const epoch = this.generation;
    const result = await this.request<PreferencesRead>('/v1/settings/' + profileDeviceClass(DeviceCapabilityPolicy.deviceType()), http.RequestMethod.GET, '');
    if (result.current) await this.verifyPreferences(result.current);
    this.assertGeneration(epoch); return result.current;
  }
  async uploadPreferences(payload: SyncedPreferences, baseRevision: number): Promise<PreferencesWrite> {
    this.guard(); if (!this.capabilities.preferences) throw new Error('NOT_CONFIGURED'); validateSyncedPreferences(payload);
    const epoch = this.generation;
    const result = await this.request<PreferencesWrite>('/v1/settings/' + profileDeviceClass(DeviceCapabilityPolicy.deviceType()), http.RequestMethod.PUT,
      JSON.stringify({ baseRevision: baseRevision, payload: payload }));
    if (result.status !== 'SAVED' && result.status !== 'CONFLICT') throw new Error('INVALID_PREFERENCES');
    await this.verifyPreferences(result.current); this.assertGeneration(epoch); return result;
  }
  async downloadProfile(id: string, copy: boolean = false, expectedLocal: string = '', shouldApply: () => boolean = (): boolean => true): Promise<ProfileEnvelope> {
    this.guard();
    if (!this.capabilities.profiles) throw new Error('NOT_CONFIGURED');
    const epoch = this.generation;
    const remote = await this.request<ProfileEnvelope>(this.profilePath(id), http.RequestMethod.GET, '');
    await this.verifyEnvelope(remote, id);
    this.assertGeneration(epoch);
    const profiles = InputProfileService.getInstance();
    if (profiles.hasProfile(id)) await this.saveLocalBranch(profiles.getProfile(id), 'before-restore');
    this.assertGeneration(epoch);
    const payload = cloudProfilePayload(remote.payload);
    if (copy) { payload.id = 'cloud-' + util.generateRandomUUID(); payload.name += '（云端副本）'; }
    if (!shouldApply()) throw new Error('ACCOUNT_CHANGED');
    if (expectedLocal && (expectedLocal === '__absent__' ? profiles.hasProfile(id) :
      !profiles.hasProfile(id) || JSON.stringify(cloudProfilePayload(profiles.getProfile(id))) !== expectedLocal)) throw new Error('PROFILE_LOCAL_CHANGED');
    await profiles.saveProfile(payload, 'cloud');
    this.assertGeneration(epoch);
    if (!copy) { this.profileHeads.set(id, remote.revision); this.conflicts.delete(id); }
    return remote;
  }
}
