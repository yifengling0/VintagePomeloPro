import {
  SteamAuthChallengeKind,
  SteamAuthResult,
  SteamCardDropEvent,
  SteamCredentialSet,
  SteamFarmSnapshot,
  SteamFarmStateRecord,
  SteamGameDrop,
  SteamGameAchievementsResult,
  SteamGameDetailBundle,
  SteamGameDetailInfo,
  SteamGameLibraryResult,
  SteamGameNewsItem,
  SteamInventoryItem,
  SteamInventoryPage,
  SteamInventoryQuery,
  SteamAuthPendingSnapshot,
  SteamSessionSnapshot
} from '../models/SteamModels';
import {
  SteamAuthEndpoint,
  SteamAuthHttpResponse,
  SteamFormField
} from '../auth/SteamAuthProtocol';
import { SteamItemAnnouncement } from '../protocol/SteamClientItemAnnouncementsCodec';
import {
  SteamFriendsList,
  SteamPersonaStates,
  SteamPlayerNicknames
} from '../protocol/SteamClientFriendsCodec';
import { SteamCmDecodedMessage } from '../protocol/SteamCmProtocol';
import { SteamBadgeSyncReport } from '../inventory/SteamBadgeContracts';

export interface IClock {
  nowEpochSeconds(): number;
}

/** Platform crypto adapter. Implementations live outside steam_core. */
export interface IBinaryCrypto {
  hmacSha1(key: Uint8Array, data: Uint8Array): Promise<Uint8Array>;
  rsaEncrypt(publicKeyPem: string, data: Uint8Array): Promise<Uint8Array>;
  aesCbcEncrypt(key: Uint8Array, iv: Uint8Array, data: Uint8Array): Promise<Uint8Array>;
  aesCbcDecrypt(key: Uint8Array, iv: Uint8Array, data: Uint8Array): Promise<Uint8Array>;
}

/** HTTP boundary used by the platform-independent modern auth service. */
export interface ISteamAuthTransport {
  postForm(endpoint: SteamAuthEndpoint, fields: Array<SteamFormField>): Promise<SteamAuthHttpResponse>;
}

/**
 * Resolves the SteamID64 that is intentionally absent from a QR token poll.
 * The host implementation must use a short-lived CM logon and return only
 * the validated decimal identifier; token material remains in the host.
 */
export interface ISteamCmIdentityResolver {
  resolveSteamId64(accountId: string, credentials: SteamCredentialSet): Promise<string>;
}

/** Read-only Web request passed from steam_core to a host adapter. */
export class SteamWebRequest {
  readonly url: string;
  readonly headers: Map<string, string>;

  constructor(url: string, headers: Map<string, string> = new Map<string, string>()) {
    this.url = url;
    this.headers = new Map<string, string>();
    headers.forEach((value: string, key: string): void => {
      this.headers.set(key, value);
    });
  }
}

/** Read-only Web response; cookies remain owned by the host session. */
export class SteamWebResponse {
  readonly statusCode: number;
  readonly headers: Map<string, string>;
  readonly body: string;

  constructor(
    statusCode: number,
    body: string,
    headers: Map<string, string> = new Map<string, string>()
  ) {
    this.statusCode = statusCode;
    this.body = body;
    this.headers = new Map<string, string>();
    headers.forEach((value: string, key: string): void => {
      this.headers.set(key, value);
    });
  }

  isOk(): boolean {
    return this.statusCode >= 200 && this.statusCode < 300;
  }
}

/**
 * Host boundary for a Steam Web session. Access tokens are accepted only for
 * the duration of open() and are never returned in a Web response or snapshot.
 */
export interface ISteamWebTransport {
  open(steamId64: string, accessToken: string): Promise<void>;
  get(request: SteamWebRequest): Promise<SteamWebResponse>;
  close(): Promise<void>;
}

export interface IStateStore {
  load(accountId: string): Promise<SteamFarmStateRecord | null>;
  save(record: SteamFarmStateRecord): Promise<boolean>;
}

export interface ISteamTransport {
  connect(): Promise<void>;
  disconnect(): Promise<void>;
}

export interface ISteamGameTransport extends ISteamTransport {
  playGames(appIds: Array<string>, gameName: string): Promise<boolean>;
  stopGames(): Promise<boolean>;
}

