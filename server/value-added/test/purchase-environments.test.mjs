import test from 'node:test';
import assert from 'node:assert/strict';
import { Readable } from 'node:stream';
import { createHash } from 'node:crypto';
import { ValueAddedDomain } from '../src/domain.mjs';
import { createApiDispatcher } from '../src/api-dispatcher.mjs';
import { LIFETIME_POLICY, LIFETIME_EXPIRES_AT, SUBSCRIPTION_POLICY } from '../src/membership-policy.mjs';
import { MemoryStore } from './memory-store.mjs';

const now = Date.now();
function fixture() {
  const store = new MemoryStore();
  // This fixture substitutes only the trusted provider boundary. Synthetic signed
  // NORMAL/SANDBOX receipts and mismatched official responses are covered by jws.test.
  const provider = { commerceReady: true, ready: false, verify: async data => JSON.parse(data) };
  const domain = new ValueAddedDomain({ store, purchaseProvider: provider,
    accountProvider: { ready: true, exchange: async subject => ({ subject, audience: 'client' }) },
    config: { appId: 'app', clientId: 'client', identitySecret: 'i'.repeat(32), purchaseBindingSecret: 'p'.repeat(32),
      isolatePurchaseEnvironments: true, enforceCheckoutReadiness: true, membershipReleaseReady: true,
      products: [{ id: 'VIP', type: 1, kind: 'membership', policyVersion: LIFETIME_POLICY },
        { id: 'auto', type: 2, kind: 'membership', policyVersion: SUBSCRIPTION_POLICY }] }, clock: () => now });
  const dispatch = createApiDispatcher(domain);
  const request = async (session, path, data, extra = {}) => {
    const req = Readable.from(data === undefined ? [] : [Buffer.from(JSON.stringify(data))]);
    Object.assign(req, { url: path, method: data === undefined ? 'GET' : 'POST', headers: {
      'content-type': 'application/json', authorization: 'Bearer ' + session.token, ...extra } });
    const result = await dispatch(req);
    return { httpStatus: result.status, ...JSON.parse(result.body) };
  };
  const login = async (subject = 'owner') => domain.exchange({ ...await domain.begin(), code: subject, clientId: 'client' });
  const fact = async (session, environment, orderId = 'shared-order') => ({
    appId: 'app', productId: 'VIP', productType: 1, environment, orderId,
    purchaseToken: environment + '-opaque-token', quantity: 1, status: 'purchased', needsFinish: false,
    purchasedAt: now - 1000, expiresAt: LIFETIME_EXPIRES_AT,
    developerPayload: (await request(session, '/v1/purchases/intent', { productId: 'VIP', productType: 1 })).developerPayload
  });
  const verify = (session, receipt, extra = {}) => request(session, '/v1/purchases/verify', {
    purchaseData: JSON.stringify(receipt), productType: receipt.productType, ...extra });
  return { domain, store, provider, request, login, fact, verify };
}

test('one checkout capability serves ordinary users without exposing tester gates or opening paid sync', () => {
  const { domain } = fixture(), capabilities = domain.capabilities();
  assert.equal(capabilities.commerce, true);
  assert.equal(capabilities.membershipPurchases, true);
  assert.equal(capabilities.sandboxCommerce, undefined);
  assert.equal(capabilities.sandboxCheckout, undefined);
  assert.equal(capabilities.profiles, false);
  assert.equal(capabilities.configuration, false);
});

test('legacy lifetime grants stay intact but cannot turn a fresh formal session into VIP', async () => {
  const f = fixture(), session = await f.login();
  const legacy = { orders: { old: { status: 'purchased', purchasedAt: now - 1000, verifiedAt: now,
    orderId: 'old', policyVersion: LIFETIME_POLICY } } };
  await f.store.transaction(async tx => tx.put('entitlement:' + session.userId, legacy, 'entitlements'));
  const current = await f.request(session, '/v1/entitlement');
  assert.equal(current.environment, 'NORMAL');
  assert.equal(current.active, false);
  assert.deepEqual(await f.store.get('entitlement:' + session.userId), legacy);
  assert.equal((await f.request(session, '/v1/purchases/intent', { productId: 'VIP', productType: 1 })).httpStatus, 200);
});

