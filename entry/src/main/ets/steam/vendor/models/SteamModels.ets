/** Connection and authentication state exposed by the platform-independent core. */
export enum SteamSessionState {
  DISCONNECTED = 'disconnected',
  CONNECTING = 'connecting',
  CONNECTED = 'connected',
  LOGGED_ON = 'logged_on',
  NEEDS_QR_SCAN = 'needs_qr_scan',
  NEEDS_EMAIL_CODE = 'needs_email_code',
  NEEDS_TWO_FACTOR = 'needs_two_factor',
  NEEDS_MOBILE_CONFIRMATION = 'needs_mobile_confirmation',
  EXPIRED = 'expired',
  RECONNECTING = 'reconnecting',
  ERROR = 'error'
}

export enum SteamAuthChallengeKind {
  NONE = 'none',
  QR_SCAN = 'qr_scan',
  EMAIL_CODE = 'email_code',
  TWO_FACTOR = 'two_factor',
  MOBILE_CONFIRMATION = 'mobile_confirmation'
}

/** Authentication transport flow; kept in snapshots so hosts can resume QR auth. */
export enum SteamAuthFlowKind {
  PASSWORD = 'password',
  QR = 'qr'
}

export enum SteamFarmState {
  IDLE = 'idle',
  STARTING = 'starting',
  FARMING = 'farming',
  PAUSED = 'paused',
  RECONNECTING = 'reconnecting',
  COMPLETED = 'completed',
  ERROR = 'error'
}

export enum SteamFarmAlgorithm {
  SIMPLE = 'simple',
  COMPLEX = 'complex'
}

export enum SteamFarmingOrder {
  UNORDERED = 'Unordered',
  APP_IDS_ASCENDING = 'AppIDsAscending',
  APP_IDS_DESCENDING = 'AppIDsDescending',
  CARD_DROPS_ASCENDING = 'CardDropsAscending',
  CARD_DROPS_DESCENDING = 'CardDropsDescending',
  HOURS_ASCENDING = 'HoursAscending',
  HOURS_DESCENDING = 'HoursDescending',
  NAMES_ASCENDING = 'NamesAscending',
  NAMES_DESCENDING = 'NamesDescending',
  RANDOM = 'Random',
  BADGE_LEVELS_ASCENDING = 'BadgeLevelsAscending',
  BADGE_LEVELS_DESCENDING = 'BadgeLevelsDescending',
  MARKETABLE_ASCENDING = 'MarketableAscending',
  MARKETABLE_DESCENDING = 'MarketableDescending'
}

/** ASF-compatible bit values for the farming preference subset implemented in core. */
export enum SteamFarmingPreference {
  NONE = 0,
  FARMING_PAUSED_BY_DEFAULT = 1,
  SHUTDOWN_ON_FARMING_FINISHED = 2,
  SEND_ON_FARMING_FINISHED = 4,
  FARM_PRIORITY_QUEUE_ONLY = 8,
  SKIP_REFUNDABLE_GAMES = 16,
  SKIP_UNPLAYED_GAMES = 32,
  ENABLE_RISKY_CARDS_DISCOVERY = 64,
  AUTO_UNPACK_BOOSTER_PACKS = 256
}

export enum SteamInventoryCardDropState {
  UNKNOWN = 'unknown',
  AVAILABLE = 'available',
  DROPPED = 'dropped',
  NOT_APPLICABLE = 'not_applicable'
}

export enum SteamProxyKind {
  SYSTEM = 'system',
  DIRECT = 'direct',
  HTTP = 'http',
  SOCKS5 = 'socks5'
}

export class SteamProxySettings {
  readonly kind: SteamProxyKind;
  readonly host: string;
  readonly port: number;
  readonly username: string;

  constructor(
    kind: SteamProxyKind = SteamProxyKind.SYSTEM,
    host: string = '',
    port: number = 0,
    username: string = ''
  ) {
    this.kind = kind;
    this.host = host;
    this.port = port;
    this.username = username;
  }
}

export class SteamAccountProfile {
  readonly accountId: string;
  displayName: string;
  steamId64: string;
  avatarUrl: string;
  profileBackgroundImageUrl: string;
  profileBackgroundMovieUrl: string;
  profileBackgroundTiled: boolean;
  miniProfileBackgroundImageUrl: string;
  miniProfileBackgroundMovieUrl: string;
  avatarFrameUrl: string;
  summary: string;
  level: number;
  countryCode: string;
  walletBalance: string;
  walletCurrency: string;
  enabled: boolean;
  proxy: SteamProxySettings;

