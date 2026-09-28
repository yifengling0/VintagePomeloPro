import { SteamProtoReader, SteamProtoWriter } from './SteamVarint';

/** One unseen inventory item announced by the Steam CM. */
export class SteamItemAnnouncementItem {
  readonly appId: string;
  readonly contextId: string;
  readonly assetId: string;
  readonly amount: string;
  readonly gainedAtEpochSeconds: number;
  readonly sourceAppId: string;

  constructor(
    appId: string = '0',
    contextId: string = '0',
    assetId: string = '0',
    amount: string = '0',
    gainedAtEpochSeconds: number = 0,
    sourceAppId: string = '0'
  ) {
    this.appId = appId;
    this.contextId = contextId;
    this.assetId = assetId;
    this.amount = amount;
    this.gainedAtEpochSeconds = gainedAtEpochSeconds;
    this.sourceAppId = sourceAppId;
  }
}

/** CMsgClientItemAnnouncements without any platform or secret state. */
export class SteamItemAnnouncement {
  readonly countNewItems: number;
  readonly items: Array<SteamItemAnnouncementItem>;

  constructor(countNewItems: number = 0, items: Array<SteamItemAnnouncementItem> = []) {
    this.countNewItems = countNewItems;
    this.items = items.slice();
  }
}

/** Static protobuf codec for CMsgClientItemAnnouncements. */
export class SteamClientItemAnnouncementsCodec {
  static encode(value: SteamItemAnnouncement): Uint8Array {
    const writer = new SteamProtoWriter();
    if (value.countNewItems > 0) {
      writer.writeUInt32(1, value.countNewItems);
    }
    for (let index = 0; index < value.items.length; index++) {
      writer.writeMessage(2, SteamClientItemAnnouncementsCodec.encodeItem(value.items[index]));
    }
    return writer.toUint8Array();
  }

  static decode(bytes: Uint8Array): SteamItemAnnouncement {
    const reader = new SteamProtoReader(bytes);
    let countNewItems = 0;
    const items: Array<SteamItemAnnouncementItem> = [];
    while (!reader.isAtEnd()) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 1 && tag.wireType === 0) {
        countNewItems = reader.readVarintNumber();
      } else if (tag.fieldNumber === 2 && tag.wireType === 2) {
        items.push(SteamClientItemAnnouncementsCodec.decodeItem(reader.readLengthDelimited()));
      } else {
        reader.skipField(tag.wireType);
      }
    }
    return new SteamItemAnnouncement(countNewItems, items);
  }

  private static encodeItem(value: SteamItemAnnouncementItem): Uint8Array {
    const writer = new SteamProtoWriter();
    if (value.appId !== '0') {
      writer.writeUInt32(1, Number(value.appId));
    }
    if (value.contextId !== '0') {
      writer.writeUInt64(2, value.contextId);
    }
    if (value.assetId !== '0') {
      writer.writeUInt64(3, value.assetId);
    }
    if (value.amount !== '0') {
      writer.writeUInt64(4, value.amount);
    }
    if (value.gainedAtEpochSeconds > 0) {
      writer.writeFixedUInt32(5, value.gainedAtEpochSeconds);
    }
    if (value.sourceAppId !== '0') {
      writer.writeUInt32(6, Number(value.sourceAppId));
    }
    return writer.toUint8Array();
  }

  private static decodeItem(bytes: Uint8Array): SteamItemAnnouncementItem {
    const reader = new SteamProtoReader(bytes);
    let appId = '0';
    let contextId = '0';
    let assetId = '0';
    let amount = '0';
    let gainedAtEpochSeconds = 0;
    let sourceAppId = '0';
    while (!reader.isAtEnd()) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 1 && tag.wireType === 0) {
        appId = reader.readVarintNumber().toString();
      } else if (tag.fieldNumber === 2 && tag.wireType === 0) {
        contextId = reader.readVarintString();
      } else if (tag.fieldNumber === 3 && tag.wireType === 0) {
        assetId = reader.readVarintString();
      } else if (tag.fieldNumber === 4 && tag.wireType === 0) {
        amount = reader.readVarintString();
      } else if (tag.fieldNumber === 5 && tag.wireType === 5) {
        gainedAtEpochSeconds = reader.readFixedUInt32();
      } else if (tag.fieldNumber === 6 && tag.wireType === 0) {
        sourceAppId = reader.readVarintNumber().toString();
      } else {
        reader.skipField(tag.wireType);
      }
    }
    return new SteamItemAnnouncementItem(
      appId,
      contextId,
      assetId,
      amount,
      gainedAtEpochSeconds,
      sourceAppId
    );
  }
}
