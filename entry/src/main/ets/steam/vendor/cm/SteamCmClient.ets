import {
  IClock,
  ISteamCmClient,
  SteamCmEvent,
  SteamCmEventListener,
  SteamCmEventType,
  SteamCmUnifiedMessage,
  SteamCmUnifiedMessageListener
} from '../contracts/SteamPlatformContracts';
import {
  ISteamCmScheduler,
  ISteamCmWireTransport,
  SteamCmClientOptions
} from '../contracts/SteamCmContracts';
import {
  SteamClientHelloCodec,
  SteamClientHelloRequest
} from '../protocol/SteamClientHelloCodec';
import {
  SteamClientGamesPlayedCodec,
  SteamGamePlayedMessage
} from '../protocol/SteamClientGamesPlayedCodec';
import {
  SteamClientItemAnnouncementsCodec,
  SteamItemAnnouncement
} from '../protocol/SteamClientItemAnnouncementsCodec';
import {
  SteamClientFriendsCodec,
  SteamFriendsList,
  SteamPersonaStates,
  SteamPlayerNicknames
} from '../protocol/SteamClientFriendsCodec';
import { SteamClientWalletInfoCodec } from '../protocol/SteamClientWalletInfoCodec';
import { SteamClientSessionTokenCodec } from '../protocol/SteamClientSessionTokenCodec';
import {
  SteamClientLogonCodec,
  SteamClientLogonRequest,
  SteamClientLogonResponse,
  SteamCmDecodedMessage,
  SteamCmMessageType,
  SteamCmProtoHeader,
  SteamCmProtocol,
  SteamCmResultCode
} from '../protocol/SteamCmProtocol';
import {
  SteamCardDropEvent,
  SteamCredentialSet,
  SteamSessionSnapshot,
  SteamSessionState
} from '../models/SteamModels';
import { ISteamCoreLogger, SteamCoreLogSupport } from '../contracts/SteamCoreLogger';
import { SteamVarint } from '../protocol/SteamVarint';

const TAG: string = 'CmClient';

class SteamCmPendingRequest {
  readonly jobId: string;
  readonly targetJobName: string;
  readonly resolve: (message: SteamCmDecodedMessage) => void;
  readonly reject: (error: Error) => void;
  timeoutHandle: number = -1;

  constructor(
    jobId: string,
    targetJobName: string,
    resolve: (message: SteamCmDecodedMessage) => void,
    reject: (error: Error) => void
  ) {
    this.jobId = jobId;
    this.targetJobName = targetJobName;
    this.resolve = resolve;
    this.reject = reject;
  }
}

/**
 * Platform-independent CM session. The host supplies only binary transport
 * and timer scheduling; credentials remain in memory and never enter events.
 */
export class SteamCmClient implements ISteamCmClient {
  private static readonly SHORTCUT_GAME_ID: string = '18446744070488326144';
  private readonly accountId: string;
  private readonly credentials: SteamCredentialSet;
  private readonly wire: ISteamCmWireTransport;
  private readonly scheduler: ISteamCmScheduler;
  private readonly clock: IClock;
  private readonly options: SteamCmClientOptions;
  private readonly logger: ISteamCoreLogger;
  private readonly listeners: Array<SteamCmEventListener> = [];
  private readonly unifiedListeners: Array<SteamCmUnifiedMessageListener> = [];
  private readonly pendingRequests: Map<string, SteamCmPendingRequest> =
    new Map<string, SteamCmPendingRequest>();
  /** callProto 的响应类型配对: jobId → 期望的响应 EMsg。 */
  private readonly pendingProtoResponses: Map<string, number> =
    new Map<string, number>();
  private nextJobId: string = '1';
  private currentSession: SteamSessionSnapshot;
  private transportConnected: boolean = false;
  private manualDisconnect: boolean = true;
  private heartbeatSeconds: number;
  private heartbeatTimer: number = -1;
  private reconnectTimer: number = -1;
  private reconnectAttempt: number = 0;
  private clientSessionId: number = 0;
  private sessionTokenValue: string = '';
  private transportClosePromise: Promise<void> | null = null;
  private transportCloseInProgress: boolean = false;
  private connectionAttempt: number = 0;

  constructor(
    accountId: string,
    credentials: SteamCredentialSet,
    wire: ISteamCmWireTransport,
    scheduler: ISteamCmScheduler,
    clock: IClock,
    options: SteamCmClientOptions,
    logger: ISteamCoreLogger | null = null
  ) {
    if (accountId.trim().length === 0) {
      throw new Error('Steam CM account id is required');
    }
    this.accountId = accountId;
    this.credentials = credentials;
    this.wire = wire;
    this.scheduler = scheduler;
    this.clock = clock;
    this.options = options;
    this.logger = logger === null ? options.logger : logger;
    this.heartbeatSeconds = options.heartbeatFallbackSeconds;
    this.currentSession = new SteamSessionSnapshot(accountId);
    this.bindTransportListeners();
    this.logger.info(TAG, 'CM client constructed; maxGames=' + options.maxGamesPlayedConcurrently.toString());
  }

  get session(): SteamSessionSnapshot {
    return this.currentSession;
  }

  /** Returns the in-memory CM token without exposing it through snapshots or events. */
  get sessionToken(): string {
    return this.sessionTokenValue;
  }

