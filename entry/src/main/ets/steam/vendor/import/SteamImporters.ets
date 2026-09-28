import {
  SteamBotConfig,
  SteamCredentialSet,
  SteamFarmingOrder,
  SteamFarmingPreference,
  SteamGlobalConfig,
  SteamGlobalConfigSecrets,
} from '../models/SteamModels';
import { SteamVarint } from '../protocol/SteamVarint';
import { SteamJsonParser, SteamJsonValue, SteamJsonValueKind } from './SteamJson';
import { ISteamCoreLogger, SteamCoreNoopLogger } from '../contracts/SteamCoreLogger';

const TAG: string = 'AsfImporter';

export class SteamImportReport {
  readonly recognizedFields: Array<string> = [];
  readonly unknownFields: Array<string> = [];
  readonly warnings: Array<string> = [];

  addRecognized(field: string): void {
    if (!this.recognizedFields.includes(field)) {
      this.recognizedFields.push(field);
    }
  }

  addUnknown(field: string): void {
    if (!this.unknownFields.includes(field)) {
      this.unknownFields.push(field);
    }
  }

  addWarning(message: string): void {
    this.warnings.push(message);
  }
}

export class SteamBotConfigImportResult {
  readonly config: SteamBotConfig;
  readonly report: SteamImportReport;
  /** Credentials are returned only in memory; hosts must move them to secure storage. */
  readonly credentials: SteamCredentialSet;

  constructor(
    config: SteamBotConfig,
    report: SteamImportReport,
    credentials: SteamCredentialSet = new SteamCredentialSet()
  ) {
    this.config = config;
    this.report = report;
    this.credentials = credentials;
  }
}

export class SteamGlobalConfigImportResult {
  readonly config: SteamGlobalConfig;
  readonly report: SteamImportReport;
  /** Sensitive fields are transient and must be moved to host secure storage. */
  readonly secrets: SteamGlobalConfigSecrets;

  constructor(
    config: SteamGlobalConfig,
    report: SteamImportReport,
    secrets: SteamGlobalConfigSecrets = new SteamGlobalConfigSecrets()
  ) {
    this.config = config;
    this.report = report;
    this.secrets = secrets;
  }
}

export class SteamMaFileImportResult {
  readonly credentials: SteamCredentialSet;
  readonly steamId64: string;
  readonly report: SteamImportReport;

  constructor(credentials: SteamCredentialSet, steamId64: string, report: SteamImportReport) {
    this.credentials = credentials;
    this.steamId64 = steamId64;
    this.report = report;
  }
}

export class SteamAsfJsonImporter {
  static importBotConfig(
    json: string,
    accountId: string,
    logger: ISteamCoreLogger = new SteamCoreNoopLogger()
  ): SteamBotConfigImportResult {
    logger.info(TAG, 'ASF bot JSON import started; jsonLength=' + json.length.toString());
    const root = SteamJsonParser.parse(json, logger);
    if (root.kind !== SteamJsonValueKind.OBJECT) {
      throw new Error('ASF bot configuration must be a JSON object');
    }
    const config = new SteamBotConfig(accountId);
    const report = new SteamImportReport();
    let accountName = '';
    let password = '';
    const names = root.propertyNames();
    for (let index = 0; index < names.length; index++) {
      const name = names[index];
      if (name === 'SteamLogin') {
        accountName = SteamAsfJsonImporter.readNullableString(root, name, report);
        report.addRecognized(name);
      } else if (name === 'SteamPassword') {
        password = SteamAsfJsonImporter.readNullableString(root, name, report);
        report.addRecognized(name);
      } else {
        SteamAsfJsonImporter.readBotField(root, name, config, report);
      }
    }
    const result = new SteamBotConfigImportResult(
      config,
      report,
      new SteamCredentialSet(accountName, password)
    );
    logger.info(TAG, 'ASF bot JSON import completed; recognized=' + report.recognizedFields.length.toString() +
      ', unknown=' + report.unknownFields.length.toString() + ', warnings=' + report.warnings.length.toString() +
      ', credentialsPresent=' + String(accountName.length > 0 || password.length > 0));
    return result;
  }

