import { common } from '@kit.AbilityKit';
import { fileIo as fs, fileUri } from '@kit.CoreFileKit';
import bundleManager from '@ohos.bundle.bundleManager';
import { hilog } from './LogService';
import preferences from '@ohos.data.preferences';
import {
  INPUT_PROFILE_SCHEMA_VERSION,
  INPUT_TEMPLATES,
  InputProfile,
  InputTemplate,
  cloneInputProfile,
  createBuiltInInputProfiles,
  createDefaultInputProfile,
  createInputProfileFromTemplate,
  exportProfileToIcpJson,
  inputTemplateForProfileId,
  importProfileFromIcpJson
} from '../model/AppModels';

const STORE_NAME = 'vintage_pomelo_input_profiles';
const PROFILES_KEY = 'profiles_v6';
const INPUT_PROFILE_DIRECTORY = 'input_profiles';
const DOMAIN = 0x0000;
const TAG = 'InputProfile';

/**
 * v6 元素化输入模型。无法读取的方案保留原始 Preferences 并阻止写回；
 * 有明确迁移器之前，不把未知格式替换为默认布局。
 */
export class InputProfileService {
  private static instance: InputProfileService | null = null;
  private listeners: Set<(id: string, removed: boolean, source: string) => void> = new Set<(id: string, removed: boolean, source: string) => void>();
  subscribeChanges(listener: (id: string, removed: boolean, source: string) => void): void { this.listeners.add(listener); }
  unsubscribeChanges(listener: (id: string, removed: boolean, source: string) => void): void { this.listeners.delete(listener); }
  private store: preferences.Preferences | null = null;
  private profiles: Map<string, InputProfile> = new Map<string, InputProfile>();
  private deleted: string[] = [];
  private incompatible: boolean = false;
  isDeleted(id: string): boolean { return this.deleted.includes(id); }
  async reloadConfiguration(): Promise<void> {
    if (!this.store) return;
    this.profiles.clear(); this.loadProfiles();
    createBuiltInInputProfiles().forEach((p: InputProfile): void => { if (!this.profiles.has(p.id)) this.profiles.set(p.id, p); });
  }
  private loadProfiles(): void {
    if (!this.store) return;
    this.incompatible = false;
    const raw = this.store.getSync(PROFILES_KEY, '') as string;
    try {
      this.deleted = JSON.parse(this.store.getSync('deleted_profiles_v1', '[]') as string) as string[];
      if (!Array.isArray(this.deleted) || this.deleted.length > 2048 || this.deleted.some((id: string): boolean => typeof id !== 'string' || id.length > 128)) throw new Error('CONFIG_INVALID');
      if (!raw) return;
      const values = JSON.parse(raw) as InputProfile[];
      if (!Array.isArray(values) || values.some((p: InputProfile): boolean => !p || p.schemaVersion !== INPUT_PROFILE_SCHEMA_VERSION)) {
        this.incompatible = true; return;
      }
      values.forEach((p: InputProfile): void => { this.profiles.set(p.id, p); });
    } catch (_) { this.incompatible = true; }
  }

  static getInstance(): InputProfileService {
    if (!InputProfileService.instance) InputProfileService.instance = new InputProfileService();
    return InputProfileService.instance;
  }

  async initialize(context: common.UIAbilityContext): Promise<void> {
    if (this.store) return;
    this.store = await preferences.getPreferences(context, STORE_NAME);
    this.loadProfiles();

    // Built-in profiles are versioned selectable presets. Keep user-created
    // profiles untouched, but refresh a built-in when its shipped layout gains
    // a newer revision (for example the physical-keyboard geometry update).
    let changed = false;
    createBuiltInInputProfiles().forEach((profile: InputProfile): void => {
      const existing = this.profiles.get(profile.id);
      if (existing && (existing.builtInRevision ?? 0) >= (profile.builtInRevision ?? 0)) return;
      this.profiles.set(profile.id, profile);
      changed = true;
    });
    if (changed) await this.persistProfiles();
  }

  getProfile(id: string): InputProfile {
    const profile = this.profiles.get(id) ?? this.profiles.get('default') ?? createDefaultInputProfile();
    return this.cloneProfile(profile);
  }

