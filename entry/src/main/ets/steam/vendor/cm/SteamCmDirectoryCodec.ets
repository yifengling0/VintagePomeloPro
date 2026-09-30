import { SteamJsonParser, SteamJsonValue, SteamJsonValueKind } from '../import/SteamJson';
import { SteamCmEndpoint } from '../contracts/SteamCmEndpoint';
import { ISteamCoreLogger, SteamCoreNoopLogger } from '../contracts/SteamCoreLogger';

const TAG: string = 'CmDirectoryCodec';

/**
 * Static decoder for ISteamDirectory/GetCMListForConnect responses.
 *
 * The live service has shipped two response shapes: a serverlist of
 * {endpoint, type} objects and a legacy websockets_addresses string array.
 * Both are accepted so the decoder keeps working regardless of which shape
 * Steam returns. Every entry is validated with SteamCmEndpoint.parse, so
 * only usable host:port websocket endpoints come back.
 */
export class SteamCmDirectoryCodec {
  /** Returns validated websocket "host:port" entries; empty when unusable. */
  static extractWebsocketServers(
    json: string,
    logger: ISteamCoreLogger = new SteamCoreNoopLogger()
  ): Array<string> {
    const servers: Array<string> = [];
    try {
      const root: SteamJsonValue = SteamJsonParser.parse(json, logger);
      if (root.kind !== SteamJsonValueKind.OBJECT) {
        return servers;
      }
      const response: SteamJsonValue | null = root.getProperty('response');
      if (response === null || response.kind !== SteamJsonValueKind.OBJECT) {
        return servers;
      }
      const serverlistEntries: number = SteamCmDirectoryCodec.collectServerlist(response, servers);
      const addressEntries: number = SteamCmDirectoryCodec.collectWebsocketAddresses(response, servers);
      logger.info(TAG, 'CM directory decoded; serverlistEntries=' + serverlistEntries.toString() +
        ', addressEntries=' + addressEntries.toString() + ', accepted=' + servers.length.toString());
    } catch (error) {
      const message: string = error instanceof Error ? error.message : String(error);
      logger.warn(TAG, 'CM directory decode failed: ' + message);
    }
    return SteamCmDirectoryCodec.deduplicate(servers);
  }

  private static collectServerlist(response: SteamJsonValue, servers: Array<string>): number {
    const serverlist: SteamJsonValue | null = response.getProperty('serverlist');
    if (serverlist === null || serverlist.kind !== SteamJsonValueKind.ARRAY) {
      return 0;
    }
    let examined: number = 0;
    for (let index = 0; index < serverlist.items.length; index++) {
      const entry: SteamJsonValue = serverlist.items[index];
      if (entry.kind !== SteamJsonValueKind.OBJECT) {
        continue;
      }
      examined++;
      const type: SteamJsonValue | null = entry.getProperty('type');
      if (type === null || type.kind !== SteamJsonValueKind.STRING ||
        type.stringValue.trim() !== 'websockets') {
        continue;
      }
      const endpoint: SteamJsonValue | null = entry.getProperty('endpoint');
      if (endpoint === null || endpoint.kind !== SteamJsonValueKind.STRING) {
        continue;
      }
      SteamCmDirectoryCodec.pushValidated(endpoint.stringValue, servers);
    }
    return examined;
  }

  private static collectWebsocketAddresses(response: SteamJsonValue, servers: Array<string>): number {
    const addresses: SteamJsonValue | null = response.getProperty('websockets_addresses');
    if (addresses === null || addresses.kind !== SteamJsonValueKind.ARRAY) {
      return 0;
    }
    let examined: number = 0;
    for (let index = 0; index < addresses.items.length; index++) {
      const entry: SteamJsonValue = addresses.items[index];
      if (entry.kind !== SteamJsonValueKind.STRING) {
        continue;
      }
      examined++;
      SteamCmDirectoryCodec.pushValidated(entry.stringValue, servers);
    }
    return examined;
  }

  private static pushValidated(raw: string, servers: Array<string>): void {
    const trimmed: string = raw.trim();
    if (trimmed.length === 0) {
      return;
    }
    try {
      servers.push(SteamCmEndpoint.parse(trimmed).toHostPort());
    } catch {
      // Skip entries the endpoint parser rejects (wrong scheme, bad port, bracket-less IPv6).
    }
  }

  private static deduplicate(servers: Array<string>): Array<string> {
    const result: Array<string> = [];
    for (let index = 0; index < servers.length; index++) {
      if (result.indexOf(servers[index]) < 0) {
        result.push(servers[index]);
      }
    }
    return result;
  }
}
