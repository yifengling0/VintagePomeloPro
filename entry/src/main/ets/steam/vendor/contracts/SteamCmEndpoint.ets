/**
 * A Steam CM endpoint in the same host[:port] form used by SteamKit2.
 * WebSocket URL construction stays in the detachable core so every host
 * adapter applies the same WSS and /cmsocket/ rules.
 */
export class SteamCmEndpoint {
  static readonly DEFAULT_SERVER: string = 'cmp1-sea1.steamserver.net:443';
  static readonly DEFAULT_WSS_PORT: number = 443;
  readonly host: string;
  readonly port: number;

  constructor(host: string, port: number = SteamCmEndpoint.DEFAULT_WSS_PORT) {
    const normalizedHost: string = host.trim();
    if (normalizedHost.length === 0) {
      throw new Error('Steam CM endpoint host is required');
    }
    if (SteamCmEndpoint.containsWhitespace(normalizedHost) ||
      normalizedHost.indexOf('/') >= 0 || normalizedHost.indexOf('?') >= 0 ||
      normalizedHost.indexOf('#') >= 0 || normalizedHost.indexOf('@') >= 0 ||
      normalizedHost.indexOf('[') >= 0 || normalizedHost.indexOf(']') >= 0) {
      throw new Error('Steam CM endpoint host is invalid');
    }
    if (!SteamCmEndpoint.isValidPort(port)) {
      throw new Error('Steam CM endpoint port must be between 1 and 65535');
    }
    this.host = normalizedHost;
    this.port = port;
  }

  static parse(server: string): SteamCmEndpoint {
    const normalized: string = server.trim();
    if (normalized.length === 0) {
      throw new Error('Steam CM endpoint is required');
    }
    const schemeSeparator: number = normalized.indexOf('://');
    if (schemeSeparator >= 0) {
      const scheme: string = normalized.substring(0, schemeSeparator).toLowerCase();
      if (scheme !== 'wss') {
        throw new Error('Steam CM endpoint must use wss://');
      }
      const authorityStart: number = schemeSeparator + 3;
      const pathStart: number = normalized.indexOf('/', authorityStart);
      const authorityEnd: number = pathStart >= 0 ? pathStart : normalized.length;
      const authority: string = normalized.substring(authorityStart, authorityEnd);
      if (authority.length === 0) {
        throw new Error('Steam CM endpoint authority is required');
      }
      if (pathStart >= 0) {
        const path: string = normalized.substring(pathStart);
        if (path.indexOf('?') >= 0 || path.indexOf('#') >= 0) {
          throw new Error('Steam CM endpoint URL must not contain a query or fragment');
        }
      }
      return SteamCmEndpoint.parseAuthority(authority);
    }
    return SteamCmEndpoint.parseAuthority(normalized);
  }

  static toWebSocketUrl(server: string): string {
    return SteamCmEndpoint.parse(server).toWebSocketUrl();
  }

  toHostPort(): string {
    const hostPart: string = this.host.indexOf(':') >= 0 ? '[' + this.host + ']' : this.host;
    return hostPart + ':' + this.port.toString();
  }

  toWebSocketUrl(): string {
    return 'wss://' + this.toHostPort() + '/cmsocket/';
  }

  private static parseAuthority(authority: string): SteamCmEndpoint {
    const normalizedAuthority: string = authority.trim();
    if (normalizedAuthority.length === 0) {
      throw new Error('Steam CM endpoint authority is required');
    }
    if (normalizedAuthority.startsWith('[')) {
      const closingBracket: number = normalizedAuthority.indexOf(']');
      if (closingBracket <= 1) {
        throw new Error('Steam CM IPv6 endpoint is invalid');
      }
      const host: string = normalizedAuthority.substring(1, closingBracket);
      const suffix: string = normalizedAuthority.substring(closingBracket + 1);
      if (suffix.length === 0) {
        return new SteamCmEndpoint(host);
      }
      if (!suffix.startsWith(':')) {
        throw new Error('Steam CM endpoint port separator is invalid');
      }
      return new SteamCmEndpoint(host, SteamCmEndpoint.parsePort(suffix.substring(1)));
    }
    const firstColon: number = normalizedAuthority.indexOf(':');
    const lastColon: number = normalizedAuthority.lastIndexOf(':');
    if (firstColon >= 0 && firstColon !== lastColon) {
      throw new Error('Steam CM IPv6 endpoint must use brackets');
    }
    if (firstColon < 0) {
      return new SteamCmEndpoint(normalizedAuthority);
    }
    const host: string = normalizedAuthority.substring(0, firstColon);
    const portText: string = normalizedAuthority.substring(firstColon + 1);
    return new SteamCmEndpoint(host, SteamCmEndpoint.parsePort(portText));
  }

  private static parsePort(portText: string): number {
    const normalizedPort: string = portText.trim();
    if (normalizedPort.length === 0) {
      throw new Error('Steam CM endpoint port is required');
    }
    for (let index = 0; index < normalizedPort.length; index++) {
      const code: number = normalizedPort.charCodeAt(index);
      if (code < 48 || code > 57) {
        throw new Error('Steam CM endpoint port must be an integer');
      }
    }
    const port: number = Number(normalizedPort);
    if (!SteamCmEndpoint.isValidPort(port)) {
      throw new Error('Steam CM endpoint port must be between 1 and 65535');
    }
    return port;
  }

  private static isValidPort(port: number): boolean {
    return Number.isSafeInteger(port) && port >= 1 && port <= 65535;
  }

  private static containsWhitespace(value: string): boolean {
    for (let index = 0; index < value.length; index++) {
      const code: number = value.charCodeAt(index);
      if (code <= 32) {
        return true;
      }
    }
    return false;
  }
}