  async connect(): Promise<void> {
    this.manualDisconnect = false;
    if (this.currentSession.state === SteamSessionState.LOGGED_ON ||
      this.currentSession.state === SteamSessionState.CONNECTED ||
      this.currentSession.state === SteamSessionState.CONNECTING) {
      this.logger.debug(TAG, 'CM connect skipped; state=' + String(this.currentSession.state));
      return;
    }
    if (this.currentSession.state === SteamSessionState.RECONNECTING && this.reconnectTimer >= 0) {
      this.logger.debug(TAG, 'CM connect skipped; reconnect already scheduled');
      return;
    }
    if (this.isTerminalAuthenticationState()) {
      throw new Error('Steam CM authentication requires a new credential flow');
    }
    this.reconnectAttempt = 0;
    this.logger.info(TAG, 'CM connect started; reconnect=false');
    await this.openTransport(false);
  }

  async disconnect(): Promise<void> {
    this.logger.info(TAG, 'CM disconnect started');
    this.manualDisconnect = true;
    this.connectionAttempt++;
    this.cancelHeartbeatTimer();
    this.cancelReconnectTimer();
    this.transportConnected = false;
    this.clientSessionId = 0;
    this.sessionTokenValue = '';
    this.rejectPendingRequests(new Error('Steam CM disconnected'));
    try {
      await this.closeTransportQuietly();
    } finally {
      this.updateSession(SteamSessionState.DISCONNECTED, 0);
      this.emit(SteamCmEventType.DISCONNECTED, 'Steam CM disconnected');
      this.logger.info(TAG, 'CM disconnect completed');
    }
  }

  async playGames(appIds: Array<string>, gameName: string): Promise<boolean> {
    if (!this.isLoggedOn()) {
      this.logger.warn(TAG, 'games played update rejected; reason=not_logged_on');
      return false;
    }
    const uniqueAppIds = this.uniqueAppIds(appIds);
    let customGameName = gameName.trim();
    const maxGames = this.options.maxGamesPlayedConcurrently;
    if (customGameName.length === 0 && uniqueAppIds.length > maxGames) {
      throw new Error('Steam allows at most ' + maxGames.toString() + ' concurrently played games');
    }
    const games: Array<SteamGamePlayedMessage> = [];
    this.logger.info(TAG, 'games played update started; requestedCount=' + uniqueAppIds.length.toString() +
      ', customNamePresent=' + String(customGameName.length > 0));
    if (customGameName.length > 0) {
      const clearBody = SteamClientGamesPlayedCodec.encode([], this.options.clientOsType);
      await this.sendProtoMessage(SteamCmMessageType.CLIENT_GAMES_PLAYED_WITH_DATA_BLOB, clearBody);
      games.push(new SteamGamePlayedMessage(SteamCmClient.SHORTCUT_GAME_ID, customGameName));
    }
    for (let index = 0; index < uniqueAppIds.length; index++) {
      if (games.length >= maxGames) {
        if (customGameName.length === 0) {
          throw new Error('Steam allows at most ' + maxGames.toString() + ' concurrently played games');
        }
        customGameName = '';
        games.splice(0, 1);
      }
      games.push(new SteamGamePlayedMessage(uniqueAppIds[index]));
    }
    const body = SteamClientGamesPlayedCodec.encode(games, this.options.clientOsType);
    await this.sendProtoMessage(SteamCmMessageType.CLIENT_GAMES_PLAYED_WITH_DATA_BLOB, body);
    this.logger.info(TAG, 'games played update completed; sentCount=' + games.length.toString());
    return true;
  }

  async stopGames(): Promise<boolean> {
    return this.playGames([], '');
  }

  async requestItemAnnouncements(): Promise<boolean> {
    if (!this.isLoggedOn()) {
      this.logger.warn(TAG, 'item announcement request rejected; reason=not_logged_on');
      return false;
    }
    await this.sendProtoMessage(SteamCmMessageType.CLIENT_REQUEST_ITEM_ANNOUNCEMENTS, new Uint8Array(0));
    this.logger.debug(TAG, 'item announcement request sent');
    return true;
  }

  async requestFriendsPersonaData(steamIds64: Array<string>, personaStateRequested: number): Promise<boolean> {
    if (!this.isLoggedOn()) {
      this.logger.warn(TAG, 'friend persona data request rejected; reason=not_logged_on');
      return false;
    }
    const uniqueIds: Array<string> = this.uniqueSteamIds64(steamIds64);
    if (uniqueIds.length <= 0) {
      this.logger.debug(TAG, 'friend persona data request skipped; reason=no_targets');
      return false;
    }
    const body = SteamClientFriendsCodec.encodeRequestFriendData(personaStateRequested, uniqueIds);
    await this.sendProtoMessage(SteamCmMessageType.CLIENT_REQUEST_FRIEND_DATA, body);
    this.logger.debug(TAG, 'friend persona data request sent; targets=' + uniqueIds.length.toString());
    return true;
  }

