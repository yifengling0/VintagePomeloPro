import {
  SteamAuthChallengeKind,
  SteamAuthFlowKind,
  SteamAuthResult,
  SteamAuthPendingSnapshot,
  SteamCredentialSet,
  SteamSessionSnapshot,
  SteamSessionState
} from '../models/SteamModels';
import { SteamAuthRequest, ISteamAuthService, IBinaryCrypto, IClock, ISteamAuthTransport } from '../contracts/SteamPlatformContracts';
import { SteamJsonParser, SteamJsonValue, SteamJsonValueKind } from '../import/SteamJson';
import { SteamBase64 } from '../protocol/SteamBase64';
import { SteamUtf8 } from '../protocol/SteamVarint';
import { SteamTotp } from '../security/SteamTotp';
import { SteamJwt } from '../security/SteamJwt';
import {
  SteamAuthEndpoint,
  SteamAuthHttpResponse,
  SteamFormField,
  SteamRsaPublicKey
} from './SteamAuthProtocol';
import { ISteamCoreLogger, SteamCoreNoopLogger } from '../contracts/SteamCoreLogger';
import {
  SteamAuthError,
  SteamAuthErrorKind,
  SteamAuthErrorSupport
} from '../contracts/SteamAuthErrors';

const TAG: string = 'AuthService';

class SteamPendingAuthSession {
  readonly accountId: string;
  readonly credentials: SteamCredentialSet;
  readonly clientId: string;
  readonly requestId: string;
  readonly steamId64: string;
  readonly challenge: SteamAuthChallengeKind;
  readonly intervalSeconds: number;
  readonly createdAtEpochSeconds: number;
  readonly expiresAtEpochSeconds: number;
  readonly flow: SteamAuthFlowKind;
  readonly challengeUrl: string;

  constructor(
    accountId: string,
    credentials: SteamCredentialSet,
    clientId: string,
    requestId: string,
    steamId64: string,
    challenge: SteamAuthChallengeKind,
    intervalSeconds: number,
    createdAtEpochSeconds: number,
    expiresAtEpochSeconds: number,
    flow: SteamAuthFlowKind = SteamAuthFlowKind.PASSWORD,
    challengeUrl: string = ''
  ) {
    this.accountId = accountId;
    this.credentials = credentials;
    this.clientId = clientId;
    this.requestId = requestId;
    this.steamId64 = steamId64;
    this.challenge = challenge;
    this.intervalSeconds = intervalSeconds;
    this.createdAtEpochSeconds = createdAtEpochSeconds;
    this.expiresAtEpochSeconds = expiresAtEpochSeconds;
    this.flow = flow;
    this.challengeUrl = challengeUrl;
  }

  toSnapshot(): SteamAuthPendingSnapshot {
    return new SteamAuthPendingSnapshot(
      this.accountId,
      this.clientId,
      this.requestId,
      this.steamId64,
      this.challenge,
      this.intervalSeconds,
      this.createdAtEpochSeconds,
      this.expiresAtEpochSeconds,
      this.flow,
      this.challengeUrl
    );
  }
}

/**
 * Native Steam AuthenticationService flow.
 *
 * This class deliberately stops at the HTTP transport boundary. It performs
 * no request encoding, cookie storage, scheduling or platform I/O itself.
 */
export class SteamAuthService implements ISteamAuthService {
  private readonly transport: ISteamAuthTransport;
  private readonly crypto: IBinaryCrypto;
  private readonly clock: IClock;
  private readonly logger: ISteamCoreLogger;
  private readonly pending: Map<string, SteamPendingAuthSession> = new Map<string, SteamPendingAuthSession>();
  private readonly knownSteamIds: Map<string, string> = new Map<string, string>();
  private readonly knownCredentials: Map<string, SteamCredentialSet> = new Map<string, SteamCredentialSet>();

  constructor(
    transport: ISteamAuthTransport,
    crypto: IBinaryCrypto,
    clock: IClock,
    logger: ISteamCoreLogger = new SteamCoreNoopLogger()
  ) {
    this.transport = transport;
    this.crypto = crypto;
    this.clock = clock;
    this.logger = logger;
    this.logger.info(TAG, 'HTTP authentication service constructed');
  }

