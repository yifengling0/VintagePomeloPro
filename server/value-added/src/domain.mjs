import { randomBytes, createHash, createHmac, timingSafeEqual } from 'node:crypto';
import { requireThat, ServiceError } from './errors.mjs';
import { deriveMembership, MEMBERSHIP_POLICY, SUBSCRIPTION_POLICY } from './membership-policy.mjs';
import { sealReceipt, openReceipt } from './subscription.mjs';
import { CloudSaves } from './cloud-saves.mjs';
import { Configurations } from './configurations.mjs';
import { ConfigurationDatabaseObjects } from './configuration-database-objects.mjs';
import { validatePreferences, preferencesEnvelope } from './preferences.mjs';
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
    this.configurations = new Configurations(this,objects.ready ? objects : new ConfigurationDatabaseObjects(store),clock);
    this.subscriptionRefreshes = new Map();
  }
  capabilities() {
    const storage = this.store.ready === true;
    const sandbox = storage && this.purchaseProvider.sandboxReady === true && this.purchaseProvider.config?.environment === 'SANDBOX';
    return { account: storage && this.accountProvider.ready, commerce: storage && this.purchaseProvider.ready || sandbox,
      ...(sandbox ? { sandboxCommerce: true } : {}),
      profiles: storage && this.purchaseProvider.ready, preferences: storage && this.purchaseProvider.ready, cloudSaves: storage && this.purchaseProvider.ready && this.saves.objects.ready,
      configuration: storage && this.purchaseProvider.ready && this.configurations.objects.ready,
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
  async intent(userId, { productId, productType, promotionalOfferId }) {
    this.gate('commerce');
    const product = this.product(productId, productType);
    if (product.kind === 'membership') {
      const entitlement = await this.store.get('entitlement:' + userId);
      requireThat(Object.keys(entitlement?.orders ?? {}).length < 256, 'ORDER_LIMIT', 409);
    }
    // 32 + 1 + 64 + 1 + 43 = 141 chars, safely under the IAP 256-character limit.
    const value = randomBytes(16).toString('hex') + '.' + userId;
    const signature = createHmac('sha256', this.config.purchaseBindingSecret).update(value + '\n' + productId).digest('base64url');
    if (promotionalOfferId !== undefined) {
      requireThat(productType === 2 && typeof promotionalOfferId === 'string' &&
        /^[a-zA-Z0-9._-]{1,64}$/.test(promotionalOfferId) && Object.hasOwn(product.promotionalPrices ?? {}, promotionalOfferId),
        'UNKNOWN_OFFER');
      return { developerPayload: value + '.' + signature,
        promotionalPurchase: this.purchaseProvider.signPromotion(productId, promotionalOfferId, userId, this.clock()) };
    }
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
    const observedAt = this.clock();
    const fact = await this.purchaseProvider.verify(purchaseData, productType);
    const product = this.product(fact.productId, fact.productType);
    if (productType === 2 && !fact.developerPayload && typeof fact.subscriptionId === 'string') {
      const key = 'subscription:' + digest(this.config.appId + '\n' + fact.subscriptionId);
      const known = await this.store.get(key);
      requireThat(known?.userId === userId && known.appId === fact.appId && known.productId === fact.productId &&
        known.generationId === fact.generationId && known.tokenHash === digest(fact.purchaseToken), 'ORDER_BINDING_MISMATCH', 403);
      const original = openReceipt(known.receiptCipher, this.config.purchaseBindingSecret, userId + '\n' + key);
      requireThat(original !== purchaseData, 'ORDER_BINDING_MISMATCH', 403);
      // Some renewal receipts omit the original payload. Requery with the already bound receipt;
      // an unknown subscription or another account can never claim it from token possession alone.
      return this.verify(userId, { purchaseData: original, productType: 2 });
    }
    requireThat(fact.productType === productType && fact.appId === this.config.appId && fact.quantity === 1 &&
      typeof fact.orderId === 'string' && fact.orderId.length > 0 && fact.orderId.length <= 256 &&
      this.verifyBinding(fact.developerPayload, userId, fact.productId) &&
      (fact.receiptDeveloperPayload === undefined || this.verifyBinding(fact.receiptDeveloperPayload, userId, fact.productId)),
      'ORDER_BINDING_MISMATCH', 403);
    requireThat(['purchased','pending','refunded','revoked','expired'].includes(fact.status), 'INVALID_ORDER');
    if (fact.status === 'pending') return { status: 'STORE_PENDING', finishAllowed: false };
    const now = this.clock();
    requireThat(Number.isFinite(fact.purchasedAt) && fact.purchasedAt > 0 && fact.purchasedAt <= now + 60000, 'INVALID_ORDER');
    if (product.kind === 'membership') requireThat(Number.isFinite(fact.expiresAt) && fact.expiresAt > fact.purchasedAt, 'INVALID_ORDER');
    if (productType === 2) return this.recordSubscription(userId, fact, purchaseData, observedAt);
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
      if (product.kind === 'membership') tx.put(entitlementKey, { ...entitlement, orders: facts }, 'entitlements');
    });
    const finishAllowed = fact.status === 'purchased' && fact.needsFinish === true;
    return { status: fact.status === 'purchased' ? (finishAllowed ? 'GRANTED' : 'ALREADY_FINISHED') : fact.status.toUpperCase(), finishAllowed,
      ...(finishAllowed ? { productId: fact.productId, productType: fact.productType,
        purchaseOrderId: fact.orderId, purchaseToken: fact.purchaseToken } : {}) };
  }
  async recordSubscription(userId, fact, receipt, observedAt) {
    requireThat(fact.policyVersion === SUBSCRIPTION_POLICY &&
      ['subscriptionId', 'subscriptionGroupId', 'generationId'].every(field => typeof fact[field] === 'string' &&
        fact[field].length > 0 && fact[field].length <= 256) &&
      ['1', '2', '3', '5'].includes(fact.subscriptionStatus) && typeof fact.autoRenew === 'boolean' &&
      typeof fact.billingRetry === 'boolean' && Number.isSafeInteger(fact.signedAt), 'INVALID_SUBSCRIPTION');
    const key = 'subscription:' + digest(this.config.appId + '\n' + fact.subscriptionId);
    const now = this.clock();
    const state = { userId, appId: fact.appId, productId: fact.productId, subscriptionId: fact.subscriptionId,
      subscriptionGroupId: fact.subscriptionGroupId, generationId: fact.generationId, status: fact.status,
      subscriptionStatus: fact.subscriptionStatus, autoRenew: fact.autoRenew, billingRetry: fact.billingRetry,
      expiresAt: fact.expiresAt, orderId: fact.orderId, purchasedAt: fact.purchasedAt, signedAt: fact.signedAt,
      verifiedAt: now, observedAt, policyVersion: SUBSCRIPTION_POLICY, tokenHash: digest(fact.purchaseToken),
      bindingHash: digest(fact.developerPayload) };
    const accepted = await this.store.transaction(async tx => {
      const existing = await tx.get(key), entitlementKey = 'entitlement:' + userId;
      const entitlement = await tx.get(entitlementKey) ?? { orders: {}, subscriptions: {} };
      if (existing) {
        requireThat(existing.userId === userId && existing.appId === state.appId && existing.productId === state.productId &&
          existing.generationId === state.generationId && existing.tokenHash === state.tokenHash,
          'ORDER_OWNER_IMMUTABLE', 403);
        // An older query completing late cannot roll back a newer renewal/refund snapshot.
        if (existing.observedAt > observedAt || existing.signedAt > state.signedAt || existing.purchasedAt > state.purchasedAt) return false;
        // Rotation is valid only for a newer, separately identified purchase whose
        // authenticated binding still proves the same immutable account owner.
        requireThat(this.verifyBinding(fact.developerPayload, userId, fact.productId) &&
          (existing.bindingHash === state.bindingHash || existing.orderId !== state.orderId && existing.purchasedAt < state.purchasedAt),
          'ORDER_BINDING_MISMATCH', 403);
        if (existing.status === 'revoked' && state.status !== 'revoked' &&
          (existing.orderId === state.orderId || state.purchasedAt <= existing.purchasedAt)) return false;
      }
      const subscriptions = entitlement.subscriptions ?? {};
      requireThat(subscriptions[key] || Object.keys(subscriptions).length < 32, 'ORDER_LIMIT', 409);
      subscriptions[key] = state;
      tx.put(key, { ...state, receiptCipher: sealReceipt(receipt, this.config.purchaseBindingSecret, userId + '\n' + key) },
        'subscriptions:' + userId);
      tx.put(entitlementKey, { ...entitlement, subscriptions }, 'entitlements');
      return true;
    });
    const finishAllowed = accepted && fact.status === 'purchased' && fact.needsFinish === true;
    return { status: !accepted ? 'SUPERSEDED' : fact.status === 'purchased' ?
      (finishAllowed ? 'GRANTED' : 'ALREADY_FINISHED') : fact.status.toUpperCase(), finishAllowed,
      ...(finishAllowed ? { productId: fact.productId, productType: 2,
        purchaseOrderId: fact.orderId, purchaseToken: fact.purchaseToken } : {}) };
  }
  async refreshSubscriptions(userId, value) {
    if (!this.capabilities().commerce) return;
    if (this.subscriptionRefreshes.has(userId)) return this.subscriptionRefreshes.get(userId);
    const work = (async () => {
      for (const [key, state] of Object.entries(value?.subscriptions ?? {})) {
        if (state.verifiedAt + FRESH_MS > this.clock()) continue;
        try {
          const stored = await this.store.get(key);
          requireThat(stored?.userId === userId, 'ORDER_OWNER_IMMUTABLE', 403);
          const receipt = openReceipt(stored.receiptCipher, this.config.purchaseBindingSecret, userId + '\n' + key);
          await this.verify(userId, { purchaseData: receipt, productType: 2 });
        } catch { /* Keep the old verification time. Provider failure cannot extend access. */ }
      }
    })();
    this.subscriptionRefreshes.set(userId, work);
    try { await work; } finally { this.subscriptionRefreshes.delete(userId); }
  }
  async entitlement(userId) {
    let value = await this.store.get('entitlement:' + userId);
    if (Object.keys(value?.subscriptions ?? {}).length) {
      await this.refreshSubscriptions(userId, value);
      value = await this.store.get('entitlement:' + userId);
    }
    const now = this.clock();
    return deriveMembership(value?.orders ?? {}, now, FRESH_MS, value?.subscriptions ?? {});
  }
  async requireMembership(userId) {
    this.gate('profiles');
    requireThat((await this.entitlement(userId)).active, 'MEMBERSHIP_REQUIRED', 403);
  }
  preferencesKey(userId, deviceClass) {
    requireThat(['tablet','desktop'].includes(deviceClass),'INVALID_PREFERENCES');
    return 'preferences:' + digest(userId + '\n' + deviceClass);
  }
  async getPreferences(userId, deviceClass) {
    await this.requireMembership(userId);
    return {current: await this.store.get(this.preferencesKey(userId,deviceClass))};
  }
  async putPreferences(userId, deviceClass, {baseRevision,payload}) {
    await this.requireMembership(userId);
    requireThat(Number.isSafeInteger(baseRevision) && baseRevision >= 0,'INVALID_PREFERENCES');
    const key = this.preferencesKey(userId,deviceClass), clean = validatePreferences(payload);
    return this.store.transaction(async tx => {
      const current = await tx.get(key);
      if ((current?.revision ?? 0) !== baseRevision) return {status:'CONFLICT',current};
      const next = preferencesEnvelope(clean,deviceClass,baseRevision+1);
      tx.put(key,next,'preferences'); return {status:'SAVED',current:next};
    });
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
