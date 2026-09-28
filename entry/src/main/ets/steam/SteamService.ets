import { common } from '@kit.AbilityKit';
import { http } from '@kit.NetworkKit';
import { hilog } from '../service/LogService';
import { SteamAuthService } from './vendor/auth/SteamAuthService';
import {
  ISteamAuthService,
  SteamAuthRequest
} from './vendor/contracts/SteamPlatformContracts';
import {
  SteamCredentialSet,
  SteamSessionState,
  SteamAuthResult,
  SteamAuthChallengeKind
} from './vendor/models/SteamModels';
import { SteamTotp } from './vendor/security/SteamTotp';
import { SteamCryptoAdapter } from './SteamCryptoAdapter';
import { SteamHttpTransport } from './SteamHttpTransport';
import { SteamClock } from './SteamClock';
import { SteamAccountStore, SteamProfile } from './SteamAccountStore';

const DOMAIN = 0x0000;
const TAG = 'SteamService';

/** 供 UI 渲染的状态机。 */
export enum SteamUiState {
  IDLE = 'idle',
  WORKING = 'working',
  NEEDS_EMAIL_CODE = 'needs_email_code',
  NEEDS_GUARD_CODE = 'needs_guard_code',
  NEEDS_CONFIRM = 'needs_confirm',
  LOGGED_IN = 'logged_in',
  ERROR = 'error'
}

export type SteamStateListener = (state: SteamUiState, message: string) => void;

/**
 * Steam 账号单例门面：登录状态机 / 会话恢复 / 令牌码 / 档案。
 * 密码仅用于登录瞬间，成功后只持久化 refresh/access token（进 AssetStore）。
 */
export class SteamService {
  private static instance: SteamService | null = null;

  static getInstance(): SteamService {
    if (!SteamService.instance) {
      SteamService.instance = new SteamService();
    }
    return SteamService.instance;
  }

  private authService: ISteamAuthService | null = null;
  private store: SteamAccountStore = SteamAccountStore.getInstance();
  private listeners: Map<string, SteamStateListener> = new Map<string, SteamStateListener>();
  private lastCredentials: SteamCredentialSet = new SteamCredentialSet();

  state: SteamUiState = SteamUiState.IDLE;
  message: string = '';
  accountName: string = '';
  steamId64: string = '';
  avatarUrl: string = '';
  personaName: string = '';
  accessToken: string = '';
  timeOffsetSeconds: number = 0;
  working: boolean = false;

  async initialize(context: common.UIAbilityContext): Promise<void> {
    if (this.authService) return;
    this.authService = new SteamAuthService(
      new SteamHttpTransport(),
      new SteamCryptoAdapter(),
      new SteamClock()
    );
    await this.store.initialize(context);
    this.timeOffsetSeconds = await this.store.loadTimeOffset();
    const profile = await this.store.loadProfile();
    this.accountName = profile.accountName;
    this.steamId64 = profile.steamId64;
    this.avatarUrl = profile.avatarUrl;
    this.personaName = profile.personaName;
    this.state = profile.steamId64.length > 0 ? SteamUiState.LOGGED_IN : SteamUiState.IDLE;
    hilog.info(DOMAIN, TAG, 'initialized; restored=%{public}d', profile.steamId64.length);
  }

  subscribe(id: string, listener: SteamStateListener): void {
    this.listeners.set(id, listener);
  }

  unsubscribe(id: string): void {
    this.listeners.delete(id);
  }

  private setState(state: SteamUiState, message: string = ''): void {
    this.state = state;
    this.message = message;
    this.listeners.forEach((listener: SteamStateListener): void => {
      listener(state, message);
    });
  }

  /** 登录页连通性自检：返回 null=成功，否则为错误文案。 */
  async probeNetwork(): Promise<string | null> {
    const body = await SteamHttpTransport.probeServerInfo();
    if (body.length === 0) {
      return '无法连接 Steam API（api.steampowered.com），请检查设备网络';
    }
    return null;
  }