  static importGlobalConfig(
    json: string,
    logger: ISteamCoreLogger = new SteamCoreNoopLogger()
  ): SteamGlobalConfigImportResult {
    logger.info(TAG, 'ASF global JSON import started; jsonLength=' + json.length.toString());
    const root = SteamJsonParser.parse(json, logger);
    if (root.kind !== SteamJsonValueKind.OBJECT) {
      throw new Error('ASF global configuration must be a JSON object');
    }
    const config = new SteamGlobalConfig();
    const report = new SteamImportReport();
    let ipcPassword = '';
    let webProxyPassword = '';
    const names = root.propertyNames();
    for (let index = 0; index < names.length; index++) {
      const name = names[index];
      const value = root.getProperty(name);
      if (value === null) {
        report.addUnknown(name);
      } else if (name === 'AutoRestart') {
        config.autoRestart = SteamAsfJsonImporter.requireBoolean(value, name);
        report.addRecognized(name);
      } else if (name === 'Blacklist') {
        config.blacklist = SteamAsfJsonImporter.readDecimalArray(value, name);
        report.addRecognized(name);
      } else if (name === 'CommandPrefix') {
        config.commandPrefix = SteamAsfJsonImporter.readNullableString(root, name, report);
        report.addRecognized(name);
      } else if (name === 'ConfirmationsLimiterDelay') {
        config.confirmationsLimiterDelay = SteamAsfJsonImporter.requireInteger(value, name);
        report.addRecognized(name);
      } else if (name === 'ConnectionTimeout') {
        config.connectionTimeout = SteamAsfJsonImporter.requireInteger(value, name);
        report.addRecognized(name);
      } else if (name === 'CurrentCulture') {
        config.currentCulture = SteamAsfJsonImporter.readNullableString(root, name, report);
        report.addRecognized(name);
      } else if (name === 'Debug') {
        config.debug = SteamAsfJsonImporter.requireBoolean(value, name);
        report.addRecognized(name);
      } else if (name === 'DefaultBot') {
        config.defaultBot = SteamAsfJsonImporter.readNullableString(root, name, report);
        report.addRecognized(name);
      } else if (name === 'FarmingDelay') {
        config.farmingDelay = SteamAsfJsonImporter.requireInteger(value, name);
        report.addRecognized(name);
      } else if (name === 'FilterBadBots') {
        config.filterBadBots = SteamAsfJsonImporter.requireBoolean(value, name);
        report.addRecognized(name);
      } else if (name === 'GiftsLimiterDelay') {
        config.giftsLimiterDelay = SteamAsfJsonImporter.requireInteger(value, name);
        report.addRecognized(name);
      } else if (name === 'Headless') {
        config.headless = SteamAsfJsonImporter.requireBoolean(value, name);
        report.addRecognized(name);
      } else if (name === 'IdleFarmingPeriod') {
        config.idleFarmingPeriod = SteamAsfJsonImporter.requireInteger(value, name);
        report.addRecognized(name);
      } else if (name === 'InventoryLimiterDelay') {
        config.inventoryLimiterDelay = SteamAsfJsonImporter.requireInteger(value, name);
        report.addRecognized(name);
      } else if (name === 'IPC') {
        config.ipc = SteamAsfJsonImporter.requireBoolean(value, name);
        report.addRecognized(name);
      } else if (name === 'IPCPassword') {
        ipcPassword = SteamAsfJsonImporter.readNullableString(root, name, report);
        report.addRecognized(name);
      } else if (name === 'IPCPasswordFormat') {
        config.ipcPasswordFormat = SteamAsfJsonImporter.readTextLike(value, name);
        report.addRecognized(name);
      } else if (name === 'LicenseID') {
        config.licenseId = SteamAsfJsonImporter.readNullableString(root, name, report);
        report.addRecognized(name);
      } else if (name === 'LoginLimiterDelay') {
        config.loginLimiterDelay = SteamAsfJsonImporter.requireInteger(value, name);
        report.addRecognized(name);
      } else if (name === 'MaxFarmingTime') {
        config.maxFarmingTime = SteamAsfJsonImporter.requireInteger(value, name);
        report.addRecognized(name);
      } else if (name === 'MaxTradeHoldDuration') {
        config.maxTradeHoldDuration = SteamAsfJsonImporter.requireInteger(value, name);
        report.addRecognized(name);
      } else if (name === 'MinFarmingDelayAfterBlock') {
        config.minFarmingDelayAfterBlock = SteamAsfJsonImporter.requireInteger(value, name);
        report.addRecognized(name);
      } else if (name === 'OptimizationMode') {
        config.optimizationMode = SteamAsfJsonImporter.requireIntegerLike(value, name);
        report.addRecognized(name);
      } else if (name === 'PluginsUpdateList') {
        config.pluginsUpdateList = SteamAsfJsonImporter.readStringArray(value, name);
        report.addRecognized(name);
      } else if (name === 'PluginsUpdateMode') {
        config.pluginsUpdateMode = SteamAsfJsonImporter.readTextLike(value, name);
        report.addRecognized(name);
      } else if (name === 'ShutdownIfPossible') {
        config.shutdownIfPossible = SteamAsfJsonImporter.requireBoolean(value, name);
        report.addRecognized(name);
      } else if (name === 'SteamMessagePrefix') {
        config.steamMessagePrefix = SteamAsfJsonImporter.readNullableString(root, name, report);
        report.addRecognized(name);
      } else if (name === 'SteamOwnerID' || name === 's_SteamOwnerID') {
        config.steamOwnerId64 = SteamAsfJsonImporter.normalizeDecimal(SteamAsfJsonImporter.readStringLike(value, name));
        report.addRecognized(name);
      } else if (name === 'SteamProtocols') {
        config.steamProtocols = SteamAsfJsonImporter.requireIntegerLike(value, name);
        report.addRecognized(name);
      } else if (name === 'UpdateChannel') {
        config.updateChannel = SteamAsfJsonImporter.readTextLike(value, name);
        report.addRecognized(name);
      } else if (name === 'UpdatePeriod') {
        config.updatePeriod = SteamAsfJsonImporter.requireInteger(value, name);
        report.addRecognized(name);
      } else if (name === 'WebLimiterDelay') {
        config.webLimiterDelay = SteamAsfJsonImporter.requireInteger(value, name);
        report.addRecognized(name);
      } else if (name === 'WebProxy') {
        config.webProxyText = SteamAsfJsonImporter.readNullableString(root, name, report);
        report.addRecognized(name);
      } else if (name === 'WebProxyUsername') {
        config.webProxyUsername = SteamAsfJsonImporter.readNullableString(root, name, report);
        report.addRecognized(name);
      } else if (name === 'WebProxyPassword') {
        webProxyPassword = SteamAsfJsonImporter.readNullableString(root, name, report);
        report.addRecognized(name);
      } else {
        report.addUnknown(name);
      }
    }
    const result = new SteamGlobalConfigImportResult(
      config,
      report,
      new SteamGlobalConfigSecrets(ipcPassword, webProxyPassword)
    );
    logger.info(TAG, 'ASF global JSON import completed; recognized=' + report.recognizedFields.length.toString() +
      ', unknown=' + report.unknownFields.length.toString() + ', warnings=' + report.warnings.length.toString() +
      ', secretsPresent=' + String(ipcPassword.length > 0 || webProxyPassword.length > 0));
    return result;
  }

