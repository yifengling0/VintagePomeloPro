import { SteamProtoReader, SteamProtoWriter, SteamUtf8, SteamVarint } from './SteamVarint';
import { SteamAuthError, SteamAuthErrorKind } from '../contracts/SteamAuthErrors';
import { SteamProtocolIdentity } from '../contracts/SteamProtocolIdentity';

export enum SteamCmMessageType {
  MULTI = 1,
  /** Server-side unified notifications use EMsg.ServiceMethod (146). */
  SERVICE_METHOD = 146,
  SERVICE_METHOD_CALL_FROM_CLIENT = 151,
  SERVICE_METHOD_SEND_TO_CLIENT = 152,
  SERVICE_METHOD_RESPONSE = 147,
  CLIENT_HEARTBEAT = 703,
  CLIENT_SESSION_TOKEN = 850,
  CLIENT_LOGON_RESPONSE = 751,
  CLIENT_LOGGED_OFF = 757,
  CLIENT_HELLO = 9805,
  SERVICE_METHOD_CALL_FROM_CLIENT_NON_AUTHED = 9804,
  CLIENT_GAMES_PLAYED_WITH_DATA_BLOB = 5410,
  CLIENT_LOGON = 5514,
  CLIENT_ITEM_ANNOUNCEMENTS = 5576,
  CLIENT_REQUEST_ITEM_ANNOUNCEMENTS = 5577,
  CLIENT_WALLET_INFO_UPDATE = 5528,
  CLIENT_PERSONA_STATE = 766,
  CLIENT_FRIENDS_LIST = 767,
  CLIENT_REQUEST_FRIEND_DATA = 815,
  CLIENT_PLAYER_NICKNAME_LIST = 5587
}

export enum SteamCmResultCode {
  OK = 1,
  EXPIRED = 27,
  LOGON_SESSION_REPLACED = 34,
  ACCOUNT_LOGON_DENIED = 63,
  INVALID_LOGIN_AUTH_CODE = 65,
  ACCOUNT_LOGON_DENIED_NO_MAIL = 66,
  EXPIRED_LOGIN_AUTH_CODE = 71,
  ACCOUNT_LOGIN_DENIED_NEED_TWO_FACTOR = 85,
  TWO_FACTOR_CODE_MISMATCH = 88
}

export enum SteamCmHeaderKind {
  UNKNOWN = 'unknown',
  LEGACY = 'legacy',
  EXTENDED = 'extended',
  PROTOBUF = 'protobuf'
}

const STEAM_PROTO_MASK: number = 2147483648;
const EXTENDED_HEADER_SIZE: number = 36;
const EXTENDED_HEADER_VERSION: number = 2;
const EXTENDED_HEADER_CANARY: number = 239;
const MAX_MULTI_UNCOMPRESSED_SIZE: number = 67108864;
const MAX_MULTI_NESTING: number = 8;

class SteamCmMultiEnvelope {
  readonly uncompressedSize: number;
  readonly messageBody: Uint8Array;

  constructor(uncompressedSize: number, messageBody: Uint8Array) {
    this.uncompressedSize = uncompressedSize;
    this.messageBody = messageBody.slice();
  }
}

export class SteamCmProtoHeader {
  readonly steamId64: string;
  readonly clientSessionId: number;
  readonly jobIdSource: string;
  readonly jobIdTarget: string;
  readonly targetJobName: string;
  readonly eresult: number;
  readonly errorMessage: string;

  constructor(
    steamId64: string = '',
    clientSessionId: number = 0,
    jobIdSource: string = '0',
    jobIdTarget: string = '0',
    targetJobName: string = '',
    eresult: number = 0,
    errorMessage: string = ''
  ) {
    this.steamId64 = steamId64.length > 0 ? steamId64 : '0';
    this.clientSessionId = clientSessionId;
    this.jobIdSource = jobIdSource.length > 0 ? jobIdSource : '0';
    this.jobIdTarget = jobIdTarget.length > 0 ? jobIdTarget : '0';
    this.targetJobName = targetJobName;
    this.eresult = eresult;
    this.errorMessage = errorMessage;
  }
}

/** Host-provided decompression boundary for compressed CMsgMulti envelopes. */
export interface ISteamCmMultiDecompressor {
  decompress(payload: Uint8Array, expectedSize: number): Promise<Uint8Array>;
}