  constructor(
    accountId: string,
    displayName: string = '',
    steamId64: string = '',
    enabled: boolean = true,
    proxy: SteamProxySettings = new SteamProxySettings(),
    avatarUrl: string = '',
    profileBackgroundImageUrl: string = '',
    profileBackgroundMovieUrl: string = '',
    profileBackgroundTiled: boolean = false,
    avatarFrameUrl: string = '',
    summary: string = '',
    level: number = 0,
    countryCode: string = '',
    walletBalance: string = '',
    walletCurrency: string = '',
    miniProfileBackgroundImageUrl: string = '',
    miniProfileBackgroundMovieUrl: string = ''
  ) {
    this.accountId = accountId;
    this.displayName = displayName;
    this.steamId64 = steamId64;
    this.avatarUrl = avatarUrl;
    this.profileBackgroundImageUrl = profileBackgroundImageUrl;
    this.profileBackgroundMovieUrl = profileBackgroundMovieUrl;
    this.profileBackgroundTiled = profileBackgroundTiled;
    this.miniProfileBackgroundImageUrl = miniProfileBackgroundImageUrl;
    this.miniProfileBackgroundMovieUrl = miniProfileBackgroundMovieUrl;
    this.avatarFrameUrl = avatarFrameUrl;
    this.summary = summary;
    this.level = level;
    this.countryCode = countryCode;
    this.walletBalance = walletBalance;
    this.walletCurrency = walletCurrency;
    this.enabled = enabled;
    this.proxy = proxy;
  }
}

/** Bot configuration contains no credentials and can be serialized safely. */
export class SteamBotConfig {
  readonly accountId: string;
  enabled: boolean = true;
  hoursUntilCardDrops: number = 3;
  maxGamesPlayedConcurrently: number = 32;
  farmingPreferences: number = SteamFarmingPreference.NONE;
  farmingOrders: Array<SteamFarmingOrder> = [];
  gamesPlayedWhileIdle: Array<string> = [];
  blacklist: Array<string> = [];
  priorityAppIds: Array<string> = [];

  constructor(accountId: string) {
    this.accountId = accountId;
  }

  hasPreference(preference: SteamFarmingPreference): boolean {
    return (this.farmingPreferences & preference) === preference;
  }
}

/**
 * Non-secret ASF global configuration supported by the migration boundary.
 * SteamOwnerID is kept as a decimal string because it is a 64-bit identifier.
 */
export class SteamGlobalConfig {
  autoRestart: boolean = true;
  blacklist: Array<string> = [];
  commandPrefix: string = '!';
  confirmationsLimiterDelay: number = 10;
  connectionTimeout: number = 90;
  currentCulture: string = '';
  debug: boolean = false;
  defaultBot: string = '';
  farmingDelay: number = 15;
  filterBadBots: boolean = true;
  giftsLimiterDelay: number = 1;
  headless: boolean = false;
  idleFarmingPeriod: number = 8;
  inventoryLimiterDelay: number = 4;
  ipc: boolean = true;
  ipcPasswordFormat: string = 'PlainText';
  licenseId: string = '';
  loginLimiterDelay: number = 10;
  maxFarmingTime: number = 10;
  maxTradeHoldDuration: number = 15;
  minFarmingDelayAfterBlock: number = 60;
  optimizationMode: number = 0;
  pluginsUpdateList: Array<string> = [];
  pluginsUpdateMode: string = 'Whitelist';
  shutdownIfPossible: boolean = false;
  steamMessagePrefix: string = '/me ';
  steamOwnerId64: string = '0';
  steamProtocols: number = 7;
  updateChannel: string = 'Stable';
  updatePeriod: number = 24;
  webLimiterDelay: number = 300;
  webProxyText: string = '';
  webProxyUsername: string = '';

  clone(): SteamGlobalConfig {
    const copy = new SteamGlobalConfig();
    copy.autoRestart = this.autoRestart;
    copy.blacklist = this.blacklist.slice();
    copy.commandPrefix = this.commandPrefix;
    copy.confirmationsLimiterDelay = this.confirmationsLimiterDelay;
    copy.connectionTimeout = this.connectionTimeout;
    copy.currentCulture = this.currentCulture;
    copy.debug = this.debug;
    copy.defaultBot = this.defaultBot;
    copy.farmingDelay = this.farmingDelay;
    copy.filterBadBots = this.filterBadBots;
    copy.giftsLimiterDelay = this.giftsLimiterDelay;
    copy.headless = this.headless;
    copy.idleFarmingPeriod = this.idleFarmingPeriod;
    copy.inventoryLimiterDelay = this.inventoryLimiterDelay;
    copy.ipc = this.ipc;
    copy.ipcPasswordFormat = this.ipcPasswordFormat;
    copy.licenseId = this.licenseId;
    copy.loginLimiterDelay = this.loginLimiterDelay;
    copy.maxFarmingTime = this.maxFarmingTime;
    copy.maxTradeHoldDuration = this.maxTradeHoldDuration;
    copy.minFarmingDelayAfterBlock = this.minFarmingDelayAfterBlock;
    copy.optimizationMode = this.optimizationMode;
    copy.pluginsUpdateList = this.pluginsUpdateList.slice();
    copy.pluginsUpdateMode = this.pluginsUpdateMode;
    copy.shutdownIfPossible = this.shutdownIfPossible;
    copy.steamMessagePrefix = this.steamMessagePrefix;
    copy.steamOwnerId64 = this.steamOwnerId64;
    copy.steamProtocols = this.steamProtocols;
    copy.updateChannel = this.updateChannel;
    copy.updatePeriod = this.updatePeriod;
    copy.webLimiterDelay = this.webLimiterDelay;
    copy.webProxyText = this.webProxyText;
    copy.webProxyUsername = this.webProxyUsername;
    return copy;
  }
}

