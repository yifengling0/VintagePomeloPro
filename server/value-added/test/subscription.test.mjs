import test from 'node:test';
import assert from 'node:assert/strict';
import { subscriptionFact, sealReceipt, openReceipt } from '../src/subscription.mjs';
import { deriveMembership, SUBSCRIPTION_POLICY } from '../src/membership-policy.mjs';
import { ValueAddedDomain } from '../src/domain.mjs';
import { MemoryStore } from './memory-store.mjs';

const now = 1800000000000;
const config = { appId: 'app', packageName: 'com.test', environment: 'SANDBOX', products: [
  { id: 'monthly', type: 2, kind: 'membership', subscriptionGroupId: 'members',
    policyVersion: SUBSCRIPTION_POLICY, prices: { CNY: [500] } } ] };
const candidate = { applicationId: 'app', productId: 'monthly', productType: '2', subscriptionId: 'sub',
  subGroupId: 'members', subGroupGenerationId: 'generation', purchaseToken: 'token', developerPayload: 'bound',
  purchaseOrderId: 'original', environment: 'SANDBOX' };
const order = { ...candidate, purchaseOrderId: 'renewal', duration: 'P1M', price: 500, currency: 'CNY',
  signedTime: now, purchaseTime: now - 1000, finishStatus: '1' };
const current = { subscriptionId: 'sub', subGroupGenerationId: 'generation', purchaseToken: 'token',
  status: '1', expiresTime: now + 3600000, lastPurchaseOrder: order, renewalInfo: { productId: 'monthly',
    subGroupGenerationId: 'generation', environment: 'SANDBOX', autoRenewStatusCode: '1', hasInBillingRetryPeriod: false } };
const group = { environment: 'SANDBOX', applicationId: 'app', packageName: 'com.test', subGroupId: 'members',
  lastSubscriptionStatus: current };
const fact = overrides => subscriptionFact({ ...group, lastSubscriptionStatus: { ...current, ...overrides } }, candidate, config, now);

test('official subscription expiry is authoritative; cancellation keeps this term, retry/expiry/revocation stop access', () => {
  const renewal = fact({});
  assert.equal(renewal.expiresAt, current.expiresTime);
  assert.equal(renewal.orderId, 'renewal');
  const cancelled = fact({ renewalInfo: { ...current.renewalInfo, autoRenewStatusCode: '0' } });
  assert.equal(cancelled.status, 'purchased'); assert.equal(cancelled.autoRenew, false);
  for (const status of ['2', '3', '5']) assert.notEqual(fact({ status }).status, 'purchased');
  assert.equal(fact({ expiresTime: now }).status, 'expired');
  assert.equal(fact({ lastPurchaseOrder: { ...order, revocationTime: now } }).status, 'revoked');
  assert.equal(fact({ lastPurchaseOrder: { ...order, developerPayload: undefined } }).developerPayload, 'bound');
});
test('group/package/token/generation/product/price/time/finish/binding tampering is rejected', () => {
  for (const bad of [{ packageName: 'other' }, { applicationId: 'other' }, { environment: 'NORMAL' }, { subGroupId: 'other' }]) {
    assert.throws(() => subscriptionFact({ ...group, ...bad }, candidate, config, now));
  }
  for (const bad of [{ purchaseToken: 'other' }, { subscriptionId: 'other' }, { subGroupGenerationId: 'other' },
    { status: '4' }, { expiresTime: NaN }, { renewalInfo: undefined }]) assert.throws(() => fact(bad));
  for (const bad of [{ price: 800 }, { duration: 'P1Y' }, { quantity: 2 }, { signedTime: now + 60001 },
    { purchaseTime: undefined }, { finishStatus: undefined }]) {
    assert.throws(() => fact({ lastPurchaseOrder: { ...order, ...bad } }));
  }
  const switched = { ...current, subscriptionId: 'other', purchaseToken: 'other' };
  assert.equal(subscriptionFact({ ...group, lastSubscriptionStatus: switched, historySubscriptionStatusList: [current] },
    candidate, config, now).status, 'expired');
});
test('duplicate subscription snapshots do not add months; stale state fails closed; encrypted receipt is owner bound', () => {
  const state = { ...fact({}), verifiedAt: now, policyVersion: SUBSCRIPTION_POLICY };
  const access = deriveMembership({}, now, 300000, { first: state, duplicate: state });
  assert.equal(access.expiresAt, current.expiresTime); assert.equal(access.active, true);
  assert.equal(deriveMembership({}, now + 300001, 300000, { first: state }).active, false);
  const sealed = sealReceipt('private-token-receipt', 'secret', 'owner-a');
  assert.ok(!sealed.includes('private-token'));
  assert.equal(openReceipt(sealed, 'secret', 'owner-a'), 'private-token-receipt');
  assert.throws(() => openReceipt(sealed, 'secret', 'owner-b'));
  assert.throws(() => openReceipt(sealed, 'different-key', 'owner-a'));
});