export class SteamCmDecodedMessage {
  readonly messageType: number;
  readonly headerKind: SteamCmHeaderKind;
  readonly protoHeader: SteamCmProtoHeader;
  readonly targetJobId: string;
  readonly sourceJobId: string;
  readonly body: Uint8Array;
  readonly frameBytes: number;
  readonly headerBytes: number;

  constructor(
    messageType: number,
    headerKind: SteamCmHeaderKind,
    protoHeader: SteamCmProtoHeader,
    targetJobId: string,
    sourceJobId: string,
    body: Uint8Array,
    frameBytes: number = 0,
    headerBytes: number = 0
  ) {
    this.messageType = messageType;
    this.headerKind = headerKind;
    this.protoHeader = protoHeader;
    this.targetJobId = targetJobId;
    this.sourceJobId = sourceJobId;
    this.body = body.slice();
    this.frameBytes = frameBytes;
    this.headerBytes = headerBytes;
  }
}

/**
 * Safe top-level frame metadata for diagnostics. It deliberately excludes
 * payload bytes, job IDs, SteamIDs and message bodies.
 */
export class SteamCmFrameInfo {
  readonly payloadBytes: number;
  readonly hasMessageHeader: boolean;
  readonly messageType: number;
  readonly headerKind: SteamCmHeaderKind;
  readonly headerLength: number;
  readonly bodyBytes: number;

  constructor(
    payloadBytes: number,
    hasMessageHeader: boolean,
    messageType: number,
    headerKind: SteamCmHeaderKind,
    headerLength: number,
    bodyBytes: number
  ) {
    this.payloadBytes = payloadBytes;
    this.hasMessageHeader = hasMessageHeader;
    this.messageType = messageType;
    this.headerKind = headerKind;
    this.headerLength = headerLength;
    this.bodyBytes = bodyBytes;
  }

  toLogString(): string {
    return 'bytes=' + this.payloadBytes.toString() +
      ', headerPresent=' + String(this.hasMessageHeader) +
      ', messageType=' + this.messageType.toString() +
      ', headerKind=' + String(this.headerKind) +
      ', headerLength=' + this.headerLength.toString() +
      ', bodyBytes=' + this.bodyBytes.toString();
  }
}

export class SteamClientLogonRequest {
  readonly accountName: string;
  readonly password: string;
  readonly accessToken: string;
  readonly steamId64: string;
  readonly protocolVersion: number;
  readonly cellId: number;
  readonly clientPackageVersion: number;
  readonly clientLanguage: string;
  readonly clientOsType: number;
  readonly shouldRememberPassword: boolean;
  readonly machineName: string;
  readonly authCode: string;
  readonly twoFactorCode: string;
  readonly loginKey: string;
  readonly supportsRateLimitResponse: boolean;

  constructor(
    accountName: string,
    password: string = '',
    accessToken: string = '',
    steamId64: string = '',
    protocolVersion: number = 65581,
    cellId: number = 0,
    clientPackageVersion: number = 1771,
    clientLanguage: string = 'english',
    clientOsType: number = 0,
    shouldRememberPassword: boolean = true,
    machineName: string = SteamProtocolIdentity.PRODUCT_NAME,
    authCode: string = '',
    twoFactorCode: string = '',
    loginKey: string = '',
    supportsRateLimitResponse: boolean = true
  ) {
    this.accountName = accountName;
    this.password = password;
    this.accessToken = accessToken;
    this.steamId64 = steamId64;
    this.protocolVersion = protocolVersion;
    this.cellId = cellId;
    this.clientPackageVersion = clientPackageVersion;
    this.clientLanguage = clientLanguage;
    this.clientOsType = clientOsType;
    this.shouldRememberPassword = shouldRememberPassword;
    this.machineName = machineName;
    this.authCode = authCode;
    this.twoFactorCode = twoFactorCode;
    this.loginKey = loginKey;
    this.supportsRateLimitResponse = supportsRateLimitResponse;
  }
}

export class SteamClientLogonResponse {
  readonly result: number;
  readonly heartbeatSeconds: number;
  readonly legacyHeartbeatSeconds: number;
  readonly extendedResult: number;
  readonly steamId64: string;
  /** Wallet balance in minor units (cents) reported by the account's own logon. */
  readonly walletBalance: string;
  /** Steam ECurrencyCode enum id of the account's own wallet currency. */
  readonly walletCurrency: number;