  async call(targetJobName: string, body: Uint8Array): Promise<SteamCmDecodedMessage> {
    if (!this.isLoggedOn()) {
      throw new Error('Steam unified message call requires a logged-on CM session');
    }
    const normalizedName: string = targetJobName.trim();
    if (normalizedName.length <= 0) {
      throw new Error('Steam unified message name is required');
    }
    const jobId: string = this.nextJobId;
    this.nextJobId = SteamVarint.incrementUInt64(jobId);
    const frame: Uint8Array = this.buildUnifiedMessage(normalizedName, body, jobId);
    return new Promise<SteamCmDecodedMessage>((resolve: (message: SteamCmDecodedMessage) => void,
      reject: (error: Error) => void): void => {
      const pending: SteamCmPendingRequest = new SteamCmPendingRequest(jobId, normalizedName, resolve, reject);
      pending.timeoutHandle = this.scheduler.schedule(this.options.requestTimeoutMilliseconds, (): void => {
        this.rejectPendingRequest(jobId,
          new Error('Steam unified message request timed out; method=' + normalizedName));
      });
      this.pendingRequests.set(jobId, pending);
      this.wire.send(frame).catch((error: Error): void => {
        this.rejectPendingRequest(jobId, error);
      });
    });
  }

  /**
   * W6: 发一条经典 EMsg (非 unified) 并按 proto header 的 jobId 配对响应。
   * 用于 Client.PICSProductInfo (5202→5203) 等请求。响应经 handleDecodedMessage 路由。
   */
  callProto(messageType: number, responseType: number, body: Uint8Array,
    stage: string): Promise<SteamCmDecodedMessage> {
    if (!this.isLoggedOn()) {
      return Promise.reject(new Error('Steam EMsg call requires a logged-on CM session'));
    }
    const jobId: string = this.nextJobId;
    this.nextJobId = SteamVarint.incrementUInt64(jobId);
    const frame: Uint8Array = this.buildProtoMessage(messageType, body, true, jobId);
    return new Promise<SteamCmDecodedMessage>((resolve: (message: SteamCmDecodedMessage) => void,
      reject: (error: Error) => void): void => {
      const pending: SteamCmPendingRequest = new SteamCmPendingRequest(jobId, stage, resolve, reject);
      pending.timeoutHandle = this.scheduler.schedule(this.options.requestTimeoutMilliseconds, (): void => {
        this.pendingProtoResponses.delete(jobId);
        this.rejectPendingRequest(jobId,
          new Error('Steam EMsg request timed out; stage=' + stage));
      });
      this.pendingProtoResponses.set(jobId, responseType);
      this.pendingRequests.set(jobId, pending);
      this.wire.send(frame).catch((error: Error): void => {
        this.pendingProtoResponses.delete(jobId);
        this.rejectPendingRequest(jobId, error);
      });
    });
  }

  async notify(targetJobName: string, body: Uint8Array): Promise<void> {    if (!this.isLoggedOn()) {
      throw new Error('Steam unified message notification requires a logged-on CM session');
    }
    const normalizedName: string = targetJobName.trim();
    if (normalizedName.length <= 0) {
      throw new Error('Steam unified message name is required');
    }
    const jobId: string = this.nextJobId;
    this.nextJobId = SteamVarint.incrementUInt64(jobId);
    await this.wire.send(this.buildUnifiedMessage(normalizedName, body, jobId));
  }

  async sendHeartBeat(): Promise<void> {
    if (!this.isLoggedOn()) {
      throw new Error('Steam CM heartbeat requires a logged-on session');
    }
    const frame = this.buildProtoMessage(SteamCmMessageType.CLIENT_HEARTBEAT, new Uint8Array(0), true);
    await this.wire.send(frame);
    this.emit(SteamCmEventType.HEARTBEAT, 'Steam CM heartbeat sent');
    this.logger.debug(TAG, 'CM heartbeat sent');
    this.scheduleHeartbeatTimer();
  }

  addEventListener(listener: SteamCmEventListener): void {
    if (this.listeners.indexOf(listener) < 0) {
      this.listeners.push(listener);
    }
  }

  removeEventListener(listener: SteamCmEventListener): void {
    const index = this.listeners.indexOf(listener);
    if (index >= 0) {
      this.listeners.splice(index, 1);
    }
  }

  addUnifiedMessageListener(listener: SteamCmUnifiedMessageListener): void {
    if (this.unifiedListeners.indexOf(listener) < 0) {
      this.unifiedListeners.push(listener);
    }
  }

  removeUnifiedMessageListener(listener: SteamCmUnifiedMessageListener): void {
    const index: number = this.unifiedListeners.indexOf(listener);
    if (index >= 0) {
      this.unifiedListeners.splice(index, 1);
    }
  }

  dispose(): void {
    this.logger.info(TAG, 'CM client disposed');
    this.manualDisconnect = true;
    this.connectionAttempt++;
    this.cancelHeartbeatTimer();
    this.cancelReconnectTimer();
    this.clientSessionId = 0;
    this.sessionTokenValue = '';
    this.rejectPendingRequests(new Error('Steam CM client disposed'));
    this.wire.clearListeners();
    this.listeners.splice(0, this.listeners.length);
    this.unifiedListeners.splice(0, this.unifiedListeners.length);
  }

  private bindTransportListeners(): void {
    this.wire.onMessage((payload: Uint8Array): void => {
      void this.handleMessage(payload);
    });
    this.wire.onClose((reason: string): void => {
      this.handleTransportClosed(reason);
    });
    this.wire.onError((error: Error): void => {
      this.handleTransportError(error);
    });
  }