  getPendingAuthSnapshot(accountId: string): SteamAuthPendingSnapshot | null {
    const pending = this.currentPending(accountId);
    return pending === null ? null : pending.toSnapshot();
  }

  restorePendingAuthSnapshot(snapshot: SteamAuthPendingSnapshot, credentials: SteamCredentialSet): boolean {
    if (!snapshot.isValidFor(snapshot.accountId, this.clock.nowEpochSeconds())) {
      this.logger.warn(TAG, 'pending authentication restore rejected; reason=expired_or_invalid');
      return false;
    }
    const pending = new SteamPendingAuthSession(
      snapshot.accountId,
      credentials,
      snapshot.clientId,
      snapshot.requestId,
      snapshot.steamId64,
      snapshot.challenge,
      snapshot.intervalSeconds,
      snapshot.createdAtEpochSeconds,
      snapshot.expiresAtEpochSeconds,
      snapshot.flow,
      snapshot.challengeUrl
    );
    this.pending.set(snapshot.accountId, pending);
    this.knownSteamIds.set(snapshot.accountId, snapshot.steamId64);
    this.knownCredentials.set(snapshot.accountId, credentials);
    this.logger.info(TAG, 'pending authentication restored; challenge=' + String(snapshot.challenge) +
      ', flow=' + String(snapshot.flow));
    return true;
  }

  async signIn(request: SteamAuthRequest): Promise<SteamAuthResult> {
    this.logger.info(TAG, 'password sign-in started; passwordPresent=' +
      String(request.credentials.password.length > 0));
    this.pending.delete(request.accountId);
    this.requireAccountName(request.credentials);
    if (request.credentials.password.length === 0) {
      throw new Error('Password is required for password sign-in');
    }
    const rsaResponse = await this.postForm(
      SteamAuthEndpoint.GET_PASSWORD_RSA_PUBLIC_KEY,
      [new SteamFormField('account_name', request.credentials.accountName)],
      'rsa_public_key'
    );
    let rsaKey: SteamRsaPublicKey;
    try {
      const rsaObject = this.responseObject(rsaResponse, 'RSA public key');
      rsaKey = this.parseRsaPublicKey(rsaObject);
    } catch (error) {
      throw SteamAuthErrorSupport.normalize(error, SteamAuthErrorKind.RESPONSE,
        'rsa_public_key_parse', 'Steam RSA public key response is invalid');
    }
    let encryptedPassword: Uint8Array;
    try {
      encryptedPassword = await this.crypto.rsaEncrypt(
        rsaKey.toPem(), SteamUtf8.encode(request.credentials.password));
    } catch (error) {
      throw SteamAuthErrorSupport.normalize(error, SteamAuthErrorKind.CRYPTO,
        'password_encrypt', 'Steam password encryption failed');
    }
    const beginResponse = await this.postForm(
      SteamAuthEndpoint.BEGIN_AUTH_SESSION,
      this.beginFields(request, rsaKey, encryptedPassword),
      'begin_auth_session'
    );
    const beginResult = beginResponse.keystoneResult();
    if (beginResult !== 0 && beginResult !== 1) {
      throw new SteamAuthError('Steam rejected password sign-in', SteamAuthErrorKind.AUTHENTICATION,
        'begin_auth_session', beginResult, beginResponse.statusCode);
    }
    let pending: SteamPendingAuthSession;
    try {
      const beginObject = this.responseObject(beginResponse, 'begin auth session');
      pending = this.parsePendingSession(request, beginObject);
    } catch (error) {
      const failure = SteamAuthErrorSupport.normalize(error, SteamAuthErrorKind.RESPONSE,
        'begin_auth_parse', 'Steam login response is invalid');
      if (failure.kind !== SteamAuthErrorKind.RESPONSE) throw failure;
      throw new SteamAuthError(failure.message, failure.kind, failure.stage, failure.code,
        beginResponse.statusCode, 0, '', 0, 0, beginResponse.body.length);
    }
    this.pending.set(request.accountId, pending);
    this.knownSteamIds.set(request.accountId, pending.steamId64);
    this.logger.info(TAG, 'password sign-in challenge received; challenge=' + String(pending.challenge));
    if (pending.challenge !== SteamAuthChallengeKind.NONE) {
      return this.challengeResult(pending);
    }
    return this.poll(request.accountId);
  }