  private static readBotField(
    root: SteamJsonValue,
    name: string,
    config: SteamBotConfig,
    report: SteamImportReport
  ): void {
    const value = root.getProperty(name);
    if (value === null) {
      report.addUnknown(name);
      return;
    }
    if (name === 'Enabled') {
      config.enabled = SteamAsfJsonImporter.requireBoolean(value, name);
      report.addRecognized(name);
    } else if (name === 'HoursUntilCardDrops') {
      const hours = SteamAsfJsonImporter.requireInteger(value, name);
      if (hours < 0 || hours > 255) {
        throw new Error(name + ' must be between 0 and 255');
      }
      config.hoursUntilCardDrops = hours;
      report.addRecognized(name);
    } else if (name === 'FarmingPreferences') {
      config.farmingPreferences = SteamAsfJsonImporter.readPreferences(value, report, name);
      report.addRecognized(name);
    } else if (name === 'FarmingOrders') {
      config.farmingOrders = SteamAsfJsonImporter.readOrders(value, report, name);
      report.addRecognized(name);
    } else if (name === 'GamesPlayedWhileIdle') {
      config.gamesPlayedWhileIdle = SteamAsfJsonImporter.readAppIds(value, name);
      if (config.gamesPlayedWhileIdle.length > config.maxGamesPlayedConcurrently) {
        report.addWarning(name + ' exceeds the core concurrent game limit');
      }
      report.addRecognized(name);
    } else if (name === 'Blacklist') {
      config.blacklist = SteamAsfJsonImporter.readAppIds(value, name);
      report.addRecognized(name);
    } else {
      report.addUnknown(name);
    }
  }