  private async openTransport(isReconnect: boolean): Promise<void> {
    if (this.transportConnected) {
      this.logger.debug(TAG, 'CM transport open skipped; already connected');
      return;
    }
    await this.waitForPendingTransportClose();
    if (this.manualDisconnect) {
      throw new Error('Steam CM connection was cancelled');
    }
    const attempt: number = this.connectionAttempt + 1;
    this.connectionAttempt = attempt;
    this.cancelReconnectTimer();
    this.cancelHeartbeatTimer();
    this.clientSessionId = 0;
    this.sessionTokenValue = '';
    if (isReconnect) {
      this.updateSession(SteamSessionState.RECONNECTING, 0);
      this.emit(SteamCmEventType.RECONNECTING, 'Steam CM reconnecting');
    } else {
      this.updateSession(SteamSessionState.CONNECTING, 0);
    }
    this.logger.info(TAG, 'CM transport connect started; reconnect=' + String(isReconnect));
    try {
      await this.wire.connect(this.options.server);
      if (attempt !== this.connectionAttempt || this.manualDisconnect) {
        throw new Error('Steam CM connection was interrupted');
      }
      this.transportConnected = true;
      this.updateSession(SteamSessionState.CONNECTED, 0);
      this.emit(SteamCmEventType.CONNECTED, 'Steam CM connected');
      await this.sendClientHello();
      await this.sendLogon();
      this.logger.info(TAG, 'CM transport connected and logon sent');
    } catch (error) {
      const normalized: Error = error instanceof Error ? error : new Error(String(error));
      this.logger.error(TAG, 'CM transport connect failed; errorType=' +
        SteamCoreLogSupport.errorKind(normalized));
      this.transportConnected = false;
      await this.closeTransportQuietly();
      if (attempt === this.connectionAttempt) {
        this.connectionAttempt++;
        this.updateSession(SteamSessionState.ERROR, 0);
        this.emit(SteamCmEventType.ERROR, 'Steam CM connection failed: ' + normalized.message);
        this.scheduleReconnect();
      }
      throw normalized;
    }
  }

  private async sendLogon(): Promise<void> {
    const accessToken = this.credentials.refreshToken.length > 0
      ? this.credentials.refreshToken
      : this.credentials.accessToken;
    const request = new SteamClientLogonRequest(
      this.credentials.accountName,
      this.credentials.password,
      accessToken,
      this.options.steamId64,
      this.options.protocolVersion,
      0,
      this.options.clientPackageVersion,
      this.options.clientLanguage,
      this.options.clientOsType,
      accessToken.length > 0 || this.credentials.loginKey.length > 0,
      this.options.machineName,
      this.options.authCode,
      this.options.twoFactorCode,
      this.credentials.loginKey,
      this.options.supportsRateLimitResponse
    );
    await this.sendProtoMessage(SteamCmMessageType.CLIENT_LOGON, SteamClientLogonCodec.encode(request));
  }

  private async sendClientHello(): Promise<void> {
    const body = SteamClientHelloCodec.encode(new SteamClientHelloRequest(this.options.protocolVersion));
    await this.sendProtoMessage(SteamCmMessageType.CLIENT_HELLO, body, false);
  }

  private async sendProtoMessage(
    messageType: number,
    body: Uint8Array,
    includeSessionIdentity: boolean = true
  ): Promise<void> {
    const frame = this.buildProtoMessage(messageType, body, includeSessionIdentity);
    await this.wire.send(frame);
  }

  private buildProtoMessage(
    messageType: number,
    body: Uint8Array,
    includeSessionIdentity: boolean,
    jobIdSource: string = '0'
  ): Uint8Array {
    if (!this.transportConnected) {
      throw new Error('Steam CM transport is not connected');
    }
    const steamId64 = includeSessionIdentity
      ? (this.currentSession.steamId64.length > 0 ? this.currentSession.steamId64 : this.options.steamId64)
      : '';
    const sessionId = includeSessionIdentity ? this.clientSessionId : 0;
    const header = new SteamCmProtoHeader(
      steamId64,
      sessionId,
      jobIdSource
    );
    return SteamCmProtocol.encodeProtobufMessage(messageType, header, body);
  }

  private buildUnifiedMessage(targetJobName: string, body: Uint8Array, jobId: string): Uint8Array {
    if (!this.transportConnected) {
      throw new Error('Steam CM transport is not connected');
    }
    const steamId64: string = this.currentSession.steamId64.length > 0
      ? this.currentSession.steamId64
      : this.options.steamId64;
    const header: SteamCmProtoHeader = new SteamCmProtoHeader(
      steamId64,
      this.clientSessionId,
      jobId,
      '0',
      targetJobName
    );
    return SteamCmProtocol.encodeProtobufMessage(
      SteamCmMessageType.SERVICE_METHOD_CALL_FROM_CLIENT,
      header,
      body
    );
  }

  private async handleMessage(payload: Uint8Array): Promise<void> {
    try {
      const messages: Array<SteamCmDecodedMessage> = await SteamCmProtocol.decodeMessagesAsync(
        payload,
        this.options.multiDecompressor
      );
      this.logger.debug(TAG, 'CM message batch received; messageCount=' + messages.length.toString() +
        ', bytes=' + payload.length.toString());
      for (let index = 0; index < messages.length; index++) {
        const message = messages[index];
        this.logger.debug(TAG, 'CM message received; messageType=' + message.messageType.toString());
        this.handleDecodedMessage(message);
      }
    } catch (error) {
      const normalized: Error = error instanceof Error ? error : new Error(String(error));
      this.logger.warn(TAG, 'CM message rejected; errorType=' + SteamCoreLogSupport.errorKind(normalized));
      this.emit(SteamCmEventType.ERROR, 'Steam CM message rejected: ' + normalized.message);
    }
  }