export enum SteamCmEventType {
  CONNECTED = 'connected',
  DISCONNECTED = 'disconnected',
  RECONNECTING = 'reconnecting',
  LOGGED_ON = 'logged_on',
  AUTH_CHALLENGE = 'auth_challenge',
  EXPIRED = 'expired',
  HEARTBEAT = 'heartbeat',
  CARD_DROP = 'card_drop',
  ITEM_ANNOUNCEMENT = 'item_announcement',
  FRIENDS_LIST = 'friends_list',
  PERSONA_STATE = 'persona_state',
  PLAYER_NICKNAMES = 'player_nicknames',
  ERROR = 'error'
}

export class SteamCmEvent {
  readonly type: SteamCmEventType;
  readonly message: string;
  readonly drop: SteamCardDropEvent | null;
  readonly itemAnnouncement: SteamItemAnnouncement | null;
  readonly friendsList: SteamFriendsList | null;
  readonly personaStates: SteamPersonaStates | null;
  readonly playerNicknames: SteamPlayerNicknames | null;
  readonly session: SteamSessionSnapshot | null;
  readonly errorCode: number;

  constructor(
    type: SteamCmEventType,
    message: string = '',
    drop: SteamCardDropEvent | null = null,
    session: SteamSessionSnapshot | null = null,
    errorCode: number = 0,
    itemAnnouncement: SteamItemAnnouncement | null = null,
    friendsList: SteamFriendsList | null = null,
    personaStates: SteamPersonaStates | null = null,
    playerNicknames: SteamPlayerNicknames | null = null
  ) {
    this.type = type;
    this.message = message;
    this.drop = drop;
    this.itemAnnouncement = itemAnnouncement;
    this.friendsList = friendsList;
    this.personaStates = personaStates;
    this.playerNicknames = playerNicknames;
    this.session = session;
    this.errorCode = errorCode;
  }
}

export type SteamCmEventListener = (event: SteamCmEvent) => void;

export class SteamCmUnifiedMessage {
  readonly methodName: string;
  readonly body: Uint8Array;

  constructor(methodName: string, body: Uint8Array) {
    this.methodName = methodName;
    this.body = body.slice();
  }
}

export type SteamCmUnifiedMessageListener = (message: SteamCmUnifiedMessage) => void;

export interface ISteamCmClient extends ISteamGameTransport {
  sendHeartBeat(): Promise<void>;
  requestItemAnnouncements(): Promise<boolean>;
  /** Requests persona data updates for up to 500 friends via EMsg 815. */
  requestFriendsPersonaData(steamIds64: Array<string>, personaStateRequested: number): Promise<boolean>;
  call(targetJobName: string, body: Uint8Array): Promise<SteamCmDecodedMessage>;
  notify(targetJobName: string, body: Uint8Array): Promise<void>;
  addEventListener(listener: SteamCmEventListener): void;
  removeEventListener(listener: SteamCmEventListener): void;
  addUnifiedMessageListener(listener: SteamCmUnifiedMessageListener): void;
  removeUnifiedMessageListener(listener: SteamCmUnifiedMessageListener): void;
  readonly session: SteamSessionSnapshot;
  /** Transient CM token; never included in events, snapshots or persistence. */
  readonly sessionToken: string;
}

export class SteamAuthRequest {
  readonly accountId: string;
  readonly credentials: SteamCredentialSet;
  readonly challengeCode: string;
  readonly challengeKind: SteamAuthChallengeKind;

  constructor(
    accountId: string,
    credentials: SteamCredentialSet,
    challengeCode: string = '',
    challengeKind: SteamAuthChallengeKind = SteamAuthChallengeKind.NONE
  ) {
    this.accountId = accountId;
    this.credentials = credentials;
    this.challengeCode = challengeCode;
    this.challengeKind = challengeKind;
  }
}

export interface ISteamAuthService {
  signIn(request: SteamAuthRequest): Promise<SteamAuthResult>;
  beginQr(request: SteamAuthRequest): Promise<SteamAuthResult>;
  completeChallenge(request: SteamAuthRequest): Promise<SteamAuthResult>;
  poll(accountId: string): Promise<SteamAuthResult>;
  refresh(accountId: string, refreshToken: string): Promise<SteamAuthResult>;
  cancelPendingAuth(accountId: string): Promise<void>;
  close(accountId: string): Promise<void>;
  getPendingAuthSnapshot(accountId: string): SteamAuthPendingSnapshot | null;
  restorePendingAuthSnapshot(snapshot: SteamAuthPendingSnapshot, credentials: SteamCredentialSet): boolean;
}