  /** 密码登录第一步：密码 → （可能）验证码挑战 → 会话。 */
  async signIn(account: string, password: string): Promise<void> {
    if (!this.authService || this.working) return;
    const name = account.trim();
    if (name.length === 0 || password.length === 0) {
      this.setState(SteamUiState.ERROR, '请输入账号和密码');
      return;
    }
    this.working = true;
    this.accountName = name;
    this.setState(SteamUiState.WORKING, '正在连接 Steam…');
    try {
      const credentials = new SteamCredentialSet(name, password);
      this.lastCredentials = credentials;
      const result = await this.authService.signIn(new SteamAuthRequest(name, credentials));
      await this.applyAuthResult(name, result, credentials);
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'signIn failed: %{public}s', JSON.stringify(e));
      this.setState(SteamUiState.ERROR, SteamService.describeError(e));
    } finally {
      this.working = false;
    }
  }

  /** 提交验证码（邮件码 / 手机令牌码）。 */
  async submitCode(code: string): Promise<void> {
    if (!this.authService || this.working) return;
    const trimmed = code.trim();
    if (trimmed.length === 0) {
      this.setState(SteamUiState.ERROR, '请输入验证码');
      return;
    }
    this.working = true;
    this.setState(SteamUiState.WORKING, '正在验证…');
    try {
      const kind: SteamAuthChallengeKind = this.state === SteamUiState.NEEDS_GUARD_CODE
        ? SteamAuthChallengeKind.TWO_FACTOR : SteamAuthChallengeKind.EMAIL_CODE;
      const request = new SteamAuthRequest(this.accountName, this.lastCredentials, trimmed, kind);
      const result = await this.authService.completeChallenge(request);
      await this.applyAuthResult(this.accountName, result, this.lastCredentials);
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'submitCode failed: %{public}s', JSON.stringify(e));
      this.setState(SteamUiState.ERROR, SteamService.describeError(e));
    } finally {
      this.working = false;
    }
  }

  /** 手机确认轮询（用户在手机 Steam 上点"允许"）。 */
  async pollOnce(): Promise<void> {
    if (!this.authService || this.working) return;
    if (this.state !== SteamUiState.NEEDS_CONFIRM) return;
    try {
      const result = await this.authService.poll(this.accountName);
      await this.applyAuthResult(this.accountName, result, this.lastCredentials);
    } catch (_) {
      // 轮询失败静默（下一轮再试）
    }
  }

  /** 会话恢复：用 AssetStore 里的 refresh_token 换新 access_token。 */
  async restoreSession(): Promise<boolean> {
    if (!this.authService) return false;
    if (this.steamId64.length === 0) return false;
    const refreshToken = await this.store.loadSecret(this.steamId64, 'refresh_token');
    if (refreshToken.length === 0) return false;
    this.working = true;
    try {
      const credentials = new SteamCredentialSet(this.accountName, '', refreshToken);
      this.lastCredentials = credentials;
      const result = await this.authService.refresh(this.accountName, refreshToken);
      if (result.session.state === SteamSessionState.LOGGED_ON) {
        await this.applyAuthResult(this.accountName, result, credentials, true);
        return true;
      }
      return false;
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'restore failed: %{public}s', JSON.stringify(e));
      return false;
    } finally {
      this.working = false;
    }
  }

  async signOut(): Promise<void> {
    if (this.authService && this.accountName.length > 0) {
      try {
        await this.authService.close(this.accountName);
      } catch (_) {
      }
    }
    await this.store.removeAllSecrets(this.steamId64);
    await this.store.clearProfile();
    this.accessToken = '';
    this.steamId64 = '';
    this.avatarUrl = '';
    this.personaName = '';
    this.accountName = '';
    this.lastCredentials = new SteamCredentialSet();
    this.setState(SteamUiState.IDLE, '已登出');
  }

  /** 当前 Steam Guard 令牌码（需已导入 shared_secret）；失败返回空串。 */
  async currentGuardCode(): Promise<string> {
    if (this.steamId64.length === 0) return '';
    const secret = await this.store.loadSecret(this.steamId64, 'shared_secret');
    if (secret.length === 0) return '';
    const now = Math.floor(Date.now() / 1000) + this.timeOffsetSeconds;
    return await SteamTotp.generateForTime(secret, now, new SteamCryptoAdapter());
  }

  private async applyAuthResult(accountId: string, result: SteamAuthResult,
    usedCredentials: SteamCredentialSet, silent: boolean = false): Promise<void> {
    const state = result.session.state;
    this.lastCredentials = result.credentials.accountName.length > 0 ? result.credentials : usedCredentials;
    if (state === SteamSessionState.NEEDS_EMAIL_CODE) {
      this.setState(SteamUiState.NEEDS_EMAIL_CODE, 'Steam 已向你的邮箱发送验证码');
      return;
    }
    if (state === SteamSessionState.NEEDS_TWO_FACTOR) {
      this.setState(SteamUiState.NEEDS_GUARD_CODE, '请输入手机令牌上的 5 位验证码');
      return;
    }
    if (state === SteamSessionState.NEEDS_MOBILE_CONFIRMATION) {
      this.setState(SteamUiState.NEEDS_CONFIRM, '请在手机 Steam 上确认本次登录');
      return;
    }
    if (state !== SteamSessionState.LOGGED_ON) {
      const msg = result.session.lastErrorMessage.length > 0
        ? result.session.lastErrorMessage : `登录失败（${state}）`;
      if (!silent) this.setState(SteamUiState.ERROR, msg);
      return;
    }
    // 登录成功
    this.accountName = accountId;
    this.steamId64 = result.session.steamId64.length > 0
      ? result.session.steamId64 : this.steamId64;
    this.accessToken = result.credentials.accessToken;
    // 凭据落安全存储（refresh_token 轮换时整体覆盖）
    if (result.credentials.refreshToken.length > 0) {
      await this.store.storeSecret(this.steamId64, 'refresh_token', result.credentials.refreshToken);
    }
    if (result.credentials.accessToken.length > 0) {
      await this.store.storeSecret(this.steamId64, 'access_token', result.credentials.accessToken);
    }
    if (result.credentials.guardData.length > 0) {
      await this.store.storeSecret(this.steamId64, 'guard_data', result.credentials.guardData);
    }
    if (usedCredentials.sharedSecret.length > 0) {
      await this.store.storeSecret(this.steamId64, 'shared_secret', usedCredentials.sharedSecret);
    }
    const profile = new SteamProfile();
    profile.accountName = this.accountName;
    profile.steamId64 = this.steamId64;
    profile.personaName = this.personaName;
    profile.avatarUrl = this.avatarUrl;
    await this.store.saveProfile(profile);
    hilog.info(DOMAIN, TAG, 'logged in; steamId=%{public}s', this.steamId64);
    this.setState(SteamUiState.LOGGED_IN, silent ? '' : '登录成功');
    if (!silent) {
      // 头像与人名尽力拉取（失败不影响登录态）
      await this.fetchProfileInfo();
    }
  }

  /** ISteamUser/GetPlayerSummaries（access_token 鉴权，尽力而为）。 */
  async fetchProfileInfo(): Promise<void> {
    if (this.accessToken.length === 0 || this.steamId64.length === 0) return;
    const request = http.createHttp();
    try {
      const url = 'https://api.steampowered.com/ISteamUser/GetPlayerSummaries/v2/?access_token=' +
        encodeURIComponent(this.accessToken) + '&steamids=' + this.steamId64;
      const response = await request.request(url, {
        method: http.RequestMethod.GET,
        connectTimeout: 10000,
        readTimeout: 10000
      });
      if (response.responseCode !== 200) return;
      const parsed = JSON.parse(String(response.result)) as GetPlayerSummariesResponse;
      const players = parsed?.response?.players;
      if (players && players.length > 0) {
        this.personaName = typeof players[0].personaname === 'string' ? players[0].personaname : '';
        this.avatarUrl = typeof players[0].avatarfull === 'string' ? players[0].avatarfull : '';
        const profile = new SteamProfile();
        profile.accountName = this.accountName;
        profile.steamId64 = this.steamId64;
        profile.personaName = this.personaName;
        profile.avatarUrl = this.avatarUrl;
        await this.store.saveProfile(profile);
        this.setState(this.state, '');
      }
    } catch (_) {
    } finally {
      request.destroy();
    }
  }

  async saveSharedSecret(secret: string): Promise<void> {
    if (this.steamId64.length === 0) {
      this.setState(SteamUiState.ERROR, '请先登录 Steam 账号');
      return;
    }
    await this.store.storeSecret(this.steamId64, 'shared_secret', secret.trim());
    this.setState(this.state, '令牌密钥已保存');
  }

  private static describeError(e: Object): string {
    const raw = JSON.stringify(e);
    if (raw.indexOf('timeout') >= 0) return '连接 Steam 超时，请检查网络';
    if (raw.indexOf('InvalidPassword') >= 0 || raw.indexOf('invalid_password') >= 0) {
      return '账号或密码错误';
    }
    return '操作失败，请稍后重试';
  }
}

interface GetPlayerSummariesResponse {
  response?: PlayerSummaryEnvelope;
}

interface PlayerSummaryEnvelope {
  players?: Array<PlayerSummary>;
}

interface PlayerSummary {
  personaname?: string;
  avatarfull?: string;
}