  async beginQr(_request: SteamAuthRequest): Promise<SteamAuthResult> {
    this.logger.warn(TAG, 'QR sign-in rejected; reason=http_transport_unsupported');
    throw new Error('HTTP Steam authentication does not support QR sign-in; use a CM WebSocket');
  }

  async completeChallenge(request: SteamAuthRequest): Promise<SteamAuthResult> {
    const pending = this.requirePending(request.accountId);
    this.logger.info(TAG, 'authentication challenge completion started; challenge=' +
      String(pending.challenge));
    if (pending.challenge === SteamAuthChallengeKind.QR_SCAN) {
      return this.poll(request.accountId);
    }
    if (pending.challenge === SteamAuthChallengeKind.MOBILE_CONFIRMATION) {
      // 2026-09-28: Steam 响应通常同时提供手机确认与令牌码; 用户嫌手机确认繁琐时
      // 可直接提交手机令牌 5 位码 (code_type 3)。空码仍按原样轮询等手机确认。
      if (request.challengeCode.trim().length === 0) {
        return this.poll(request.accountId);
      }
    }
    let code = request.challengeCode.trim();
    if (code.length === 0 && pending.challenge === SteamAuthChallengeKind.TWO_FACTOR) {
      const secret = request.credentials.sharedSecret.length > 0
        ? request.credentials.sharedSecret
        : pending.credentials.sharedSecret;
      if (secret.length > 0) {
        code = await SteamTotp.generateForTime(secret, this.clock.nowEpochSeconds(), this.crypto);
      }
    }
    if (code.length === 0) {
      const message = pending.challenge === SteamAuthChallengeKind.TWO_FACTOR
        ? 'Steam Guard device code is required when no shared secret is configured'
        : 'Steam Guard email code is required';
      throw new SteamAuthError(message, SteamAuthErrorKind.VALIDATION, 'challenge_completion');
    }
    const codeType = pending.challenge === SteamAuthChallengeKind.EMAIL_CODE ? '2' : '3';
    const updateResponse = await this.postForm(
      SteamAuthEndpoint.UPDATE_STEAM_GUARD_CODE,
      [
        new SteamFormField('client_id', pending.clientId),
        new SteamFormField('steamid', pending.steamId64),
        new SteamFormField('code', code),
        new SteamFormField('code_type', codeType)
      ],
      'update_steam_guard_code'
    );
    this.responseObject(updateResponse, 'Steam Guard code update');
    this.logger.info(TAG, 'authentication challenge submitted; codePresent=true');
    // Steam needs time to mint tokens after a guard code is accepted. The first
    // poll often returns the previous challenge state while the code is still
    // being processed; the client polls again instead of re-asking the user for
    // a code that Steam has already consumed.
    return await this.pollAfterChallengeCode(request.accountId);
  }

  /**
   * Polls after a guard code submission until Steam settles the session.
   * PollAuthSessionStatus legitimately reports a pending result while the
   * submitted code is processed; a single poll would then surface the old
   * challenge even though Steam has already accepted the code.
   */
  private async pollAfterChallengeCode(accountId: string): Promise<SteamAuthResult> {
    const maxAttempts: number = 10;
    const intervalMilliseconds: number = 1000;
    for (let attempt: number = 0; attempt < maxAttempts; attempt++) {
      const result: SteamAuthResult = await this.poll(accountId);
      if (result.session.state === SteamSessionState.LOGGED_ON ||
        result.session.state === SteamSessionState.ERROR) {
        return result;
      }
      if (attempt < maxAttempts - 1) {
        await this.delay(intervalMilliseconds);
      }
    }
    return await this.poll(accountId);
  }

  private async delay(milliseconds: number): Promise<void> {
    await new Promise<void>((resolve: () => void): void => {
      setTimeout((): void => {
        resolve();
      }, milliseconds);
    });
  }

