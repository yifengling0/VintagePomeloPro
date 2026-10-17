import { InputProfile, ControlElement, InputBinding, GamepadButtonMapping } from './AppModels';

export function isValueAddedVisibleOnDevice(type: string): boolean {
  return type === 'tablet' || type === 'pc' || type === '2in1';
}

export function profileDeviceClass(type: string): string {
  if (!isValueAddedVisibleOnDevice(type)) throw new Error('FEATURE_HIDDEN');
  return type === 'tablet' ? 'tablet' : 'desktop';
}

export interface ValueAddedConfig {
  apiBaseUrl: string;
  cloudFunctionName?: string;
  cloudFunctionVersion?: string;
  accountClientId: string;
  membershipProductId: string;
  membershipProductType: number;
  membershipPromotionalOfferId?: string;
  membershipOneTimeProductId?: string;
  membershipOneTimePurchases?: boolean;
  membershipSubscriptionGroupId?: string;
  membershipOperatorName?: string;
  coffeeProductId: string;
}

export function validateValueAddedConfig(config: ValueAddedConfig): void {
  if (!config || typeof config.apiBaseUrl !== 'string' || typeof config.accountClientId !== 'string') {
    throw new Error('NOT_CONFIGURED');
  }
  // Configuration is shipped with the app, never supplied by a deep link or an order.
  const https = /^https:\/\/[a-z0-9][a-z0-9.-]+(?::443)?(?:\/[a-zA-Z0-9_-]+)*$/.test(config.apiBaseUrl);
  const fn = config.cloudFunctionName;
  if (fn !== undefined && (typeof fn !== 'string' || !/^[a-zA-Z0-9_-]{1,128}$/.test(fn))) throw new Error('NOT_CONFIGURED');
  if (config.cloudFunctionVersion !== undefined && (typeof config.cloudFunctionVersion !== 'string' ||
    !/^(\$latest|[1-9][0-9]{0,8})$/.test(config.cloudFunctionVersion))) throw new Error('NOT_CONFIGURED');
  if ((!https && !fn) || (config.apiBaseUrl !== '' && !https) ||
    !/^[a-zA-Z0-9._-]{3,128}$/.test(config.accountClientId)) throw new Error('NOT_CONFIGURED');
  if (config.membershipProductType !== 2 && config.membershipProductType !== 3) {
    throw new Error('NOT_CONFIGURED');
  }
  if (config.membershipPromotionalOfferId !== undefined &&
    (config.membershipProductType !== 2 || typeof config.membershipPromotionalOfferId !== 'string' ||
      !/^[a-zA-Z0-9._-]{1,64}$/.test(config.membershipPromotionalOfferId))) throw new Error('NOT_CONFIGURED');
  if (config.membershipProductId && config.membershipProductId === config.coffeeProductId) {
    throw new Error('NOT_CONFIGURED');
  }
  for (const id of [config.membershipOneTimeProductId, config.membershipSubscriptionGroupId]) {
    if (id !== undefined && (typeof id !== 'string' || !/^[a-zA-Z0-9._-]{1,128}$/.test(id))) throw new Error('NOT_CONFIGURED');
  }
  if (config.membershipOneTimeProductId && [config.membershipProductId, config.coffeeProductId].includes(config.membershipOneTimeProductId)) {
    throw new Error('NOT_CONFIGURED');
  }
  if (config.membershipOneTimePurchases !== undefined && typeof config.membershipOneTimePurchases !== 'boolean') {
    throw new Error('NOT_CONFIGURED');
  }
  if (config.membershipOperatorName !== undefined && (typeof config.membershipOperatorName !== 'string' ||
    config.membershipOperatorName.length > 128 || /[\x00-\x1f]/.test(config.membershipOperatorName))) throw new Error('NOT_CONFIGURED');
}

export interface AccountSession {
  token: string;
  userId: string;
  expiresAt: number;
}
export interface MemberEntitlement {
  nextRenewalAt?: number;
  nextRenewalPrice?: number;
  renewalCurrency?: string;
  subscriptionProductId?: string;
  refreshRequired?: boolean;
  subscriptionStatus?: string;
  autoRenew?: boolean;
  billingRetry?: boolean;
  active: boolean;
  expiresAt: number;
  serverTime: number;
}
export interface ServiceCapabilities {
  membershipPurchases?: boolean;
  renewalReminders?: boolean;
  account: boolean;
  commerce: boolean;
  sandboxCommerce?: boolean;
  sandboxCheckout?: boolean;
  profiles: boolean;
  preferences?: boolean;
  cloudSaves: boolean;
  configuration?: boolean;
  reason: string;
}
export interface ProfileSummary {
  variant: string;
  name: string;
  revision: number;
  deviceClass: string;
}
export interface ProfileEnvelope {
  schemaVersion: number;
  profileFamily: string;
  deviceClass: string;
  variant: string;
  revision: number;
  baseRevision: number;
  payloadSha256: string;
  payload: InputProfile;
}
export interface ProfileConflict {
  local: ProfileEnvelope;
  remote: ProfileEnvelope;
}