  private static readPreferences(value: SteamJsonValue, report: SteamImportReport, field: string): number {
    if (value.kind === SteamJsonValueKind.NUMBER) {
      return SteamAsfJsonImporter.requireInteger(value, field);
    }
    const names: Array<string> = [];
    if (value.kind === SteamJsonValueKind.STRING) {
      SteamAsfJsonImporter.appendFlagNames(value.stringValue, names);
    } else if (value.kind === SteamJsonValueKind.ARRAY) {
      for (let index = 0; index < value.items.length; index++) {
        const item = value.items[index];
        if (item.kind !== SteamJsonValueKind.STRING) {
          throw new Error(field + ' array must contain strings');
        }
        names.push(item.stringValue);
      }
    } else {
      throw new Error(field + ' must be a number, string, or string array');
    }
    let result = 0;
    for (let index = 0; index < names.length; index++) {
      const preference = SteamAsfJsonImporter.preferenceFromName(names[index]);
      if (preference === null) {
        report.addWarning(field + ' contains an unknown flag: ' + names[index]);
      } else {
        result |= preference;
      }
    }
    return result;
  }

  private static readOrders(value: SteamJsonValue, report: SteamImportReport, field: string): Array<SteamFarmingOrder> {
    const values: Array<string> = [];
    if (value.kind === SteamJsonValueKind.ARRAY) {
      for (let index = 0; index < value.items.length; index++) {
        const item = value.items[index];
        if (item.kind === SteamJsonValueKind.STRING) {
          values.push(item.stringValue);
        } else if (item.kind === SteamJsonValueKind.NUMBER) {
          values.push(item.numberValue);
        } else {
          throw new Error(field + ' array contains an invalid value');
        }
      }
    } else if (value.kind === SteamJsonValueKind.STRING) {
      SteamAsfJsonImporter.appendFlagNames(value.stringValue, values);
    } else {
      throw new Error(field + ' must be a string or string array');
    }
    const result: Array<SteamFarmingOrder> = [];
    for (let index = 0; index < values.length; index++) {
      const order = SteamAsfJsonImporter.orderFromName(values[index]);
      if (order === null) {
        report.addWarning(field + ' contains an unknown order: ' + values[index]);
      } else {
        result.push(order);
      }
    }
    return result;
  }

  private static readAppIds(value: SteamJsonValue, field: string): Array<string> {
    if (value.kind !== SteamJsonValueKind.ARRAY) {
      throw new Error(field + ' must be an array');
    }
    const result: Array<string> = [];
    for (let index = 0; index < value.items.length; index++) {
      const item = value.items[index];
      const raw = item.kind === SteamJsonValueKind.NUMBER ? item.numberValue : item.kind === SteamJsonValueKind.STRING ? item.stringValue : '';
      if (raw.length === 0) {
        throw new Error(field + ' contains a non-numeric app id');
      }
      const encoded = SteamVarint.encodeUInt64(raw);
      const normalized = SteamVarint.decodeUInt64(encoded).value;
      if (!result.includes(normalized)) {
        result.push(normalized);
      }
    }
    return result;
  }