  constructor(
    result: number = 0,
    heartbeatSeconds: number = 0,
    legacyHeartbeatSeconds: number = 0,
    extendedResult: number = 0,
    steamId64: string = '',
    walletBalance: string = '',
    walletCurrency: number = 0
  ) {
    this.result = result;
    this.heartbeatSeconds = heartbeatSeconds;
    this.legacyHeartbeatSeconds = legacyHeartbeatSeconds;
    this.extendedResult = extendedResult;
    this.steamId64 = steamId64;
    this.walletBalance = walletBalance;
    this.walletCurrency = walletCurrency;
  }
}

export class SteamClientLogonCodec {
  static encode(request: SteamClientLogonRequest): Uint8Array {
    const writer = new SteamProtoWriter();
    if (request.protocolVersion > 0) {
      writer.writeUInt32(1, request.protocolVersion);
    }
    if (request.cellId > 0) {
      writer.writeUInt32(3, request.cellId);
    }
    if (request.clientPackageVersion > 0) {
      writer.writeUInt32(5, request.clientPackageVersion);
    }
    if (request.clientLanguage.length > 0) {
      writer.writeString(6, request.clientLanguage);
    }
    if (request.clientOsType >= 0) {
      writer.writeUInt32(7, request.clientOsType);
    } else if (request.clientOsType === -1) {
      writer.writeUInt32(7, 4294967295);
    }
    if (request.shouldRememberPassword) {
      writer.writeUInt32(8, 1);
    }
    if (request.steamId64.length > 0) {
      writer.writeFixedUInt64(22, request.steamId64);
    }
    if (request.accountName.length > 0) {
      writer.writeString(50, request.accountName);
    }
    if (request.password.length > 0) {
      writer.writeString(51, request.password);
    }
    if (request.loginKey.length > 0) {
      writer.writeString(60, request.loginKey);
    }
    if (request.authCode.length > 0) {
      writer.writeString(84, request.authCode);
    }
    if (request.machineName.length > 0) {
      writer.writeString(96, request.machineName);
    }
    if (request.twoFactorCode.length > 0) {
      writer.writeString(101, request.twoFactorCode);
    }
    if (request.supportsRateLimitResponse) {
      writer.writeUInt32(102, 1);
    }
    if (request.accessToken.length > 0) {
      writer.writeString(108, request.accessToken);
    }
    return writer.toUint8Array();
  }

  static decodeResponse(bytes: Uint8Array): SteamClientLogonResponse {
    const reader = new SteamProtoReader(bytes);
    let result = 0;
    let heartbeatSeconds = 0;
    let legacyHeartbeatSeconds = 0;
    let extendedResult = 0;
    let steamId64 = '';
    let walletBalance = '';
    let walletCurrency = 0;
    while (!reader.isAtEnd()) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 1 && tag.wireType === 0) {
        result = reader.readVarintNumber();
      } else if (tag.fieldNumber === 2 && tag.wireType === 0) {
        legacyHeartbeatSeconds = reader.readVarintNumber();
      } else if (tag.fieldNumber === 3 && tag.wireType === 0) {
        heartbeatSeconds = reader.readVarintNumber();
      } else if (tag.fieldNumber === 10 && tag.wireType === 0) {
        extendedResult = reader.readVarintNumber();
      } else if (tag.fieldNumber === 20 && tag.wireType === 1) {
        steamId64 = reader.readFixedUInt64();
      } else if (tag.fieldNumber === 32 && tag.wireType === 0) {
        walletBalance = reader.readVarintNumber().toString();
      } else if (tag.fieldNumber === 33 && tag.wireType === 0) {
        walletCurrency = reader.readVarintNumber();
      } else {
        reader.skipField(tag.wireType);
      }
    }
    return new SteamClientLogonResponse(
      result,
      heartbeatSeconds,
      legacyHeartbeatSeconds,
      extendedResult,
      steamId64,
      walletBalance,
      walletCurrency
    );
  }
}

