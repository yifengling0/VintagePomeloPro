import { SteamCmEndpoint } from './SteamCmEndpoint';
import {
  ISteamCmMultiDecompressor,
  SteamCmDecodedMessage
} from '../protocol/SteamCmProtocol';
import { ISteamCoreLogger, SteamCoreNoopLogger } from './SteamCoreLogger';
import { SteamProtocolIdentity } from './SteamProtocolIdentity';

/** Binary transport boundary implemented by a host, never by steam_core. */
export type SteamCmWireMessageListener = (payload: Uint8Array) => void;
export type SteamCmWireCloseListener = (reason: string) => void;
export type SteamCmWireErrorListener = (error: Error) => void;

export interface ISteamCmWireTransport {
  connect(server: string): Promise<void>;
  send(payload: Uint8Array): Promise<void>;
  close(): Promise<void>;
  onMessage(listener: SteamCmWireMessageListener): void;
  onClose(listener: SteamCmWireCloseListener): void;
  onError(listener: SteamCmWireErrorListener): void;
  clearListeners(): void;
}

export enum SteamCmAuthClientState {
  DISCONNECTED = 'disconnected',
  CONNECTING = 'connecting',
  CONNECTED = 'connected',
  ERROR = 'error'
}

export class SteamCmAuthClientOptions {
  readonly server: string;
  readonly protocolVersion: number;
  readonly requestTimeoutMilliseconds: number;
  readonly multiDecompressor: ISteamCmMultiDecompressor | null;
  readonly logger: ISteamCoreLogger;

  constructor(
    server: string,
    protocolVersion: number = 65581,
    requestTimeoutMilliseconds: number = 30000,
    logger: ISteamCoreLogger = new SteamCoreNoopLogger(),
    multiDecompressor: ISteamCmMultiDecompressor | null = null
  ) {
    SteamCmEndpoint.parse(server);
    if (protocolVersion <= 0 || protocolVersion > 4294967295 || Math.floor(protocolVersion) !== protocolVersion) {
      throw new Error('Steam CM auth protocol version is invalid');
    }
    if (requestTimeoutMilliseconds <= 0 || Math.floor(requestTimeoutMilliseconds) !== requestTimeoutMilliseconds) {
      throw new Error('Steam CM auth request timeout is invalid');
    }
    this.server = server.trim();
    this.protocolVersion = protocolVersion;
    this.requestTimeoutMilliseconds = requestTimeoutMilliseconds;
    this.logger = logger;
    this.multiDecompressor = multiDecompressor;
  }
}

export class SteamCmAuthClientEvent {
  readonly state: SteamCmAuthClientState;
  readonly message: string;

  constructor(state: SteamCmAuthClientState, message: string) {
    this.state = state;
    this.message = message;
  }
}

export type SteamCmAuthClientEventListener = (event: SteamCmAuthClientEvent) => void;

export interface ISteamCmAuthClient {
  connect(): Promise<void>;
  disconnect(): Promise<void>;
  call(targetJobName: string, body: Uint8Array): Promise<SteamCmDecodedMessage>;
  addEventListener(listener: SteamCmAuthClientEventListener): void;
  removeEventListener(listener: SteamCmAuthClientEventListener): void;
  readonly state: SteamCmAuthClientState;
  dispose(): void;
}

/** Timer boundary used by the core so tests do not depend on wall-clock APIs. */
export interface ISteamCmScheduler {
  schedule(delayMilliseconds: number, callback: () => void): number;
  cancel(handle: number): void;
}

export class SteamCmClientOptions {
  readonly server: string;
  readonly steamId64: string;
  readonly machineName: string;
  readonly protocolVersion: number;
  readonly clientPackageVersion: number;
  readonly clientLanguage: string;
  readonly clientOsType: number;
  readonly heartbeatFallbackSeconds: number;
  readonly reconnectInitialDelaySeconds: number;
  readonly reconnectMaxDelaySeconds: number;
  readonly maxReconnectAttempts: number;
  readonly maxGamesPlayedConcurrently: number;
  /** One-shot credentials are supplied by the host and never persisted by the core. */
  readonly authCode: string;
  readonly twoFactorCode: string;
  readonly supportsRateLimitResponse: boolean;
  readonly requestItemAnnouncementsOnLogin: boolean;
  readonly requestTimeoutMilliseconds: number;
  readonly multiDecompressor: ISteamCmMultiDecompressor | null;
  readonly logger: ISteamCoreLogger;

  constructor(
    server: string,
    steamId64: string = '',
    machineName: string = SteamProtocolIdentity.PRODUCT_NAME,
    protocolVersion: number = 65581,
    clientPackageVersion: number = 1771,
    clientLanguage: string = 'english',
    clientOsType: number = 0,
    heartbeatFallbackSeconds: number = 30,
    reconnectInitialDelaySeconds: number = 2,
    reconnectMaxDelaySeconds: number = 60,
    maxReconnectAttempts: number = 0,
    maxGamesPlayedConcurrently: number = 32,
    authCode: string = '',
    twoFactorCode: string = '',
    supportsRateLimitResponse: boolean = true,
    requestItemAnnouncementsOnLogin: boolean = true,
    logger: ISteamCoreLogger = new SteamCoreNoopLogger(),
    requestTimeoutMilliseconds: number = 30000,
    multiDecompressor: ISteamCmMultiDecompressor | null = null
  ) {
    SteamCmEndpoint.parse(server);
    if (heartbeatFallbackSeconds <= 0 || reconnectInitialDelaySeconds <= 0 || reconnectMaxDelaySeconds <= 0) {
      throw new Error('Steam CM timing values must be positive');
    }
    if (reconnectMaxDelaySeconds < reconnectInitialDelaySeconds) {
      throw new Error('Steam CM maximum reconnect delay must not be smaller than the initial delay');
    }
    if (maxReconnectAttempts < 0 || Math.floor(maxReconnectAttempts) !== maxReconnectAttempts) {
      throw new Error('Steam CM maximum reconnect attempts is invalid');
    }
    if (requestTimeoutMilliseconds <= 0 || Math.floor(requestTimeoutMilliseconds) !== requestTimeoutMilliseconds) {
      throw new Error('Steam CM request timeout is invalid');
    }
    if (maxGamesPlayedConcurrently <= 0 || maxGamesPlayedConcurrently > 32 || Math.floor(maxGamesPlayedConcurrently) !== maxGamesPlayedConcurrently) {
      throw new Error('Steam CM concurrent game limit must be between 1 and 32');
    }
    this.server = server.trim();
    this.steamId64 = steamId64;
    this.machineName = machineName;
    this.protocolVersion = protocolVersion;
    this.clientPackageVersion = clientPackageVersion;
    this.clientLanguage = clientLanguage;
    this.clientOsType = clientOsType;
    this.heartbeatFallbackSeconds = heartbeatFallbackSeconds;
    this.reconnectInitialDelaySeconds = reconnectInitialDelaySeconds;
    this.reconnectMaxDelaySeconds = reconnectMaxDelaySeconds;
    this.maxReconnectAttempts = maxReconnectAttempts;
    this.maxGamesPlayedConcurrently = maxGamesPlayedConcurrently;
    this.authCode = authCode;
    this.twoFactorCode = twoFactorCode;
    this.supportsRateLimitResponse = supportsRateLimitResponse;
    this.requestItemAnnouncementsOnLogin = requestItemAnnouncementsOnLogin;
    this.requestTimeoutMilliseconds = requestTimeoutMilliseconds;
    this.multiDecompressor = multiDecompressor;
    this.logger = logger;
  }
}