  private handleDecodedMessage(message: SteamCmDecodedMessage): void {
    // W6: callProto 的 EMsg 响应配对 (PICS 5203 等) — 按 jobId 找 pending
    const expectedResponseType: number | undefined =
      this.pendingProtoResponses.get(message.protoHeader.jobIdTarget);
    if (expectedResponseType !== undefined && message.messageType === expectedResponseType) {
      this.pendingProtoResponses.delete(message.protoHeader.jobIdTarget);
      const pending: SteamCmPendingRequest | undefined =
        this.pendingRequests.get(message.protoHeader.jobIdTarget);
      if (pending !== undefined) {
        this.pendingRequests.delete(message.protoHeader.jobIdTarget);
        this.scheduler.cancel(pending.timeoutHandle);
        pending.resolve(message);
      }
      return;
    }
    if (message.messageType === SteamCmMessageType.SERVICE_METHOD_RESPONSE) {
      const jobId: string = message.protoHeader.jobIdTarget;
      const pending: SteamCmPendingRequest | undefined = this.pendingRequests.get(jobId);
      if (pending !== undefined) {
        this.pendingRequests.delete(jobId);
        this.scheduler.cancel(pending.timeoutHandle);
        if (message.protoHeader.eresult !== 0 && message.protoHeader.eresult !== SteamCmResultCode.OK) {
          this.logger.warn(TAG, 'CM unified message failed; method=' + pending.targetJobName +
            ', result=' + message.protoHeader.eresult.toString() +
            ', serverMessagePresent=' + String(message.protoHeader.errorMessage.length > 0));
          pending.reject(new Error(message.protoHeader.errorMessage.length > 0
            ? message.protoHeader.errorMessage
            : 'Steam unified message failed; method=' + pending.targetJobName +
              ', result=' + message.protoHeader.eresult.toString()));
        } else {
          pending.resolve(message);
        }
      }
      return;
    }
    if (message.messageType === SteamCmMessageType.SERVICE_METHOD ||
      message.messageType === SteamCmMessageType.SERVICE_METHOD_SEND_TO_CLIENT) {
      this.logger.debug(TAG, 'CM unified notification received; method=' +
        message.protoHeader.targetJobName + ', bodyBytes=' + message.body.length.toString() +
        ', listenerCount=' + this.unifiedListeners.length.toString());
      this.emitUnifiedMessage(message.protoHeader.targetJobName, message.body);
      return;
    }
    if (message.messageType === SteamCmMessageType.CLIENT_SESSION_TOKEN) {
      const sessionToken = SteamClientSessionTokenCodec.decode(message.body);
      this.sessionTokenValue = sessionToken.token === '0' ? '' : sessionToken.token;
      return;
    }
    if (message.messageType === SteamCmMessageType.CLIENT_WALLET_INFO_UPDATE) {
      const wallet = SteamClientWalletInfoCodec.decode(message.body);
      if (wallet.hasWallet && wallet.balance !== '0') {
        this.logger.debug(TAG, 'CM wallet info updated; currency=' + wallet.currency.toString());
        this.updateSession(this.currentSession.state, this.currentSession.lastErrorCode,
          this.currentSession.steamId64, wallet.balance, wallet.currency);
      }
      return;
    }
    if (message.messageType === SteamCmMessageType.CLIENT_LOGON_RESPONSE) {
      this.handleLogonResponse(SteamClientLogonCodec.decodeResponse(message.body), message.protoHeader);
      return;
    }
    if (message.messageType === SteamCmMessageType.CLIENT_HEARTBEAT) {
      if (this.isLoggedOn()) {
        this.sendHeartBeat().catch((error: Error): void => {
          this.handleTransportError(error);
        });
      }
      return;
    }
    if (message.messageType === SteamCmMessageType.CLIENT_ITEM_ANNOUNCEMENTS) {
      const announcement: SteamItemAnnouncement = SteamClientItemAnnouncementsCodec.decode(message.body);
      this.emit(
        SteamCmEventType.ITEM_ANNOUNCEMENT,
        'Steam inventory item announcement received',
        null,
        0,
        announcement
      );
      return;
    }
    if (message.messageType === SteamCmMessageType.CLIENT_FRIENDS_LIST) {
      const friendsList: SteamFriendsList = SteamClientFriendsCodec.decodeFriendsList(message.body);
      this.emitFriendsPayload(
        SteamCmEventType.FRIENDS_LIST,
        'Steam friends list received',
        friendsList,
        null,
        null
      );
      return;
    }
    if (message.messageType === SteamCmMessageType.CLIENT_PERSONA_STATE) {
      const personaStates: SteamPersonaStates = SteamClientFriendsCodec.decodePersonaStates(message.body);
      this.emitFriendsPayload(
        SteamCmEventType.PERSONA_STATE,
        'Steam persona state updated',
        null,
        personaStates,
        null
      );
      return;
    }
    if (message.messageType === SteamCmMessageType.CLIENT_PLAYER_NICKNAME_LIST) {
      const nicknames: SteamPlayerNicknames = SteamClientFriendsCodec.decodePlayerNicknames(message.body);
      this.emitFriendsPayload(
        SteamCmEventType.PLAYER_NICKNAMES,
        'Steam player nicknames received',
        null,
        null,
        nicknames
      );
      return;
    }
    if (message.messageType === SteamCmMessageType.CLIENT_LOGGED_OFF) {
      const response = SteamClientLogonCodec.decodeResponse(message.body);
      this.handleLoggedOff(response.result);
    }
  }

