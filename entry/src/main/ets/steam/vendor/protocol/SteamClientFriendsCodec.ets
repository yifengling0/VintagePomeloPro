import { SteamProtoReader, SteamProtoWriter, SteamUtf8 } from './SteamVarint';

/** EFriendRelationship subset used to classify friends list entries. */
export enum SteamFriendRelationship {
  NONE = 0,
  BLOCKED = 1,
  REQUEST_RECIPIENT = 2,
  FRIEND = 3,
  REQUEST_INITIATOR = 4,
  IGNORED = 5,
  IGNORED_FRIEND = 6
}

/** EPersonaState subset used for presence display. */
export enum SteamPersonaStateValue {
  OFFLINE = 0,
  ONLINE = 1,
  BUSY = 2,
  AWAY = 3,
  SNOOZE = 4,
  LOOKING_TO_TRADE = 5,
  LOOKING_TO_PLAY = 6,
  INVISIBLE = 7
}

/** EClientPersonaStateFlag mask used when requesting friend persona data. */
export class SteamClientPersonaStateRequestFlags {
  static readonly STATUS: number = 1;
  static readonly PLAYER_NAME: number = 2;
  static readonly PRESENCE: number = 16;
  static readonly LAST_SEEN: number = 64;
  static readonly GAME_EXTRA_INFO: number = 256;
  /** Default mask: everything the social page renders. */
  static readonly DEFAULT: number = SteamClientPersonaStateRequestFlags.STATUS |
    SteamClientPersonaStateRequestFlags.PLAYER_NAME |
    SteamClientPersonaStateRequestFlags.PRESENCE |
    SteamClientPersonaStateRequestFlags.LAST_SEEN |
    SteamClientPersonaStateRequestFlags.GAME_EXTRA_INFO;
}

/** One friend entry from CMsgClientFriendsList (SteamID64 kept as decimal string). */
export class SteamFriendListEntry {
  readonly steamId64: string;
  readonly relationship: SteamFriendRelationship;

  constructor(steamId64: string = '0', relationship: SteamFriendRelationship = SteamFriendRelationship.NONE) {
    this.steamId64 = steamId64;
    this.relationship = relationship;
  }
}

/** CMsgClientFriendsList without any platform or secret state. */
export class SteamFriendsList {
  readonly incremental: boolean;
  readonly friends: Array<SteamFriendListEntry>;

  constructor(incremental: boolean = false, friends: Array<SteamFriendListEntry> = []) {
    this.incremental = incremental;
    this.friends = friends.slice();
  }
}

/** One persona update from CMsgClientPersonaState. */
export class SteamPersonaStateEntry {
  readonly steamId64: string;
  readonly personaState: SteamPersonaStateValue;
  readonly playerName: string;
  readonly gameAppId: string;
  readonly gameName: string;
  readonly gameId: string;
  readonly avatarHashHex: string;
  readonly lastLogoffEpochSeconds: number;
  readonly lastLogonEpochSeconds: number;

  constructor(
    steamId64: string = '0',
    personaState: SteamPersonaStateValue = SteamPersonaStateValue.OFFLINE,
    playerName: string = '',
    gameAppId: string = '0',
    gameName: string = '',
    gameId: string = '0',
    avatarHashHex: string = '',
    lastLogoffEpochSeconds: number = 0,
    lastLogonEpochSeconds: number = 0
  ) {
    this.steamId64 = steamId64;
    this.personaState = personaState;
    this.playerName = playerName;
    this.gameAppId = gameAppId;
    this.gameName = gameName;
    this.gameId = gameId;
    this.avatarHashHex = avatarHashHex;
    this.lastLogoffEpochSeconds = lastLogoffEpochSeconds;
    this.lastLogonEpochSeconds = lastLogonEpochSeconds;
  }
}

/** CMsgClientPersonaState without any platform or secret state. */
export class SteamPersonaStates {
  readonly statusFlags: number;
  readonly friends: Array<SteamPersonaStateEntry>;

  constructor(statusFlags: number = 0, friends: Array<SteamPersonaStateEntry> = []) {
    this.statusFlags = statusFlags;
    this.friends = friends.slice();
  }
}

/** One nickname entry from CMsgClientPlayerNicknameList. */
export class SteamPlayerNicknameEntry {
  readonly steamId64: string;
  readonly nickname: string;

  constructor(steamId64: string = '0', nickname: string = '') {
    this.steamId64 = steamId64;
    this.nickname = nickname;
  }
}

/** CMsgClientPlayerNicknameList without any platform or secret state. */
export class SteamPlayerNicknames {
  readonly removal: boolean;
  readonly incremental: boolean;
  readonly nicknames: Array<SteamPlayerNicknameEntry>;

