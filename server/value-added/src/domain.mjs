import { randomBytes, createHash, createHmac, timingSafeEqual } from 'node:crypto';
import { requireThat, ServiceError } from './errors.mjs';
import { deriveMembership, MEMBERSHIP_POLICY } from './membership-policy.mjs';
import { CloudSaves } from './cloud-saves.mjs';
import { validateProfile, profileEnvelope } from './profiles.mjs';
const digest = value => createHash('sha256').update(value).digest('hex');
const random = () => randomBytes(32).toString('base64url');
const same = (a, b) => typeof a === 'string' && typeof b === 'string' && a.length === b.length &&
  timingSafeEqual(Buffer.from(a), Buffer.from(b));
const FRESH_MS = 5 * 60 * 1000;

/** The provider returns only server-verified facts; client JWS claims never reach this layer as identity. */
export class ValueAddedDomain {
  constructor({ store, accountProvider, purchaseProvider, config, objects = {ready:false}, clock = Date.now }) {
    this.store = store; this.accountProvider = accountProvider; this.purchaseProvider = purchaseProvider;
    this.config = config; this.clock = clock; this.saves = new CloudSaves(this,objects,clock);
  }
  capabilities() {
    const storage = this.store.ready === true;
    return { account: storage && this.accountProvider.ready, commerce: storage && this.purchaseProvider.ready,
      profiles: storage && this.purchaseProvider.ready, cloudSaves: storage && this.purchaseProvider.ready && this.saves.objects.ready,
      reason: storage ? (this.purchaseProvider.ready ? '' : 'IAP_NOT_CONFIGURED') : 'CLOUDDB_NOT_VERIFIED' };
  }
  gate(feature) { requireThat(this.capabilities()[feature], 'NOT_CONFIGURED', 503); }
  async begin() {
    this.gate('account');
    const attemptId = random(), state = random();
    await this.store.transaction(async tx => tx.put('attempt:' + digest(attemptId),
      { stateHash: digest(state), expiresAt: this.clock() + 300000, consumed: false }, 'attempt'));
    return { attemptId, state };
  }
  async exchange({ attemptId, state, code, clientId }) {
    this.gate('account');
    requireThat(clientId === this.config.clientId && typeof code === 'string' && code.length > 0 && code.length < 16384 &&
      typeof attemptId === 'string' && attemptId.length < 128 && typeof state === 'string' && state.length < 128, 'INVALID_AUTH');
    const now = this.clock();
    // Consume before calling the provider. A timeout requires a fresh authorization; no replay across processes.
    await this.store.transaction(async tx => {
      const key = 'attempt:' + digest(attemptId), attempt = await tx.get(key);
      requireThat(attempt && !attempt.consumed && attempt.expiresAt > now && same(attempt.stateHash, digest(state)), 'AUTH_STATE_MISMATCH');
      tx.put(key, { ...attempt, consumed: true }, 'attempt');
    });
    const identity = await this.accountProvider.exchange(code);
    requireThat(identity?.subject && identity.audience === this.config.clientId, 'INVALID_IDENTITY', 401);
    const userId = createHmac('sha256', this.config.identitySecret).update(identity.subject).digest('hex');
    const token = random(), expiresAt = now + 30 * 60 * 1000;
    await this.store.transaction(async tx => tx.put('session:' + digest(token), { userId, expiresAt, revoked: false }, 'session'));
    return { token, userId, expiresAt };
  }
  async authenticate(token) {
    requireThat(typeof token === 'string' && /^[A-Za-z0-9_-]{43}$/.test(token), 'SIGN_IN_REQUIRED', 401);
    const session = await this.store.get('session:' + digest(token));
    requireThat(session && !session.revoked && session.expiresAt > this.clock(), 'SIGN_IN_REQUIRED', 401);
    return session.userId;
  }
  async logout(token) {
    await this.authenticate(token);
    await this.store.transaction(async tx => {
      const key = 'session:' + digest(token), session = await tx.get(key);
      if (session) tx.put(key, { ...session, revoked: true }, 'session');
    });
    return {};
  }
  product(id, type) {
    const product = this.config.products.find(p => p.id === id && p.type === type);
    requireThat(product, 'UNKNOWN_PRODUCT');
    return product;
  }
  async intent(userId, { productId, productType }) {
    this.gate('commerce');
    const product = this.product(productId, productType);
    if (product.kind === 'membership') {
      const entitlement = await this.store.get('entitlement:' + userId);
      requireThat(Object.keys(entitlement?.orders ?? {}).length < 256, 'ORDER_LIMIT', 409);
    }
    // 32 + 1 + 64 + 1 + 43 = 141 chars, safely under the IAP 256-character limit.
    const value = randomBytes(16).toString('hex') + '.' + userId;
    const signature = createHmac('sha256', this.config.purchaseBindingSecret).update(value + '\n' + productId).digest('base64url');
    return { developerPayload: value + '.' + signature };
  }
  verifyBinding(binding, userId, productId) {
    if (typeof binding !== 'string') return false;
    const parts = binding.split('.');
    if (parts.length !== 3 || parts[1] !== userId || !/^[a-f0-9]{32}$/.test(parts[0])) return false;
    const expected = createHmac('sha256', this.config.purchaseBindingSecret)
      .update(parts[0] + '.' + userId + '\n' + productId).digest('base64url');
    return same(expected, parts[2]);
  }
  async verify(userId, { purchaseData, productType }) {
    this.gate('commerce');
    requireThat(typeof purchaseData === 'string' && Buffer.byteLength(purchaseData) < 131072, 'INVALID_ORDER');
    const fact = await this.purchaseProvider.verify(purchaseData, productType);
    const product = this.product(fact.productId, fact.productType);
    requireThat(fact.productType === productType && fact.appId === this.config.appId && fact.quantity === 1 &&
      typeof fact.orderId === 'string' && fact.orderId.length > 0 && fact.orderId.length <= 256 &&
      this.verifyBinding(fact.developerPayload, userId, fact.productId), 'ORDER_BINDING_MISMATCH', 403);
    requireThat(['purchased','pending','refunded','revoked','expired'].includes(fact.status), 'INVALID_ORDER');
    if (fact.status === 'pending') return { status: 'STORE_PENDING', finishAllowed: false };
    const now = this.clock();
    requireThat(Number.isFinite(fact.purchasedAt) && fact.purchasedAt > 0 && fact.purchasedAt <= now + 60000, 'INVALID_ORDER');
    if (product.kind === 'membership') requireThat(Number.isFinite(fact.expiresAt) && fact.expiresAt > fact.purchasedAt, 'INVALID_ORDER');
    const key = 'order:' + digest(this.config.appId + '\n' + fact.orderId);
    const normalized = { userId, appId: fact.appId, productId: fact.productId, productType: fact.productType,
      orderId: fact.orderId, kind: product.kind, status: fact.status, purchasedAt: fact.purchasedAt,
      expiresAt: product.kind === 'membership' ? fact.expiresAt : 0, verifiedAt: now,
      policyVersion: product.kind === 'membership' ? MEMBERSHIP_POLICY : 'coffee-v1',
      tokenHash: digest(fact.purchaseToken), bindingHash: digest(fact.developerPayload) };
    await this.store.transaction(async tx => {
      const existing = await tx.get(key);
      const entitlementKey = 'entitlement:' + userId;
      const entitlement = product.kind === 'membership' ? await tx.get(entitlementKey) : null;
      if (existing) {
        requireThat(existing.userId === userId && existing.productId === normalized.productId && existing.appId === normalized.appId &&
          existing.productType === normalized.productType && existing.bindingHash === normalized.bindingHash,
          'ORDER_OWNER_IMMUTABLE', 403);
        requireThat(existing.purchasedAt === normalized.purchasedAt, 'ORDER_FACT_IMMUTABLE', 403);
        normalized.expiresAt = existing.expiresAt;
        normalized.policyVersion = existing.policyVersion;
        // A refunded/revoked order cannot be resurrected by an old signed receipt.
        if (['refunded','revoked'].includes(existing.status) && !['refunded','revoked'].includes(normalized.status)) {
          throw new ServiceError('ORDER_REVOKED', 409);
        }
      }
      const facts = entitlement?.orders ?? {};
      if (product.kind === 'membership') {
        requireThat(facts[key] || Object.keys(facts).length < 256, 'ORDER_LIMIT', 409);
        facts[key] = { status: normalized.status, purchasedAt: normalized.purchasedAt, orderId: normalized.orderId,
          policyVersion: normalized.policyVersion, verifiedAt: now };
      }
      tx.put(key, normalized, 'orders:' + userId);
      if (product.kind === 'membership') tx.put(entitlementKey, { orders: facts }, 'entitlements');
    });
    const finishAllowed = fact.status === 'purchased' && fact.needsFinish === true;
    return { status: fact.status === 'purchased' ? (finishAllowed ? 'GRANTED' : 'ALREADY_FINISHED') : fact.status.toUpperCase(), finishAllowed,
      ...(finishAllowed ? { productId: fact.productId, productType: fact.productType,
        purchaseOrderId: fact.orderId, purchaseToken: fact.purchaseToken } : {}) };
  }
  async entitlement(userId) {
    const value = await this.store.get('entitlement:' + userId);
    const now = this.clock();
    return deriveMembership(value?.orders ?? {}, now, FRESH_MS);
  }
  async requireMembership(userId) {
    this.gate('profiles');
    requireThat((await this.entitlement(userId)).active, 'MEMBERSHIP_REQUIRED', 403);
  }
  profileKey(userId, deviceClass, id) {
    requireThat(['tablet','desktop'].includes(deviceClass) && /^[a-zA-Z0-9_-]{1,128}$/.test(id), 'INVALID_PROFILE');
    return 'profile:' + digest(userId + '\n' + deviceClass + '\n' + id);
  }
  async listProfiles(userId, deviceClass) {
    await this.requireMembership(userId);
    requireThat(['tablet','desktop'].includes(deviceClass), 'INVALID_PROFILE');
    const index = await this.store.get('profile-index:' + digest(userId + '\n' + deviceClass));
    return { profiles: index?.summaries ?? [] };
  }
  async getProfile(userId, deviceClass, id) {
    await this.requireMembership(userId);
    const value = await this.store.get(this.profileKey(userId, deviceClass, id));
    requireThat(value, 'PROFILE_NOT_FOUND', 404); return value;
  }
  async putProfile(userId, deviceClass, id, { baseRevision, payload }) {
    await this.requireMembership(userId);
    const key = this.profileKey(userId, deviceClass, id), clean = validateProfile(payload);
    requireThat(clean.id === id && Number.isSafeInteger(baseRevision) && baseRevision >= 0, 'INVALID_PROFILE');
    return this.store.transaction(async tx => {
      const current = await tx.get(key);
      const indexKey = 'profile-index:' + digest(userId + '\n' + deviceClass);
      const index = await tx.get(indexKey) ?? { ids: [], summaries: [] };
      if (!current) requireThat(index.ids.length < 256, 'PROFILE_LIMIT', 409);
      if ((current?.revision ?? 0) !== baseRevision) return { status: 'CONFLICT', current };
      const next = profileEnvelope(clean, deviceClass, baseRevision + 1, baseRevision);
      tx.put(key, next, 'profiles:' + userId + ':' + deviceClass);
      if (!index.ids.includes(id)) index.ids.push(id);
      index.summaries = (index.summaries ?? []).filter(item => item.variant !== id);
      index.summaries.push({variant: id, name: clean.name, revision: next.revision, deviceClass});
      tx.put(indexKey, index, 'profile-index');
      return { status: 'SAVED', current: next };
    });
  }
}