function domainFixture() {
  let time = now, latest, queries = 0, outage = false;
  const store = new MemoryStore();
  const provider = { ready: true, verify: async receipt => {
    queries++; if (outage) throw Error('offline');
    const supplied = JSON.parse(receipt);
    return { ...latest, developerPayload: supplied.developerPayload };
  } };
  const d = new ValueAddedDomain({ store, purchaseProvider: provider, accountProvider: { ready: true },
    config: { ...config, clientId: 'client', identitySecret: 'i'.repeat(32), purchaseBindingSecret: 's'.repeat(32) },
    clock: () => time });
  return { d, store, now: () => time, queries: () => queries, advance: ms => { time += ms; },
    latest: value => { latest = value; }, outage: value => { outage = value; } };
}
test('cancelled renewal keeps the already-paid term across service restart and ends at the official expiry', async () => {
  const f = domainFixture(), user = 'a'.repeat(64);
  const { developerPayload } = await f.d.intent(user, { productId: 'monthly', productType: 2 });
  const receipt = JSON.stringify({ developerPayload });
  const cancelled = { ...fact({ renewalInfo: { ...current.renewalInfo, autoRenewStatusCode: '0' } }), developerPayload };
  f.latest(cancelled); await f.d.verify(user, { purchaseData: receipt, productType: 2 });
  const restarted = new ValueAddedDomain({ store: f.store, purchaseProvider: f.d.purchaseProvider,
    accountProvider: { ready: true }, config: f.d.config, clock: f.now });
  const beforeExpiry = await restarted.entitlement(user);
  assert.equal(beforeExpiry.active, true); assert.equal(beforeExpiry.autoRenew, false);
  assert.equal(beforeExpiry.expiresAt, current.expiresTime);
  f.advance(300001); assert.equal((await restarted.entitlement(user)).active, true);
  assert.equal((await f.store.list('subscriptions:' + user)).length, 1);
  f.advance(current.expiresTime - f.now());
  assert.equal((await restarted.entitlement(user)).active, false, 'no extra term is invented after cancellation');
});
test('renewal replaces one subscription, automatic recheck sees cancellations and refunds, failure cannot extend membership', async () => {
  const f = domainFixture(), user = 'a'.repeat(64);
  const { developerPayload } = await f.d.intent(user, { productId: 'monthly', productType: 2 });
  const receipt = JSON.stringify({ developerPayload });
  const initial = { ...fact({}), developerPayload };
  f.latest(initial);
  const verify = () => f.d.verify(user, { purchaseData: receipt, productType: 2 });
  await Promise.all([verify(), verify(), verify()]);
  assert.equal((await f.d.entitlement(user)).expiresAt, initial.expiresAt);
  const stored = await f.store.list('subscriptions:' + user);
  assert.equal(stored.length, 1); assert.ok(stored[0].receiptCipher); assert.ok(!JSON.stringify(stored).includes(developerPayload));
  f.advance(300001);
  f.latest({ ...initial, orderId: 'second-renewal', purchasedAt: f.now() - 1000, signedAt: f.now(),
    expiresAt: initial.expiresAt + 3600000, autoRenew: false });
  assert.equal((await f.d.entitlement(user)).expiresAt, initial.expiresAt + 3600000);
  assert.equal((await f.d.entitlement(user)).autoRenew, false);
  assert.equal((await f.store.list('subscriptions:' + user)).length, 1);
  f.advance(300001); f.outage(true);
  const offline = await f.d.entitlement(user);
  assert.equal(offline.active, false); assert.equal(offline.refreshRequired, true);
  f.outage(false); f.latest({ ...initial, status: 'revoked', subscriptionStatus: '5', signedAt: f.now() });
  assert.equal((await f.d.entitlement(user)).active, false);
  await assert.rejects(f.d.verify('b'.repeat(64), { purchaseData: receipt, productType: 2 }), /ORDER_BINDING_MISMATCH/);
});
test('late old snapshot cannot roll back newer refund or use a missing payload to claim another account', async () => {
  const f = domainFixture(), user = 'a'.repeat(64);
  const { developerPayload } = await f.d.intent(user, { productId: 'monthly', productType: 2 });
  const original = { ...fact({}), developerPayload };
  await f.d.recordSubscription(user, { ...original, status: 'revoked', subscriptionStatus: '5' }, 'original-receipt', now + 10);
  assert.equal((await f.d.recordSubscription(user, original, 'original-receipt', now)).status, 'SUPERSEDED');
  assert.equal((await f.d.recordSubscription(user, original, 'original-receipt', now + 20)).status, 'SUPERSEDED');
  assert.equal((await f.d.entitlement(user)).active, false);
  f.latest(original);
  await assert.rejects(f.d.verify('b'.repeat(64), { purchaseData: '{}', productType: 2 }), /ORDER_BINDING_MISMATCH/);
});

