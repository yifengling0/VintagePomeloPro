import { cryptoFramework } from '@kit.CryptoArchitectureKit';
import { IBinaryCrypto } from './vendor/contracts/SteamPlatformContracts';
import { SteamBase64 } from './vendor/protocol/SteamBase64';

/**
 * CryptoArchitectureKit 实现的宿主加密适配（注入 steam_core vendor）。
 * 全部异步实现，与 @kit.CryptoArchitectureKit 的 Promise API 对齐。
 */
export class SteamCryptoAdapter implements IBinaryCrypto {
  async hmacSha1(key: Uint8Array, data: Uint8Array): Promise<Uint8Array> {
    const generator = cryptoFramework.createSymKeyGenerator('HMAC');
    const symKey = await generator.convertKey(new cryptoFramework.DataBlob(key));
    const mac = cryptoFramework.createMac('SHA1');
    await mac.init(symKey);
    await mac.update({ data: data });
    const out = await mac.doFinal();
    return out.data;
  }

  async rsaEncrypt(publicKeyPem: string, data: Uint8Array): Promise<Uint8Array> {
    const der = SteamCryptoAdapter.pemToDer(publicKeyPem);
    const generator = cryptoFramework.createAsyKeyGenerator('RSA2048');
    const pubKey = await generator.convertKey(new cryptoFramework.DataBlob(der));
    const cipher = cryptoFramework.createCipher('RSA2048|PKCS1');
    await cipher.init(cryptoFramework.CryptoMode.ENCRYPT_MODE, pubKey, null);
    const out = await cipher.doFinal({ data: data });
    return out.data;
  }

  async aesCbcEncrypt(key: Uint8Array, iv: Uint8Array, data: Uint8Array): Promise<Uint8Array> {
    return SteamCryptoAdapter.aesCbc(true, key, iv, data);
  }

  async aesCbcDecrypt(key: Uint8Array, iv: Uint8Array, data: Uint8Array): Promise<Uint8Array> {
    return SteamCryptoAdapter.aesCbc(false, key, iv, data);
  }

  private static async aesCbc(encrypt: boolean, key: Uint8Array, iv: Uint8Array,
    data: Uint8Array): Promise<Uint8Array> {
    const bits = key.byteLength * 8;
    const spec = `AES${bits}|CBC|PKCS7`;
    const generator = cryptoFramework.createSymKeyGenerator(`AES${bits}`);
    const symKey = await generator.convertKey(new cryptoFramework.DataBlob(key));
    const cipher = cryptoFramework.createCipher(spec);
    const params: cryptoFramework.IvParamsSpec = {
      algName: 'IvParamsSpec',
      iv: new cryptoFramework.DataBlob(iv)
    };
    await cipher.init(encrypt ? cryptoFramework.CryptoMode.ENCRYPT_MODE
      : cryptoFramework.CryptoMode.DECRYPT_MODE, symKey, params);
    const out = await cipher.doFinal({ data: data });
    return out.data;
  }

  /** X.509 SPKI PEM → DER 原始字节（供 convertKey 使用）。 */
  private static pemToDer(pem: string): Uint8Array {
    const body = pem
      .replace('-----BEGIN PUBLIC KEY-----', '')
      .replace('-----END PUBLIC KEY-----', '')
      .replace(/\s+/g, '');
    return SteamBase64.decode(body);
  }
}
