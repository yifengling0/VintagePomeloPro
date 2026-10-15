import { common } from '@kit.AbilityKit';
import preferences from '@ohos.data.preferences';
import http from '@ohos.net.http';
import util from '@ohos.util';
import { DeviceCapabilityPolicy } from '../../common/DeviceCapabilityPolicy';
import { ConfigurationZipMember } from '../../model/ConfigurationZip';
import { UserConfiguration, validateUserConfiguration } from '../../model/UserConfiguration';
import { UserConfigurationRepository } from '../UserConfigurationRepository';
import { ConfigurationFiles as files } from '../ConfigurationFiles';
import { ValueAddedService } from './ValueAddedService';

interface ConfigurationPart { sha256: string; size: number; }
interface ConfigurationMember { name: string; size: number; sha256: string; chunks: ConfigurationPart[]; }
interface ConfigurationManifest { formatVersion: number; deviceClass: string; totalBytes: number; members: ConfigurationMember[]; }
interface ConfigurationHead { id: string; revision: number; sha256: string; totalBytes: number; createdAt: number; }
interface ConfigurationCurrent { head: ConfigurationHead | null; manifest?: ConfigurationManifest; quotaBytes: number; }
interface ConfigurationBegin { status: string; id?: string; missing?: string[]; head?: ConfigurationHead | null; }
interface ConfigurationPublish { status: string; head: ConfigurationHead; }
interface ConfigurationChunk { sha256: string; data: string; }
interface ConfigurationSyncBaseline { owner: string; revision: number; digest: string; localDigest: string; syncedAt: number; }
interface ConfigurationSyncState { owner: string; baselines: ConfigurationSyncBaseline[]; pendingId?: string; pendingOwner?: string; }
export class ConfigurationCloudSyncService {
  private static instance: ConfigurationCloudSyncService | null = null;
  private store: preferences.Preferences | null = null;
  private data: ConfigurationSyncState = { owner: '', baselines: [] };
  private conflict: ConfigurationCurrent | null = null;
  private statusText: string = '';
  private bytes: number = -1;
  private bytesOwner: string = '';
  static getInstance(): ConfigurationCloudSyncService { if (!ConfigurationCloudSyncService.instance) ConfigurationCloudSyncService.instance = new ConfigurationCloudSyncService(); return ConfigurationCloudSyncService.instance; }
  async initialize(context: common.UIAbilityContext, previousOwner: string = ''): Promise<void> {
    if (this.store) return;
    this.store = await preferences.getPreferences(context, 'configuration_cloud_journal');
    const raw = this.store.getSync('state_v1', '') as string;
    if (raw) { this.data = JSON.parse(raw) as ConfigurationSyncState;
      if (!this.data || !Array.isArray(this.data.baselines) || this.data.baselines.length > 8) throw new Error('CONFIG_INVALID'); }
    if (!this.data.owner && previousOwner && UserConfigurationRepository.getInstance().hasLocalUserData()) {
      this.data.owner = previousOwner; await this.persist();
    }
  }
  status(): string { return this.statusText; }
  usedBytes(): number { return this.bytesOwner === ValueAddedService.getInstance().getUserId() ? this.bytes : -1; }
  hasConflict(): boolean { return !!this.conflict; }
  lastSyncedAt(): number { return this.data.baselines.find((b: ConfigurationSyncBaseline): boolean => b.owner === ValueAddedService.getInstance().getUserId())?.syncedAt ?? 0; }
  private async persist(): Promise<void> { if (this.store) { await this.store.put('state_v1', JSON.stringify(this.data)); await this.store.flush(); } }
  private async manifest(members: ConfigurationZipMember[]): Promise<ConfigurationManifest> {
    const result: ConfigurationManifest = { formatVersion: 1, deviceClass: DeviceCapabilityPolicy.isTabletDevice() ? 'tablet' : 'desktop', totalBytes: 0, members: [] };
    for (const member of members) {
      const spec: ConfigurationMember = { name: member.name, size: member.bytes.length, sha256: await files.hash(member.bytes), chunks: [] };
      for (let offset = 0; offset < member.bytes.length; offset += 262144) { const bytes = member.bytes.slice(offset, offset + 262144);
        spec.chunks.push({ sha256: await files.hash(bytes), size: bytes.length }); }
      result.members.push(spec); result.totalBytes += member.bytes.length;
    }
    return result;
  }
  private async download(current: ConfigurationCurrent, check: () => void, knownManifest: ConfigurationZipMember | null): Promise<ConfigurationZipMember[]> {
    const manifest = current.manifest;
    if (!current.head || !manifest || manifest.formatVersion !== 1 || manifest.members.length > 257 || manifest.totalBytes > 67108864) throw new Error('CONFIG_INVALID');
    const api = ValueAddedService.getInstance(), members: ConfigurationZipMember[] = [];
    if (await files.hash(files.bytes(JSON.stringify(manifest))) !== current.head.sha256) throw new Error('CONFIG_HASH_MISMATCH');
    for (const member of manifest.members) {
      if (!Number.isSafeInteger(member.size) || member.size < 1 || member.size > 16777216 || member.chunks.length !== Math.ceil(member.size / 262144)) throw new Error('CONFIG_INVALID');
      if (member.name === 'manifest.json' && knownManifest) { members.push(knownManifest); continue; }
      const cached = await UserConfigurationRepository.getInstance().cachedAsset(member.name, member.size, member.sha256); check();
      if (cached) { members.push({ name: member.name, bytes: cached }); continue; }
      const bytes = new Uint8Array(member.size); let offset = 0;
      for (const part of member.chunks) {
        check(); const response = await api.configurationRequest<ConfigurationChunk>('/v1/configuration/uploads/' + current.head.id + '/chunks/' + part.sha256, http.RequestMethod.GET); check();
        const chunk = new util.Base64Helper().decodeSync(response.data);
        if (response.sha256 !== part.sha256 || chunk.length !== part.size || await files.hash(chunk) !== part.sha256 || offset + chunk.length > bytes.length) throw new Error('CONFIG_HASH_MISMATCH');
        bytes.set(chunk, offset); offset += chunk.length;
      }
      if (offset !== bytes.length || await files.hash(bytes) !== member.sha256) throw new Error('CONFIG_HASH_MISMATCH');
      members.push({ name: member.name, bytes: bytes });
    }
    await UserConfigurationRepository.getInstance().validateMembers(members); return members;
  }
  private base(owner: string): ConfigurationSyncBaseline {
    let base = this.data.baselines.find((b: ConfigurationSyncBaseline): boolean => b.owner === owner);
    if (!base) { if (this.data.baselines.length >= 8) throw new Error('SYNC_RECORD_LIMIT'); base = { owner: owner, revision: 0, digest: '', localDigest: '', syncedAt: 0 }; this.data.baselines.push(base); }
    return base;
  }
  async sync(check: () => void, decision: string = '', migrateLegacy: (() => Promise<void>) | null = null): Promise<string> {
    const api = ValueAddedService.getInstance(), repo = UserConfigurationRepository.getInstance(), owner = api.getUserId(); check();
    if (!repo.canWrite()) throw new Error('CONFIG_REQUIRES_UPDATE');
    if (this.data.owner && this.data.owner !== owner && repo.hasLocalUserData() && decision !== 'cloud') {
      if (decision !== 'local') {
        this.conflict = await api.configurationRequest<ConfigurationCurrent>('/v1/configuration/' + (DeviceCapabilityPolicy.isTabletDevice() ? 'tablet' : 'desktop'), http.RequestMethod.GET); check();
        this.statusText = '本地配置属于其他账号，未自动上传'; return this.statusText;
      }
    }
    const type = DeviceCapabilityPolicy.isTabletDevice() ? 'tablet' : 'desktop';
    if (this.data.pendingId && this.data.pendingOwner === owner) {
      try {
        await api.configurationRequest<ConfigurationBegin>('/v1/configuration/uploads/' + this.data.pendingId + '/abort', http.RequestMethod.POST, '{}');
      } catch (error) {
        check();
        // Expired uploads may already be removed by server cleanup. Reconcile
        // against the current complete head after discarding only this marker.
        if ((error as Error).message !== 'CONFIG_NOT_FOUND') throw error as Error;
      }
      check();
      this.data.pendingId = ''; this.data.pendingOwner = ''; await this.persist();
    }
    const current = await api.configurationRequest<ConfigurationCurrent>('/v1/configuration/' + type, http.RequestMethod.GET); check();
    if (current.quotaBytes !== 67108864) throw new Error('CONFIG_INVALID'); this.bytes = current.head?.totalBytes ?? 0; this.bytesOwner = owner;
    if (!current.head && migrateLegacy) { await migrateLegacy(); check(); }
    const base = this.base(owner), revision = repo.revision(), members = await repo.snapshot(); check();
    const manifest = await this.manifest(members), digest = await files.hash(files.bytes(JSON.stringify(manifest)));
    let knownManifest: ConfigurationZipMember | null = null;
    // A regular conflict choice is not permission to downgrade an unreadable future format.
    if (current.head && current.head.sha256 !== digest && (current.head.sha256 !== base.digest || decision === 'cloud')) {
      await api.configurationRequest<ConfigurationBegin>('/v1/configuration/uploads/' + current.head.id + '/lease', http.RequestMethod.POST, '{}'); check();
      const spec = current.manifest?.members.find((m: ConfigurationMember): boolean => m.name === 'manifest.json');
      if (!spec || spec.size > 8388608 || spec.chunks.length !== Math.ceil(spec.size / 262144)) throw new Error('CONFIG_INVALID');
      const bytes = new Uint8Array(spec.size); let offset = 0;
      for (const part of spec.chunks) {
        const response = await api.configurationRequest<ConfigurationChunk>('/v1/configuration/uploads/' + current.head.id + '/chunks/' + part.sha256, http.RequestMethod.GET); check();
        const data = new util.Base64Helper().decodeSync(response.data);
        if (response.sha256 !== part.sha256 || await files.hash(data) !== part.sha256 || data.length !== part.size || offset + data.length > bytes.length) throw new Error('CONFIG_HASH_MISMATCH');
        bytes.set(data, offset); offset += data.length;
      }
      if (offset !== bytes.length || await files.hash(bytes) !== spec.sha256) throw new Error('CONFIG_HASH_MISMATCH');
      validateUserConfiguration(JSON.parse(files.text(bytes)) as UserConfiguration);
      knownManifest = { name: 'manifest.json', bytes: bytes };
    }
    if (current.head && (decision === 'cloud' || !repo.hasLocalUserData() ||
      current.head.revision !== base.revision && base.localDigest === digest)) {
      if (!knownManifest) {
        await api.configurationRequest<ConfigurationBegin>('/v1/configuration/uploads/' + current.head.id + '/lease', http.RequestMethod.POST, '{}'); check();
      }
      const remote = await this.download(current, check, knownManifest); check();
      await repo.restore(remote, revision); check();
      const restored = await this.manifest(await repo.snapshot()); check();
      base.revision = current.head.revision; base.digest = current.head.sha256;
      base.localDigest = await files.hash(files.bytes(JSON.stringify(restored))); base.syncedAt = Date.now();
      this.data.owner = owner; this.conflict = null; await this.persist(); this.statusText = '已恢复应用配置'; return this.statusText;
    }
    if (current.head && current.head.sha256 === digest) {
      const changed = base.revision !== current.head.revision || base.localDigest !== digest || this.data.owner !== owner;
      base.revision = current.head.revision; base.digest = digest; base.localDigest = digest; base.syncedAt = current.head.createdAt;
      this.data.owner = owner; this.conflict = null; if (changed) await this.persist(); this.statusText = '云端备份已更新'; return this.statusText;
    }
    if (current.head && current.head.revision !== base.revision && decision !== 'local') {
      this.conflict = current; this.statusText = '本机与云端均有配置，双方已保留'; return this.statusText;
    }
    if (repo.revision() !== revision) throw new Error('CONFIG_LOCAL_CHANGED');
    const begun = await api.configurationRequest<ConfigurationBegin>('/v1/configuration/uploads', http.RequestMethod.POST,
      JSON.stringify({ baseRevision: current.head?.revision ?? 0, manifest: manifest })); check();
    if (begun.status !== 'READY' || !begun.id || !begun.missing) throw new Error('CONFIG_CONFLICT');
    this.data.pendingId = begun.id; this.data.pendingOwner = owner; await this.persist(); check();
    const needed = new Set<string>(begun.missing);
    for (let index = 0; index < members.length; index++) {
      const member = members[index], spec = manifest.members[index]; let offset = 0;
      for (const part of spec.chunks) {
        check(); if (needed.has(part.sha256)) {
          await api.configurationRequest<ConfigurationBegin>('/v1/configuration/uploads/' + begun.id + '/chunks', http.RequestMethod.PUT,
            JSON.stringify({ sha256: part.sha256, data: new util.Base64Helper().encodeToStringSync(member.bytes.slice(offset, offset + part.size)) })); check(); needed.delete(part.sha256);
        } offset += part.size;
      }
    }
    if (repo.revision() !== revision) throw new Error('CONFIG_LOCAL_CHANGED'); check();
    const published = await api.configurationRequest<ConfigurationPublish>('/v1/configuration/uploads/' + begun.id + '/finalize', http.RequestMethod.POST, '{}'); check();
    if (published.status !== 'PUBLISHED' || published.head.sha256 !== digest) throw new Error('CONFIG_CONFLICT');
    base.revision = published.head.revision; base.digest = digest; base.localDigest = digest; base.syncedAt = published.head.createdAt;
    this.data.pendingId = ''; this.data.pendingOwner = '';
    this.bytes = published.head.totalBytes; this.data.owner = owner; this.conflict = null; await this.persist();
    this.statusText = repo.revision() !== revision ? '检测到本地修改，等待备份' : '云端备份已更新'; return this.statusText;
  }
}