  async poll(accountId: string): Promise<SteamAuthResult> {
    this.logger.debug(TAG, 'authentication poll started');
    const pending = this.requirePending(accountId);
    try {
      const pollResponse = await this.postForm(
        SteamAuthEndpoint.POLL_AUTH_SESSION,
        [
          new SteamFormField('client_id', pending.clientId),
          new SteamFormField('request_id', pending.requestId)
        ],
        'poll_auth_session'
      );
      const pollObject = this.responseObject(pollResponse, 'poll auth session');
      const accessToken = this.optionalString(pollObject, 'access_token');
      const refreshToken = this.optionalString(pollObject, 'refresh_token');
      const oauthToken = this.optionalString(pollObject, 'oauth_token');
      const newGuardData = this.optionalString(pollObject, 'new_guard_data');
      if (refreshToken.length > 0) {
        const credentials = this.completedCredentials(
          pending,
          accessToken,
          refreshToken,
          oauthToken,
          newGuardData
        );
        this.pending.delete(accountId);
        this.knownCredentials.set(accountId, credentials);
        this.logger.info(TAG, 'authentication poll completed; loggedOn=true');
        return new SteamAuthResult(
          this.loggedOnSnapshot(pending, 0),
          credentials,
          SteamAuthChallengeKind.NONE
        );
      }
      const errorCode = this.optionalNumber(pollObject, 'eresult');
      const errorMessage = this.optionalString(pollObject, 'error_message');
      const hasTerminalError = errorMessage.length > 0 || !this.isPollPendingResult(errorCode);
      const state = hasTerminalError
        ? SteamSessionState.ERROR
        : this.challengeState(pending.challenge);
      if (hasTerminalError) {
        this.pending.delete(accountId);
      }
      this.logger.debug(TAG, 'authentication poll result; state=' + String(state) +
        ', errorCode=' + errorCode.toString() +
        ', errorMessagePresent=' + String(errorMessage.length > 0));
      return new SteamAuthResult(
        new SteamSessionSnapshot(
          pending.accountId,
          state,
          pending.steamId64,
          '',
          0,
          0,
          errorCode,
          errorMessage,
          hasTerminalError ? String(SteamAuthErrorKind.SERVER) : '',
          hasTerminalError ? 'poll_auth_session' : ''
        ),
        new SteamCredentialSet(),
        pending.challenge,
        pending.flow,
        pending.challengeUrl
      );
    } catch (error) {
      const normalized: Error = error instanceof Error
        ? error
        : new Error('Steam authentication poll failed');
      const authError: SteamAuthError = SteamAuthErrorSupport.normalize(
        normalized,
        SteamAuthErrorKind.AUTHENTICATION,
        'poll_auth_session',
        'Steam authentication poll failed'
      );
      if (authError.kind === SteamAuthErrorKind.SERVER ||
        authError.kind === SteamAuthErrorKind.RESPONSE ||
        authError.kind === SteamAuthErrorKind.PROTOCOL) {
        this.pending.delete(accountId);
      }
      throw authError;
    }
  }

  async cancelPendingAuth(accountId: string): Promise<void> {
    this.pending.delete(accountId.trim());
    this.logger.info(TAG, 'pending authentication cancelled');
  }

  async close(accountId: string): Promise<void> {
    const normalizedId = accountId.trim();
    this.pending.delete(normalizedId);
    this.knownSteamIds.delete(normalizedId);
    this.knownCredentials.delete(normalizedId);
    this.logger.info(TAG, 'HTTP authentication service closed');
  }