  private static readDecimalArray(value: SteamJsonValue, field: string): Array<string> {
    if (value.kind !== SteamJsonValueKind.ARRAY) {
      throw new Error(field + ' must be an array');
    }
    const result: Array<string> = [];
    for (let index = 0; index < value.items.length; index++) {
      const item = value.items[index];
      const raw = SteamAsfJsonImporter.readStringLike(item, field);
      const normalized = SteamAsfJsonImporter.normalizeDecimal(raw);
      if (!result.includes(normalized)) {
        result.push(normalized);
      }
    }
    return result;
  }

  private static readStringArray(value: SteamJsonValue, field: string): Array<string> {
    if (value.kind !== SteamJsonValueKind.ARRAY) {
      throw new Error(field + ' must be an array');
    }
    const result: Array<string> = [];
    for (let index = 0; index < value.items.length; index++) {
      const item = value.items[index];
      if (item.kind !== SteamJsonValueKind.STRING) {
        throw new Error(field + ' array must contain strings');
      }
      if (!result.includes(item.stringValue)) {
        result.push(item.stringValue);
      }
    }
    return result;
  }

  private static appendFlagNames(value: string, output: Array<string>): void {
    const parts = value.split(',');
    for (let index = 0; index < parts.length; index++) {
      const nested = parts[index].split('|');
      for (let nestedIndex = 0; nestedIndex < nested.length; nestedIndex++) {
        const item = nested[nestedIndex].trim();
        if (item.length > 0) {
          output.push(item);
        }
      }
    }
  }

  private static preferenceFromName(name: string): SteamFarmingPreference | null {
    if (name === 'FarmingPausedByDefault') {
      return SteamFarmingPreference.FARMING_PAUSED_BY_DEFAULT;
    }
    if (name === 'ShutdownOnFarmingFinished') {
      return SteamFarmingPreference.SHUTDOWN_ON_FARMING_FINISHED;
    }
    if (name === 'SendOnFarmingFinished') {
      return SteamFarmingPreference.SEND_ON_FARMING_FINISHED;
    }
    if (name === 'FarmPriorityQueueOnly') {
      return SteamFarmingPreference.FARM_PRIORITY_QUEUE_ONLY;
    }
    if (name === 'SkipRefundableGames') {
      return SteamFarmingPreference.SKIP_REFUNDABLE_GAMES;
    }
    if (name === 'SkipUnplayedGames') {
      return SteamFarmingPreference.SKIP_UNPLAYED_GAMES;
    }
    if (name === 'EnableRiskyCardsDiscovery') {
      return SteamFarmingPreference.ENABLE_RISKY_CARDS_DISCOVERY;
    }
    if (name === 'AutoUnpackBoosterPacks') {
      return SteamFarmingPreference.AUTO_UNPACK_BOOSTER_PACKS;
    }
    return null;
  }