  private handleLogonResponse(response: SteamClientLogonResponse, responseHeader: SteamCmProtoHeader): void {
    if (response.result === SteamCmResultCode.OK) {
      this.clientSessionId = responseHeader.clientSessionId;
      this.heartbeatSeconds = response.heartbeatSeconds > 0
        ? response.heartbeatSeconds
        : (response.legacyHeartbeatSeconds > 0 ? response.legacyHeartbeatSeconds : this.options.heartbeatFallbackSeconds);
      const steamId64 = response.steamId64.length > 0
        ? response.steamId64
        : (responseHeader.steamId64 !== '0' ? responseHeader.steamId64 : this.options.steamId64);
      this.updateSession(SteamSessionState.LOGGED_ON, 0, steamId64,
        response.walletBalance, response.walletCurrency);
      this.reconnectAttempt = 0;
      this.cancelReconnectTimer();
      this.scheduleHeartbeatTimer();
      this.emit(SteamCmEventType.LOGGED_ON, 'Steam CM login succeeded');
      if (this.options.requestItemAnnouncementsOnLogin) {
        this.requestItemAnnouncements().catch((error: Error): void => {
          this.handleTransportError(error);
        });
      }
      return;
    }
    this.sessionTokenValue = '';
    if (response.result === SteamCmResultCode.ACCOUNT_LOGON_DENIED ||
      response.result === SteamCmResultCode.INVALID_LOGIN_AUTH_CODE ||
      response.result === SteamCmResultCode.ACCOUNT_LOGON_DENIED_NO_MAIL ||
      response.result === SteamCmResultCode.EXPIRED_LOGIN_AUTH_CODE) {
      this.cancelHeartbeatTimer();
      this.updateSession(SteamSessionState.NEEDS_EMAIL_CODE, response.result);
      this.emit(SteamCmEventType.AUTH_CHALLENGE, 'Steam Guard email code is required', null, response.result);
      return;
    }
    if (response.result === SteamCmResultCode.ACCOUNT_LOGIN_DENIED_NEED_TWO_FACTOR ||
      response.result === SteamCmResultCode.TWO_FACTOR_CODE_MISMATCH) {
      this.cancelHeartbeatTimer();
      this.updateSession(SteamSessionState.NEEDS_TWO_FACTOR, response.result);
      this.emit(SteamCmEventType.AUTH_CHALLENGE, 'Steam Guard two-factor code is required', null, response.result);
      return;
    }
    if (response.result === SteamCmResultCode.EXPIRED ||
      response.result === SteamCmResultCode.LOGON_SESSION_REPLACED) {
      this.cancelHeartbeatTimer();
      this.updateSession(SteamSessionState.EXPIRED, response.result);
      this.emit(SteamCmEventType.EXPIRED, 'Steam CM login expired', null, response.result);
      return;
    }
    this.updateSession(SteamSessionState.ERROR, response.result);
    this.emit(SteamCmEventType.ERROR, 'Steam CM login failed', null, response.result);
    this.scheduleReconnect();
  }

  private handleLoggedOff(result: number): void {
    if (this.transportCloseInProgress) {
      return;
    }
    this.connectionAttempt++;
    this.transportConnected = false;
    this.clientSessionId = 0;
    this.sessionTokenValue = '';
    this.rejectPendingRequests(new Error('Steam CM session logged off'));
    this.cancelHeartbeatTimer();
    this.closeTransportQuietly().catch((_error: Error): void => {
      this.logger.warn(TAG, 'CM logged-off transport cleanup failed; errorType=' +
        SteamCoreLogSupport.errorKind(_error));
    });
    if (result === SteamCmResultCode.EXPIRED || result === SteamCmResultCode.LOGON_SESSION_REPLACED) {
      this.updateSession(SteamSessionState.EXPIRED, result);
      this.emit(SteamCmEventType.EXPIRED, 'Steam CM logged off the session', null, result);
      return;
    }
    this.updateSession(SteamSessionState.RECONNECTING, result);
    this.emit(SteamCmEventType.DISCONNECTED, 'Steam CM logged off', null, result);
    this.scheduleReconnect();
  }

  private handleTransportClosed(reason: string): void {
    this.logger.warn(TAG, 'CM transport closed; reasonPresent=' + String(reason.length > 0));
    if (this.transportCloseInProgress) {
      return;
    }
    this.connectionAttempt++;
    this.transportConnected = false;
    this.clientSessionId = 0;
    this.sessionTokenValue = '';
    this.rejectPendingRequests(new Error(reason.length > 0 ? reason : 'Steam CM connection closed'));
    this.cancelHeartbeatTimer();
    if (this.manualDisconnect) {
      this.updateSession(SteamSessionState.DISCONNECTED, 0);
      return;
    }
    if (this.isTerminalAuthenticationState()) {
      this.emit(SteamCmEventType.DISCONNECTED, reason.length > 0 ? reason : 'Steam CM connection closed');
      return;
    }
    this.updateSession(SteamSessionState.RECONNECTING, 0);
    this.emit(SteamCmEventType.DISCONNECTED, reason.length > 0 ? reason : 'Steam CM connection closed');
    this.scheduleReconnect();
  }

