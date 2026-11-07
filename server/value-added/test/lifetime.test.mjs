import test from 'node:test';
import assert from 'node:assert/strict';
import { generateKeyPairSync } from 'node:crypto';
import { deriveMembership, LIFETIME_EXPIRES_AT, LIFETIME_POLICY, MEMBERSHIP_POLICY } from '../src/membership-policy.mjs';
import { HuaweiPurchaseProvider } from '../src/huawei-providers.mjs';
import { ValueAddedDomain } from '../src/domain.mjs';
import { membershipProductsVerified } from '../src/main.mjs';
import { MemoryStore } from './memory-store.mjs';

const now = Date.parse('2026-11-07T00:00:00+08:00');
const lifetime = { id: 'VIP', type: 3, kind: 'membership', purchasesEnabled: true,
  policyVersion: LIFETIME_POLICY, prices: { CNY: [6800] },
  promotionalPrices: { time_limit: { CNY: [3800] } } };
const owner = 'a'.repeat(64);

test('lifetime purchase is permanently active and needs no periodic revalidation', () => {
  const order = { status: 'purchased', policyVersion: LIFETIME_POLICY, orderId: 'lifetime-order',
    purchasedAt: now - 86400000, verifiedAt: now - 86400000 };
  const value = deriveMembership({ lifetime: order }, now, 300000);
  assert.equal(value.active, true);
  assert.equal(value.lifetime, true);
  assert.equal(value.expiresAt, LIFETIME_EXPIRES_AT);
  assert.equal(value.refreshRequired, false);

  const refunded = deriveMembership({ lifetime: { ...order, status: 'refunded' } }, now, 300000);
  assert.equal(refunded.active, false);

  const stalePass = deriveMembership({ pass: { ...order, policyVersion: MEMBERSHIP_POLICY,
    purchasedAt: now - 40 * 86400000 } }, now, 300000);
  assert.equal(stalePass.active, false);
});

test('lifetime offer is signed for the configured one-time product', () => {
  const pair = generateKeyPairSync('ec', { namedCurve: 'prime256v1' });
  const provider = new HuaweiPurchaseProvider({ appId: 'app', issuerId: 'issuer', keyId: 'kid',
    products: [lifetime], privateKeyPem: pair.privateKey.export({ type: 'pkcs8', format: 'pem' }) });
  provider.ready = true;
  const signed = provider.signPromotion('VIP', 'time_limit', owner, now);
  const payload = JSON.parse(Buffer.from(signed.jwsRepresentation.split('.')[1], 'base64url'));
  const data = JSON.parse(payload.data);
  assert.deepEqual(data.productItem, { applicationId: 'app', productId: 'VIP' });
  assert.equal(data.offerInfo.offerId, 'time_limit');
  assert.equal(data.offerInfo.applicationUserName, owner);
  assert.throws(() => provider.signPromotion('VIP', 'unknown', owner, now), /UNKNOWN_OFFER/);
});

test('lifetime product enters purchase intent with its configured promotion only', async () => {
  let signs = 0;
  const domain = new ValueAddedDomain({ store: new MemoryStore(), accountProvider: { ready: true },
    purchaseProvider: { ready: true, signPromotion: (id, offer, user) => {
      signs++;
      return { promotionalOfferId: offer, applicationUserName: user, jwsRepresentation: 'opaque' };
    } },
    config: { products: [lifetime], purchaseBindingSecret: 'x'.repeat(32) }, clock: () => now });
  const intent = await domain.intent(owner, { productId: 'VIP', productType: 3, promotionalOfferId: 'time_limit' });
  assert.equal(intent.promotionalPurchase.promotionalOfferId, 'time_limit');
  assert.equal(signs, 1);
  await assert.rejects(domain.intent(owner,
    { productId: 'VIP', productType: 3, promotionalOfferId: 'unknown' }), /UNKNOWN_OFFER/);
  assert.equal((await domain.intent(owner, { productId: 'VIP', productType: 3 })).promotionalPurchase, undefined);
});

test('release readiness requires a verified lifetime SKU', () => {
  const release = { membershipProductId: 'vip_monthly_auto', lifetimeProductId: 'VIP' };
  assert.equal(membershipProductsVerified([lifetime], release), true);
  assert.equal(membershipProductsVerified([lifetime], { ...release, lifetimeProductId: 'other' }), false);
});