function allowedKeys(value: Object, keys: string[]): void {
  if (!value || typeof value !== 'object' || Array.isArray(value) ||
    Object.keys(value).some((key: string): boolean => keys.indexOf(key) < 0)) {
    throw new Error('INVALID_PROFILE');
  }
}
function boundedString(value: string, max: number): void {
  if (typeof value !== 'string' || value.length > max || /[\u0000-\u0008\u000b\u000c\u000e-\u001f]/.test(value)) {
    throw new Error('INVALID_PROFILE');
  }
}
function boundedNumber(value: number, min: number, max: number): void {
  if (typeof value !== 'number' || !Number.isFinite(value) || value < min || value > max) {
    throw new Error('INVALID_PROFILE');
  }
}
function optionalNumber(value: number | undefined, min: number, max: number): void {
  if (value !== undefined) boundedNumber(value, min, max);
}
function validateBinding(binding: InputBinding): void {
  allowedKeys(binding, ['kind', 'code']);
  if (['NONE', 'KEY', 'MOUSE'].indexOf(binding.kind) < 0) throw new Error('INVALID_PROFILE');
  boundedNumber(binding.code, 0, 65535);
  if (!Number.isInteger(binding.code)) throw new Error('INVALID_PROFILE');
}
function validateElement(element: ControlElement): void {
  allowedKeys(element, ['id', 'type', 'shape', 'x', 'y', 'scale', 'widthScale', 'heightScale',
    'panelWidthRatio', 'panelHeightRatio', 'keyWidthRatio', 'keyHeightRatio', 'opacity', 'bindings',
    'toggleSwitch', 'text', 'iconId', 'visible', 'keyList', 'orientation', 'outputMode']);
  boundedString(element.id, 128);
  boundedString(element.text, 128);
  if (!element.id || ['PANEL', 'BUTTON', 'D_PAD', 'STICK', 'TRACKPAD', 'RANGE_BUTTON'].indexOf(element.type) < 0 ||
    ['CIRCLE', 'RECT', 'ROUND_RECT', 'SQUARE'].indexOf(element.shape) < 0 ||
    ['vertical', 'horizontal'].indexOf(element.orientation) < 0 || ['keys', 'mouse'].indexOf(element.outputMode) < 0 ||
    typeof element.toggleSwitch !== 'boolean' || typeof element.visible !== 'boolean') throw new Error('INVALID_PROFILE');
  boundedNumber(element.x, 0, 1); boundedNumber(element.y, 0, 1);
  boundedNumber(element.scale, 0.1, 5); boundedNumber(element.opacity, -1, 100);
  boundedNumber(element.iconId, 0, 65535);
  optionalNumber(element.widthScale, 0.2, 12); optionalNumber(element.heightScale, 0.2, 12);
  optionalNumber(element.panelWidthRatio, 0, 1); optionalNumber(element.panelHeightRatio, 0, 1);
  optionalNumber(element.keyWidthRatio, 0, 1); optionalNumber(element.keyHeightRatio, 0, 1);
  if (!Array.isArray(element.bindings) || element.bindings.length > 32 ||
    !Array.isArray(element.keyList) || element.keyList.length > 256) throw new Error('INVALID_PROFILE');
  element.bindings.forEach(validateBinding);
  element.keyList.forEach((key: number): void => {
    boundedNumber(key, 0, 65535);
    if (!Number.isInteger(key)) throw new Error('INVALID_PROFILE');
  });
}

