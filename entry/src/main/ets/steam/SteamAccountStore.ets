import { asset } from '@kit.AssetStoreKit';
import { util } from '@kit.ArkTS';
import { common } from '@kit.AbilityKit';
import preferences from '@ohos.data.preferences';

/** Steam 凭据种类（AssetStore alias 尾段）。 */
export const STEAM_SECRET_KINDS: Array<string> = [
  'refresh_token', 'access_token', 'client_refresh_token',
  'shared_secret', 'identity_secret', 'device_id', 'guard_data'
];

/** 非敏感档案（preferences 明文可存）。 */
export class SteamProfile {
  accountName: string = '';
  steamId64: string = '';
  avatarUrl: string = '';
  personaName: string = '';
}

/**
 * Steam 账号数据存储：
 * - 敏感凭据（token/shared_secret 等）→ @ohos.security.asset（芯片级，DEVICE_FIRST_UNLOCKED）
 * - 非敏感档案与库缓存 → 独立 preferences（steam_profile_v1）
 * 绝不把凭据写进 preferences，也绝不把任何真实凭据提交进仓库。
 */
export class SteamAccountStore {
  private static instance: SteamAccountStore | null = null;
  private prefs: preferences.Preferences | null = null;

  static getInstance(): SteamAccountStore {
    if (!SteamAccountStore.instance) {
      SteamAccountStore.instance = new SteamAccountStore();
    }
    return SteamAccountStore.instance;
  }

  async initialize(context: common.UIAbilityContext): Promise<void> {
    if (this.prefs) return;
    this.prefs = await preferences.getPreferences(context, 'steam_profile_v1');
  }

  private static utf8Bytes(value: string): Uint8Array {
    const encoder = new util.TextEncoder();
    return encoder.encodeInto(value);
  }

  private static utf8String(bytes: Uint8Array): string {
    const decoder = util.TextDecoder.create('utf-8');
    return decoder.decodeToString(bytes);
  }

  private static alias(steamId64: string, kind: string): string {
    return `steam.${steamId64}.${kind}`;
  }

  /** 写入一条敏感凭据（幂等覆盖）。 */
  async storeSecret(steamId64: string, kind: string, value: string): Promise<void> {
    const map: asset.AssetMap = new Map<asset.Tag, asset.Value>();
    map.set(asset.Tag.ALIAS, SteamAccountStore.utf8Bytes(SteamAccountStore.alias(steamId64, kind)));
    map.set(asset.Tag.SECRET, SteamAccountStore.utf8Bytes(value));
    map.set(asset.Tag.ACCESSIBILITY, asset.Accessibility.DEVICE_FIRST_UNLOCKED);
    map.set(asset.Tag.CONFLICT_RESOLUTION, asset.ConflictResolution.OVERWRITE);
    try {
      await asset.remove(map);
    } catch (_) {
    }
    await asset.add(map);
  }

  /** 读取敏感凭据；不存在返回空串。 */
  async loadSecret(steamId64: string, kind: string): Promise<string> {
    const query: asset.AssetMap = new Map<asset.Tag, asset.Value>();
    query.set(asset.Tag.ALIAS, SteamAccountStore.utf8Bytes(SteamAccountStore.alias(steamId64, kind)));
    query.set(asset.Tag.RETURN_TYPE, asset.ReturnType.ALL);
    try {
      const results = await asset.query(query);
      if (results.length === 0) return '';
      const secret = results[0].get(asset.Tag.SECRET);
      if (secret === undefined) return '';
      return SteamAccountStore.utf8String(secret as Uint8Array);
    } catch (_) {
      return '';
    }
  }

  async removeSecret(steamId64: string, kind: string): Promise<void> {
    const map: asset.AssetMap = new Map<asset.Tag, asset.Value>();
    map.set(asset.Tag.ALIAS, SteamAccountStore.utf8Bytes(SteamAccountStore.alias(steamId64, kind)));
    try { await asset.remove(map); } catch (_) {}
  }

  /** 清空该账号全部 Steam 凭据（登出用，幂等）。 */
  async removeAllSecrets(steamId64: string): Promise<void> {
    for (const kind of STEAM_SECRET_KINDS) {
      const map: asset.AssetMap = new Map<asset.Tag, asset.Value>();
      map.set(asset.Tag.ALIAS, SteamAccountStore.utf8Bytes(SteamAccountStore.alias(steamId64, kind)));
      try {
        await asset.remove(map);
      } catch (_) {
      }
    }
  }

  async saveProfile(profile: SteamProfile): Promise<void> {
    if (!this.prefs) return;
    const json = JSON.stringify(profile);
    await this.prefs.put('profile', json);
    await this.prefs.flush();
  }

  async loadProfile(): Promise<SteamProfile> {
    if (!this.prefs) return new SteamProfile();
    const raw = await this.prefs.get('profile', '') as string;
    const profile = new SteamProfile();
    if (!raw) return profile;
    try {
      const parsed = JSON.parse(raw) as SteamProfile;
      if (parsed && typeof parsed === 'object') {
        profile.accountName = typeof parsed.accountName === 'string' ? parsed.accountName : '';
        profile.steamId64 = typeof parsed.steamId64 === 'string' ? parsed.steamId64 : '';
        profile.avatarUrl = typeof parsed.avatarUrl === 'string' ? parsed.avatarUrl : '';
        profile.personaName = typeof parsed.personaName === 'string' ? parsed.personaName : '';
      }
    } catch (_) {
    }
    return profile;
  }

  async clearProfile(): Promise<void> {
    if (!this.prefs) return;
    await this.prefs.put('profile', '');
    await this.prefs.flush();
  }

  async saveLibraryCache(json: string): Promise<void> {
    if (!this.prefs) return;
    await this.prefs.put('library_cache', json);
    await this.prefs.flush();
  }

  async loadLibraryCache(): Promise<string> {
    if (!this.prefs) return '';
    return await this.prefs.get('library_cache', '') as string;
  }

  async saveTimeOffset(offsetSeconds: number): Promise<void> {
    if (!this.prefs) return;
    await this.prefs.put('time_offset', offsetSeconds);
    await this.prefs.flush();
  }

  async loadTimeOffset(): Promise<number> {
    if (!this.prefs) return 0;
    return await this.prefs.get('time_offset', 0) as number;
  }
}
