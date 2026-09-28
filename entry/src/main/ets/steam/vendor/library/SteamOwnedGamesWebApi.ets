import {
  SteamWebRequest,
  SteamWebResponse,
  ISteamWebTransport
} from '../contracts/SteamPlatformContracts';
import { SteamProtocolIdentity } from '../contracts/SteamProtocolIdentity';
import { ISteamCoreLogger, SteamCoreNoopLogger } from '../contracts/SteamCoreLogger';
import { SteamGameLibraryResult, SteamLibraryGame } from '../models/SteamModels';
import {
  SteamJsonParser,
  SteamJsonValue,
  SteamJsonValueKind
} from '../import/SteamJson';
import { SteamVarint } from '../protocol/SteamVarint';

const TAG: string = 'OwnedGamesWebApi';
const CDN_IMAGE_BASE: string =
  'https://cdn.akamai.steamstatic.com/steamcommunity/public/images/apps/';
const CDN_CAPSULE_BASE: string =
  'https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/';

/**
 * Builds the authenticated IPlayerService/GetOwnedGames Web API URL used by
 * the Steam client's own library screens. The OAuth access token is passed as
 * a query parameter in place of a publisher API key; the token only lives in
 * the in-memory request and never enters logs, snapshots or caches.
 */
export class SteamOwnedGamesWebApiEndpoint {
  static buildUrl(
    steamId64: string,
    accessToken: string,
    language: string = 'english'
  ): string {
    const normalizedSteamId: string = SteamOwnedGamesWebApiEndpoint.normalizeSteamId64(steamId64);
    const normalizedToken: string = accessToken.trim();
    if (normalizedToken.length <= 0) {
      throw new Error('Steam owned games Web API requires an access token');
    }
    const selectedLanguage: string = language.trim().length > 0 ? language.trim() : 'english';
    return 'https://api.steampowered.com/IPlayerService/GetOwnedGames/v1/?' +
      'access_token=' + encodeURIComponent(normalizedToken) +
      '&steamid=' + encodeURIComponent(normalizedSteamId) +
      '&include_appinfo=true' +
      '&include_played_free_games=true' +
      '&include_free_sub=true' +
      '&skip_unvetted_apps=false' +
      '&include_extended_appinfo=true' +
      '&language=' + encodeURIComponent(selectedLanguage) +
      '&format=json';
  }

  private static normalizeSteamId64(steamId64: string): string {
    const normalized: string = steamId64.trim();
    if (normalized.length <= 0) {
      throw new Error('Steam owned games steamId64 must not be empty');
    }
    return SteamVarint.decodeUInt64(SteamVarint.encodeUInt64(normalized)).value;
  }
}

/**
 * Parses the IPlayerService/GetOwnedGames JSON response into the same
 * SteamLibraryGame model used by the CM unified message, so one merger can
 * combine all sources. Playtimes arrive in minutes and are converted to
 * seconds exactly like the CM path.
 */