  getProfiles(): InputProfile[] {
    const values: InputProfile[] = [];
    this.profiles.forEach((profile: InputProfile): void => {
      values.push(this.cloneProfile(profile));
    });
    values.sort((left: InputProfile, right: InputProfile): number => {
      const leftTemplate = inputTemplateForProfileId(left.id);
      const rightTemplate = inputTemplateForProfileId(right.id);
      if (leftTemplate && rightTemplate) {
        return INPUT_TEMPLATES.findIndex((template: InputTemplate): boolean =>
          template.builtInProfileId === left.id) -
          INPUT_TEMPLATES.findIndex((template: InputTemplate): boolean =>
            template.builtInProfileId === right.id);
      }
      if (leftTemplate) return -1;
      if (rightTemplate) return 1;
      return left.name.localeCompare(right.name);
    });
    return values;
  }

  getDesktopProfiles(): InputProfile[] {
    return this.getProfiles().filter((profile: InputProfile): boolean => !profile.id.startsWith('app-'));
  }

  hasDesktopProfile(id: string): boolean {
    return id.length > 0 && !id.startsWith('app-') && this.profiles.has(id);
  }

  hasProfile(id: string): boolean {
    return this.profiles.has(id);
  }

  hasDesktopProfileName(name: string): boolean {
    const normalized = name.trim().toLocaleLowerCase();
    if (normalized.length === 0) return false;
    return this.getDesktopProfiles().some((profile: InputProfile): boolean =>
      profile.name.trim().toLocaleLowerCase() === normalized);
  }

  ensureProfile(id: string, name: string, baseProfileId: string = 'default'): InputProfile {
    const profile = this.profiles.get(id);
    if (profile && profile.schemaVersion === INPUT_PROFILE_SCHEMA_VERSION) return this.cloneProfile(profile);
    const base = this.profiles.get(baseProfileId) ?? this.profiles.get('default') ?? createDefaultInputProfile();
    return this.cloneProfile(base, id, name);
  }

  async duplicateProfile(sourceId: string, name: string): Promise<InputProfile> {
    const source = this.profiles.get(sourceId) ?? this.profiles.get('default') ?? createDefaultInputProfile();
    let suffix = Date.now().toString(36);
    let id = 'desktop-' + suffix;
    let collision = 1;
    while (this.profiles.has(id)) {
      id = 'desktop-' + suffix + '-' + collision.toString();
      collision++;
    }
    const profile = this.cloneProfile(source, id, name.trim() || '桌面输入方案');
    await this.saveProfile(profile);
    return this.cloneProfile(profile);
  }

  async removeProfile(id: string): Promise<boolean> {
    if (this.incompatible) throw new Error('CONFIG_REQUIRES_UPDATE');
    if (inputTemplateForProfileId(id) || !this.profiles.has(id)) return false;
    this.profiles.delete(id);
    this.deleted = this.deleted.filter((value: string): boolean => value !== id); this.deleted.push(id);
    await this.persistProfiles();
    this.listeners.forEach((listener: (id: string, removed: boolean, source: string) => void): void => listener(id, true, 'local'));
    return true;
  }

  async saveProfile(profile: InputProfile, source: string = 'local'): Promise<void> {
    if (this.incompatible) throw new Error('CONFIG_REQUIRES_UPDATE');
    if (source === 'local') this.deleted = this.deleted.filter((id: string): boolean => id !== profile.id);
    const normalized = this.cloneProfile(profile);
    this.profiles.set(normalized.id, normalized);
    await this.persistProfiles();
    this.listeners.forEach((listener: (id: string, removed: boolean, source: string) => void): void => listener(normalized.id, false, source));
  }

  /**
   * 从模板新建输入方案。id 自动生成，名称允许与现有方案重复。
   */
  async createProfileFromTemplate(name: string, templateId: string): Promise<InputProfile> {
    let suffix = Date.now().toString(36);
    let id = 'desktop-' + suffix;
    let collision = 1;
    while (this.profiles.has(id)) {
      id = 'desktop-' + suffix + '-' + collision.toString();
      collision++;
    }
    const profile = createInputProfileFromTemplate(id, name.trim() || '桌面输入方案', templateId);
    await this.saveProfile(profile);
    return this.cloneProfile(profile);
  }