/** Sensitive values are transient importer output and must be moved to secure storage by the host. */
export class SteamGlobalConfigSecrets {
  readonly ipcPassword: string;
  readonly webProxyPassword: string;

  constructor(ipcPassword: string = '', webProxyPassword: string = '') {
    this.ipcPassword = ipcPassword;
    this.webProxyPassword = webProxyPassword;
  }
}

/** Secrets are memory-only credentials passed to authentication adapters. */
export class SteamCredentialSet {
  readonly accountName: string;
  readonly password: string;
  readonly refreshToken: string;
  readonly loginKey: string;
  readonly sharedSecret: string;
  readonly identitySecret: string;
  readonly deviceId: string;
  readonly accessToken: string;
  readonly oauthToken: string;
  readonly guardData: string;

  constructor(
    accountName: string = '',
    password: string = '',
    refreshToken: string = '',
    loginKey: string = '',
    sharedSecret: string = '',
    identitySecret: string = '',
    deviceId: string = '',
    accessToken: string = '',
    oauthToken: string = '',
    guardData: string = ''
  ) {
    this.accountName = accountName;
    this.password = password;
    this.refreshToken = refreshToken;
    this.loginKey = loginKey;
    this.sharedSecret = sharedSecret;
    this.identitySecret = identitySecret;
    this.deviceId = deviceId;
    this.accessToken = accessToken;
    this.oauthToken = oauthToken;
    this.guardData = guardData;
  }
}

/** Session snapshot intentionally excludes all secret token material. */
export class SteamSessionSnapshot {
  readonly accountId: string;
  readonly state: SteamSessionState;
  readonly steamId64: string;
  readonly server: string;
  readonly connectedAtEpochSeconds: number;
  readonly expiresAtEpochSeconds: number;
  readonly lastErrorCode: number;
  readonly lastErrorMessage: string;
  readonly lastErrorType: string;
  readonly lastErrorStage: string;
  readonly lastErrorHttpStatusCode: number;
  readonly lastErrorMessageType: number;
  readonly lastErrorHeaderKind: string;
  readonly lastErrorFrameBytes: number;
  readonly lastErrorHeaderBytes: number;
  readonly lastErrorBodyBytes: number;
  readonly lastErrorServerMessage: string;
  /** Wallet balance in minor units (cents) from the CM logon response. */
  readonly walletBalance: string;
  /** Steam ECurrencyCode enum id of the account's own wallet currency. */
  readonly walletCurrency: number;

  constructor(
    accountId: string,
    state: SteamSessionState = SteamSessionState.DISCONNECTED,
    steamId64: string = '',
    server: string = '',
    connectedAtEpochSeconds: number = 0,
    expiresAtEpochSeconds: number = 0,
    lastErrorCode: number = 0,
    lastErrorMessage: string = '',
    lastErrorType: string = '',
    lastErrorStage: string = '',
    lastErrorHttpStatusCode: number = 0,
    lastErrorMessageType: number = 0,
    lastErrorHeaderKind: string = '',
    lastErrorFrameBytes: number = 0,
    lastErrorHeaderBytes: number = 0,
    lastErrorBodyBytes: number = 0,
    lastErrorServerMessage: string = '',
    walletBalance: string = '',
    walletCurrency: number = 0
  ) {
    this.accountId = accountId;
    this.state = state;
    this.steamId64 = steamId64;
    this.server = server;
    this.connectedAtEpochSeconds = connectedAtEpochSeconds;
    this.expiresAtEpochSeconds = expiresAtEpochSeconds;
    this.lastErrorCode = lastErrorCode;
    this.lastErrorMessage = lastErrorMessage;
    this.lastErrorType = lastErrorType;
    this.lastErrorStage = lastErrorStage;
    this.lastErrorHttpStatusCode = lastErrorHttpStatusCode;
    this.lastErrorMessageType = lastErrorMessageType;
    this.lastErrorHeaderKind = lastErrorHeaderKind;
    this.lastErrorFrameBytes = lastErrorFrameBytes;
    this.lastErrorHeaderBytes = lastErrorHeaderBytes;
    this.lastErrorBodyBytes = lastErrorBodyBytes;
    this.lastErrorServerMessage = lastErrorServerMessage;
    this.walletBalance = walletBalance;
    this.walletCurrency = walletCurrency;
  }
}

