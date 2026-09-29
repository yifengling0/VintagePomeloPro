import { SteamProtoReader } from './SteamVarint';

/** CMsgClientWalletInfoUpdate push without any platform or secret state. */
export class SteamWalletInfoUpdate {
  readonly hasWallet: boolean;
  /** Wallet balance in minor units, decimal-string to keep int32 semantics explicit. */
  readonly balance: string;
  readonly currency: number;
  readonly balanceDelayed: string;

  constructor(
    hasWallet: boolean = false,
    balance: string = '0',
    currency: number = 0,
    balanceDelayed: string = '0'
  ) {
    this.hasWallet = hasWallet;
    this.balance = balance;
    this.currency = currency;
    this.balanceDelayed = balanceDelayed;
  }
}

/** Static protobuf codec for CMsgClientWalletInfoUpdate (EMsg 5528). */
export class SteamClientWalletInfoCodec {
  static decode(bytes: Uint8Array): SteamWalletInfoUpdate {
    const reader = new SteamProtoReader(bytes);
    let hasWallet = false;
    let balance = '0';
    let currency = 0;
    let balanceDelayed = '0';
    while (!reader.isAtEnd()) {
      const tag = reader.readTag();
      if (tag === null) {
        break;
      }
      if (tag.fieldNumber === 1 && tag.wireType === 0) {
        hasWallet = reader.readVarintNumber() !== 0;
      } else if (tag.fieldNumber === 2 && tag.wireType === 0) {
        balance = SteamClientWalletInfoCodec.readSignedInt(reader.readVarintNumber()).toString();
      } else if (tag.fieldNumber === 3 && tag.wireType === 0) {
        currency = SteamClientWalletInfoCodec.readSignedInt(reader.readVarintNumber());
      } else if (tag.fieldNumber === 4 && tag.wireType === 0) {
        balanceDelayed = SteamClientWalletInfoCodec.readSignedInt(reader.readVarintNumber()).toString();
      } else {
        reader.skipField(tag.wireType);
      }
    }
    return new SteamWalletInfoUpdate(hasWallet, balance, currency, balanceDelayed);
  }

  private static readSignedInt(value: number): number {
    if (!Number.isSafeInteger(value)) {
      return 0;
    }
    return value > 2147483647 ? value - 4294967296 : value;
  }
}