export interface ISteamWebSession {
  open(accountId: string, credentials: SteamCredentialSet): Promise<SteamSessionSnapshot>;
  close(accountId: string): Promise<void>;
  syncBadges(accountId: string): Promise<Array<SteamGameDrop>>;
  syncBadgesWithReport(accountId: string): Promise<SteamBadgeSyncReport>;
  syncInventory(accountId: string, query: SteamInventoryQuery): Promise<SteamInventoryPage>;
  /** Fetches the full owned game list through IPlayerService/GetOwnedGames with the OAuth access token. */
  syncOwnedGamesViaWebApi(accountId: string, accessToken: string, language?: string): Promise<SteamGameLibraryResult>;
  /** Fetches only the owned-app count through the store personalization endpoint (session cookies). */
  syncOwnedGameCountViaDynamicStore(accountId: string): Promise<SteamGameLibraryResult>;
  /** Fetches the store-level detail of one game (appdetails, public endpoint). */
  fetchGameDetail(accountId: string, appId: string, language?: string): Promise<SteamGameDetailInfo | null>;
  /** Fetches the recent news of one game (ISteamNews, keyless endpoint). */
  fetchGameNews(accountId: string, appId: string, count?: number): Promise<Array<SteamGameNewsItem>>;
  /**
   * Fetches the account's achievement state of one game. Prefers the
   * authenticated ISteamUserStats Web API (GetPlayerAchievements plus the
   * keyless GetSchemaForGame merge) and falls back to the community stats
   * page. The OAuth access token is passed in the query string, never logged.
   */
  fetchGameAchievements(
    accountId: string,
    appId: string,
    accessToken: string,
    language?: string
  ): Promise<SteamGameAchievementsResult | null>;
}

export interface IInventoryService {
  sync(accountId: string, query: SteamInventoryQuery): Promise<SteamInventoryPage>;
  getCached(accountId: string, query: SteamInventoryQuery): Promise<SteamInventoryPage | null>;
  getItem(accountId: string, assetId: string): Promise<SteamInventoryItem | null>;
}

export enum SteamFarmEventType {
  STATE_CHANGED = 'state_changed',
  CARD_DROPPED = 'card_dropped',
  BADGE_SYNCED = 'badge_synced',
  PLAN_CHANGED = 'plan_changed',
  PERSISTENCE_FAILED = 'persistence_failed',
  TRANSPORT_FAILED = 'transport_failed'
}

export class SteamFarmEvent {
  readonly type: SteamFarmEventType;
  readonly snapshot: SteamFarmSnapshot;
  readonly drop: SteamCardDropEvent | null;
  readonly badgeReport: SteamBadgeSyncReport | null;
  readonly message: string;

  constructor(
    type: SteamFarmEventType,
    snapshot: SteamFarmSnapshot,
    drop: SteamCardDropEvent | null = null,
    message: string = '',
    badgeReport: SteamBadgeSyncReport | null = null
  ) {
    this.type = type;
    this.snapshot = snapshot;
    this.drop = drop;
    this.badgeReport = badgeReport;
    this.message = message;
  }
}

export type SteamFarmEventListener = (event: SteamFarmEvent) => void;

export interface IFarmingCoordinator {
  load(): Promise<SteamFarmSnapshot>;
  start(): Promise<SteamFarmSnapshot>;
  pause(): Promise<SteamFarmSnapshot>;
  resume(): Promise<SteamFarmSnapshot>;
  stop(): Promise<SteamFarmSnapshot>;
  playManual(appIds: Array<string>, gameName: string): Promise<SteamFarmSnapshot>;
  resetManual(): Promise<SteamFarmSnapshot>;
  setPriorityAppIds(appIds: Array<string>): Promise<SteamFarmSnapshot>;
  refreshGames(games: Array<SteamGameDrop>): Promise<SteamFarmSnapshot>;
  onCardDrop(drop: SteamCardDropEvent): Promise<SteamFarmSnapshot>;
  tick(): Promise<SteamFarmSnapshot>;
  getSnapshot(): SteamFarmSnapshot;
  addEventListener(listener: SteamFarmEventListener): void;
  removeEventListener(listener: SteamFarmEventListener): void;
}

export { SteamCardDropEvent };