/**
 * Non-secret state required to continue a modern Steam authentication
 * challenge after the host process is recreated.
 *
 * The host must combine this snapshot with credentials loaded from secure
 * storage before restoring the in-memory authentication service.
 */
export class SteamAuthPendingSnapshot {
  static readonly MAX_LIFETIME_SECONDS: number = 900;
  readonly accountId: string;
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

  isValidFor(accountId: string, nowEpochSeconds: number): boolean {
    if (this.accountId !== accountId || accountId.trim().length <= 0) {
      return false;
    }
    if (this.clientId.trim().length <= 0 || this.requestId.trim().length <= 0) {
      return false;
    }
    if (this.challenge === SteamAuthChallengeKind.QR_SCAN &&
      (this.flow !== SteamAuthFlowKind.QR || this.challengeUrl.trim().length <= 0)) {
      return false;
    }
    if (this.flow === SteamAuthFlowKind.QR &&
      (this.challenge !== SteamAuthChallengeKind.QR_SCAN || this.challengeUrl.trim().length <= 0)) {
      return false;
    }
    if (!Number.isSafeInteger(this.intervalSeconds) || this.intervalSeconds <= 0) {
      return false;
    }
    if (!Number.isSafeInteger(this.createdAtEpochSeconds) || this.createdAtEpochSeconds <= 0) {
      return false;
    }
    if (!Number.isSafeInteger(this.expiresAtEpochSeconds) ||
      this.expiresAtEpochSeconds <= this.createdAtEpochSeconds) {
      return false;
    }
    const lifetime = this.expiresAtEpochSeconds - this.createdAtEpochSeconds;
    if (lifetime > SteamAuthPendingSnapshot.MAX_LIFETIME_SECONDS) {
      return false;
    }
    return nowEpochSeconds < this.expiresAtEpochSeconds;
  }
}

export class SteamGameDrop {
  readonly appId: string;
  readonly name: string;
  hoursPlayed: number;
  cardsRemaining: number;
  badgeLevel: number;
  marketable: boolean;
  playableAppId: string;
  readonly imageUrl: string;
  readonly storeUrl: string;
  readonly badgeImageUrl: string;
  readonly badgeUrl: string;
  /** True when the row carries a crafted badge (badge_gamebadge_ id); false for card-drop-only rows whose badge has not been crafted yet. */
  readonly badgeCrafted: boolean;

  constructor(
    appId: string,
    name: string = '',
    hoursPlayed: number = 0,
    cardsRemaining: number = 0,
    badgeLevel: number = 0,
    marketable: boolean = false,
    playableAppId: string = '',
    imageUrl: string = '',
    storeUrl: string = '',
    badgeImageUrl: string = '',
    badgeUrl: string = '',
    badgeCrafted: boolean = false
  ) {
    this.appId = appId;
    this.name = name;
    this.hoursPlayed = hoursPlayed;
    this.cardsRemaining = cardsRemaining;
    this.badgeLevel = badgeLevel;
    this.marketable = marketable;
    this.playableAppId = playableAppId.length > 0 ? playableAppId : appId;
    this.imageUrl = imageUrl.length > 0
      ? imageUrl
      : 'https://cdn.cloudflare.steamstatic.com/steam/apps/' + appId + '/header.jpg';
    this.storeUrl = storeUrl.length > 0
      ? storeUrl
      : 'https://store.steampowered.com/app/' + appId + '/';
    this.badgeImageUrl = badgeImageUrl;
    this.badgeUrl = badgeUrl;
    this.badgeCrafted = badgeCrafted;
  }

  clone(): SteamGameDrop {
    return new SteamGameDrop(
      this.appId,
      this.name,
      this.hoursPlayed,
      this.cardsRemaining,
      this.badgeLevel,
      this.marketable,
      this.playableAppId,
      this.imageUrl,
      this.storeUrl,
      this.badgeImageUrl,
      this.badgeUrl,
      this.badgeCrafted
    );
  }
}

export class SteamCommunityBadge {
  readonly badgeId: string;
  readonly name: string;
  readonly experience: number;
  readonly imageUrl: string;
  readonly badgeUrl: string;