export class SteamOwnedGamesWebApiParser {
  static parse(
    body: string,
    logger: ISteamCoreLogger = new SteamCoreNoopLogger()
  ): SteamGameLibraryResult {
    const root: SteamJsonValue = SteamJsonParser.parse(body, logger);
    if (root.kind !== SteamJsonValueKind.OBJECT) {
      throw new Error('Steam owned games Web API response is not a JSON object');
    }
    const errorValue: SteamJsonValue | null = root.getProperty('error');
    if (errorValue !== null && errorValue.kind === SteamJsonValueKind.STRING) {
      throw new Error('Steam owned games Web API returned an error: ' + errorValue.stringValue);
    }
    const response: SteamJsonValue | null = root.getProperty('response');
    if (response === null || response.kind !== SteamJsonValueKind.OBJECT) {
      throw new Error('Steam owned games Web API response is missing its response object');
    }
    const gameCount: number = SteamOwnedGamesWebApiParser.readNumber(
      response.getProperty('game_count'), 0);
    const gamesValue: SteamJsonValue | null = response.getProperty('games');
    const games: Array<SteamLibraryGame> = [];
    const warnings: Array<string> = [];
    if (gamesValue !== null && gamesValue.kind === SteamJsonValueKind.ARRAY) {
      for (let index = 0; index < gamesValue.items.length; index++) {
        const entry: SteamJsonValue = gamesValue.items[index];
        if (entry.kind !== SteamJsonValueKind.OBJECT) {
          continue;
        }
        const appId: number = SteamOwnedGamesWebApiParser.readNumber(
          entry.getProperty('appid'), 0);
        if (appId <= 0) {
          warnings.push('owned games Web API entry is missing its appid');
          continue;
        }
        const name: string = SteamOwnedGamesWebApiParser.readString(entry.getProperty('name'));
        const iconHash: string = SteamOwnedGamesWebApiParser.readString(
          entry.getProperty('img_icon_url'));
        const logoHash: string = SteamOwnedGamesWebApiParser.readString(
          entry.getProperty('img_logo_url'));
        const capsuleFilename: string = SteamOwnedGamesWebApiParser.readString(
          entry.getProperty('capsule_filename'));
        const iconUrl: string = iconHash.length > 0
          ? CDN_IMAGE_BASE + appId.toString() + '/' + iconHash + '.jpg'
          : '';
        const logoUrl: string = capsuleFilename.length > 0
          ? CDN_CAPSULE_BASE + appId.toString() + '/' + capsuleFilename
          : (logoHash.length > 0
            ? CDN_IMAGE_BASE + appId.toString() + '/' + logoHash + '.jpg'
            : '');
        games.push(new SteamLibraryGame(
          appId.toString(),
          name,
          logoUrl,
          iconUrl,
          SteamOwnedGamesWebApiParser.minutesToSeconds(
            SteamOwnedGamesWebApiParser.readNumber(entry.getProperty('playtime_forever'), 0)),
          SteamOwnedGamesWebApiParser.minutesToSeconds(
            SteamOwnedGamesWebApiParser.readNumber(entry.getProperty('playtime_2weeks'), 0)),
          SteamOwnedGamesWebApiParser.readNumber(entry.getProperty('rtime_last_played'), 0),
          false
        ));
      }
    }
    const totalCount: number = gameCount > 0 ? gameCount : games.length;
    logger.info(TAG, 'owned games Web API response parsed; games=' + games.length.toString() +
      ', total=' + totalCount.toString() + ', warnings=' + warnings.length.toString());
    return new SteamGameLibraryResult(games, totalCount, warnings, 'json');
  }

  private static readNumber(value: SteamJsonValue | null, fallback: number): number {
    if (value === null || value.kind !== SteamJsonValueKind.NUMBER) {
      return fallback;
    }
    const parsed: number = Number(value.numberValue);
    return Number.isFinite(parsed) ? parsed : fallback;
  }

  private static readString(value: SteamJsonValue | null): string {
    if (value === null || value.kind !== SteamJsonValueKind.STRING) {
      return '';
    }
    return value.stringValue;
  }

  private static minutesToSeconds(minutes: number): number {
    if (!Number.isFinite(minutes) || minutes < 0) {
      return 0;
    }
    const seconds: number = Math.round(minutes * 60);
    return Number.isSafeInteger(seconds) ? seconds : 0;
  }
}

/**
 * Fetches the complete owned game list over the authenticated Web API using
 * the account's OAuth access token. Unlike the community tile page this
 * endpoint returns every owned game with appinfo, playtime and artwork, which
 * keeps the library total correct even when the CM channel is unavailable.
 */
export class SteamOwnedGamesWebApiClient {
  static async fetch(
    transport: ISteamWebTransport,
    steamId64: string,
    accessToken: string,
    language: string = 'english',
    logger: ISteamCoreLogger = new SteamCoreNoopLogger()
  ): Promise<SteamGameLibraryResult> {
    const url: string = SteamOwnedGamesWebApiEndpoint.buildUrl(steamId64, accessToken, language);
    const headers: Map<string, string> = new Map<string, string>();
    headers.set('Accept', 'application/json');
    headers.set('User-Agent', SteamProtocolIdentity.userAgent('SteamWebApi'));
    headers.set('Referer', 'https://store.steampowered.com/');
    logger.info(TAG, 'owned games Web API request started; steamIdPresent=' +
      String(steamId64.trim().length > 0) + ', accessTokenPresent=' + String(accessToken.trim().length > 0));
    const response: SteamWebResponse = await transport.get(new SteamWebRequest(url, headers));
    if (!response.isOk()) {
      logger.warn(TAG, 'owned games Web API request failed; status=' +
        response.statusCode.toString());
      throw new Error('Steam owned games Web API request failed with HTTP ' +
        response.statusCode.toString());
    }
    const result: SteamGameLibraryResult = SteamOwnedGamesWebApiParser.parse(response.body, logger);
    logger.info(TAG, 'owned games Web API request completed; games=' +
      result.games.length.toString() + ', total=' + result.totalCount.toString());
    return result;
  }
}