  async refresh(accountId: string, refreshToken: string): Promise<SteamAuthResult> {
    this.logger.info(TAG, 'access token refresh started; refreshTokenPresent=' +
      String(refreshToken.trim().length > 0));
    if (refreshToken.trim().length === 0) {
      throw new Error('Refresh token is required');
    }
    const fields: Array<SteamFormField> = [
      new SteamFormField('refresh_token', refreshToken)
    ];
    const knownSteamId = this.knownSteamIds.get(accountId) ?? SteamJwt.subjectClaim(refreshToken);
    if (knownSteamId !== undefined && knownSteamId.length > 0) {
      fields.push(new SteamFormField('steamid', knownSteamId));
    }
    const response = await this.postForm(
      SteamAuthEndpoint.GENERATE_ACCESS_TOKEN,
      fields,
      'access_token_refresh'
    );
    const responseObject = this.responseObject(response, 'access token refresh');
    const accessToken = this.optionalString(responseObject, 'access_token');
    const nextRefreshToken = this.optionalString(responseObject, 'refresh_token');
    if (accessToken.length === 0) {
      throw new Error('Steam refresh response did not contain an access token');
    }
    const stored = this.knownCredentials.get(accountId);
    const credentials = new SteamCredentialSet(
      stored === undefined ? '' : stored.accountName,
      '',
      nextRefreshToken.length > 0 ? nextRefreshToken : refreshToken,
      stored === undefined ? '' : stored.loginKey,
      stored === undefined ? '' : stored.sharedSecret,
      stored === undefined ? '' : stored.identitySecret,
      stored === undefined ? '' : stored.deviceId,
      accessToken,
      this.optionalString(responseObject, 'oauth_token'),
      stored === undefined ? '' : stored.guardData
    );
    const responseSteamId = this.optionalString(responseObject, 'steamid');
    const sessionSteamId = responseSteamId.length > 0
      ? responseSteamId
      : (knownSteamId === undefined ? '' : knownSteamId);
    this.knownSteamIds.set(accountId, sessionSteamId);
    this.knownCredentials.set(accountId, credentials);
    const session = new SteamSessionSnapshot(
      accountId,
      SteamSessionState.LOGGED_ON,
      sessionSteamId,
      '',
      this.clock.nowEpochSeconds(),
      0,
      0
    );
    this.logger.info(TAG, 'access token refresh completed; loggedOn=true');
    return new SteamAuthResult(session, credentials, SteamAuthChallengeKind.NONE);
  }

  private beginFields(
    request: SteamAuthRequest,
    rsaKey: SteamRsaPublicKey,
    encryptedPassword: Uint8Array
  ): Array<SteamFormField> {
    // 2026-09-28 真机实测: 表单(form-urlencoded)模式下 Steam 拒绝 device_details 字段
    // (HTTP 400 "verify that all required parameters are being sent"), 任何 JSON 内容变体均被拒,
    // 省略该字段即通过。platform_type 单独字段保留, Guard 挑战路由不受影响。
    const fields: Array<SteamFormField> = [
      new SteamFormField('account_name', request.credentials.accountName),
      new SteamFormField('encrypted_password', SteamBase64.encode(encryptedPassword)),
      new SteamFormField('encryption_timestamp', rsaKey.timestamp.toString()),
      new SteamFormField('remember_login', '1'),
      new SteamFormField('persistence', '1'),
      new SteamFormField('website_id', 'Community'),
      new SteamFormField('platform_type', '3'),
      new SteamFormField('language', '0'),
      new SteamFormField('qos_level', '2')
    ];
    if (request.credentials.guardData.length > 0) {
      fields.push(new SteamFormField('guard_data', request.credentials.guardData));
    }
    return fields;
  }

  private async postForm(
    endpoint: SteamAuthEndpoint,
    fields: Array<SteamFormField>,
    stage: string
  ): Promise<SteamAuthHttpResponse> {
    try {
      return await this.transport.postForm(endpoint, fields);
    } catch (error) {
      throw SteamAuthErrorSupport.normalize(
        error,
        SteamAuthErrorKind.TRANSPORT,
        stage,
        'Steam authentication transport failed'
      );
    }
  }

  private parseRsaPublicKey(value: SteamJsonValue): SteamRsaPublicKey {
    const modulus = this.requiredString(value, 'publickey_mod');
    const exponent = this.requiredString(value, 'publickey_exp');
    const timestamp = this.requiredNumber(value, 'timestamp');
    return new SteamRsaPublicKey(modulus, exponent, timestamp);
  }

  private parsePendingSession(request: SteamAuthRequest, value: SteamJsonValue): SteamPendingAuthSession {
    const clientId = this.requiredString(value, 'client_id');
    const requestId = this.requiredString(value, 'request_id');
    const steamId64 = this.optionalString(value, 'steamid');
    const interval = this.optionalNumber(value, 'interval');
    const challenge = this.parseChallenge(value, request.credentials);
    const createdAtEpochSeconds = this.clock.nowEpochSeconds();
    return new SteamPendingAuthSession(
      request.accountId,
      request.credentials,
      clientId,
      requestId,
      steamId64,
      challenge,
      interval > 0 ? interval : 5,
      createdAtEpochSeconds,
      createdAtEpochSeconds + SteamAuthPendingSnapshot.MAX_LIFETIME_SECONDS
    );
  }