  constructor(
    badgeId: string,
    name: string = '',
    experience: number = 0,
    imageUrl: string = '',
    badgeUrl: string = ''
  ) {
    this.badgeId = badgeId;
    this.name = name;
    this.experience = experience;
    this.imageUrl = imageUrl;
    this.badgeUrl = badgeUrl;
  }
}

export class SteamCardDropEvent {
  readonly appId: string;
  readonly assetId: string;
  readonly receivedAtEpochSeconds: number;

  constructor(appId: string, assetId: string = '', receivedAtEpochSeconds: number = 0) {
    this.appId = appId;
    this.assetId = assetId;
    this.receivedAtEpochSeconds = receivedAtEpochSeconds;
  }
}

export class SteamInventoryItem {
  readonly assetId: string;
  readonly classId: string;
  readonly instanceId: string;
  readonly appId: string;
  readonly contextId: string;
  readonly amount: number;
  readonly originalAmount: number;
  readonly name: string;
  readonly type: string;
  readonly description: string;
  readonly iconUrl: string;
  readonly iconUrlLarge: string;
  readonly marketHashName: string;
  readonly marketName: string;
  readonly marketable: boolean;
  readonly tradable: boolean;
  readonly isTradingCard: boolean;
  /** Raw inventory asset status; Steam does not define this as card-drop state. */
  readonly assetStatus: number;
  readonly cardDropState: SteamInventoryCardDropState;
  /** Origin game for Steam Community items; differs from the economy app ID (usually 753). */
  readonly ownerAppId: string;
  readonly ownerAppName: string;

  constructor(
    assetId: string,
    classId: string,
    instanceId: string,
    appId: string,
    contextId: string,
    amount: number = 1,
    name: string = '',
    type: string = '',
    marketable: boolean = false,
    tradable: boolean = false,
    cardDropState: SteamInventoryCardDropState = SteamInventoryCardDropState.UNKNOWN,
    originalAmount: number = -1,
    description: string = '',
    iconUrl: string = '',
    iconUrlLarge: string = '',
    marketHashName: string = '',
    marketName: string = '',
    isTradingCard: boolean = false,
    assetStatus: number = 0,
    ownerAppId: string = '',
    ownerAppName: string = ''
  ) {
    this.assetId = assetId;
    this.classId = classId;
    this.instanceId = instanceId;
    this.appId = appId;
    this.contextId = contextId;
    this.amount = amount;
    this.originalAmount = originalAmount < 0 ? amount : originalAmount;
    this.name = name;
    this.type = type;
    this.description = description;
    this.iconUrl = iconUrl;
    this.iconUrlLarge = iconUrlLarge;
    this.marketHashName = marketHashName;
    this.marketName = marketName;
    this.marketable = marketable;
    this.tradable = tradable;
    this.isTradingCard = isTradingCard;
    this.assetStatus = assetStatus;
    this.cardDropState = cardDropState;
    this.ownerAppId = ownerAppId.length > 0 ? ownerAppId : appId;
    this.ownerAppName = ownerAppName;
  }
}

export class SteamInventoryQuery {
  readonly appId: string;
  readonly contextId: string;
  readonly page: number;
  readonly pageSize: number;
  readonly search: string;
  readonly marketableOnly: boolean;
  readonly tradableOnly: boolean;
  /** Steam's opaque pagination cursor; never convert it to a JavaScript number. */
  readonly startAssetId: string;
  readonly language: string;

  constructor(
    appId: string = '753',
    contextId: string = '6',
    page: number = 1,
    pageSize: number = 100,
    search: string = '',
    marketableOnly: boolean = false,
    tradableOnly: boolean = false,
    startAssetId: string = '',
    language: string = 'english'
  ) {
    this.appId = appId;
    this.contextId = contextId;
    this.page = page;
    this.pageSize = pageSize;
    this.search = search;
    this.marketableOnly = marketableOnly;
    this.tradableOnly = tradableOnly;
    this.startAssetId = startAssetId;
    this.language = language;
  }
}

export class SteamInventoryPage {
  readonly query: SteamInventoryQuery;
  readonly items: Array<SteamInventoryItem>;
  readonly totalCount: number;
  readonly hasMore: boolean;
  /** Cursor for the next read-only inventory request, if Steam returned one. */
  readonly nextStartAssetId: string;
  readonly syncedAtEpochSeconds: number;

  constructor(
    query: SteamInventoryQuery,
    items: Array<SteamInventoryItem> = [],
    totalCount: number = 0,
    hasMore: boolean = false,
    syncedAtEpochSeconds: number = 0,
    nextStartAssetId: string = ''
  ) {
    this.query = query;
    this.items = items.slice();
    this.totalCount = totalCount;
    this.hasMore = hasMore;
    this.syncedAtEpochSeconds = syncedAtEpochSeconds;
    this.nextStartAssetId = nextStartAssetId;
  }
}

