import { common } from '@kit.AbilityKit';
import preferences from '@ohos.data.preferences';
import http from '@ohos.net.http';
import util from '@ohos.util';
import { DeviceCapabilityPolicy } from '../../common/DeviceCapabilityPolicy';
import { ConfigurationZipMember } from '../../model/ConfigurationZip';
import { UserConfiguration, validateUserConfiguration } from '../../model/UserConfiguration';
import { CLOUD_CONFIGURATION_BYTES, CLOUD_CONFIGURATION_SCOPE, projectCloudUserConfiguration, validateCloudUserConfiguration } from '../../model/CloudUserConfiguration';
import { UserConfigurationRepository } from '../UserConfigurationRepository';
import { ConfigurationFiles as files } from '../ConfigurationFiles';
import { ValueAddedService } from './ValueAddedService';

interface ConfigurationPart { sha256: string; size: number; }
interface ConfigurationMember { name: string; size: number; sha256: string; chunks: ConfigurationPart[]; }
interface ConfigurationManifest { formatVersion: number; scope?: string; deviceClass: string; totalBytes: number; members: ConfigurationMember[]; }
interface ConfigurationHead { id: string; revision: number; sha256: string; totalBytes: number; createdAt: number; }
interface ConfigurationCurrent { head: ConfigurationHead | null; manifest?: ConfigurationManifest; quotaBytes: number; }
interface ConfigurationBegin { status: string; id?: string; missing?: string[]; head?: ConfigurationHead | null; }
interface ConfigurationPublish { status: string; head: ConfigurationHead; }
interface ConfigurationChunk { sha256: string; data: string; }
interface ConfigurationSyncBaseline { owner: string; revision: number; digest: string; localDigest: string; syncedAt: number; scope?: string; }
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
    if (!this.data.owner && previousOwner && UserConfigurationRepository.getInstance().hasCloudUserData()) {
      this.data.owner = previousOwner; await this.persist();
    }
  }
  status(): string { return this.statusText; }
  usedBytes(): number { return this.bytesOwner === ValueAddedService.getInstance().getUserId() ? this.bytes : -1; }
  hasConflict(): boolean { return !!this.conflict; }
  lastSyncedAt(): number { return this.data.baselines.find((b: ConfigurationSyncBaseline): boolean => b.owner === ValueAddedService.getInstance().getUserId())?.syncedAt ?? 0; }
  private async persist(): Promise<void> { if (this.store) { await this.store.put('state_v1', JSON.stringify(this.data)); await this.store.flush(); } }
  private async manifest(members: ConfigurationZipMember[]): Promise<ConfigurationManifest> {
    if (members.length !== 1 || members[0].name !== 'manifest.json' || members[0].bytes.length > CLOUD_CONFIGURATION_BYTES) throw new Error('CONFIG_QUOTA');
    const member = members[0], digest = await files.hash(member.bytes);
    return { formatVersion: 1, scope: CLOUD_CONFIGURATION_SCOPE,
      deviceClass: DeviceCapabilityPolicy.isTabletDevice() ? 'tablet' : 'desktop', totalBytes: member.bytes.length,
      members: [{ name: member.name, size: member.bytes.length, sha256: digest, chunks: [{ sha256: digest, size: member.bytes.length }] }] };
  }
  /** Older backups are projected from their JSON only; assets are never downloaded. */
  private async downloadSettings(current: ConfigurationCurrent, check: () => void): Promise<ConfigurationZipMember[]> {
    const manifest = current.manifest;
    if (!current.head || !manifest || manifest.formatVersion !== 1 || !Array.isArray(manifest.members) || manifest.members.length > 257 ||
      await files.hash(files.bytes(JSON.stringify(manifest))) !== current.head.sha256) throw new Error('CONFIG_HASH_MISMATCH');
    const spec = manifest.members.find((m: ConfigurationMember): boolean => m.name === 'manifest.json');
    // The legacy JSON may contain library metadata. Read it once to retain only user settings.
    if (!spec || !Number.isSafeInteger(spec.size) || spec.size < 1 || spec.size > 8388608 || spec.chunks.length !== Math.ceil(spec.size / 262144)) throw new Error('CONFIG_INVALID');
    if (manifest.scope === CLOUD_CONFIGURATION_SCOPE && (manifest.members.length !== 1 || spec.size > CLOUD_CONFIGURATION_BYTES || manifest.totalBytes !== spec.size)) throw new Error('CONFIG_SCOPE_UNSUPPORTED');
    await ValueAddedService.getInstance().configurationRequest<ConfigurationBegin>('/v1/configuration/uploads/' + current.head.id + '/lease', http.RequestMethod.POST, '{}'); check();
    const bytes = new Uint8Array(spec.size); let offset = 0;
    for (const part of spec.chunks) {
      const response = await ValueAddedService.getInstance().configurationRequest<ConfigurationChunk>('/v1/configuration/uploads/' + current.head.id + '/chunks/' + part.sha256, http.RequestMethod.GET); check();
      const data = new util.Base64Helper().decodeSync(response.data);
      if (response.sha256 !== part.sha256 || data.length !== part.size || await files.hash(data) !== part.sha256 || offset + data.length > bytes.length) throw new Error('CONFIG_HASH_MISMATCH');
      bytes.set(data, offset); offset += data.length;
    }
    if (offset !== bytes.length || await files.hash(bytes) !== spec.sha256) throw new Error('CONFIG_HASH_MISMATCH');
    const value = JSON.parse(files.text(bytes)) as UserConfiguration; validateUserConfiguration(value);
    if (manifest.scope === CLOUD_CONFIGURATION_SCOPE) validateCloudUserConfiguration(value);
    const projected = files.bytes(JSON.stringify(projectCloudUserConfiguration(value)));
    if (projected.length > CLOUD_CONFIGURATION_BYTES) throw new Error('CONFIG_QUOTA');
    return [{ name: 'manifest.json', bytes: projected }];
  }
  private base(owner: string): ConfigurationSyncBaseline {
    let base = this.data.baselines.find((b: ConfigurationSyncBaseline): boolean => b.owner === owner);
    if (!base) { if (this.data.baselines.length >= 8) throw new Error('SYNC_RECORD_LIMIT'); base = { owner: owner, revision: 0, digest: '', localDigest: '', syncedAt: 0 }; this.data.baselines.push(base); }
    return base;
  }
  private async remember(base: ConfigurationSyncBaseline, head: ConfigurationHead, digest: string, owner: string): Promise<void> {
    base.revision = head.revision; base.digest = head.sha256; base.localDigest = digest; base.syncedAt = head.createdAt; base.scope = CLOUD_CONFIGURATION_SCOPE;
    this.data.owner = owner; this.conflict = null; await this.persist();
  }
  async sync(check: () => void, decision: string = '', migrateLegacy: (() => Promise<void>) | null = null): Promise<string> {
    const api = ValueAddedService.getInstance(), repo = UserConfigurationRepository.getInstance(), owner = api.getUserId(); check();
    if (!repo.canWrite()) throw new Error('CONFIG_REQUIRES_UPDATE');
    const type = DeviceCapabilityPolicy.isTabletDevice() ? 'tablet' : 'desktop';
    if (this.data.owner && this.data.owner !== owner && repo.hasCloudUserData() && decision !== 'cloud' && decision !== 'local') {
      this.conflict = await api.configurationRequest<ConfigurationCurrent>('/v1/configuration/' + type, http.RequestMethod.GET); check();
      this.statusText = '本地配置属于其他账号，未自动上传'; return this.statusText;
    }
    if (this.data.pendingId && this.data.pendingOwner === owner) {
      try { await api.configurationRequest<ConfigurationBegin>('/v1/configuration/uploads/' + this.data.pendingId + '/abort', http.RequestMethod.POST, '{}'); }
      catch (error) { check(); if ((error as Error).message !== 'CONFIG_NOT_FOUND') throw error as Error; }
      check(); this.data.pendingId = ''; this.data.pendingOwner = ''; await this.persist();
    }
    const current = await api.configurationRequest<ConfigurationCurrent>('/v1/configuration/' + type, http.RequestMethod.GET); check();
    if (![CLOUD_CONFIGURATION_BYTES, 67108864].includes(current.quotaBytes)) throw new Error('CONFIG_INVALID');
    this.bytes = current.head?.totalBytes ?? 0; this.bytesOwner = owner;
    if (!current.head && migrateLegacy) { await migrateLegacy(); check(); }
    const base = this.base(owner);
    let revision = repo.cloudRevision(), members = await repo.cloudSnapshot(); check();
    let manifest = await this.manifest(members), digest = await files.hash(files.bytes(JSON.stringify(manifest)));
    const legacy = !!current.head && current.manifest?.scope !== CLOUD_CONFIGURATION_SCOPE;
    let remote: ConfigurationZipMember[] | null = null, remoteDigest: string = '', restored: boolean = false;
    if (current.head && current.head.sha256 !== digest && (current.head.sha256 !== base.digest || decision === 'cloud' || legacy)) {
      remote = await this.downloadSettings(current, check); check();
      remoteDigest = await files.hash(files.bytes(JSON.stringify(await this.manifest(remote))));
    }
    const sameLegacySettings = legacy && remoteDigest === digest;
    if (current.head && (decision === 'cloud' || !repo.hasCloudUserData() ||
      current.head.revision !== base.revision && base.scope === CLOUD_CONFIGURATION_SCOPE && base.localDigest === digest)) {
      if (!remote) remote = await this.downloadSettings(current, check);
      await repo.restore(remote, revision, true); check(); restored = true;
      revision = repo.cloudRevision(); members = await repo.cloudSnapshot(); check();
      manifest = await this.manifest(members); digest = await files.hash(files.bytes(JSON.stringify(manifest)));
      if (!legacy) {
        await this.remember(base, current.head, digest, owner); this.statusText = '已恢复应用配置'; return this.statusText;
      }
    }
    if (current.head && current.head.sha256 === digest) {
      await this.remember(base, current.head, digest, owner); this.statusText = '云端备份已更新'; return this.statusText;
    }
    if (current.head && current.head.revision !== base.revision && decision !== 'local' && !restored && !sameLegacySettings) {
      this.conflict = current; this.statusText = '本机与云端均有配置，双方已保留'; return this.statusText;
    }
    if (repo.cloudRevision() !== revision) throw new Error('CONFIG_LOCAL_CHANGED');
    const begun = await api.configurationRequest<ConfigurationBegin>('/v1/configuration/uploads', http.RequestMethod.POST,
      JSON.stringify({ baseRevision: current.head?.revision ?? 0, manifest: manifest })); check();
    if (begun.status !== 'READY' || !begun.id || !begun.missing) throw new Error('CONFIG_CONFLICT');
    this.data.pendingId = begun.id; this.data.pendingOwner = owner; await this.persist(); check();
    const part = manifest.members[0].chunks[0];
    if (begun.missing.includes(part.sha256)) {
      await api.configurationRequest<ConfigurationBegin>('/v1/configuration/uploads/' + begun.id + '/chunks', http.RequestMethod.PUT,
        JSON.stringify({ sha256: part.sha256, data: new util.Base64Helper().encodeToStringSync(members[0].bytes) })); check();
    }
    if (repo.cloudRevision() !== revision) throw new Error('CONFIG_LOCAL_CHANGED');
    const published = await api.configurationRequest<ConfigurationPublish>('/v1/configuration/uploads/' + begun.id + '/finalize', http.RequestMethod.POST, '{}'); check();
    if (published.status !== 'PUBLISHED' || published.head.sha256 !== digest) throw new Error('CONFIG_CONFLICT');
    this.data.pendingId = ''; this.data.pendingOwner = ''; this.bytes = published.head.totalBytes;
    await this.remember(base, published.head, digest, owner);
    this.statusText = repo.cloudRevision() !== revision ? '检测到本地修改，等待备份' : restored ? '已恢复应用配置' : '云端备份已更新';
    return this.statusText;
  }
}