  private static orderFromName(name: string): SteamFarmingOrder | null {
    if (name === 'Unordered' || name === '0') {
      return SteamFarmingOrder.UNORDERED;
    }
    if (name === 'AppIDsAscending' || name === '1') {
      return SteamFarmingOrder.APP_IDS_ASCENDING;
    }
    if (name === 'AppIDsDescending' || name === '2') {
      return SteamFarmingOrder.APP_IDS_DESCENDING;
    }
    if (name === 'CardDropsAscending' || name === '3') {
      return SteamFarmingOrder.CARD_DROPS_ASCENDING;
    }
    if (name === 'CardDropsDescending' || name === '4') {
      return SteamFarmingOrder.CARD_DROPS_DESCENDING;
    }
    if (name === 'HoursAscending' || name === '5') {
      return SteamFarmingOrder.HOURS_ASCENDING;
    }
    if (name === 'HoursDescending' || name === '6') {
      return SteamFarmingOrder.HOURS_DESCENDING;
    }
    if (name === 'NamesAscending' || name === '7') {
      return SteamFarmingOrder.NAMES_ASCENDING;
    }
    if (name === 'NamesDescending' || name === '8') {
      return SteamFarmingOrder.NAMES_DESCENDING;
    }
    if (name === 'Random' || name === '9') {
      return SteamFarmingOrder.RANDOM;
    }
    if (name === 'BadgeLevelsAscending' || name === '10') {
      return SteamFarmingOrder.BADGE_LEVELS_ASCENDING;
    }
    if (name === 'BadgeLevelsDescending' || name === '11') {
      return SteamFarmingOrder.BADGE_LEVELS_DESCENDING;
    }
    if (name === 'MarketableAscending' || name === '14') {
      return SteamFarmingOrder.MARKETABLE_ASCENDING;
    }
    if (name === 'MarketableDescending' || name === '15') {
      return SteamFarmingOrder.MARKETABLE_DESCENDING;
    }
    return null;
  }

  private static requireBoolean(value: SteamJsonValue, field: string): boolean {
    if (value.kind !== SteamJsonValueKind.BOOLEAN) {
      throw new Error(field + ' must be a boolean');
    }
    return value.booleanValue;
  }

  private static requireIntegerLike(value: SteamJsonValue, field: string): number {
    if (value.kind === SteamJsonValueKind.NUMBER) {
      return SteamAsfJsonImporter.requireInteger(value, field);
    }
    if (value.kind !== SteamJsonValueKind.STRING) {
      throw new Error(field + ' must be a number or numeric string');
    }
    const result = Number(value.stringValue);
    if (!Number.isSafeInteger(result)) {
      throw new Error(field + ' must be a safe integer');
    }
    return result;
  }

  private static readTextLike(value: SteamJsonValue, field: string): string {
    if (value.kind === SteamJsonValueKind.STRING || value.kind === SteamJsonValueKind.NUMBER) {
      return value.kind === SteamJsonValueKind.STRING ? value.stringValue : value.numberValue;
    }
    throw new Error(field + ' must be a string or number');
  }

  private static readNullableString(root: SteamJsonValue, field: string, report: SteamImportReport): string {
    const value = root.getProperty(field);
    if (value === null || value.kind === SteamJsonValueKind.NULL) {
      return '';
    }
    if (value.kind !== SteamJsonValueKind.STRING) {
      throw new Error(field + ' must be a string or null');
    }
    return value.stringValue;
  }

  private static requireInteger(value: SteamJsonValue, field: string): number {
    if (value.kind !== SteamJsonValueKind.NUMBER) {
      throw new Error(field + ' must be a number');
    }
    const result = Number(value.numberValue);
    if (!Number.isSafeInteger(result)) {
      throw new Error(field + ' must be a safe integer');
    }
    return result;
  }

  private static readStringLike(value: SteamJsonValue, field: string): string {
    if (value.kind === SteamJsonValueKind.STRING) {
      return value.stringValue;
    }
    if (value.kind === SteamJsonValueKind.NUMBER) {
      return value.numberValue;
    }
    throw new Error(field + ' must be a decimal string or number');
  }

  private static normalizeDecimal(value: string): string {
    const encoded = SteamVarint.encodeUInt64(value);
    return SteamVarint.decodeUInt64(encoded).value;
  }
}