export class SteamFarmPlan {
  readonly algorithm: SteamFarmAlgorithm;
  readonly activeAppIds: Array<string>;
  readonly gamesToFarm: Array<SteamGameDrop>;
  readonly estimatedSeconds: number;
  readonly reason: string;

  constructor(
    algorithm: SteamFarmAlgorithm,
    activeAppIds: Array<string>,
    gamesToFarm: Array<SteamGameDrop>,
    estimatedSeconds: number,
    reason: string
  ) {
    this.algorithm = algorithm;
    this.activeAppIds = activeAppIds.slice();
    this.gamesToFarm = gamesToFarm.slice();
    this.estimatedSeconds = estimatedSeconds;
    this.reason = reason;
  }
}

/** How a paused farm will resume; mirrors the ASF pause command family. */
export enum SteamFarmPauseMode {
  NONE = 'none',
  PERMANENT = 'permanent',
  TEMPORARY = 'temporary',
  TIMED = 'timed'
}

export class SteamFarmSnapshot {
  readonly accountId: string;
  readonly state: SteamFarmState;
  readonly paused: boolean;
  readonly nowFarming: boolean;
  readonly manualMode: boolean;
  readonly manualAppIds: Array<string>;
  readonly manualGameName: string;
  readonly currentAppIds: Array<string>;
  readonly games: Array<SteamGameDrop>;
  readonly estimatedSeconds: number;
  readonly updatedAtEpochSeconds: number;
  readonly lastError: string;
  readonly pauseMode: SteamFarmPauseMode;
  readonly resumeAtEpochSeconds: number;

  constructor(
    accountId: string,
    state: SteamFarmState,
    paused: boolean,
    nowFarming: boolean,
    currentAppIds: Array<string>,
    games: Array<SteamGameDrop>,
    estimatedSeconds: number,
    updatedAtEpochSeconds: number,
    lastError: string = '',
    manualMode: boolean = false,
    manualAppIds: Array<string> = [],
    manualGameName: string = '',
    pauseMode: SteamFarmPauseMode = SteamFarmPauseMode.NONE,
    resumeAtEpochSeconds: number = 0
  ) {
    this.accountId = accountId;
    this.state = state;
    this.paused = paused;
    this.nowFarming = nowFarming;
    this.manualMode = manualMode;
    this.manualAppIds = manualAppIds.slice();
    this.manualGameName = manualGameName;
    this.currentAppIds = currentAppIds.slice();
    this.games = games.slice();
    this.estimatedSeconds = estimatedSeconds;
    this.updatedAtEpochSeconds = updatedAtEpochSeconds;
    this.lastError = lastError;
    this.pauseMode = pauseMode;
    this.resumeAtEpochSeconds = resumeAtEpochSeconds;
  }
}

/** Only non-secret farming state is persisted. */
export class SteamFarmStateRecord {
  readonly accountId: string;
  readonly state: SteamFarmState;
  readonly paused: boolean;
  readonly manualMode: boolean;
  readonly manualAppIds: Array<string>;
  readonly manualGameName: string;
  readonly manualPreviousState: SteamFarmState;
  readonly manualPreviousPaused: boolean;
  readonly manualPreviousAppIds: Array<string>;
  readonly currentAppIds: Array<string>;
  readonly games: Array<SteamGameDrop>;
  readonly updatedAtEpochSeconds: number;
  readonly pauseMode: SteamFarmPauseMode;
  readonly resumeAtEpochSeconds: number;

  constructor(
    accountId: string,
    state: SteamFarmState,
    paused: boolean,
    currentAppIds: Array<string>,
    games: Array<SteamGameDrop>,
    updatedAtEpochSeconds: number,
    manualMode: boolean = false,
    manualAppIds: Array<string> = [],
    manualGameName: string = '',
    manualPreviousState: SteamFarmState = SteamFarmState.IDLE,
    manualPreviousPaused: boolean = false,
    manualPreviousAppIds: Array<string> = [],
    pauseMode: SteamFarmPauseMode = SteamFarmPauseMode.NONE,
    resumeAtEpochSeconds: number = 0
  ) {
    this.accountId = accountId;
    this.state = state;
    this.paused = paused;
    this.manualMode = manualMode;
    this.manualAppIds = manualAppIds.slice();
    this.manualGameName = manualGameName;
    this.manualPreviousState = manualPreviousState;
    this.manualPreviousPaused = manualPreviousPaused;
    this.manualPreviousAppIds = manualPreviousAppIds.slice();
    this.currentAppIds = currentAppIds.slice();
    this.games = games.slice();
    this.updatedAtEpochSeconds = updatedAtEpochSeconds;
    this.pauseMode = pauseMode;
    this.resumeAtEpochSeconds = resumeAtEpochSeconds;
  }
}