test('signed test receipts affect only their own session; normal and test order IDs and refunds are isolated', async () => {
  const f = fixture(), tester = await f.login(), formal = await f.login();
  assert.equal(tester.userId, formal.userId);
  const sandbox = await f.fact(tester, 'SANDBOX'), normal = await f.fact(formal, 'NORMAL');
  const testGrant = await f.verify(tester, sandbox, { environment: 'NORMAL' });
  assert.equal(testGrant.httpStatus, 200);
  assert.equal(testGrant.environment, 'SANDBOX', 'a client flag cannot relabel the verified order');
  assert.equal((await f.request(tester, '/v1/entitlement')).active, true);
  assert.equal((await f.request(tester, '/v1/entitlement')).environment, 'SANDBOX');
  assert.equal((await f.request(formal, '/v1/entitlement')).active, false);
  const fresh = await f.login();
  assert.equal((await f.request(fresh, '/v1/entitlement')).active, false, 'reinstall/new login starts with formal entitlements');
  assert.equal((await f.request(formal, '/v1/entitlement?environment=SANDBOX')).httpStatus, 404);
  assert.equal((await f.request(formal, '/v1/entitlement', undefined, { 'x-purchase-environment': 'SANDBOX' })).active, false);
  assert.equal((await f.verify(formal, normal)).environment, 'NORMAL');
  assert.equal((await f.request(formal, '/v1/entitlement')).active, true);
  assert.equal((await f.store.list('orders:' + tester.userId)).length, 2);
  await f.verify(formal, sandbox);
  assert.equal((await f.request(formal, '/v1/entitlement')).environment, 'NORMAL', 'restoring test orders cannot downgrade formal rights');
  await f.verify(tester, { ...sandbox, status: 'revoked' });
  assert.equal((await f.request(tester, '/v1/entitlement')).active, false);
  assert.equal((await f.request(formal, '/v1/entitlement')).active, true);
  await assert.rejects(f.domain.verify(tester.userId, { purchaseData: JSON.stringify(sandbox), productType: 1 }), /ORDER_REVOKED/);
});

test('missing or unknown verified environments, wrong owners and pending orders never acquire rights', async () => {
  const f = fixture(), session = await f.login(), other = await f.login('other');
  const receipt = await f.fact(session, 'SANDBOX');
  for (const environment of [undefined, 'AUTO', 'NORMAL ']) {
    assert.equal((await f.verify(session, { ...receipt, environment })).httpStatus, 400);
  }
  assert.equal((await f.verify(other, receipt)).httpStatus, 403);
  assert.equal((await f.verify(session, { ...receipt, status: 'pending' })).finishAllowed, false);
  assert.equal((await f.request(session, '/v1/entitlement')).environment, 'NORMAL');
  assert.equal((await f.request(session, '/v1/entitlement')).active, false);
});

test('a real historical order regains formal rights only after provider verification; legacy revocations stay final', async () => {
  const f = fixture(), session = await f.login(), receipt = await f.fact(session, 'NORMAL');
  const key = 'order:' + createHash('sha256').update('app\n' + receipt.orderId).digest('hex');
  const legacy = { userId: session.userId, appId: 'app', productId: 'VIP', productType: 1,
    bindingHash: createHash('sha256').update(receipt.developerPayload).digest('hex'),
    purchasedAt: receipt.purchasedAt, expiresAt: receipt.expiresAt, policyVersion: LIFETIME_POLICY, status: 'purchased' };
  await f.store.transaction(async tx => tx.put(key, legacy, 'orders:' + session.userId));
  assert.equal((await f.request(session, '/v1/entitlement')).active, false);
  assert.equal((await f.verify(session, receipt)).environment, 'NORMAL');
  assert.equal((await f.request(session, '/v1/entitlement')).active, true);
  assert.deepEqual(await f.store.get(key), legacy, 'the historical row is retained');
  const other = fixture(), otherSession = await other.login(), otherReceipt = await other.fact(otherSession, 'NORMAL');
  await other.store.transaction(async tx => tx.put(key, { ...legacy, status: 'revoked' }, 'orders:' + otherSession.userId));
  // Preserve the original bound receipt so this checks revocation rather than a binding mismatch.
  otherReceipt.developerPayload = receipt.developerPayload;
  assert.equal((await other.verify(otherSession, otherReceipt)).httpStatus, 409);
  assert.equal((await other.request(otherSession, '/v1/entitlement')).active, false);
});

test('sandbox subscriptions never schedule formal renewal notices or unlock formal paid features', async () => {
  const f = fixture(), session = await f.login();
  const base = await f.fact(session, 'SANDBOX');
  const fact = { ...base, productId: 'auto', productType: 2, subscriptionId: 'sub', subscriptionGroupId: 'group',
    generationId: 'gen', subscriptionStatus: '1', autoRenew: true, billingRetry: false,
    signedAt: now, policyVersion: SUBSCRIPTION_POLICY, expiresAt: now + 86400000,
    nextRenewalAt: now + 86400000, nextRenewalPrice: 500, renewalCurrency: 'CNY' };
  // The fixture needs an authentic binding for the selected SKU, with no intent
  // bypass of reminder readiness in the public checkout path.
  const noReadiness = Object.assign(Object.create(f.domain), { config: { ...f.domain.config, enforceCheckoutReadiness: false } });
  fact.developerPayload = (await noReadiness.intent(session.userId, { productId: 'auto', productType: 2 })).developerPayload;
  assert.equal((await f.verify(session, fact)).environment, 'SANDBOX');
  assert.equal((await f.store.list('renewal-jobs')).length, 0);
  assert.equal((await f.domain.entitlement(session.userId)).active, false);
  await assert.rejects(f.domain.requireMembership(session.userId), /NOT_CONFIGURED/);
  await assert.rejects(f.domain.intent(session.userId, { productId: 'auto', productType: 2 }), /REMINDERS_UNAVAILABLE/);
  const normal = { ...fact, environment: 'NORMAL' };
  assert.equal((await f.verify(session, normal)).environment, 'NORMAL');
  assert.equal((await f.store.list('renewal-jobs')).length, 1);
  assert.equal((await f.domain.entitlement(session.userId)).active, true);
});