  private currentPending(accountId: string): SteamPendingAuthSession | null {
    const pending = this.pending.get(accountId);
    if (pending === undefined) {
      return null;
    }
    if (!pending.toSnapshot().isValidFor(accountId, this.clock.nowEpochSeconds())) {
      this.pending.delete(accountId);
      return null;
    }
    return pending;
  }

  private requirePending(accountId: string): SteamPendingAuthSession {
    const pending = this.currentPending(accountId);
    if (pending === null) {
      throw new Error('No pending Steam authentication session');
    }
    return pending;
  }

  private parseChallenge(value: SteamJsonValue, credentials: SteamCredentialSet): SteamAuthChallengeKind {
    const confirmations = value.getProperty('allowed_confirmations');
    let email = false;
    let twoFactor = false;
    let mobile = false;
    if (confirmations !== null && confirmations.kind === SteamJsonValueKind.ARRAY) {
      for (let index = 0; index < confirmations.items.length; index++) {
        const confirmation = confirmations.items[index];
        if (confirmation.kind !== SteamJsonValueKind.OBJECT) {
          continue;
        }
        const type = this.optionalNumber(confirmation, 'confirmation_type');
        if (type === 2) {
          email = true;
        } else if (type === 3) {
          twoFactor = true;
        } else if (type === 4 || type === 5) {
          mobile = true;
        }
      }
    }
    if (mobile) {
      return SteamAuthChallengeKind.MOBILE_CONFIRMATION;
    }
    if (twoFactor || (credentials.sharedSecret.length > 0 && !email)) {
      return SteamAuthChallengeKind.TWO_FACTOR;
    }
    if (email) {
      return SteamAuthChallengeKind.EMAIL_CODE;
    }
    return SteamAuthChallengeKind.NONE;
  }

  private challengeResult(pending: SteamPendingAuthSession): SteamAuthResult {
    return new SteamAuthResult(
      new SteamSessionSnapshot(
        pending.accountId,
        this.challengeState(pending.challenge),
        pending.steamId64,
        '',
        0,
        0,
        0
      ),
      new SteamCredentialSet(),
      pending.challenge
    );
  }

  private completedCredentials(
    pending: SteamPendingAuthSession,
    accessToken: string,
    refreshToken: string,
    oauthToken: string,
    guardData: string = ''
  ): SteamCredentialSet {
    return new SteamCredentialSet(
      pending.credentials.accountName,
      '',
      refreshToken,
      pending.credentials.loginKey,
      pending.credentials.sharedSecret,
      pending.credentials.identitySecret,
      pending.credentials.deviceId,
      accessToken,
      oauthToken,
      guardData.length > 0 ? guardData : pending.credentials.guardData
    );
  }

  private loggedOnSnapshot(pending: SteamPendingAuthSession, errorCode: number): SteamSessionSnapshot {
    return new SteamSessionSnapshot(
      pending.accountId,
      SteamSessionState.LOGGED_ON,
      pending.steamId64,
      '',
      this.clock.nowEpochSeconds(),
      0,
      errorCode
    );
  }

  private challengeState(challenge: SteamAuthChallengeKind): SteamSessionState {
    if (challenge === SteamAuthChallengeKind.QR_SCAN) {
      return SteamSessionState.NEEDS_QR_SCAN;
    }
    if (challenge === SteamAuthChallengeKind.EMAIL_CODE) {
      return SteamSessionState.NEEDS_EMAIL_CODE;
    }
    if (challenge === SteamAuthChallengeKind.TWO_FACTOR) {
      return SteamSessionState.NEEDS_TWO_FACTOR;
    }
    if (challenge === SteamAuthChallengeKind.MOBILE_CONFIRMATION) {
      return SteamSessionState.NEEDS_MOBILE_CONFIRMATION;
    }
    return SteamSessionState.CONNECTING;
  }

  /**
   * PollAuthSessionStatus uses an EResult-shaped field for both the current
   * waiting state and terminal failures. A missing/OK/Fail result without a
   * token or server message is still a live challenge; other result codes are
   * actionable failures and must end the pending session.
   */
  private isPollPendingResult(errorCode: number): boolean {
    return errorCode === 0 || errorCode === 1 || errorCode === 2;
  }