export class SteamAuthResult {
  readonly session: SteamSessionSnapshot;
  readonly credentials: SteamCredentialSet;
  readonly challenge: SteamAuthChallengeKind;
  readonly flow: SteamAuthFlowKind;
  readonly qrChallengeUrl: string;

  constructor(
    session: SteamSessionSnapshot,
    credentials: SteamCredentialSet = new SteamCredentialSet(),
    challenge: SteamAuthChallengeKind = SteamAuthChallengeKind.NONE,
    flow: SteamAuthFlowKind = SteamAuthFlowKind.PASSWORD,
    qrChallengeUrl: string = ''
  ) {
    this.session = session;
    this.credentials = credentials;
    this.challenge = challenge;
    this.flow = flow;
    this.qrChallengeUrl = qrChallengeUrl;
  }
}

/** One entry of the account's Steam game library (community library page data). */
export class SteamLibraryGame {
  readonly appId: string;
  readonly name: string;
  /** Full logo URL (large header image), empty when Steam did not publish one. */
  readonly logoUrl: string;
  /** Full icon URL (small square image), empty when Steam did not publish one. */
  readonly iconUrl: string;
  readonly playtimeForeverSeconds: number;
  readonly playtimeTwoWeeksSeconds: number;
  /** Unix epoch seconds of the last launch, 0 when the game was never played. */
  readonly lastPlayedEpochSeconds: number;
  /** Whether the account marked this game as a favorite in its library. */
  readonly favorite: boolean;

  constructor(
    appId: string,
    name: string,
    logoUrl: string = '',
    iconUrl: string = '',
    playtimeForeverSeconds: number = 0,
    playtimeTwoWeeksSeconds: number = 0,
    lastPlayedEpochSeconds: number = 0,
    favorite: boolean = false
  ) {
    this.appId = appId;
    this.name = name;
    this.logoUrl = logoUrl;
    this.iconUrl = iconUrl;
    this.playtimeForeverSeconds = playtimeForeverSeconds;
    this.playtimeTwoWeeksSeconds = playtimeTwoWeeksSeconds;
    this.lastPlayedEpochSeconds = lastPlayedEpochSeconds;
    this.favorite = favorite;
  }
}

/** Parsed result of one Steam game library synchronization. */
export class SteamGameLibraryResult {
  readonly games: Array<SteamLibraryGame>;
  readonly totalCount: number;
  readonly warnings: Array<string>;
  /** 'cm' for the Player.GetOwnedGames unified message, 'json' for the owned-games Web API, 'html' for the DesktopUI tile page, 'xml' for the XML fallback. */
  readonly source: string;

  constructor(
    games: Array<SteamLibraryGame> = [],
    totalCount: number = 0,
    warnings: Array<string> = [],
    source: string = ''
  ) {
    this.games = games.slice();
    this.totalCount = totalCount;
    this.warnings = warnings.slice();
    this.source = source;
  }
}

/** One store feature/category entry, for example "Steam Cloud" or "Steam Achievements". */
export class SteamGameDetailCategory {
  readonly id: string;
  readonly description: string;

  constructor(id: string = '', description: string = '') {
    this.id = id;
    this.description = description;
  }
}

/** Store-level detail of one game fetched from the public appdetails endpoint. */
export class SteamGameDetailInfo {
  readonly appId: string;
  readonly name: string;
  readonly headerImageUrl: string;
  readonly capsuleImageUrl: string;
  readonly shortDescription: string;
  readonly detailedDescription: string;
  readonly developers: Array<string>;
  readonly publishers: Array<string>;
  readonly releaseDateText: string;
  readonly genres: Array<string>;
  readonly categories: Array<SteamGameDetailCategory>;
  readonly isFree: boolean;

  constructor(
    appId: string = '',
    name: string = '',
    headerImageUrl: string = '',
    capsuleImageUrl: string = '',
    shortDescription: string = '',
    detailedDescription: string = '',
    developers: Array<string> = [],
    publishers: Array<string> = [],
    releaseDateText: string = '',
    genres: Array<string> = [],
    categories: Array<SteamGameDetailCategory> = [],
    isFree: boolean = false
  ) {
    this.appId = appId;
    this.name = name;
    this.headerImageUrl = headerImageUrl;
    this.capsuleImageUrl = capsuleImageUrl;
    this.shortDescription = shortDescription;
    this.detailedDescription = detailedDescription;
    this.developers = developers.slice();
    this.publishers = publishers.slice();
    this.releaseDateText = releaseDateText;
    this.genres = genres.slice();
    this.categories = categories.slice();
    this.isFree = isFree;
  }
}

