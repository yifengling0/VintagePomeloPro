import { common } from '@kit.AbilityKit';
import { http } from '@kit.NetworkKit';
import { hilog } from '../service/LogService';
import { SteamAuthService } from './vendor/auth/SteamAuthService';
import {
  ISteamAuthService,
  SteamAuthRequest
} from './vendor/contracts/SteamPlatformContracts';
import { SteamAuthEndpoint, SteamFormField, SteamRsaPublicKey } from './vendor/auth/SteamAuthProtocol';
import { SteamAuthError, SteamAuthErrorKind, SteamAuthErrorSupport } from './vendor/contracts/SteamAuthErrors';
import { SteamJwt } from './vendor/security/SteamJwt';
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
import { SteamDownloadService } from './SteamDownloadService';
import { SteamClientQrProtocol } from './SteamClientQrProtocol';
import { SteamCmAuthClient } from './vendor/cm/SteamCmAuthClient';
import { SteamCmAuthClientOptions } from './vendor/contracts/SteamCmContracts';
import { SteamCoreNoopLogger } from './vendor/contracts/SteamCoreLogger';
import { SteamCmWebSocketTransport } from './SteamCmWebSocket';
import { SteamCmMultiDecompressor, SteamCmScheduler } from './SteamCmRuntime';
import { SteamMaFileImporter, SteamMaFileImportResult } from './vendor/import/SteamImporters';

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
  private lastChallengeState: SteamUiState = SteamUiState.IDLE;
  private initializeTask: Promise<void> | null = null;

  async initialize(context: common.UIAbilityContext): Promise<void> {
    if (!this.initializeTask) {
      this.initializeTask = this.initializeStore(context);
    }
    try {
      await this.initializeTask;
    } catch (e) {
      this.initializeTask = null;
      this.authService = null;
      throw new Error('Steam 存储初始化失败');
    }
  }

  private async initializeStore(context: common.UIAbilityContext): Promise<void> {
    await this.store.initialize(context);
    this.authService = new SteamAuthService(
      new SteamHttpTransport(),
      new SteamCryptoAdapter(),
      new SteamClock()
    );
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
    if (state === SteamUiState.NEEDS_EMAIL_CODE || state === SteamUiState.NEEDS_GUARD_CODE
      || state === SteamUiState.NEEDS_CONFIRM) {
      // 验证失败时回到当前验证界面而不是弹回登录表单（避免"重输→拒绝→重登"循环观感）
      this.lastChallengeState = state;
    }
    this.listeners.forEach((listener: SteamStateListener): void => {
      listener(state, message);
    });
  }

  /** 登录页连通性自检：返回 null=成功，否则为错误文案。 */
  async probeNetwork(): Promise<string | null> {
    const body = await SteamHttpTransport.probeServerInfo();
    if (body.startsWith('ERR:')) {
      return `无法连接 Steam API（${body.substring(4)}）`;
    }
    if (body.length === 0) {
      return '无法连接 Steam API（api.steampowered.com），请检查设备网络';
    }
    return null;
  }

  /** Public pre-login check: imports a Steam RSA key and encrypts fixed non-secret bytes. */
  async probePasswordLogin(): Promise<string | null> {
    let stage = 'rsa_public_key';
    try {
      const response = await new SteamHttpTransport().postForm(SteamAuthEndpoint.GET_PASSWORD_RSA_PUBLIC_KEY,
        [new SteamFormField('account_name', 'vintagepomelo_diagnostic')]);
      if (response.statusCode !== 200 || response.keystoneError()) {
        throw new SteamAuthError('RSA request rejected', SteamAuthErrorKind.SERVER,
          stage, 0, response.statusCode);
      }
      stage = 'rsa_public_key_parse';
      const envelope = JSON.parse(response.body) as RsaProbeEnvelope;
      const key = envelope.response;
      if (!key || !key.publickey_mod || !key.publickey_exp || !key.timestamp) {
        throw new Error('RSA fields missing');
      }
      stage = 'password_encrypt';
      const publicKey = new SteamRsaPublicKey(key.publickey_mod, key.publickey_exp, Number(key.timestamp));
      const encrypted = await new SteamCryptoAdapter().rsaEncrypt(publicKey.toPem(), new Uint8Array([0x56, 0x50]));
      if (encrypted.byteLength !== 256) throw new Error('RSA ciphertext size is invalid');
      hilog.info(DOMAIN, TAG, 'password preflight passed; no credentials used');
      return null;
    } catch (error) {
      const failure = SteamAuthErrorSupport.normalize(error, SteamAuthErrorKind.UNKNOWN, stage, 'Login preflight failed');
      hilog.warn(DOMAIN, TAG, 'password preflight failed: %{public}s', SteamAuthErrorSupport.diagnosticLabel(failure));
      return SteamService.describeError(failure);
    }
  }

  // Community 登录二维码走 HTTP；下载专用 Steam Client 二维码走 CM WebSocket。

  private qrClientId: string = '';
  private qrRequestId: string = '';
  private qrGeneration: number = 0;
  private clientQrClientId: string = '';
  private clientQrRequestId: Uint8Array = new Uint8Array(0);
  private clientQrCmClient: SteamCmAuthClient | null = null;
  private clientQrGeneration: number = 0;
  private clientQrChallengeUrl: string = '';
  private clientQrSawRemoteInteraction: boolean = false;
  private clientQrPollCount: number = 0;

  /** CM 内容下载需要 Steam Client 平台令牌；Community 令牌会被 CM 拒绝。 */
  async getClientRefreshToken(): Promise<string> {
    if (!this.steamId64) return '';
    return await this.store.loadSecret(this.steamId64, 'client_refresh_token');
  }

  async clearClientRefreshToken(): Promise<void> {
    if (this.steamId64) await this.store.removeSecret(this.steamId64, 'client_refresh_token');
  }

  async beginClientQrLogin(): Promise<string> {
    if (!this.steamId64) throw new Error('请先登录 Steam 账号');
    this.cancelClientQrLogin();
    const generation = this.clientQrGeneration;
    const client = new SteamCmAuthClient(new SteamCmWebSocketTransport(), new SteamCmScheduler(),
      new SteamCmAuthClientOptions('cmp1-sea1.steamserver.net:443', 65580, 30000,
        new SteamCoreNoopLogger(), new SteamCmMultiDecompressor()));
    this.clientQrCmClient = client;
    try {
      await client.connect();
      if (generation !== this.clientQrGeneration) throw new Error('扫码会话已取消');
      const reply = await client.call('Authentication.BeginAuthSessionViaQR#1',
        SteamClientQrProtocol.beginRequest());
      if (generation !== this.clientQrGeneration) throw new Error('扫码会话已取消');
      if (reply.protoHeader.eresult !== 1) {
        throw new Error('Steam CM 拒绝客户端授权二维码 (EResult ' + reply.protoHeader.eresult + ')');
      }
      const body = SteamClientQrProtocol.decodeBegin(reply.body);
      this.clientQrClientId = body.clientId;
      this.clientQrRequestId = body.requestId;
      this.clientQrChallengeUrl = body.challengeUrl;
      hilog.info(DOMAIN, TAG, 'Steam Client CM QR ready; pollInterval=%{public}d',
        Math.round(body.intervalSeconds));
      return body.challengeUrl;
    } catch (error) {
      if (generation === this.clientQrGeneration) this.cancelClientQrLogin();
      throw error instanceof Error ? error : new Error('Steam CM 客户端授权请求失败');
    }
  }

  getClientQrChallengeUrl(): string { return this.clientQrChallengeUrl; }

  async pollClientQrLogin(): Promise<'pending' | 'scanned' | 'approved' | 'invalid'> {
    if (!this.clientQrClientId || !this.clientQrCmClient || this.clientQrRequestId.length === 0) return 'invalid';
    const generation = this.clientQrGeneration;
    const reply = await this.clientQrCmClient.call('Authentication.PollAuthSessionStatus#1',
      SteamClientQrProtocol.pollRequest(this.clientQrClientId, this.clientQrRequestId));
    if (generation !== this.clientQrGeneration) return 'invalid';
    if (reply.protoHeader.eresult !== 1) {
      hilog.warn(DOMAIN, TAG, 'Steam Client CM QR poll rejected: result=%{public}d', reply.protoHeader.eresult);
      throw new Error('Steam 客户端授权轮询失败 (EResult ' + reply.protoHeader.eresult + ')');
    }
    const body = SteamClientQrProtocol.decodePoll(reply.body);
    if (body.newClientId && body.newClientId !== this.clientQrClientId) {
      this.clientQrClientId = body.newClientId;
      hilog.info(DOMAIN, TAG, 'Steam Client QR client ID rotated');
    }
    if (body.newChallengeUrl && body.newChallengeUrl !== this.clientQrChallengeUrl) {
      this.clientQrChallengeUrl = body.newChallengeUrl;
      hilog.info(DOMAIN, TAG, 'Steam Client QR challenge URL refreshed');
    }
    this.clientQrPollCount++;
    if (body.hadRemoteInteraction && !this.clientQrSawRemoteInteraction) {
      this.clientQrSawRemoteInteraction = true;
      hilog.info(DOMAIN, TAG, 'Steam Client QR remote interaction detected');
    }
    const refreshToken = body.refreshToken;
    if (!refreshToken) {
      if (this.clientQrPollCount === 1 || this.clientQrPollCount % 6 === 0) {
        hilog.info(DOMAIN, TAG, 'Steam Client QR pending; polls=%{public}d scanned=%{public}s',
          this.clientQrPollCount, String(this.clientQrSawRemoteInteraction));
      }
      return this.clientQrSawRemoteInteraction ? 'scanned' : 'pending';
    }
    if (!SteamJwt.hasAudience(refreshToken, 'client')) {
      hilog.warn(DOMAIN, TAG, 'Steam Client QR returned a non-client token');
      this.cancelClientQrLogin();
      throw new Error('Steam 返回的扫码令牌不是客户端授权，请重新扫码');
    }
    const steamId = SteamJwt.subjectClaim(refreshToken);
    if (steamId !== this.steamId64) {
      this.cancelClientQrLogin();
      throw new Error('扫码授权账号与当前游戏库账号不一致');
    }
    await this.store.storeSecret(steamId, 'client_refresh_token', refreshToken);
    this.cancelClientQrLogin();
    hilog.info(DOMAIN, TAG, 'Steam Client authorization completed');
    return 'approved';
  }

  cancelClientQrLogin(): void {
    this.clientQrGeneration++;
    const client = this.clientQrCmClient;
    this.clientQrCmClient = null;
    if (client) void client.disconnect().catch((): void => {});
    this.clientQrClientId = '';
    this.clientQrRequestId = new Uint8Array(0);
    this.clientQrChallengeUrl = '';
    this.clientQrSawRemoteInteraction = false;
    this.clientQrPollCount = 0;
  }

  /** QR 第一步: 发起扫码会话, 返回 challenge_url（给 QRCode 组件渲染）。 */
  async beginQrLogin(): Promise<string> {
    this.cancelQrLogin();
    const generation = this.qrGeneration;
    this.qrClientId = '';
    this.qrRequestId = '';
    const transport = new SteamHttpTransport();
    // website_id 与 PC 实测一致; 空 body 在 @ohos http 上行为不确定, 显式带一个字段
    const response = await transport.postForm(SteamAuthEndpoint.BEGIN_QR_SESSION,
      [new SteamFormField('website_id', 'Community')]);
    if (generation !== this.qrGeneration) throw new Error('扫码会话已取消');
    if (response.keystoneError().length > 0) throw new Error('Steam 拒绝扫码请求');
    if (response.statusCode !== 200) {
      throw new Error('HTTP ' + response.statusCode);
    }
    const parsed = JSON.parse(response.body) as QrBeginEnvelope;
    const body = parsed?.response;
    if (!body || !body.client_id || !body.challenge_url || !body.request_id) {
      throw new Error('Steam 未返回扫码会话');
    }
    this.qrClientId = body.client_id;
    this.qrRequestId = body.request_id;
    return body.challenge_url;
  }

  /** QR 轮询: pending=继续等; approved=登录完成; invalid=二维码已失效。 */
  async pollQrLogin(): Promise<'pending' | 'approved' | 'invalid'> {
    if (this.qrClientId.length === 0) return 'invalid';
    const generation = this.qrGeneration;
    const transport = new SteamHttpTransport();
    const response = await transport.postForm(SteamAuthEndpoint.POLL_AUTH_SESSION, [
      new SteamFormField('client_id', this.qrClientId),
      new SteamFormField('request_id', this.qrRequestId)
    ]);
    if (generation !== this.qrGeneration) return 'invalid';
    if (response.statusCode !== 200 || response.keystoneError().length > 0) {
      return 'invalid';
    }
    const parsed = JSON.parse(response.body) as QrPollEnvelope;
    const body = parsed?.response;
    const refreshToken = body?.refresh_token ?? '';
    const accessToken = body?.access_token ?? '';
    if (refreshToken.length === 0) {
      return 'pending';
    }
    // 手机已确认: 与密码登录成功路径同款落库
    const steamId = SteamJwt.subjectClaim(refreshToken);
    if (steamId.length === 0) {
      this.setState(SteamUiState.ERROR, '扫码已确认但令牌解析失败');
      return 'invalid';
    }
    this.accountName = this.accountName.length > 0 ? this.accountName : 'Steam 用户';
    this.steamId64 = steamId;
    this.accessToken = accessToken;
    await this.store.storeSecret(steamId, 'refresh_token', refreshToken);
    if (accessToken.length > 0) {
      await this.store.storeSecret(steamId, 'access_token', accessToken);
    }
    const profile = new SteamProfile();
    profile.accountName = this.accountName;
    profile.steamId64 = steamId;
    profile.personaName = this.personaName;
    profile.avatarUrl = this.avatarUrl;
    await this.store.saveProfile(profile);
    this.qrClientId = '';
    this.qrRequestId = '';
    hilog.info(DOMAIN, TAG, 'QR login completed; steamId=%{public}s', steamId);
    this.setState(SteamUiState.LOGGED_IN, '扫码登录成功');
    void this.fetchProfileInfo();
    return 'approved';
  }

  /** 取消/作废当前扫码会话（页面离开或重新生成时调）。 */
  cancelQrLogin(): void {
    this.qrGeneration++;
    this.qrClientId = '';
    this.qrRequestId = '';
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
    this.cancelQrLogin();
    this.lastChallengeState = SteamUiState.IDLE;
    this.accountName = name;
    this.setState(SteamUiState.WORKING, '正在连接 Steam…');
    try {
      const credentials = new SteamCredentialSet(name, password);
      this.lastCredentials = credentials;
      const result = await this.authService.signIn(new SteamAuthRequest(name, credentials));
      await this.applyAuthResult(name, result, credentials);
    } catch (e) {
      const failure = SteamAuthErrorSupport.normalize(e, SteamAuthErrorKind.UNKNOWN, 'sign_in', 'Steam login failed');
      hilog.warn(DOMAIN, TAG, 'signIn failed: %{public}s', SteamAuthErrorSupport.diagnosticLabel(failure));
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
      // NEEDS_CONFIRM 也允许改输令牌码 (Steam 响应同时提供手机确认与 TOTP 时, 用户可选手输)
      const kind: SteamAuthChallengeKind = this.lastChallengeState === SteamUiState.NEEDS_EMAIL_CODE
        ? SteamAuthChallengeKind.EMAIL_CODE : SteamAuthChallengeKind.TWO_FACTOR;
      const request = new SteamAuthRequest(this.accountName, this.lastCredentials, trimmed, kind);
      const result = await this.authService.completeChallenge(request);
      await this.applyAuthResult(this.accountName, result, this.lastCredentials);
    } catch (e) {
      const failure = SteamAuthErrorSupport.normalize(e, SteamAuthErrorKind.UNKNOWN, 'submit_code', 'Steam challenge failed');
      hilog.warn(DOMAIN, TAG, 'submitCode failed: %{public}s', SteamAuthErrorSupport.diagnosticLabel(failure));
      // 留在当前验证界面显示真实原因（弹回登录表单会造成"重输→拒绝→重登"循环观感）
      this.setState(this.lastChallengeState === SteamUiState.IDLE
        ? SteamUiState.ERROR : this.lastChallengeState, SteamService.describeError(e));
    } finally {
      this.working = false;
    }
  }

  /** 手机确认轮询（用户在手机 Steam 上点"允许"）。 */
  async pollOnce(): Promise<void> {
    if (!this.authService || this.working) return;
    if (this.state !== SteamUiState.NEEDS_CONFIRM) return;
    this.working = true;
    try {
      const result = await this.authService.poll(this.accountName);
      await this.applyAuthResult(this.accountName, result, this.lastCredentials);
    } catch (_) {
      // 轮询失败静默（下一轮再试）
    } finally {
      this.working = false;
    }
  }

  /** 会话恢复：用 AssetStore 里的 refresh_token 换新 access_token。 */
  private restoreTask: Promise<boolean> | null = null;

  /** CM content service uses the existing login's refresh token. */
  async getRefreshToken(): Promise<string> {
    if (this.lastCredentials.refreshToken.length > 0) {
      return this.lastCredentials.refreshToken;
    }
    if (this.steamId64.length === 0) return '';
    return await this.store.loadSecret(this.steamId64, 'refresh_token');
  }


  async restoreSession(): Promise<boolean> {
    if (this.restoreTask) return this.restoreTask;
    const task = this.restoreStoredSession();
    this.restoreTask = task;
    try {
      return await task;
    } finally {
      if (this.restoreTask === task) this.restoreTask = null;
    }
  }

  private async restoreStoredSession(): Promise<boolean> {
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
      const failure = SteamAuthErrorSupport.normalize(e, SteamAuthErrorKind.UNKNOWN,
        'restore_session', 'Steam session restore failed');
      hilog.warn(DOMAIN, TAG, 'restore failed: %{public}s', SteamAuthErrorSupport.diagnosticLabel(failure));
      return false;
    } finally {
      this.working = false;
    }
  }

  async signOut(): Promise<void> {
    this.cancelQrLogin();
    this.cancelClientQrLogin();
    await SteamDownloadService.getInstance().closeSession();
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
      if (!silent) {
        // 新设备首次登录的常见终局: Steam 作废了验证会话, 只认手机确认
        this.setState(SteamUiState.ERROR,
          msg + '。首次登录请在手机 Steam App → Steam Guard → 「确认登录」里允许本次请求（只需一次）');
      }
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

  /** maFile 导入（W3）：解析 JSON，shared_secret/identity_secret/device_id 逐项入 AssetStore。
   * 存储键用 maFile 自带 steamid（与登录账号一致时令牌立即生效）。返回给 UI 的报告文案。 */
  async importMaFile(json: string): Promise<string> {
    let result: SteamMaFileImportResult;
    try {
      result = SteamMaFileImporter.importMaFile(json);
    } catch (e) {
      const reason = (e as Error).message;
      return 'maFile 解析失败：' + (reason.length > 0 ? reason : '格式不正确');
    }
    const steamId = result.steamId64;
    const cred = result.credentials;
    try {
      await this.store.storeSecret(steamId, 'shared_secret', cred.sharedSecret);
      await this.store.storeSecret(steamId, 'identity_secret', cred.identitySecret);
      if (cred.deviceId.length > 0) {
        await this.store.storeSecret(steamId, 'device_id', cred.deviceId);
      }
    } catch (_) {
      return '密钥写入安全存储失败，请重试';
    }
    const parts: Array<string> = ['导入成功：识别 ' + result.report.recognizedFields.length + ' 项密钥'];
    if (steamId !== this.steamId64) {
      parts.push(this.steamId64.length > 0
        ? '注意：maFile 属于其他账号，登录该账号后令牌生效'
        : '注意：当前未登录，登录 maFile 对应账号后令牌生效');
    }
    if (result.report.unknownFields.length > 0) {
      parts.push('忽略未知字段 ' + result.report.unknownFields.length + ' 个');
    }
    if (result.report.warnings.length > 0) {
      parts.push('警告 ' + result.report.warnings.length + ' 条');
    }
    hilog.info(DOMAIN, TAG, 'maFile imported; steamid=%{public}s, recognized=%{public}d',
      steamId, result.report.recognizedFields.length);
    return parts.join('；');
  }

  private static describeError(e: Object): string {
    const raw = (e as Error).message || JSON.stringify(e);
    if (e instanceof SteamAuthError) {
      const detail = `（${e.stage}${e.code ? ' / ' + e.code : ''}${e.httpStatusCode ? ' / HTTP ' + e.httpStatusCode : ''}）`;
      if (e.kind === SteamAuthErrorKind.CRYPTO || e.stage === 'password_encrypt') {
        return '设备上的密码加密失败' + detail;
      }
      if (e.kind === SteamAuthErrorKind.TRANSPORT || e.kind === SteamAuthErrorKind.TIMEOUT) {
        return '连接 Steam 失败，请检查网络' + detail;
      }
      if (e.code === 5 || /InvalidPassword|invalid_password|InvalidPassword/i.test(raw)) return '账号或密码错误';
      if (e.code === 84) return 'Steam 登录请求过于频繁，请稍后再试';
      if (e.kind === SteamAuthErrorKind.AUTHENTICATION) return 'Steam 拒绝登录' + detail;
      if (e.httpStatusCode) return 'Steam 登录接口请求失败' + detail;
      if (e.stage === 'rsa_public_key_parse' || e.stage === 'begin_auth_parse') {
        return 'Steam 登录响应解析失败' + detail;
      }
    }
    if (raw.indexOf('rejected:') >= 0) {
      // keystone x-error 头透传的 Steam 拒绝（验证码错误/会话不接受该类型等）
      const reason = raw.substring(raw.indexOf('rejected:') + 9).replace(/"$/, '');
      return 'Steam 未接受：' + reason + '（看手机 App 当前 5 位码重试；'
        + '新设备首次登录 Steam 通常要求在手机上确认一次，确认后启动器会记住本设备）';
    }
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

interface RsaProbeEnvelope {
  response?: RsaProbeKey;
}

interface RsaProbeKey {
  publickey_mod?: string;
  publickey_exp?: string;
  timestamp?: string;
}

/** QR 扫码会话发起响应（仅取用到的字段）。 */
interface QrBeginEnvelope {
  response?: QrBeginBody;
}

interface QrBeginBody {
  client_id?: string;
  request_id?: string;
  challenge_url?: string;
  interval?: number;
}

/** QR 轮询响应（手机确认后带 token）。 */
interface QrPollEnvelope {
  response?: QrPollBody;
}

interface QrPollBody {
  refresh_token?: string;
  access_token?: string;
  had_remote_interaction?: boolean;
  new_client_id?: string;
  new_challenge_url?: string;
}

interface PlayerSummaryEnvelope {
  players?: Array<PlayerSummary>;
}

interface PlayerSummary {
  personaname?: string;
  avatarfull?: string;
}