test('same-account repurchase rotates the signed binding without changing subscription ownership', async () => {
  const f=domainFixture(),user='a'.repeat(64);
  const first=await f.d.intent(user,{productId:'monthly',productType:2});
  const initial={...fact({}),developerPayload:first.developerPayload};
  f.latest(initial);await f.d.verify(user,{purchaseData:JSON.stringify(first),productType:2});
  f.advance(1000);
  const second=await f.d.intent(user,{productId:'monthly',productType:2});
  const next={...initial,orderId:'repurchase',purchasedAt:f.now(),signedAt:f.now(),developerPayload:second.developerPayload};
  f.latest(next);
  assert.equal((await f.d.verify(user,{purchaseData:JSON.stringify(second),productType:2})).status,'ALREADY_FINISHED');
  assert.equal((await f.d.entitlement(user)).active,true);
  assert.equal((await f.store.list('subscriptions:'+user)).length,1);
  assert.equal((await f.d.recordSubscription(user,initial,JSON.stringify(first),f.now())).status,'SUPERSEDED');
  assert.equal((await f.d.entitlement(user)).active,true);
  await assert.rejects(f.d.verify('b'.repeat(64),{purchaseData:JSON.stringify(second),productType:2}),/ORDER_BINDING_MISMATCH/);
  const forged=await f.d.intent('b'.repeat(64),{productId:'monthly',productType:2});
  await assert.rejects(f.d.recordSubscription(user,{...next,developerPayload:forged.developerPayload},'unused',f.now()),/ORDER_BINDING_MISMATCH/);
  const third=await f.d.intent(user,{productId:'monthly',productType:2});
  await assert.rejects(f.d.recordSubscription(user,{...next,developerPayload:third.developerPayload},'unused',f.now()),/ORDER_BINDING_MISMATCH/);
});
test('official latest order carries both signed bindings when a repurchase replaces the original payload', async () => {
  const f=domainFixture(),user='a'.repeat(64);
  const first=await f.d.intent(user,{productId:'monthly',productType:2});
  const second=await f.d.intent(user,{productId:'monthly',productType:2});
  const replacement=subscriptionFact({...group,lastSubscriptionStatus:{...current,lastPurchaseOrder:{...order,developerPayload:second.developerPayload}}},
    {...candidate,developerPayload:first.developerPayload},config,now);
  assert.equal(replacement.developerPayload,second.developerPayload);
  assert.equal(replacement.receiptDeveloperPayload,first.developerPayload);
  f.d.purchaseProvider.verify=async()=>replacement;
  assert.equal((await f.d.verify(user,{purchaseData:'signed-receipt-fixture',productType:2})).status,'ALREADY_FINISHED');
  const other=await f.d.intent('b'.repeat(64),{productId:'monthly',productType:2});
  f.d.purchaseProvider.verify=async()=>({...replacement,receiptDeveloperPayload:other.developerPayload});
  await assert.rejects(f.d.verify(user,{purchaseData:'signed-receipt-fixture',productType:2}),/ORDER_BINDING_MISMATCH/);
});