/** One entry of the ISteamNews/GetNewsForApp response. */
export class SteamGameNewsItem {
  readonly gid: string;
  readonly title: string;
  readonly url: string;
  readonly author: string;
  readonly dateEpochSeconds: number;
  readonly contents: string;
  readonly feedLabel: string;

  constructor(
    gid: string = '',
    title: string = '',
    url: string = '',
    author: string = '',
    dateEpochSeconds: number = 0,
    contents: string = '',
    feedLabel: string = ''
  ) {
    this.gid = gid;
    this.title = title;
    this.url = url;
    this.author = author;
    this.dateEpochSeconds = dateEpochSeconds;
    this.contents = contents;
    this.feedLabel = feedLabel;
  }
}

/** One achievement entry of the community stats page or the Web API merge. */
export class SteamGameAchievementInfo {
  /** Stable achievement identifier (schema "name"/apiname); used as list key. */
  readonly apiName: string;
  readonly name: string;
  readonly achieved: boolean;
  readonly unlockTimeEpochSeconds: number;
  readonly description: string;
  readonly iconUrl: string;

  constructor(
    name: string = '',
    achieved: boolean = false,
    unlockTimeEpochSeconds: number = 0,
    description: string = '',
    iconUrl: string = '',
    apiName: string = ''
  ) {
    this.apiName = apiName;
    this.name = name;
    this.achieved = achieved;
    this.unlockTimeEpochSeconds = unlockTimeEpochSeconds;
    this.description = description;
    this.iconUrl = iconUrl;
  }
}

/** Achievement summary of one game for the account. */
export class SteamGameAchievementsResult {
  readonly totalCount: number;
  readonly unlockedCount: number;
  readonly achievements: Array<SteamGameAchievementInfo>;

  constructor(
    totalCount: number = 0,
    unlockedCount: number = 0,
    achievements: Array<SteamGameAchievementInfo> = []
  ) {
    this.totalCount = totalCount;
    this.unlockedCount = unlockedCount;
    this.achievements = achievements.slice();
  }
}

/**
 * One achievement entry of the authenticated ISteamUserStats
 * GetPlayerAchievements response. "name" and "description" are only present
 * when Steam returns localized data for the requested language.
 */
export class SteamPlayerAchievementEntry {
  readonly apiName: string;
  readonly achieved: boolean;
  readonly unlockTimeEpochSeconds: number;
  readonly name: string;
  readonly description: string;

  constructor(
    apiName: string = '',
    achieved: boolean = false,
    unlockTimeEpochSeconds: number = 0,
    name: string = '',
    description: string = ''
  ) {
    this.apiName = apiName;
    this.achieved = achieved;
    this.unlockTimeEpochSeconds = unlockTimeEpochSeconds;
    this.name = name;
    this.description = description;
  }
}

/** Parsed ISteamUserStats/GetPlayerAchievements response. */
export class SteamPlayerAchievementsData {
  readonly success: boolean;
  readonly achievements: Array<SteamPlayerAchievementEntry>;

  constructor(success: boolean = false, achievements: Array<SteamPlayerAchievementEntry> = []) {
    this.success = success;
    this.achievements = achievements.slice();
  }
}

/**
 * One achievement of the keyless ISteamUserStats GetSchemaForGame response;
 * this is the authoritative total list with localized names and icon URLs.
 */
export class SteamAchievementSchemaEntry {
  readonly apiName: string;
  readonly displayName: string;
  readonly description: string;
  readonly iconUrl: string;
  readonly iconGrayUrl: string;
  readonly hidden: boolean;

  constructor(
    apiName: string = '',
    displayName: string = '',
    description: string = '',
    iconUrl: string = '',
    iconGrayUrl: string = '',
    hidden: boolean = false
  ) {
    this.apiName = apiName;
    this.displayName = displayName;
    this.description = description;
    this.iconUrl = iconUrl;
    this.iconGrayUrl = iconGrayUrl;
    this.hidden = hidden;
  }
}

/** Parsed ISteamUserStats/GetSchemaForGame achievement schema. */
export class SteamAchievementSchemaData {
  readonly achievements: Array<SteamAchievementSchemaEntry>;

  constructor(achievements: Array<SteamAchievementSchemaEntry> = []) {
    this.achievements = achievements.slice();
  }
}

/** Everything the game detail page shows; every part can be independently unavailable. */
export class SteamGameDetailBundle {
  readonly info: SteamGameDetailInfo | null;
  readonly news: Array<SteamGameNewsItem>;
  readonly achievements: SteamGameAchievementsResult | null;

  constructor(
    info: SteamGameDetailInfo | null = null,
    news: Array<SteamGameNewsItem> = [],
    achievements: SteamGameAchievementsResult | null = null
  ) {
    this.info = info;
    this.news = news.slice();
    this.achievements = achievements;
  }
}