export class SteamMaFileImporter {
  static importMaFile(
    json: string,
    logger: ISteamCoreLogger = new SteamCoreNoopLogger()
  ): SteamMaFileImportResult {
    logger.info(TAG, 'maFile import started; jsonLength=' + json.length.toString());
    const root = SteamJsonParser.parse(json, logger);
    if (root.kind !== SteamJsonValueKind.OBJECT) {
      throw new Error('maFile must be a JSON object');
    }
    const report = new SteamImportReport();
    const steamIdValue = root.getProperty('steamid');
    if (steamIdValue === null) {
      throw new Error('maFile steamid is required');
    }
    const steamId64 = SteamMaFileImporter.normalizeDecimal(SteamMaFileImporter.readStringLike(steamIdValue, 'steamid'));
    const accountName = SteamMaFileImporter.readOptionalString(root, 'account_name', report);
    const sharedSecret = SteamMaFileImporter.readRequiredString(root, 'shared_secret', report);
    const identitySecret = SteamMaFileImporter.readRequiredString(root, 'identity_secret', report);
    const deviceId = SteamMaFileImporter.readOptionalString(root, 'device_id', report);
    let accessToken = '';
    let oauthToken = '';
    const session = root.getProperty('Session');
    if (session !== null) {
      if (session.kind !== SteamJsonValueKind.OBJECT) {
        report.addWarning('Session is present but is not an object');
      } else {
        accessToken = SteamMaFileImporter.readNestedOptionalString(session, 'AccessToken', report, 'Session.AccessToken');
        oauthToken = SteamMaFileImporter.readNestedOptionalString(session, 'OAuthToken', report, 'Session.OAuthToken');
        const sessionNames = session.propertyNames();
        for (let index = 0; index < sessionNames.length; index++) {
          const name = sessionNames[index];
          if (name !== 'AccessToken' && name !== 'OAuthToken' && name !== 'SteamID') {
            report.addUnknown('Session.' + name);
          }
        }
      }
      report.addRecognized('Session');
    }
    const known: Array<string> = [
      'steamid',
      'account_name',
      'shared_secret',
      'identity_secret',
      'device_id',
      'Session'
    ];
    const names = root.propertyNames();
    for (let index = 0; index < names.length; index++) {
      if (!known.includes(names[index])) {
        report.addUnknown(names[index]);
      }
    }
    const credentials = new SteamCredentialSet(
      accountName,
      '',
      '',
      '',
      sharedSecret,
      identitySecret,
      deviceId,
      accessToken,
      oauthToken
    );
    const result = new SteamMaFileImportResult(credentials, steamId64, report);
    logger.info(TAG, 'maFile import completed; steamIdPresent=' + String(steamId64.length > 0) +
      ', recognized=' + report.recognizedFields.length.toString() +
      ', unknown=' + report.unknownFields.length.toString() + ', warnings=' + report.warnings.length.toString());
    return result;
  }

  private static readRequiredString(root: SteamJsonValue, name: string, report: SteamImportReport): string {
    const value = root.getProperty(name);
    if (value === null) {
      throw new Error('maFile ' + name + ' is required');
    }
    if (value.kind !== SteamJsonValueKind.STRING || value.stringValue.length === 0) {
      throw new Error('maFile ' + name + ' must be a non-empty string');
    }
    report.addRecognized(name);
    return value.stringValue;
  }

  private static readOptionalString(root: SteamJsonValue, name: string, report: SteamImportReport): string {
    const value = root.getProperty(name);
    if (value === null) {
      return '';
    }
    if (value.kind !== SteamJsonValueKind.STRING) {
      throw new Error('maFile ' + name + ' must be a string');
    }
    report.addRecognized(name);
    return value.stringValue;
  }

  private static readNestedOptionalString(root: SteamJsonValue, name: string, report: SteamImportReport, reportName: string): string {
    const value = root.getProperty(name);
    if (value === null) {
      return '';
    }
    if (value.kind !== SteamJsonValueKind.STRING) {
      throw new Error('maFile ' + reportName + ' must be a string');
    }
    report.addRecognized(reportName);
    return value.stringValue;
  }

  private static readStringLike(value: SteamJsonValue, field: string): string {
    if (value.kind === SteamJsonValueKind.STRING) {
      return value.stringValue;
    }
    if (value.kind === SteamJsonValueKind.NUMBER) {
      return value.numberValue;
    }
    throw new Error('maFile ' + field + ' must be a decimal string or number');
  }

  private static normalizeDecimal(value: string): string {
    const encoded = SteamVarint.encodeUInt64(value);
    return SteamVarint.decodeUInt64(encoded).value;
  }
}