  private handleTransportError(error: Error): void {
    this.logger.error(TAG, 'CM transport error; errorType=' + SteamCoreLogSupport.errorKind(error));
    if (this.transportCloseInProgress) {
      return;
    }
    this.connectionAttempt++;
    this.transportConnected = false;
    this.clientSessionId = 0;
    this.sessionTokenValue = '';
    this.rejectPendingRequests(error);
    this.cancelHeartbeatTimer();
    this.emit(SteamCmEventType.ERROR, 'Steam CM transport error: ' + error.message);
    this.closeTransportQuietly().catch((_error: Error): void => {
      this.logger.warn(TAG, 'CM transport error cleanup failed; errorType=' +
        SteamCoreLogSupport.errorKind(_error));
    });
    if (!this.manualDisconnect && !this.isTerminalAuthenticationState()) {
      this.scheduleReconnect();
    }
  }

  private scheduleHeartbeatTimer(): void {
    this.cancelHeartbeatTimer();
    if (!this.isLoggedOn()) {
      return;
    }
    const delay = Math.max(1, this.heartbeatSeconds) * 1000;
    this.logger.debug(TAG, 'CM heartbeat scheduled; delaySeconds=' + (delay / 1000).toString());
    this.heartbeatTimer = this.scheduler.schedule(delay, (): void => {
      this.heartbeatTimer = -1;
      this.sendHeartBeat().catch((error: Error): void => {
        this.handleTransportError(error);
      });
    });
  }

  private scheduleReconnect(): void {
    if (this.manualDisconnect || this.isTerminalAuthenticationState() || this.reconnectTimer >= 0) {
      return;
    }
    if (this.options.maxReconnectAttempts > 0 && this.reconnectAttempt >= this.options.maxReconnectAttempts) {
      this.updateSession(SteamSessionState.ERROR, this.currentSession.lastErrorCode);
      return;
    }
    const exponent = Math.min(this.reconnectAttempt, 10);
    const exponentialDelay = this.options.reconnectInitialDelaySeconds * Math.pow(2, exponent);
    const delaySeconds = Math.min(this.options.reconnectMaxDelaySeconds, exponentialDelay);
    this.reconnectAttempt++;
    this.logger.info(TAG, 'CM reconnect scheduled; attempt=' + this.reconnectAttempt.toString() +
      ', delaySeconds=' + delaySeconds.toString());
    this.updateSession(SteamSessionState.RECONNECTING, 0);
    this.emit(SteamCmEventType.RECONNECTING, 'Steam CM retry scheduled in ' + delaySeconds.toString() + ' seconds');
    this.reconnectTimer = this.scheduler.schedule(delaySeconds * 1000, (): void => {
      this.reconnectTimer = -1;
      this.openTransport(true).catch((error: Error): void => {
        this.handleTransportError(error);
      });
    });
  }

  private cancelHeartbeatTimer(): void {
    if (this.heartbeatTimer >= 0) {
      this.scheduler.cancel(this.heartbeatTimer);
      this.heartbeatTimer = -1;
    }
  }

  private cancelReconnectTimer(): void {
    if (this.reconnectTimer >= 0) {
      this.scheduler.cancel(this.reconnectTimer);
      this.reconnectTimer = -1;
    }
  }

  private updateSession(state: SteamSessionState, errorCode: number, steamId64: string = '',
    walletBalance: string = '', walletCurrency: number = 0): void {
    const previousState: SteamSessionState = this.currentSession.state;
    const nextSteamId = steamId64.length > 0 ? steamId64 : this.currentSession.steamId64;
    const nextErrorCode = errorCode !== 0 || state === SteamSessionState.CONNECTED ||
      state === SteamSessionState.LOGGED_ON || state === SteamSessionState.DISCONNECTED
      ? errorCode
      : this.currentSession.lastErrorCode;
    const connectedAt = state === SteamSessionState.CONNECTED || state === SteamSessionState.LOGGED_ON
      ? (this.currentSession.connectedAtEpochSeconds > 0 ? this.currentSession.connectedAtEpochSeconds : this.clock.nowEpochSeconds())
      : this.currentSession.connectedAtEpochSeconds;
    const nextWalletBalance = walletBalance.length > 0 ? walletBalance : this.currentSession.walletBalance;
    const nextWalletCurrency = walletCurrency > 0 ? walletCurrency : this.currentSession.walletCurrency;
    this.currentSession = new SteamSessionSnapshot(
      this.accountId,
      state,
      nextSteamId,
      this.options.server,
      connectedAt,
      this.currentSession.expiresAtEpochSeconds,
      nextErrorCode,
      this.currentSession.lastErrorMessage,
      this.currentSession.lastErrorType,
      this.currentSession.lastErrorStage,
      this.currentSession.lastErrorHttpStatusCode,
      this.currentSession.lastErrorMessageType,
      this.currentSession.lastErrorHeaderKind,
      this.currentSession.lastErrorFrameBytes,
      this.currentSession.lastErrorHeaderBytes,
      this.currentSession.lastErrorBodyBytes,
      this.currentSession.lastErrorServerMessage,
      nextWalletBalance,
      nextWalletCurrency
    );
    if (previousState !== state || errorCode !== 0) {
      this.logger.state(TAG, String(previousState), String(state), 'errorCode=' + errorCode.toString());
    }
  }