export class SteamCmProtocol {
  /** Returns only non-secret shape information suitable for failure logs. */
  static inspectFrame(bytes: Uint8Array): SteamCmFrameInfo {
    if (bytes.length < 4) {
      return new SteamCmFrameInfo(
        bytes.length,
        false,
        0,
        SteamCmHeaderKind.UNKNOWN,
        0,
        0
      );
    }
    const rawMessageType = SteamVarint.decodeFixedUInt32(bytes, 0).value;
    const isProtobuf = rawMessageType >= STEAM_PROTO_MASK;
    const messageType = isProtobuf ? rawMessageType - STEAM_PROTO_MASK : rawMessageType;
    if (isProtobuf) {
      if (bytes.length < 8) {
        return new SteamCmFrameInfo(
          bytes.length,
          true,
          messageType,
          SteamCmHeaderKind.PROTOBUF,
          0,
          0
        );
      }
      const headerLength = SteamVarint.decodeFixedUInt32(bytes, 4).value;
      const bodyOffset = 8 + headerLength;
      const bodyBytes = bodyOffset <= bytes.length ? bytes.length - bodyOffset : 0;
      return new SteamCmFrameInfo(
        bytes.length,
        true,
        messageType,
        SteamCmHeaderKind.PROTOBUF,
        headerLength,
        bodyBytes
      );
    }
    if (bytes.length >= 5 && bytes[4] === EXTENDED_HEADER_SIZE) {
      const headerVersion: number = bytes.length >= 7
        ? SteamCmProtocol.readUInt16(bytes, 5)
        : 0;
      return new SteamCmFrameInfo(
        bytes.length,
        true,
        messageType,
        SteamCmHeaderKind.EXTENDED,
        EXTENDED_HEADER_SIZE,
        headerVersion === EXTENDED_HEADER_VERSION && bytes.length >= 40
          ? bytes.length - 40
          : 0
      );
    }
    if (bytes.length >= 20) {
      return new SteamCmFrameInfo(
        bytes.length,
        true,
        messageType,
        SteamCmHeaderKind.LEGACY,
        16,
        bytes.length - 20
      );
    }
    return new SteamCmFrameInfo(
      bytes.length,
      true,
      messageType,
      SteamCmHeaderKind.UNKNOWN,
      0,
      0
    );
  }

  static encodeProtobufMessage(
    messageType: number,
    header: SteamCmProtoHeader,
    body: Uint8Array
  ): Uint8Array {
    if (messageType <= 0 || messageType >= STEAM_PROTO_MASK || Math.floor(messageType) !== messageType) {
      throw new Error('Invalid Steam CM message type');
    }
    const headerBytes = SteamCmProtocol.encodeProtoHeader(header);
    const messageValue = messageType + STEAM_PROTO_MASK;
    const result = new Uint8Array(8 + headerBytes.length + body.length);
    SteamCmProtocol.writeFixedUInt32(result, 0, messageValue);
    SteamCmProtocol.writeFixedUInt32(result, 4, headerBytes.length);
    SteamCmProtocol.copy(result, 8, headerBytes);
    SteamCmProtocol.copy(result, 8 + headerBytes.length, body);
    return result;
  }

  static decodeMessage(bytes: Uint8Array): SteamCmDecodedMessage {
    if (bytes.length < 4) {
      throw SteamCmProtocol.protocolFailure(
        'Steam CM message is shorter than its fixed message header',
        'cm_message_header'
      );
    }
    const rawMessageType = SteamVarint.decodeFixedUInt32(bytes, 0).value;
    if (rawMessageType >= STEAM_PROTO_MASK) {
      return SteamCmProtocol.decodeProtobufMessage(bytes, rawMessageType - STEAM_PROTO_MASK);
    }
    return SteamCmProtocol.decodeLegacyMessage(bytes, rawMessageType);
  }

  /**
   * Decodes one CM frame and expands SteamKit2's protobuf Multi envelope.
   * Multi payloads are length-prefixed CM frames, not protobuf messages that
   * can be passed directly to the normal message decoder.
   */
  static decodeMessages(bytes: Uint8Array): Array<SteamCmDecodedMessage> {
    return SteamCmProtocol.decodeMessagesAtDepth(bytes, 0);
  }

  /**
   * Async counterpart used by hosts that provide the gzip implementation.
   * CMsgMulti compression is deliberately kept outside the platform-neutral
   * protocol layer because HarmonyOS and other hosts expose different zlib APIs.
   */
  static async decodeMessagesAsync(
    bytes: Uint8Array,
    decompressor: ISteamCmMultiDecompressor | null = null
  ): Promise<Array<SteamCmDecodedMessage>> {
    return await SteamCmProtocol.decodeMessagesAtDepthAsync(bytes, 0, decompressor);
  }