  constructor(
    removal: boolean = false,
    incremental: boolean = false,
    nicknames: Array<SteamPlayerNicknameEntry> = []
  ) {
    this.removal = removal;
    this.incremental = incremental;
    this.nicknames = nicknames.slice();
  }
}

/**
 * Static protobuf codecs for the CM friends channel: CMsgClientFriendsList,
 * CMsgClientPersonaState, CMsgClientRequestFriendData and
 * CMsgClientPlayerNicknameList. Field numbers follow SteamKit2 generated models
 * (SteamMsgClientServerFriends.cs).
 */
export class SteamClientFriendsCodec {
  static decodeFriendsList(bytes: Uint8Array): SteamFriendsList {
    const reader = new SteamProtoReader(bytes);
    let incremental = false;
    const friends: Array<SteamFriendListEntry> = [];
    while (!reader.isAtEnd()) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 1 && tag.wireType === 0) {
        incremental = reader.readVarintNumber() !== 0;
      } else if (tag.fieldNumber === 2 && tag.wireType === 2) {
        friends.push(SteamClientFriendsCodec.decodeFriendListEntry(reader.readLengthDelimited()));
      } else {
        reader.skipField(tag.wireType);
      }
    }
    return new SteamFriendsList(incremental, friends);
  }

  static encodeRequestFriendData(personaStateRequested: number, steamIds64: Array<string>): Uint8Array {
    if (steamIds64.length > 500) {
      throw new Error('Steam allows at most 500 friends per persona state request');
    }
    const writer = new SteamProtoWriter();
    if (personaStateRequested > 0) {
      writer.writeUInt32(1, personaStateRequested);
    }
    for (let index = 0; index < steamIds64.length; index++) {
      const steamId64 = steamIds64[index].trim();
      if (steamId64.length > 0) {
        writer.writeFixedUInt64(2, steamId64);
      }
    }
    return writer.toUint8Array();
  }

  static decodePersonaStates(bytes: Uint8Array): SteamPersonaStates {
    const reader = new SteamProtoReader(bytes);
    let statusFlags = 0;
    const friends: Array<SteamPersonaStateEntry> = [];
    while (!reader.isAtEnd()) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 1 && tag.wireType === 0) {
        statusFlags = reader.readVarintNumber();
      } else if (tag.fieldNumber === 2 && tag.wireType === 2) {
        friends.push(SteamClientFriendsCodec.decodePersonaStateEntry(reader.readLengthDelimited()));
      } else {
        reader.skipField(tag.wireType);
      }
    }
    return new SteamPersonaStates(statusFlags, friends);
  }

  static decodePlayerNicknames(bytes: Uint8Array): SteamPlayerNicknames {
    const reader = new SteamProtoReader(bytes);
    let removal = false;
    let incremental = false;
    const nicknames: Array<SteamPlayerNicknameEntry> = [];
    while (!reader.isAtEnd()) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 1 && tag.wireType === 0) {
        removal = reader.readVarintNumber() !== 0;
      } else if (tag.fieldNumber === 2 && tag.wireType === 0) {
        incremental = reader.readVarintNumber() !== 0;
      } else if (tag.fieldNumber === 3 && tag.wireType === 2) {
        nicknames.push(SteamClientFriendsCodec.decodePlayerNicknameEntry(reader.readLengthDelimited()));
      } else {
        reader.skipField(tag.wireType);
      }
    }
    return new SteamPlayerNicknames(removal, incremental, nicknames);
  }

  private static decodeFriendListEntry(bytes: Uint8Array): SteamFriendListEntry {
    const reader = new SteamProtoReader(bytes);
    let steamId64 = '0';
    let relationship: SteamFriendRelationship = SteamFriendRelationship.NONE;
    while (!reader.isAtEnd()) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 1 && tag.wireType === 1) {
        steamId64 = reader.readFixedUInt64();
      } else if (tag.fieldNumber === 2 && tag.wireType === 0) {
        relationship = SteamClientFriendsCodec.relationshipOf(reader.readVarintNumber());
      } else {
        reader.skipField(tag.wireType);
      }
    }
    return new SteamFriendListEntry(steamId64, relationship);
  }

  private static decodePersonaStateEntry(bytes: Uint8Array): SteamPersonaStateEntry {
    const reader = new SteamProtoReader(bytes);
    let steamId64 = '0';
    let personaState: SteamPersonaStateValue = SteamPersonaStateValue.OFFLINE;
    let playerName = '';
    let gameAppId = '0';
    let gameName = '';
    let gameId = '0';
    let avatarHashHex = '';
    let lastLogoffEpochSeconds = 0;
    let lastLogonEpochSeconds = 0;
    while (!reader.isAtEnd()) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 1 && tag.wireType === 1) {
        steamId64 = reader.readFixedUInt64();
      } else if (tag.fieldNumber === 2 && tag.wireType === 0) {
        personaState = SteamClientFriendsCodec.personaStateOf(reader.readVarintNumber());
      } else if (tag.fieldNumber === 3 && tag.wireType === 0) {
        gameAppId = reader.readVarintNumber().toString();
      } else if (tag.fieldNumber === 15 && tag.wireType === 2) {
        playerName = SteamClientFriendsCodec.decodeUtf8(reader.readLengthDelimited());
      } else if (tag.fieldNumber === 31 && tag.wireType === 2) {
        avatarHashHex = SteamClientFriendsCodec.bytesToHex(reader.readLengthDelimited());
      } else if (tag.fieldNumber === 45 && tag.wireType === 0) {
        lastLogoffEpochSeconds = reader.readVarintNumber();
      } else if (tag.fieldNumber === 46 && tag.wireType === 0) {
        lastLogonEpochSeconds = reader.readVarintNumber();
      } else if (tag.fieldNumber === 55 && tag.wireType === 2) {
        gameName = SteamClientFriendsCodec.decodeUtf8(reader.readLengthDelimited());
      } else if (tag.fieldNumber === 56 && tag.wireType === 1) {
        gameId = reader.readFixedUInt64();
      } else {
        reader.skipField(tag.wireType);
      }
    }
    return new SteamPersonaStateEntry(
      steamId64,
      personaState,
      playerName,
      gameAppId,
      gameName,
      gameId,
      avatarHashHex,
      lastLogoffEpochSeconds,
      lastLogonEpochSeconds
    );
  }

  private static decodePlayerNicknameEntry(bytes: Uint8Array): SteamPlayerNicknameEntry {
    const reader = new SteamProtoReader(bytes);
    let steamId64 = '0';
    let nickname = '';
    while (!reader.isAtEnd()) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 1 && tag.wireType === 1) {
        steamId64 = reader.readFixedUInt64();
      } else if (tag.fieldNumber === 3 && tag.wireType === 2) {
        nickname = SteamClientFriendsCodec.decodeUtf8(reader.readLengthDelimited());
      } else {
        reader.skipField(tag.wireType);
      }
    }
    return new SteamPlayerNicknameEntry(steamId64, nickname);
  }

  private static relationshipOf(value: number): SteamFriendRelationship {
    if (value === SteamFriendRelationship.BLOCKED) {
      return SteamFriendRelationship.BLOCKED;
    }
    if (value === SteamFriendRelationship.REQUEST_RECIPIENT) {
      return SteamFriendRelationship.REQUEST_RECIPIENT;
    }
    if (value === SteamFriendRelationship.FRIEND) {
      return SteamFriendRelationship.FRIEND;
    }
    if (value === SteamFriendRelationship.REQUEST_INITIATOR) {
      return SteamFriendRelationship.REQUEST_INITIATOR;
    }
    if (value === SteamFriendRelationship.IGNORED) {
      return SteamFriendRelationship.IGNORED;
    }
    if (value === SteamFriendRelationship.IGNORED_FRIEND) {
      return SteamFriendRelationship.IGNORED_FRIEND;
    }
    return SteamFriendRelationship.NONE;
  }

  private static personaStateOf(value: number): SteamPersonaStateValue {
    if (value === SteamPersonaStateValue.ONLINE) {
      return SteamPersonaStateValue.ONLINE;
    }
    if (value === SteamPersonaStateValue.BUSY) {
      return SteamPersonaStateValue.BUSY;
    }
    if (value === SteamPersonaStateValue.AWAY) {
      return SteamPersonaStateValue.AWAY;
    }
    if (value === SteamPersonaStateValue.SNOOZE) {
      return SteamPersonaStateValue.SNOOZE;
    }
    if (value === SteamPersonaStateValue.LOOKING_TO_TRADE) {
      return SteamPersonaStateValue.LOOKING_TO_TRADE;
    }
    if (value === SteamPersonaStateValue.LOOKING_TO_PLAY) {
      return SteamPersonaStateValue.LOOKING_TO_PLAY;
    }
    if (value === SteamPersonaStateValue.INVISIBLE) {
      return SteamPersonaStateValue.INVISIBLE;
    }
    return SteamPersonaStateValue.OFFLINE;
  }

  private static decodeUtf8(bytes: Uint8Array): string {
    return SteamUtf8.decode(bytes);
  }

  private static bytesToHex(bytes: Uint8Array): string {
    let hex = '';
    for (let index = 0; index < bytes.length; index++) {
      const high: number = (bytes[index] >> 4) & 0x0F;
      const low: number = bytes[index] & 0x0F;
      hex += String.fromCharCode(SteamClientFriendsCodec.HEX_DIGITS.charCodeAt(high)) +
        String.fromCharCode(SteamClientFriendsCodec.HEX_DIGITS.charCodeAt(low));
    }
    return hex;
  }

  private static readonly HEX_DIGITS: string = '0123456789abcdef';
}