  private emit(
    type: SteamCmEventType,
    message: string,
    drop: SteamCardDropEvent | null = null,
    errorCode: number = 0,
    itemAnnouncement: SteamItemAnnouncement | null = null
  ): void {
    this.logger.debug(TAG, 'CM event emitted; type=' + String(type) + ', errorCode=' + errorCode.toString());
    const event = new SteamCmEvent(type, message, drop, this.currentSession, errorCode, itemAnnouncement);
    for (let index = 0; index < this.listeners.length; index++) {
      try {
        this.listeners[index](event);
      } catch (error) {
        const normalized: Error = error instanceof Error ? error : new Error('CM event listener failed');
        this.logger.warn(TAG, 'CM event listener failed; errorType=' +
          SteamCoreLogSupport.errorKind(normalized));
      }
    }
  }

  private emitFriendsPayload(
    type: SteamCmEventType,
    message: string,
    friendsList: SteamFriendsList | null,
    personaStates: SteamPersonaStates | null,
    playerNicknames: SteamPlayerNicknames | null
  ): void {
    this.logger.debug(TAG, 'CM event emitted; type=' + String(type));
    const event = new SteamCmEvent(
      type,
      message,
      null,
      this.currentSession,
      0,
      null,
      friendsList,
      personaStates,
      playerNicknames
    );
    for (let index = 0; index < this.listeners.length; index++) {
      try {
        this.listeners[index](event);
      } catch (error) {
        const normalized: Error = error instanceof Error ? error : new Error('CM event listener failed');
        this.logger.warn(TAG, 'CM event listener failed; errorType=' +
          SteamCoreLogSupport.errorKind(normalized));
      }
    }
  }

  private emitUnifiedMessage(methodName: string, body: Uint8Array): void {
    const message: SteamCmUnifiedMessage = new SteamCmUnifiedMessage(methodName, body);
    const listeners: Array<SteamCmUnifiedMessageListener> = this.unifiedListeners.slice();
    for (let index: number = 0; index < listeners.length; index++) {
      try {
        listeners[index](message);
      } catch (error) {
        const normalized: Error = error instanceof Error ? error : new Error('Unified message listener failed');
        this.logger.warn(TAG, 'CM unified listener failed; errorType=' + SteamCoreLogSupport.errorKind(normalized));
      }
    }
  }

  private rejectPendingRequest(jobId: string, error: Error): void {
    const pending: SteamCmPendingRequest | undefined = this.pendingRequests.get(jobId);
    if (pending === undefined) {
      return;
    }
    this.pendingRequests.delete(jobId);
    this.scheduler.cancel(pending.timeoutHandle);
    pending.reject(error);
  }

  private rejectPendingRequests(error: Error): void {
    const values: Array<SteamCmPendingRequest> = Array.from(this.pendingRequests.values());
    this.pendingRequests.clear();
    for (let index: number = 0; index < values.length; index++) {
      this.scheduler.cancel(values[index].timeoutHandle);
      values[index].reject(error);
    }
  }

  private isLoggedOn(): boolean {
    return this.transportConnected && this.currentSession.state === SteamSessionState.LOGGED_ON;
  }

  private isTerminalAuthenticationState(): boolean {
    return this.currentSession.state === SteamSessionState.EXPIRED ||
      this.currentSession.state === SteamSessionState.NEEDS_EMAIL_CODE ||
      this.currentSession.state === SteamSessionState.NEEDS_TWO_FACTOR ||
      this.currentSession.state === SteamSessionState.NEEDS_MOBILE_CONFIRMATION;
  }

  private uniqueAppIds(appIds: Array<string>): Array<string> {
    const result: Array<string> = [];
    const known: Map<string, boolean> = new Map<string, boolean>();
    for (let index = 0; index < appIds.length; index++) {
      const appId = appIds[index].trim();
      if (appId.length === 0 || appId === '0' || known.has(appId)) {
        continue;
      }
      known.set(appId, true);
      result.push(appId);
    }
    return result;
  }

  private uniqueSteamIds64(steamIds64: Array<string>): Array<string> {
    const result: Array<string> = [];
    const known: Map<string, boolean> = new Map<string, boolean>();
    for (let index = 0; index < steamIds64.length; index++) {
      const steamId64 = steamIds64[index].trim();
      if (steamId64.length === 0 || steamId64 === '0' || known.has(steamId64)) {
        continue;
      }
      known.set(steamId64, true);
      result.push(steamId64);
    }
    return result;
  }

  private closeTransportQuietly(): Promise<void> {
    if (this.transportClosePromise !== null) {
      return this.transportClosePromise;
    }
    this.transportCloseInProgress = true;
    const closePromise = this.performTransportClose();
    this.transportClosePromise = closePromise;
    return closePromise;
  }

  private async performTransportClose(): Promise<void> {
    try {
      await this.wire.close();
    } catch (_error) {
      this.logger.warn(TAG, 'CM transport close failed; errorType=' +
        SteamCoreLogSupport.errorKind(_error));
    } finally {
      this.transportCloseInProgress = false;
      this.transportClosePromise = null;
    }
  }

  private async waitForPendingTransportClose(): Promise<void> {
    const pending = this.transportClosePromise;
    if (pending !== null) {
      await pending;
    }
  }
}
