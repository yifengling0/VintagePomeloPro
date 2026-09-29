import { SteamProtoReader, SteamProtoWriter, SteamUtf8 } from './SteamVarint';

export class SteamGamePlayedMessage {
  readonly gameId: string;
  readonly gameExtraInfo: string;

  constructor(gameId: string, gameExtraInfo: string = '') {
    this.gameId = gameId;
    this.gameExtraInfo = gameExtraInfo;
  }
}

export class SteamClientGamesPlayedPayload {
  readonly games: Array<SteamGamePlayedMessage>;
  readonly clientOsType: number;

  constructor(games: Array<SteamGamePlayedMessage> = [], clientOsType: number = 0) {
    this.games = games.slice();
    this.clientOsType = clientOsType;
  }
}

/** Static codec for CMsgClientGamesPlayed. Unknown fields are skipped. */
export class SteamClientGamesPlayedCodec {
  static encode(games: Array<SteamGamePlayedMessage>, clientOsType: number = 0): Uint8Array {
    const writer = new SteamProtoWriter();
    for (let index = 0; index < games.length; index++) {
      const game = games[index];
      const gameWriter = new SteamProtoWriter();
      gameWriter.writeFixedUInt64(2, game.gameId);
      if (game.gameExtraInfo.length > 0) {
        gameWriter.writeString(7, game.gameExtraInfo);
      }
      writer.writeMessage(1, gameWriter.toUint8Array());
    }
    if (clientOsType >= 0) {
      writer.writeUInt32(2, clientOsType);
    }
    return writer.toUint8Array();
  }

  static encodePayload(payload: SteamClientGamesPlayedPayload): Uint8Array {
    return SteamClientGamesPlayedCodec.encode(payload.games, payload.clientOsType);
  }

  static decode(bytes: Uint8Array): Array<SteamGamePlayedMessage> {
    return SteamClientGamesPlayedCodec.decodePayload(bytes).games;
  }

  static decodePayload(bytes: Uint8Array): SteamClientGamesPlayedPayload {
    const reader = new SteamProtoReader(bytes);
    const games: Array<SteamGamePlayedMessage> = [];
    let clientOsType = 0;
    while (!reader.isAtEnd()) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber !== 1 || tag.wireType !== 2) {
        if (tag.fieldNumber === 2 && tag.wireType === 0) {
          clientOsType = reader.readVarintNumber();
        } else {
          reader.skipField(tag.wireType);
        }
        continue;
      }
      games.push(SteamClientGamesPlayedCodec.decodeGame(reader.readLengthDelimited()));
    }
    return new SteamClientGamesPlayedPayload(games, clientOsType);
  }

  static encodeHeartBeat(): Uint8Array {
    return new Uint8Array(0);
  }

  private static decodeGame(bytes: Uint8Array): SteamGamePlayedMessage {
    const reader = new SteamProtoReader(bytes);
    let gameId = '';
    let gameExtraInfo = '';
    while (!reader.isAtEnd()) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 2 && tag.wireType === 1) {
        gameId = reader.readFixedUInt64();
      } else if (tag.fieldNumber === 7 && tag.wireType === 2) {
        gameExtraInfo = SteamUtf8.decode(reader.readLengthDelimited());
      } else {
        reader.skipField(tag.wireType);
      }
    }
    return new SteamGamePlayedMessage(gameId, gameExtraInfo);
  }
}