  /**
   * 把默认方案重置为通用模板，其它方案不受影响。
   */
  async resetDefaultProfile(): Promise<InputProfile> {
    return this.resetBuiltInProfile('default');
  }

  /** Restore one selectable built-in profile to its shipped template. */
  async resetBuiltInProfile(profileId: string): Promise<InputProfile> {
    const template = inputTemplateForProfileId(profileId);
    if (!template) return this.getProfile(profileId);
    const profile = createInputProfileFromTemplate(
      template.builtInProfileId, template.profileName, template.id);
    await this.saveProfile(profile);
    return this.cloneProfile(profile);
  }

  /**
   * 导出方案为 Winlator 兼容 .icp JSON 文本。
   */
  exportProfileToIcp(profileId: string): string {
    const profile = this.getProfile(profileId);
    return exportProfileToIcpJson(profile);
  }

  /**
   * 导入 .icp JSON 并保存为新方案；返回导入后的方案。
   */
  async importProfileFromIcp(json: string, profileName?: string): Promise<InputProfile> {
    let suffix = Date.now().toString(36);
    let id = 'desktop-' + suffix;
    let collision = 1;
    while (this.profiles.has(id)) {
      id = 'desktop-' + suffix + '-' + collision.toString();
      collision++;
    }
    const profile = importProfileFromIcpJson(json, id, profileName);
    await this.saveProfile(profile);
    return this.cloneProfile(profile);
  }

  /**
   * 把方案导出到 Download/<bundle>/input_profiles/<name>.icp，返回文件路径。
   */
  async exportProfileToFile(profileId: string): Promise<string> {
    const json = this.exportProfileToIcp(profileId);
    const directory = this.resolveInputProfileDirectory();
    if (!directory) throw new Error('无法解析 Download 输入方案目录');
    const safeName = this.sanitizeFileName(this.getProfile(profileId).name);
    const target = directory.endsWith('/') ? directory + safeName + '.icp' : directory + '/' + safeName + '.icp';
    this.ensureDirectory(directory);
    const output = fs.openSync(target, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY | fs.OpenMode.TRUNC);
    try {
      fs.writeSync(output.fd, json);
    } finally {
      fs.closeSync(output);
    }
    hilog.info(DOMAIN, TAG, 'Exported input profile to %{public}s', target);
    return target;
  }

  private cloneProfile(profile: InputProfile, profileId?: string, profileName?: string): InputProfile {
    return cloneInputProfile(profile, profileId, profileName);
  }

  private async persistProfiles(): Promise<void> {
    if (!this.store || this.incompatible) return;
    const values: InputProfile[] = [];
    this.profiles.forEach((value: InputProfile): void => {
      values.push(cloneInputProfile(value));
    });
    await this.store.put(PROFILES_KEY, JSON.stringify(values));
    await this.store.put('deleted_profiles_v1', JSON.stringify(this.deleted));
    await this.store.flush();
  }

  private resolveInputProfileDirectory(): string {
    try {
      const bundleInfo = bundleManager.getBundleInfoForSelfSync(
        bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_APPLICATION);
      const uri = `file://docs/storage/Users/currentUser/Download/${bundleInfo.name}/${INPUT_PROFILE_DIRECTORY}`;
      return new fileUri.FileUri(uri).path;
    } catch (_) {
      return '';
    }
  }

  private ensureDirectory(path: string): boolean {
    try {
      const existing = fs.statSync(path);
      return existing.isDirectory();
    } catch (_) {
    }
    try {
      fs.mkdirSync(path);
      return true;
    } catch (_) {
      return false;
    }
  }

  private sanitizeFileName(name: string): string {
    const cleaned = name.replace(/[\\/:*?"<>|]/g, '_').replace(/\s+/g, ' ').trim();
    return (cleaned.length > 0 ? cleaned : 'input-profile').slice(0, 48);
  }
}