  private static encodeProtoHeader(header: SteamCmProtoHeader): Uint8Array {
    const writer = new SteamProtoWriter();
    if (header.steamId64 !== '0') {
      writer.writeFixedUInt64(1, header.steamId64);
    }
    if (header.clientSessionId !== 0) {
      writer.writeInt32(2, header.clientSessionId);
    }
    if (header.jobIdSource !== '0') {
      writer.writeFixedUInt64(10, header.jobIdSource);
    }
    if (header.jobIdTarget !== '0') {
      writer.writeFixedUInt64(11, header.jobIdTarget);
    }
    if (header.targetJobName.length > 0) {
      writer.writeString(12, header.targetJobName);
    }
    if (header.eresult !== 0) {
      writer.writeInt32(13, header.eresult);
    }
    if (header.errorMessage.length > 0) {
      writer.writeString(14, header.errorMessage);
    }
    return writer.toUint8Array();
  }

  private static decodeProtoHeader(bytes: Uint8Array): SteamCmProtoHeader {
    const reader = new SteamProtoReader(bytes);
    let steamId64 = '0';
    let clientSessionId = 0;
    let jobIdSource = '0';
    let jobIdTarget = '0';
    let targetJobName = '';
    let eresult = 0;
    let errorMessage = '';
    while (!reader.isAtEnd()) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 1 && tag.wireType === 1) {
        steamId64 = reader.readFixedUInt64();
      } else if (tag.fieldNumber === 2 && tag.wireType === 0) {
        clientSessionId = reader.readInt32();
      } else if (tag.fieldNumber === 10 && tag.wireType === 1) {
        jobIdSource = reader.readFixedUInt64();
      } else if (tag.fieldNumber === 11 && tag.wireType === 1) {
        jobIdTarget = reader.readFixedUInt64();
      } else if (tag.fieldNumber === 12 && tag.wireType === 2) {
        targetJobName = SteamUtf8.decode(reader.readLengthDelimited());
      } else if (tag.fieldNumber === 13 && tag.wireType === 0) {
        eresult = reader.readInt32();
      } else if (tag.fieldNumber === 14 && tag.wireType === 2) {
        errorMessage = SteamUtf8.decode(reader.readLengthDelimited());
      } else {
        reader.skipField(tag.wireType);
      }
    }
    return new SteamCmProtoHeader(
      steamId64,
      clientSessionId,
      jobIdSource,
      jobIdTarget,
      targetJobName,
      eresult,
      errorMessage
    );
  }

  private static decodeProtobufMessage(bytes: Uint8Array, messageType: number): SteamCmDecodedMessage {
    if (bytes.length < 8) {
      throw SteamCmProtocol.protocolFailure(
        'Steam CM protobuf message is missing its header length',
        'cm_protobuf_header'
      );
    }
    const headerLength = SteamVarint.decodeFixedUInt32(bytes, 4).value;
    const bodyOffset = 8 + headerLength;
    if (headerLength < 0 || bodyOffset > bytes.length) {
      throw SteamCmProtocol.protocolFailure(
        'Steam CM protobuf header exceeds the message length',
        'cm_protobuf_header'
      );
    }
    const headerBytes = bytes.slice(8, bodyOffset);
    const body = bytes.slice(bodyOffset);
    let header: SteamCmProtoHeader;
    try {
      header = SteamCmProtocol.decodeProtoHeader(headerBytes);
    } catch (error) {
      const normalized: Error = error instanceof Error
        ? error
        : new Error('Steam CM protobuf header could not be decoded');
      throw SteamCmProtocol.protocolFailure(
        'Steam CM protobuf header decode failed: ' + normalized.message,
        'cm_protobuf_header'
      );
    }
    return new SteamCmDecodedMessage(
      messageType,
      SteamCmHeaderKind.PROTOBUF,
      header,
      header.jobIdTarget,
      header.jobIdSource,
      body,
      bytes.length,
      headerLength
    );
  }

  private static decodeMessagesAtDepth(
    bytes: Uint8Array,
    depth: number
  ): Array<SteamCmDecodedMessage> {
    if (depth > MAX_MULTI_NESTING) {
      throw SteamCmProtocol.protocolFailure(
        'Steam CM Multi nesting is too deep',
        'cm_multi_depth'
      );
    }
    const message = SteamCmProtocol.decodeMessage(bytes);
    if (message.messageType !== SteamCmMessageType.MULTI) {
      const result = new Array<SteamCmDecodedMessage>();
      result.push(message);
      return result;
    }
    const frames = SteamCmProtocol.decodeMultiFrames(message.body);
    const result = new Array<SteamCmDecodedMessage>();
    for (let index = 0; index < frames.length; index++) {
      const nested = SteamCmProtocol.decodeMessagesAtDepth(frames[index], depth + 1);
      for (let nestedIndex = 0; nestedIndex < nested.length; nestedIndex++) {
        result.push(nested[nestedIndex]);
      }
    }
    return result;
  }

  private static async decodeMessagesAtDepthAsync(
    bytes: Uint8Array,
    depth: number,
    decompressor: ISteamCmMultiDecompressor | null
  ): Promise<Array<SteamCmDecodedMessage>> {
    if (depth > MAX_MULTI_NESTING) {
      throw SteamCmProtocol.protocolFailure(
        'Steam CM Multi nesting is too deep',
        'cm_multi_depth'
      );
    }
    const message = SteamCmProtocol.decodeMessage(bytes);
    if (message.messageType !== SteamCmMessageType.MULTI) {
      const result = new Array<SteamCmDecodedMessage>();
      result.push(message);
      return result;
    }
    const frames = await SteamCmProtocol.decodeMultiFramesAsync(message.body, decompressor);
    const result = new Array<SteamCmDecodedMessage>();
    for (let index = 0; index < frames.length; index++) {
      const nested = await SteamCmProtocol.decodeMessagesAtDepthAsync(frames[index], depth + 1, decompressor);
      for (let nestedIndex = 0; nestedIndex < nested.length; nestedIndex++) {
        result.push(nested[nestedIndex]);
      }
    }
    return result;
  }

  private static decodeMultiFrames(bytes: Uint8Array): Array<Uint8Array> {
    const envelope = SteamCmProtocol.readMultiEnvelope(bytes);
    if (envelope.uncompressedSize > 0) {
      throw SteamCmProtocol.protocolFailure(
        'Compressed Steam CM Multi messages require a host decompressor',
        'cm_multi_decompress'
      );
    }
    return SteamCmProtocol.splitMultiFrames(envelope.messageBody);
  }

  private static async decodeMultiFramesAsync(
    bytes: Uint8Array,
    decompressor: ISteamCmMultiDecompressor | null
  ): Promise<Array<Uint8Array>> {
    const envelope = SteamCmProtocol.readMultiEnvelope(bytes);
    let messageBody = envelope.messageBody;
    if (envelope.uncompressedSize > 0) {
      if (decompressor === null) {
        throw SteamCmProtocol.protocolFailure(
          'Compressed Steam CM Multi messages require a host decompressor',
          'cm_multi_decompress'
        );
      }
      messageBody = await decompressor.decompress(messageBody, envelope.uncompressedSize);
      if (messageBody.length !== envelope.uncompressedSize) {
        throw SteamCmProtocol.protocolFailure(
          'Steam CM Multi decompressor returned an unexpected size',
          'cm_multi_decompress'
        );
      }
    }
    return SteamCmProtocol.splitMultiFrames(messageBody);
  }

  private static readMultiEnvelope(bytes: Uint8Array): SteamCmMultiEnvelope {
    const reader = new SteamProtoReader(bytes);
    let uncompressedSize = 0;
    let messageBody = new Uint8Array(0);
    while (!reader.isAtEnd()) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 1 && tag.wireType === 0) {
        uncompressedSize = reader.readVarintNumber();
      } else if (tag.fieldNumber === 2 && tag.wireType === 2) {
        messageBody = reader.readLengthDelimited();
      } else {
        reader.skipField(tag.wireType);
      }
    }
    if (!Number.isSafeInteger(uncompressedSize) ||
      uncompressedSize < 0 || uncompressedSize > MAX_MULTI_UNCOMPRESSED_SIZE) {
      throw SteamCmProtocol.protocolFailure(
        'Steam CM Multi uncompressed size is invalid',
        'cm_multi_envelope'
      );
    }
    return new SteamCmMultiEnvelope(uncompressedSize, messageBody);
  }

  private static splitMultiFrames(messageBody: Uint8Array): Array<Uint8Array> {
    const result = new Array<Uint8Array>();
    let offset = 0;
    while (offset < messageBody.length) {
      if (offset + 4 > messageBody.length) {
        throw SteamCmProtocol.protocolFailure(
          'Steam CM Multi frame length is incomplete',
          'cm_multi_frame'
        );
      }
      const frameLength = SteamVarint.decodeFixedUInt32(messageBody, offset).value;
      offset += 4;
      if (frameLength <= 0 || frameLength > 2147483647 ||
        offset + frameLength > messageBody.length) {
        throw SteamCmProtocol.protocolFailure(
          'Steam CM Multi frame length is invalid',
          'cm_multi_frame'
        );
      }
      result.push(messageBody.slice(offset, offset + frameLength));
      offset += frameLength;
    }
    return result;
  }

  private static decodeLegacyMessage(bytes: Uint8Array, messageType: number): SteamCmDecodedMessage {
    if (bytes.length >= 5 && bytes[4] === EXTENDED_HEADER_SIZE) {
      if (bytes.length < 7) {
        throw SteamCmProtocol.protocolFailure(
          'Steam CM extended header is missing its version',
          'cm_extended_header'
        );
      }
      const headerVersion: number = SteamCmProtocol.readUInt16(bytes, 5);
      if (headerVersion !== EXTENDED_HEADER_VERSION) {
        throw SteamCmProtocol.protocolFailure(
          'Steam CM extended header version is unsupported',
          'cm_extended_header'
        );
      }
      if (bytes.length < 40) {
        throw SteamCmProtocol.protocolFailure(
          'Steam CM extended header is incomplete',
          'cm_extended_header'
        );
      }
      if (bytes[23] !== EXTENDED_HEADER_CANARY) {
        throw SteamCmProtocol.protocolFailure(
          'Steam CM extended header canary is invalid',
          'cm_extended_header'
        );
      }
      const targetJobId = SteamVarint.decodeFixedUInt64(bytes, 7).value;
      const sourceJobId = SteamVarint.decodeFixedUInt64(bytes, 15).value;
      const steamId64 = SteamVarint.decodeFixedUInt64(bytes, 24).value;
      const sessionIdRaw = SteamVarint.decodeFixedUInt32(bytes, 32).value;
      const sessionId = SteamCmProtocol.toSignedInt32(sessionIdRaw);
      const header = new SteamCmProtoHeader(steamId64, sessionId, sourceJobId, targetJobId);
      return new SteamCmDecodedMessage(
        messageType,
        SteamCmHeaderKind.EXTENDED,
        header,
        targetJobId,
        sourceJobId,
        bytes.slice(40),
        bytes.length,
        EXTENDED_HEADER_SIZE
      );
    }
    if (bytes.length < 20) {
      throw SteamCmProtocol.protocolFailure(
        'Steam CM legacy message is missing its job header',
        'cm_legacy_header'
      );
    }
    const targetJobId = SteamVarint.decodeFixedUInt64(bytes, 4).value;
    const sourceJobId = SteamVarint.decodeFixedUInt64(bytes, 12).value;
    const header = new SteamCmProtoHeader('0', 0, sourceJobId, targetJobId);
    return new SteamCmDecodedMessage(
      messageType,
      SteamCmHeaderKind.LEGACY,
      header,
        targetJobId,
        sourceJobId,
        bytes.slice(20),
        bytes.length,
        16
    );
  }

  private static writeFixedUInt32(target: Uint8Array, offset: number, value: number): void {
    const bytes = SteamVarint.encodeFixedUInt32(value);
    for (let index = 0; index < bytes.length; index++) {
      target[offset + index] = bytes[index];
    }
  }

  private static copy(target: Uint8Array, offset: number, source: Uint8Array): void {
    for (let index = 0; index < source.length; index++) {
      target[offset + index] = source[index];
    }
  }

  private static readUInt16(bytes: Uint8Array, offset: number): number {
    return bytes[offset] + bytes[offset + 1] * 256;
  }

  private static toSignedInt32(value: number): number {
    return value > 2147483647 ? value - 4294967296 : value;
  }

  private static protocolFailure(message: string, stage: string): SteamAuthError {
    return new SteamAuthError(message, SteamAuthErrorKind.PROTOCOL, stage);
  }
}