/** Exact native-v6 allowlist. JSON is validated before copying, never spread from preferences. */
export function validateCloudProfile(profile: InputProfile): InputProfile {
  allowedKeys(profile, ['id', 'name', 'schemaVersion', 'builtInRevision', 'cursorSpeed', 'overlayOpacity',
    'gamepadMappings', 'elements']);
  boundedString(profile.id, 128); boundedString(profile.name, 128);
  if (!/^[a-zA-Z0-9_-]{1,128}$/.test(profile.id) || profile.schemaVersion !== 6) throw new Error('INVALID_PROFILE');
  boundedNumber(profile.cursorSpeed, 0.1, 5); boundedNumber(profile.overlayOpacity, 0, 100);
  optionalNumber(profile.builtInRevision, 0, 1000000);
  if (!Array.isArray(profile.elements) || profile.elements.length > 512 ||
    !Array.isArray(profile.gamepadMappings) || profile.gamepadMappings.length > 128) throw new Error('INVALID_PROFILE');
  const ids = new Set<string>();
  profile.elements.forEach((element: ControlElement): void => {
    validateElement(element);
    if (ids.has(element.id)) throw new Error('INVALID_PROFILE');
    ids.add(element.id);
  });
  profile.gamepadMappings.forEach((mapping: GamepadButtonMapping): void => {
    allowedKeys(mapping, ['id', 'label', 'buttonCode', 'evdevCode']);
    boundedString(mapping.id, 128); boundedString(mapping.label, 128);
    boundedNumber(mapping.buttonCode, 0, 65535); boundedNumber(mapping.evdevCode, 0, 65535);
  });
  if (JSON.stringify(profile).length > 262144) throw new Error('INVALID_PROFILE');
  return JSON.parse(JSON.stringify(profile)) as InputProfile;
}

/** Local legacy fields are explicitly omitted; all current fields are preserved verbatim. */
export function cloudProfilePayload(profile: InputProfile): InputProfile {
  const result: InputProfile = {
    id: profile.id, name: profile.name, schemaVersion: profile.schemaVersion,
    builtInRevision: profile.builtInRevision, cursorSpeed: profile.cursorSpeed,
    overlayOpacity: profile.overlayOpacity, gamepadMappings: profile.gamepadMappings, elements: profile.elements
  };
  return validateCloudProfile(JSON.parse(JSON.stringify(result)) as InputProfile);
}

export function validateProfileEnvelope(value: ProfileEnvelope, deviceClass: string, variant: string): void {
  allowedKeys(value, ['schemaVersion', 'profileFamily', 'deviceClass', 'variant', 'revision', 'baseRevision',
    'payloadSha256', 'payload']);
  if (value.schemaVersion !== 1 || value.profileFamily !== 'input/native-v6' || value.deviceClass !== deviceClass ||
    value.variant !== variant || value.payload.id !== variant || !Number.isSafeInteger(value.revision) ||
    value.revision < 1 || !Number.isSafeInteger(value.baseRevision) || value.baseRevision < 0 ||
    !/^[a-f0-9]{64}$/.test(value.payloadSha256)) throw new Error('INVALID_PROFILE');
  validateCloudProfile(value.payload);
}

export interface SaveManifestEntry {
  relativePath: string;
  size: number;
  modifiedAt: number;
  sha256: string;
}
export interface SaveManifest {
  schemaVersion: number;
  snapshotId: string;
  appId: string;
  rootAlias: string;
  selectedPath: string;
  createdAt: number;
  origin?: string;
  gameTitle?: string;
  gameIdentity?: string;
  files: SaveManifestEntry[];
}
export function safeRelativeSavePath(path: string): string {
  if (typeof path !== 'string' || !path || path.length > 512 || /[\\:\u0000-\u001f]/.test(path) ||
    path.startsWith('/') || path.endsWith('/') || path.split('/').some((part: string): boolean =>
      !part || part === '.' || part === '..' || part.endsWith('.') || part.endsWith(' '))) {
    throw new Error('UNSAFE_SAVE_PATH');
  }
  return path;
}
export function validateSaveEntries(entries: SaveManifestEntry[], allowEmpty: boolean = false): void {
  if (!Array.isArray(entries) || (!allowEmpty && entries.length < 1) || entries.length > 1024) throw new Error('SAVE_QUOTA');
  let total = 0;
  const paths = new Set<string>();
  entries.forEach((entry: SaveManifestEntry): void => {
    const path = safeRelativeSavePath(entry.relativePath).toLocaleLowerCase();
    if (paths.has(path)) throw new Error('SAVE_PATH_COLLISION');
    paths.add(path);
    if (!Number.isSafeInteger(entry.size) || entry.size < 0 || entry.size > 67108864 ||
      !Number.isFinite(entry.modifiedAt) || !/^[a-fA-F0-9]{64}$/.test(entry.sha256)) throw new Error('INVALID_SAVE_MANIFEST');
    total += entry.size;
  });
  if (total > 268435456) throw new Error('SAVE_QUOTA');
}