  private responseObject(response: SteamAuthHttpResponse, operation: string): SteamJsonValue {
    // 2026-09-28: keystone 错误头检查（验证码被拒等错误以 HTTP 200 + x-error 头返回,
    // 原实现只看 statusCode, 把拒绝当成功, 造成无限要求重输验证码的循环）
    const keystoneError = response.keystoneError();
    if (keystoneError.length > 0) {
      throw new SteamAuthError(
        operation + ' rejected: ' + keystoneError,
        SteamAuthErrorKind.SERVER,
        operation
      );
    }
    if (response.statusCode < 200 || response.statusCode >= 300) {
      let remoteCode = 0;
      let remoteMessage = '';
      try {
        const root = SteamJsonParser.parse(response.body, this.logger);
        const candidate = root.kind === SteamJsonValueKind.OBJECT
          ? root.getProperty('response')
          : null;
        if (candidate !== null && candidate.kind === SteamJsonValueKind.OBJECT) {
          remoteCode = this.optionalNumber(candidate, 'eresult');
          remoteMessage = this.optionalString(candidate, 'error_message');
        }
      } catch {
        remoteCode = 0;
        remoteMessage = '';
      }
      const suffix = remoteMessage.length > 0 ? ': ' + remoteMessage : '';
      throw new SteamAuthError(
        operation + ' failed with HTTP status ' + response.statusCode.toString() + suffix,
        SteamAuthErrorKind.SERVER,
        operation,
        remoteCode,
        response.statusCode,
        0,
        '',
        0,
        0,
        0,
        remoteMessage
      );
    }
    const root = SteamJsonParser.parse(response.body, this.logger);
    if (root.kind !== SteamJsonValueKind.OBJECT) {
      throw new Error(operation + ' returned a non-object response');
    }
    const value = root.getProperty('response');
    if (value === null || value.kind !== SteamJsonValueKind.OBJECT) {
      throw new Error(operation + ' response object is missing');
    }
    const success = value.getProperty('success');
    if (success !== null && success.kind === SteamJsonValueKind.NUMBER && success.numberValue !== '1') {
      this.throwRejectedResponse(value, operation, response.statusCode);
    }
    if (success !== null && success.kind === SteamJsonValueKind.BOOLEAN && !success.booleanValue) {
      this.throwRejectedResponse(value, operation, response.statusCode);
    }
    return value;
  }

  private throwRejectedResponse(value: SteamJsonValue, operation: string, httpStatusCode: number): void {
    const code = this.optionalNumber(value, 'eresult');
    const remoteMessage = this.optionalString(value, 'error_message');
    const suffix = remoteMessage.length > 0 ? ': ' + remoteMessage : '';
    throw new SteamAuthError(
      operation + ' was rejected by Steam' + suffix,
      SteamAuthErrorKind.SERVER,
      operation,
      code,
      httpStatusCode,
      0,
      '',
      0,
      0,
      0,
      remoteMessage
    );
  }

  private requireAccountName(credentials: SteamCredentialSet): void {
    if (credentials.accountName.trim().length === 0) {
      throw new Error('Steam account name is required');
    }
  }

  private requiredString(value: SteamJsonValue, name: string): string {
    const result = this.optionalString(value, name);
    if (result.length === 0) {
      throw new Error('Steam response field is missing: ' + name);
    }
    return result;
  }

  private optionalString(value: SteamJsonValue, name: string, fallback: string = ''): string {
    const property = value.getProperty(name);
    if (property === null) {
      return fallback;
    }
    if (property.kind === SteamJsonValueKind.STRING) {
      return property.stringValue;
    }
    if (property.kind === SteamJsonValueKind.NUMBER) {
      return property.numberValue;
    }
    return fallback;
  }

  private requiredNumber(value: SteamJsonValue, name: string): number {
    const property = value.getProperty(name);
    if (property === null) {
      throw new Error('Steam response number is missing: ' + name);
    }
    const result = this.optionalNumber(value, name);
    if (result < 0) {
      throw new Error('Steam response number is invalid: ' + name);
    }
    return result;
  }

  private optionalNumber(value: SteamJsonValue, name: string): number {
    const property = value.getProperty(name);
    if (property === null) {
      return 0;
    }
    if (property.kind === SteamJsonValueKind.NUMBER) {
      return Number(property.numberValue);
    }
    if (property.kind === SteamJsonValueKind.STRING) {
      return Number(property.stringValue);
    }
    return 0;
  }
}
