import { common } from '@kit.AbilityKit';
import preferences from '@ohos.data.preferences';
import { DeviceCapabilityPolicy } from '../../common/DeviceCapabilityPolicy';
import { InputProfile, createBuiltInInputProfiles, inputTemplateForProfileId } from '../../model/AppModels';
import { ProfileSummary, cloudProfilePayload } from '../../model/ValueAddedModels';
import { AutomaticSyncData, SyncAccountState, SyncBaseline, SyncItemStatus, PreferencesEnvelope,
  ProfileOwner, SyncedPreferences, matchesShippedInputTemplate, preferencesPayload, validateSyncedPreferences, validateAutomaticSyncData } from '../../model/AccountSyncModels';
import { ValueAddedService } from './ValueAddedService';
import { InputProfileService } from '../InputProfileService';
import { ThemeSettingsService } from '../ThemeSettingsService';
import { AppSettingsStore } from '../AppSettingsStore';
import { UserConfigurationRepository } from '../UserConfigurationRepository';
import { ConfigurationCloudSyncService } from './ConfigurationCloudSyncService';

export type AutomaticSyncListener = () => void;
/** One foreground queue. Account ownership and CAS baselines are persisted; credentials never are. */
export class AccountAutomaticSyncService {
  private static instance: AccountAutomaticSyncService | null = null;
  private context: common.UIAbilityContext | null = null;
  private store: preferences.Preferences | null = null;
  private data: AutomaticSyncData = { schemaVersion: 1, lastOwner: '', preferencesOwner: '', profileOwners: [], accounts: [] };
  private listeners: Set<AutomaticSyncListener> = new Set<AutomaticSyncListener>();
  private items: SyncItemStatus[] = [];
  private overall: string = '等待登录';
  private foreground: boolean = true;
  private busy: boolean = false;
  private preparing: boolean = false;
  private applying: boolean = false;
  private observedPreferences: string = '';
  private localRevision: number = 0;
  private epoch: number = 0;
  private timer: number = -1;
  private rerun: boolean = false;
  private reconcilingPurchases: boolean = false;
  private retryMs: number = 30000;
  private preferenceConflict: PreferencesEnvelope | null = null;
  private configurationDecision: string = '';
  private configurationCheckedOwner: string = '';
  private configurationCheckedRevision: number = -1;
  private forceConfigurationCheck: boolean = false;
  private persistedState: string = '';
  configurationEnabled(): boolean { return this.profilesEnabled() && this.preferencesEnabled(); }
  configurationConflict(): boolean { return ConfigurationCloudSyncService.getInstance().hasConflict(); }
  configurationBytes(): number { return ConfigurationCloudSyncService.getInstance().usedBytes(); }
  async resolveConfiguration(useCloud: boolean): Promise<void> {
    this.configurationDecision = useCloud ? 'cloud' : 'local'; await this.syncNow();
  }
  static getInstance(): AccountAutomaticSyncService {
    if (!AccountAutomaticSyncService.instance) AccountAutomaticSyncService.instance = new AccountAutomaticSyncService();
    return AccountAutomaticSyncService.instance;
  }
  subscribe(listener: AutomaticSyncListener): void { this.listeners.add(listener); }
  unsubscribe(listener: AutomaticSyncListener): void { this.listeners.delete(listener); }
  status(): string { return this.overall; }
  itemStates(): SyncItemStatus[] { return this.items.slice(); }
  hasPreferencesConflict(): boolean { return !!this.preferenceConflict; }
  profilesEnabled(): boolean { return this.currentAccount()?.profilesEnabled ?? true; }
  preferencesEnabled(): boolean { return this.currentAccount()?.preferencesEnabled ?? true; }
  lastSyncedAt(): number { return this.currentAccount()?.lastSyncedAt ?? 0; }
  private emit(): void { this.listeners.forEach((listener: AutomaticSyncListener): void => listener()); }
  private api(): ValueAddedService { return ValueAddedService.getInstance(); }
  private currentAccount(): SyncAccountState | undefined {
    const id = DeviceCapabilityPolicy.supportsValueAddedFeatures() ? this.api().getUserId() : '';
    return this.data.accounts.find((account: SyncAccountState): boolean => account.userId === id);
  }
  async initialize(context: common.UIAbilityContext): Promise<void> {
    if (!DeviceCapabilityPolicy.supportsValueAddedFeatures() || this.context) return;
    this.context = context;
    try {
      this.store = await preferences.getPreferences(context, 'account_automatic_sync');
      UserConfigurationRepository.getInstance().subscribe((): void => { this.localChanged('configuration', '应用配置', 'configuration'); });
      const raw = this.store.getSync('state_v1', '') as string;
      if (raw) {
        if (raw.length > 524288) throw new Error('INVALID_SYNC_STATE');
        const value = JSON.parse(raw) as AutomaticSyncData;
        validateAutomaticSyncData(value);
        this.data = value;
      }
      this.persistedState = raw;
      await ConfigurationCloudSyncService.getInstance().initialize(context, this.data.lastOwner);
      InputProfileService.getInstance().subscribeChanges((id: string, removed: boolean, source: string): void => {
        if (source === 'cloud' || this.api().getCapabilities().configuration) return;
        const owner = this.api().getUserId() || this.data.lastOwner;
        if (owner) {
          this.ownProfile(id, owner);
          const account = this.data.accounts.find((entry: SyncAccountState): boolean => entry.userId === owner);
          if (account) {
            account.removedProfiles = account.removedProfiles.filter((value: string): boolean => value !== id);
            if (removed) account.removedProfiles.push(id);
          }
          void this.persist().catch((): void => { this.overall = '同步记录保存失败'; this.emit(); });
        }
        this.localChanged(id, removed ? id : InputProfileService.getInstance().getProfile(id).name, 'profiles');
      });
      this.observedPreferences = JSON.stringify(preferencesPayload(ThemeSettingsService.getInstance().getSettings()));
      AppSettingsStore.getInstance().subscribeGlobalChanges((): void => {
        if (this.api().getCapabilities().configuration) return;
        const current = JSON.stringify(preferencesPayload(ThemeSettingsService.getInstance().getSettings()));
        if (current === this.observedPreferences) return;
        this.observedPreferences = current;
        if (this.applying) return;
        this.data.preferencesOwner = this.api().getUserId() || this.data.lastOwner;
        void this.persist().catch((): void => { this.overall = '同步记录保存失败'; this.emit(); });
        this.localChanged('preferences', '外观与默认输入方案', 'preferences');
      });
      this.api().subscribeAccount((userId: string): void => {
        void this.accountChanged(userId).catch((): void => { this.overall = '同步记录保存失败'; this.emit(); });
      });
      this.api().subscribeEntitlement((): void => {
        if (!this.reconcilingPurchases) this.requestSync(0, true);
      });
      await this.api().initialize(context);
      await this.accountChanged(this.api().getUserId());
    } catch (error) {
      this.overall = (error as Error).message === 'NOT_CONFIGURED' ? '云同步暂未接通' : '同步暂不可用，请稍后重试'; this.emit();
    }
  }
  private ownProfile(id: string, userId: string): void {
    this.data.profileOwners = this.data.profileOwners.filter((owner: ProfileOwner): boolean => owner.id !== id);
    if (this.data.profileOwners.length >= 2048) throw new Error('SYNC_RECORD_LIMIT');
    this.data.profileOwners.push({ id: id, userId: userId });
  }
  private async persist(): Promise<void> {
    if (!this.store) throw new Error('SYNC_STORAGE_UNAVAILABLE');
    validateAutomaticSyncData(this.data);
    const raw = JSON.stringify(this.data);
    if (raw.length > 524288) throw new Error('SYNC_RECORD_LIMIT');
    if (raw === this.persistedState) return;
    await this.store.put('state_v1', raw); await this.store.flush();
    this.persistedState = raw;
  }
  private async accountChanged(userId: string): Promise<void> {
    this.configurationCheckedOwner = ''; this.configurationCheckedRevision = -1;
    this.configurationDecision = '';
    this.epoch++; const epoch = this.epoch; this.clearTimer(); this.items = []; this.preferenceConflict = null;
    if (!userId) {
      this.overall = this.api().hasRememberedAccount() ? '正在恢复登录，稍后自动重试' : '等待登录'; this.emit();
      this.requestSync(this.retryMs); return;
    }
    if (!/^[a-f0-9]{64}$/.test(userId)) { this.overall = '账号身份无效'; this.emit(); return; }
    let account = this.data.accounts.find((entry: SyncAccountState): boolean => entry.userId === userId);
    if (!account) {
      if (this.data.accounts.length >= 8) { this.overall = '此设备保存的账号同步记录已满'; this.emit(); return; }
      account = { userId: userId, profilesEnabled: true, preferencesEnabled: true, profiles: [], preferences: null, removedProfiles: [], lastSyncedAt: 0 };
      this.data.accounts.push(account);
    }
    for (const profile of InputProfileService.getInstance().getProfiles()) {
      if (!this.data.profileOwners.some((owner: ProfileOwner): boolean => owner.id === profile.id)) this.ownProfile(profile.id, this.data.lastOwner || userId);
    }
    if (!this.data.preferencesOwner) this.data.preferencesOwner = this.data.lastOwner || userId;
    this.data.lastOwner = userId;
    try { await this.persist(); } catch (_) { this.overall = '同步记录保存失败'; this.emit(); return; }
    if (epoch !== this.epoch || this.api().getUserId() !== userId) return;
    this.retryMs = 30000; this.overall = '等待同步'; this.emit(); this.requestSync(0);
  }
  setForeground(value: boolean): void {
    this.foreground = value;
    if (!value) { this.epoch++; this.clearTimer(); this.overall = this.api().getUserId() ? '返回应用后继续同步' : '等待登录'; this.emit(); }
    else if (this.context) this.requestSync(0, true);
  }
  async setEnabled(family: string, value: boolean): Promise<void> {
    const account = this.currentAccount(); if (!account) throw new Error('SIGN_IN_REQUIRED');
    this.epoch++;
    if (family === 'profiles') account.profilesEnabled = value;
    else if (family === 'configuration') { account.profilesEnabled = value; account.preferencesEnabled = value; }
    else if (family === 'preferences') account.preferencesEnabled = value;
    else throw new Error('INVALID_SYNC_FAMILY');
    await this.persist(); this.emit(); this.requestSync(0, true);
  }
  private clearTimer(): void { if (this.timer >= 0) clearTimeout(this.timer); this.timer = -1; }
  private localChanged(id: string, name: string, family: string): void {
    this.localRevision++;
    const account = this.currentAccount();
    const enabled = family === 'profiles' ? account?.profilesEnabled : account?.preferencesEnabled;
    if (account && enabled) {
      this.overall = '检测到本地修改，等待备份';
      this.state(id, name, this.foreground ? '等待备份' : '返回应用后继续备份');
    }
    this.requestSync();
  }
  requestSync(delay: number = 700, force: boolean = false): void {
    if (force) this.forceConfigurationCheck = true;
    if (!this.context || !this.foreground || !this.api().getUserId() && !this.api().hasRememberedAccount()) return;
    if (this.busy || this.preparing) { this.rerun = true; return; }
    this.clearTimer(); this.timer = setTimeout((): void => { this.timer = -1; void this.syncNow(); }, delay);
  }
  private valid(epoch: number, owner: string): boolean {
    return this.foreground && this.epoch === epoch && this.api().getUserId() === owner;
  }
  private check(epoch: number, owner: string): void { if (!this.valid(epoch, owner)) throw new Error('ACCOUNT_CHANGED'); }
  private state(id: string, name: string, state: string, lastSyncedAt: number = 0): void {
    this.items = this.items.filter((item: SyncItemStatus): boolean => item.id !== id);
    this.items.push({ id: id, name: name, state: state, lastSyncedAt: lastSyncedAt }); this.emit();
  }
  private base(account: SyncAccountState, id: string, revision: number, hash: string): void {
    account.profiles = account.profiles.filter((value: SyncBaseline): boolean => value.id !== id);
    if (account.profiles.length >= 256) throw new Error('SYNC_RECORD_LIMIT');
    account.profiles.push({ id: id, revision: revision, hash: hash });
  }
  async syncNow(): Promise<void> {
    if (!this.context || !this.foreground || this.busy || this.preparing || !DeviceCapabilityPolicy.supportsValueAddedFeatures()) return;
    if (this.api().getCapabilities().configuration && this.currentAccount() && !this.configurationEnabled()) {
      this.overall = '自动备份已关闭'; this.emit(); return;
    }
    // Idle configuration has no timer and no cloud read, entitlement call, journal write or upload.
    if (!this.forceConfigurationCheck && !this.configurationDecision &&
      this.configurationCheckedOwner === this.api().getUserId() &&
      this.configurationCheckedRevision === (this.api().getCapabilities().configuration ?
        UserConfigurationRepository.getInstance().revision() : this.localRevision)) return;
    this.forceConfigurationCheck = false;
    // Renew the short-lived server session with an already authorized system identity, including after process restart.
    this.preparing = true;
    try { await this.api().resumeAccount(); }
    catch (_) {
      this.preparing = false; this.retryMs = Math.min(300000, this.retryMs * 2);
      this.overall = '登录连接暂不可用，等待自动重试'; this.emit(); this.requestSync(this.retryMs, true); return;
    } finally { this.preparing = false; }
    if (!this.foreground || this.busy) return;
    const account = this.currentAccount();
    if (!account) {
      this.overall = this.api().hasRememberedAccount() ? '正在恢复登录，稍后自动重试' : '等待登录'; this.emit();
      if (this.api().hasRememberedAccount()) this.requestSync(this.retryMs);
      return;
    }
    this.busy = true; this.rerun = false;
    const epoch = this.epoch, owner = account.userId, localRevision = this.localRevision;
    let retry: boolean = false;
    this.overall = '正在同步'; this.emit();
    try {
      const api = this.api();
      await api.refreshCapabilities(); this.check(epoch, owner);
      const capabilities = api.getCapabilities();
      const recovery = api.getPurchaseRecoveryStatus();
      if (capabilities.commerce && recovery && (!['RESTORED', 'FINISHED', 'ALREADY_FINISHED'].includes(recovery) ||
        api.getPurchaseRecoveryCheckedAt() + 60000 <= Date.now())) {
        this.reconcilingPurchases = true;
        try { await api.restorePurchases(); }
        catch (_) { /* The independently verified server entitlement remains authoritative, even if an old receipt cannot finish. */ }
        finally { this.reconcilingPurchases = false; }
        this.check(epoch, owner);
      }
      this.items = [];
      if (!account.profilesEnabled && !account.preferencesEnabled) { this.overall = '自动同步已关闭'; return; }
      if (!account.profilesEnabled) this.state('profiles-status', '输入方案', '自动同步已关闭');
      else if (!capabilities.profiles) this.state('profiles-status', '输入方案', '云同步暂未开放');
      if (!account.preferencesEnabled) this.state('preferences', '外观与默认输入方案', '自动同步已关闭');
      else if (!capabilities.preferences) this.state('preferences', '外观与默认输入方案', '云同步暂未开放');
      const available = account.profilesEnabled && capabilities.profiles || account.preferencesEnabled && capabilities.preferences;
      if (!available) { this.overall = '已开启的云同步暂未开放'; return; }
      const entitlement = await api.entitlement(); this.check(epoch, owner);
      if (!entitlement.active || entitlement.refreshRequired || entitlement.expiresAt <= entitlement.serverTime) {
        this.overall = entitlement.refreshRequired ? '会员权益待确认，同步已暂停' :
          entitlement.subscriptionStatus === '2' ? '会员已到期，云端备份已暂停' :
          entitlement.subscriptionStatus === '5' ? '订阅已撤销，云端备份已暂停' :
          entitlement.billingRetry ? '续费未完成，云端备份已暂停' : '开通 VIP 后自动备份';
        return;
      }
      if (capabilities.configuration) {
        if (!this.configurationEnabled()) { this.overall = '自动备份已关闭'; return; }
        this.applying = true;
        try {
          this.overall = '正在备份应用配置'; this.emit();
          const unified = ConfigurationCloudSyncService.getInstance();
          this.overall = await unified.sync((): void => this.check(epoch, owner), this.configurationDecision, async (): Promise<void> => {
            // Migrate the old cloud backup before the first complete head can replace it.
            await this.syncProfiles(account, epoch); await this.syncPreferences(account, epoch);
          });
          this.configurationDecision = ''; this.items = [{ id: 'configuration', name: '应用配置', state: this.overall, lastSyncedAt: unified.lastSyncedAt() }];
          account.lastSyncedAt = unified.lastSyncedAt(); await this.persist();
          if (this.overall === '云端备份已更新' || this.overall === '已恢复应用配置') {
            this.configurationCheckedOwner = owner;
            this.configurationCheckedRevision = UserConfigurationRepository.getInstance().revision();
            this.retryMs = 30000;
          }
          retry = this.overall === '检测到本地修改，等待备份';
        } finally { this.applying = false; }
        return;
      }
      if (account.profilesEnabled && capabilities.profiles) await this.syncProfiles(account, epoch);
      if (account.preferencesEnabled && capabilities.preferences) await this.syncPreferences(account, epoch);
      this.check(epoch, owner);
      if (!capabilities.profiles && !capabilities.preferences) { this.overall = '云同步暂未开放'; return; }
      const conflict = this.items.some((item: SyncItemStatus): boolean => item.state === '有冲突，双方版本已保留');
      const held = this.items.some((item: SyncItemStatus): boolean => item.state === '属于其他账号，未上传');
      const waiting = this.items.some((item: SyncItemStatus): boolean => item.state.startsWith('等待'));
      const unavailable = this.items.some((item: SyncItemStatus): boolean => item.state === '云同步暂未开放');
      this.overall = localRevision !== this.localRevision ? '检测到本地修改，等待备份' :
        conflict ? '有冲突，等待处理' : waiting ? '部分内容等待同步' : held ? '其他账号的本地内容未上传' :
        unavailable ? '部分云同步暂未开放' : this.items.some((item: SyncItemStatus): boolean => item.state === '已恢复') ? '已恢复云端备份' : '已同步';
      if (localRevision === this.localRevision && !conflict && !waiting && !held && !unavailable) account.lastSyncedAt = Date.now();
      await this.persist(); this.retryMs = 30000;
      if (localRevision === this.localRevision && !conflict && !waiting && !held && !unavailable) {
        this.configurationCheckedOwner = owner; this.configurationCheckedRevision = localRevision;
      }
    } catch (error) {
      if (this.valid(epoch, owner)) {
        const message = (error as Error).message;
        console.warn('[AccountAutomaticSync] sync failed: ' + (/^[A-Z][A-Z0-9_]{1,63}$/.test(message) ? message : 'FAILED'));
        this.overall = message === 'SIGN_IN_REQUIRED' ? '登录已过期，等待重新登录' :
          message === 'CONFIG_REQUIRES_UPDATE' ? '此配置需要更新应用后恢复，原备份已保留' :
          message === 'CONFIG_LOCAL_CHANGED' ? '检测到新修改，等待备份' :
          message === 'CONFIG_QUOTA' ? '配置超出备份容量，本地与原云端备份已保留' :
          message === 'CONFIG_HASH_MISMATCH' || message === 'CONFIG_INCOMPLETE' || message === 'CONFIG_INVALID' ? '配置校验失败，原内容已保留' :
          message === 'CONFIG_RECONCILIATION_REQUIRED' ? '上传尚待云端确认，原备份已保留' :
          message === 'MEMBERSHIP_REQUIRED' ? '会员权益待确认，同步已暂停' :
          message === 'PROFILE_HASH_MISMATCH' || message === 'INVALID_PREFERENCES' || message === 'INVALID_PROFILE' ? '云端校验失败，本地内容已保留' :
          message === 'PROFILE_LOCAL_CHANGED' ? '检测到新修改，等待同步' :
          message === 'SYNC_RECORD_LIMIT' || message === 'INVALID_SYNC_STATE' ? '同步记录已满或异常，同步已暂停' : '连接暂不可用，等待自动重试';
        this.items.forEach((item: SyncItemStatus): void => {
          if (item.state === '正在同步' || item.state === '正在检查') item.state = '等待自动重试';
        });
        this.retryMs = Math.min(300000, this.retryMs * 2);
        retry = !['CONFIG_REQUIRES_UPDATE', 'CONFIG_QUOTA', 'CONFIG_INVALID', 'CONFIG_HASH_MISMATCH', 'CONFIG_INCOMPLETE',
          'MEMBERSHIP_REQUIRED', 'SYNC_RECORD_LIMIT', 'INVALID_SYNC_STATE'].includes(message);
        if (retry) this.forceConfigurationCheck = true;
      }
    } finally {
      this.busy = false; this.emit();
      if (this.foreground) {
        if (this.rerun || retry) {
          this.requestSync(this.rerun ? 700 : this.retryMs);
        }
      }
    }
  }
  private async syncProfiles(account: SyncAccountState, epoch: number): Promise<void> {
    const api = this.api(), profiles = InputProfileService.getInstance(), owner = account.userId;
    this.state('profiles-status', '输入方案', '正在检查');
    const index = await api.listCloudProfiles(); this.check(epoch, owner);
    this.items = this.items.filter((item: SyncItemStatus): boolean => item.id !== 'profiles-status');
    const ids = new Set<string>(); profiles.getProfiles().forEach((profile: InputProfile): void => { ids.add(profile.id); });
    index.forEach((profile: ProfileSummary): void => { ids.add(profile.variant); });
    if (ids.size > 256) throw new Error('SYNC_RECORD_LIMIT');
    const shipped = createBuiltInInputProfiles();
    for (const id of ids) {
      this.check(epoch, owner);
      if (account.removedProfiles.indexOf(id) >= 0) { this.state(id, id, '已在本机移除，云端保留'); continue; }
      const local = profiles.hasProfile(id) ? profiles.getProfile(id) : null;
      const name = local?.name ?? index.find((item: ProfileSummary): boolean => item.variant === id)?.name ?? id;
      this.state(id, name, '正在同步');
      const serialized = local ? JSON.stringify(cloudProfilePayload(local)) : '__absent__';
      const hash = local ? await api.payloadHash(serialized) : '';
      this.check(epoch, owner);
      const baseline = account.profiles.find((base: SyncBaseline): boolean => base.id === id);
      const remoteIndex = index.find((item: ProfileSummary): boolean => item.variant === id);
      const own = this.data.profileOwners.find((item: ProfileOwner): boolean => item.id === id)?.userId === owner;
      if (api.getConflict(id)) { this.state(id, name, '有冲突，双方版本已保留'); continue; }
      if (remoteIndex) {
        if (baseline && baseline.revision === remoteIndex.revision && hash === baseline.hash && own) {
          this.state(id, name, '已同步', account.lastSyncedAt); continue;
        }
        const remote = await api.readCloudProfile(id); this.check(epoch, owner);
        if (hash === remote.payloadSha256) {
          api.setProfileBaseRevision(id, remote.revision); this.base(account, id, remote.revision, hash); this.ownProfile(id, owner);
          this.state(id, name, '已同步', Date.now()); continue;
        }
        const template = shipped.find((profile: InputProfile): boolean => profile.id === id);
        const pristine = !!local && !!template && matchesShippedInputTemplate(local, template);
        this.check(epoch, owner);
        const olderTemplate = !!local && !!inputTemplateForProfileId(id) &&
          (remote.payload.builtInRevision ?? 0) < (local.builtInRevision ?? 0);
        const safeDownload = !olderTemplate && (!local || !own || baseline && hash === baseline.hash || !baseline && !!pristine);
        if (safeDownload) {
          const applied = await api.downloadProfile(id, false, serialized, (): boolean => this.valid(epoch, owner));
          this.check(epoch, owner); this.base(account, id, applied.revision, applied.payloadSha256); this.ownProfile(id, owner);
          this.state(id, name, '已恢复', Date.now()); continue;
        }
      }
      if (!own) { this.state(id, name, '属于其他账号，未上传'); continue; }
      if (!local) continue;
      api.setProfileBaseRevision(id, baseline?.revision ?? 0);
      const result = await api.uploadProfile(local); this.check(epoch, owner);
      if (result === 'CONFLICT') this.state(id, name, '有冲突，双方版本已保留');
      else { this.base(account, id, api.profileRevision(id), hash); this.state(id, name, '已同步', Date.now()); }
    }
  }
  private async applyPreferences(remote: PreferencesEnvelope, expected: string, epoch: number, owner: string): Promise<void> {
    this.check(epoch, owner); validateSyncedPreferences(remote.payload);
    const theme = ThemeSettingsService.getInstance();
    if (JSON.stringify(preferencesPayload(theme.getSettings())) !== expected) throw new Error('PROFILE_LOCAL_CHANGED');
    if (!this.store) throw new Error('SYNC_STORAGE_UNAVAILABLE');
    await this.store.put('before_preferences', expected); await this.store.flush(); this.check(epoch, owner);
    if (JSON.stringify(preferencesPayload(theme.getSettings())) !== expected) throw new Error('PROFILE_LOCAL_CHANGED');
    if (!this.preferencesReady(remote)) throw new Error('PREFERENCES_PROFILE_PENDING');
    const settings = theme.getSettings(), value = remote.payload;
    settings.themeMode = value.themeMode; settings.uiSkin = value.uiSkin; settings.consolePalette = value.consolePalette;
    settings.consoleCardLayout = value.consoleCardLayout; settings.viewMode = value.viewMode; settings.useGlassEffect = value.useGlassEffect;
    settings.desktopInputProfileId = value.desktopInputProfileId;
    this.applying = true;
    try { await theme.update(settings); } finally { this.applying = false; }
    this.check(epoch, owner); this.data.preferencesOwner = owner;
  }
  private preferencesReady(remote: PreferencesEnvelope): boolean {
    const id = remote.payload.desktopInputProfileId;
    const owner = this.api().getUserId(), account = this.currentAccount();
    return InputProfileService.getInstance().hasDesktopProfile(id) && !this.api().getConflict(id) &&
      !!account && account.removedProfiles.indexOf(id) < 0 &&
      this.data.profileOwners.some((item: ProfileOwner): boolean => item.id === id && item.userId === owner);
  }
  private async syncPreferences(account: SyncAccountState, epoch: number): Promise<void> {
    const api = this.api(), owner = account.userId;
    this.state('preferences', '外观与默认输入方案', '正在同步');
    const serialized = JSON.stringify(preferencesPayload(ThemeSettingsService.getInstance().getSettings()));
    const hash = await api.payloadHash(serialized); this.check(epoch, owner);
    const remote = await api.readPreferences(); this.check(epoch, owner);
    const baseline = account.preferences;
    if (remote && remote.payloadSha256 === hash) {
      this.preferenceConflict = null;
      account.preferences = { id: 'preferences', revision: remote.revision, hash: hash }; this.data.preferencesOwner = owner;
      this.state('preferences', '外观与默认输入方案', '已同步', Date.now()); return;
    }
    if (remote && (!baseline || hash === baseline.hash || this.data.preferencesOwner !== owner)) {
      if (!this.preferencesReady(remote)) {
        this.state('preferences', '外观与默认输入方案', '等待默认输入方案就绪'); return;
      }
      await this.applyPreferences(remote, serialized, epoch, owner);
      account.preferences = { id: 'preferences', revision: remote.revision, hash: remote.payloadSha256 };
      this.preferenceConflict = null; this.state('preferences', '外观与默认输入方案', '已恢复', Date.now()); return;
    }
    if (this.data.preferencesOwner !== owner) { this.state('preferences', '外观与默认输入方案', '属于其他账号，未上传'); return; }
    if (remote && (!baseline || remote.revision !== baseline.revision)) {
      this.preferenceConflict = remote; this.state('preferences', '外观与默认输入方案', '有冲突，双方版本已保留'); return;
    }
    const result = await api.uploadPreferences(JSON.parse(serialized) as SyncedPreferences, baseline?.revision ?? 0); this.check(epoch, owner);
    if (result.status === 'CONFLICT') { this.preferenceConflict = result.current; this.state('preferences', '外观与默认输入方案', '有冲突，双方版本已保留'); }
    else { account.preferences = { id: 'preferences', revision: result.current.revision, hash: hash };
      this.preferenceConflict = null; this.state('preferences', '外观与默认输入方案', '已同步', Date.now()); }
  }
  async resolveProfile(id: string, useCloud: boolean): Promise<void> {
    const account = this.currentAccount(); if (!account || this.busy) throw new Error('BUSY');
    const epoch = this.epoch, owner = account.userId, api = this.api();
    const local = InputProfileService.getInstance().getProfile(id);
    const serialized = JSON.stringify(cloudProfilePayload(local));
    this.busy = true;
    try {
      if (useCloud) {
        const applied = await api.downloadProfile(id, false, serialized, (): boolean => this.valid(epoch, owner));
        this.check(epoch, owner); this.base(account, id, applied.revision, applied.payloadSha256);
      } else {
        const result = await api.uploadProfile(local, true); this.check(epoch, owner);
        if (result === 'CONFLICT') { this.state(id, local.name, '有冲突，双方版本已保留'); return; }
        this.base(account, id, api.profileRevision(id), await api.payloadHash(serialized));
      }
      this.ownProfile(id, owner); await this.persist(); this.state(id, local.name, '已同步', Date.now());
    } finally { this.busy = false; this.requestSync(0); }
  }
  async resolvePreferences(useCloud: boolean): Promise<void> {
    const account = this.currentAccount(), conflict = this.preferenceConflict;
    if (!account || !conflict || this.busy) throw new Error('BUSY');
    const epoch = this.epoch, owner = account.userId, serialized = JSON.stringify(preferencesPayload(ThemeSettingsService.getInstance().getSettings()));
    this.busy = true;
    try {
      if (useCloud) {
        const remote = await this.api().readPreferences(); this.check(epoch, owner);
        if (!remote) throw new Error('INVALID_PREFERENCES'); await this.applyPreferences(remote, serialized, epoch, owner);
        account.preferences = { id: 'preferences', revision: remote.revision, hash: remote.payloadSha256 };
      } else {
        const result = await this.api().uploadPreferences(JSON.parse(serialized) as SyncedPreferences, conflict.revision); this.check(epoch, owner);
        if (result.status === 'CONFLICT') { this.preferenceConflict = result.current; return; }
        account.preferences = { id: 'preferences', revision: result.current.revision, hash: result.current.payloadSha256 };
      }
      this.preferenceConflict = null; await this.persist();
    } finally { this.busy = false; this.requestSync(0); }
  }
}
