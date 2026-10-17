import test from 'node:test';
import assert from 'node:assert/strict';
import { generateKeyPairSync, verify, constants, createHmac, randomUUID } from 'node:crypto';
import { ValueAddedDomain } from '../src/domain.mjs';
import { SUBSCRIPTION_POLICY } from '../src/membership-policy.mjs';
import { createRenewalTimer } from '../src/renewal-timer.mjs';
import { HuaweiPushSender } from '../src/huawei-push.mjs';
import { createCloudFunction } from '../src/cloud-function.mjs';
import { MemoryStore } from './memory-store.mjs';
import { createPushReceiptCallback } from '../src/push-receipts.mjs';
const DAY = 86400000, user = 'a'.repeat(64), installationId = '00000000-0000-4000-a000-000000000001';
async function fixture() {
  let now = 1800000000000, sends = [], queries = 0, fail = false, providerFail = false;
  const store = new MemoryStore();
  const sender = { ready: true, send: async (token, body) => {
    if (fail) throw Error('push offline'); sends.push({ token, ...body }); return { requestId: 'accepted' };
  } };
  const d = new ValueAddedDomain({ store, pushSender: sender, clock: () => now,
    accountProvider: { ready: true }, purchaseProvider: { ready: true, verify: async () => {
      queries++; if (providerFail) throw Error('provider offline'); return structuredClone(fact);
    } }, config: { appId: 'app', purchaseBindingSecret: 'secret', products: [{ id: 'auto', type: 2, kind: 'membership' }] } });
  const intent = await d.intent(user, { productId: 'auto', productType: 2 });
  const fact = { appId: 'app', productId: 'auto', productType: 2, quantity: 1, developerPayload: intent.developerPayload,
    orderId: 'order', status: 'purchased', subscriptionId: 'sub', subscriptionGroupId: 'group', generationId: 'generation',
    subscriptionStatus: '1', autoRenew: true, billingRetry: false, expiresAt: now + 7 * DAY,
    purchasedAt: now - 23 * DAY, signedAt: now, purchaseToken: 'token', policyVersion: SUBSCRIPTION_POLICY,
    nextRenewalAt: now + 7 * DAY, nextRenewalPrice: 500, renewalCurrency: 'CNY' };
  await d.verify(user, { purchaseData: 'protected receipt', productType: 2 });
  await d.reminders.run(); // First pass has no device; persisted retry is exercised later.
  await d.reminders.register(user, { installationId, token: 'push-token', enabled: true });
  now += 15 * 60000;
  return { d, store, sends, fact, advance: ms => { now += ms; }, queries: () => queries,
    fail: value => { fail = value; }, providerFail: value => { providerFail = value; } };
}
test('official recheck precedes sending; acceptance and actual opening are separate; cycles are durable and deduplicated', async () => {
  const f = await fixture(); const before = f.queries();
  await Promise.all([f.d.reminders.run(), f.d.reminders.run()]);
  assert.ok(f.queries() > before); assert.equal(f.sends.length, 1);
  const [receipt] = await f.store.list('renewal-deliveries:' + user);
  assert.equal(receipt.displayStatus, 'UNKNOWN'); assert.ok(receipt.acceptedAt);
  await f.d.reminders.opened('b'.repeat(64), { deliveryId: receipt.deliveryId, installationId });
  assert.equal((await f.store.get(receipt.id)).displayStatus, 'UNKNOWN');
  await f.d.reminders.opened(user, { deliveryId: receipt.deliveryId, installationId });
  assert.equal((await f.store.get(receipt.id)).displayStatus, 'OPENED');
  await f.d.reminders.run(); assert.equal(f.sends.length, 1);
  f.advance(31 * DAY); f.fact.expiresAt += 31 * DAY; f.fact.nextRenewalAt += 31 * DAY;
  f.fact.signedAt += 31 * DAY;
  await f.d.reminders.run(); assert.equal(f.sends.length, 2, 'sent jobs discover the next official cycle');
});
test('cancel/refund/revocation discovered immediately before dispatch suppresses notification', async () => {
  for (const status of ['cancelled', 'refunded', 'revoked']) {
    const f = await fixture(); f.fact.autoRenew = false;
    if (status !== 'cancelled') f.fact.status = status;
    await f.d.reminders.run(); assert.equal(f.sends.length, 0);
    const [job] = await f.store.list('renewal-jobs'); assert.equal(job.enabled, false);
  }
});
test('push/provider failures retry without asserting delivery; missing official plan is never fabricated', async () => {
  const f = await fixture(); f.fail(true);
  const result = await f.d.reminders.run(); assert.equal(result.failed, 1);
  assert.equal((await f.store.list('renewal-deliveries:' + user)).length, 0);
  f.fail(false); await f.d.reminders.run(); assert.equal(f.sends.length, 0);
  f.advance(15 * 60000); f.providerFail(true); await f.d.reminders.run(); assert.equal(f.sends.length, 0);
  f.advance(15 * 60000); f.providerFail(false); delete f.fact.nextRenewalPrice;
  await f.d.reminders.run(); assert.equal(f.sends.length, 0);
  f.advance(15 * 60000); f.fact.nextRenewalPrice = 500;
  await f.d.reminders.run(); assert.equal(f.sends.length, 1);
});
test('account switch/token rotation and permission revocation remove old-account delivery eligibility', async () => {
  const f = await fixture();
  await f.d.reminders.register('b'.repeat(64), { installationId, token: 'new-token', enabled: true });
  assert.equal(await f.d.reminders.available(user), false);
  await f.d.reminders.run(); assert.equal(f.sends.length, 0);
  await f.d.reminders.revoke('b'.repeat(64), { installationId });
  assert.equal(await f.d.reminders.available('b'.repeat(64)), false);
});
test('timer scans more than 256 jobs; public JSON transport cannot execute a timer', async () => {
  const f = await fixture();
  await f.store.transaction(async tx => {
    for (let n = 0; n < 300; n++) { const id = 'job-' + String(n).padStart(3, '0'); tx.put(id, { id }, 'renewal-jobs'); }
  });
  const seen = new Set(); f.d.reminders.process = async id => { seen.add(id); return false; };
  assert.equal((await f.d.reminders.run()).complete, true); assert.equal(seen.size, 301);
  const secret = 's'.repeat(32), timer = createRenewalTimer(f.d, secret);
  await assert.rejects(timer({ triggerType: 'HTTP', userData: secret }), /TIMER_ONLY/);
  await assert.rejects(timer({ triggerType: 'TIMER', userData: 'wrong' }), /TIMER_ONLY/);
  await timer({ triggerType: 'TIMER', userData: secret });
  const publicTransport = createCloudFunction(f.d);
  assert.equal((await publicTransport({ triggerType: 'TIMER', userData: secret })).status, 400);
});
test('new automatic purchases require a fresh timer heartbeat and registered device; recovery remains available', async () => {
  const f = await fixture(); f.d.config.enforceCheckoutReadiness = true; f.d.config.membershipReleaseReady = true;
  f.fact.autoRenew = false; f.fact.status = 'expired'; f.fact.expiresAt -= 8 * DAY;
  await f.d.verify(user, { purchaseData: 'protected receipt', productType: 2 });
  await f.d.intent(user, { productId: 'auto', productType: 2 });
  f.advance(3600001);
  await assert.rejects(f.d.intent(user, { productId: 'auto', productType: 2 }), /REMINDERS_UNAVAILABLE/);
});
test('explicit sandbox checkout remains separate from reminder readiness and never applies to NORMAL', async () => {
  const f = await fixture();
  f.d.config.enforceCheckoutReadiness=true; f.d.config.membershipReleaseReady=true;
  f.fact.autoRenew=false; f.fact.status='expired'; f.fact.expiresAt-=8*DAY;
  await f.d.verify(user,{purchaseData:'protected receipt',productType:2});
  f.d.reminders.sender.ready=false;
  f.d.purchaseProvider.sandboxReady=true; f.d.purchaseProvider.config={environment:'SANDBOX'};
  await assert.rejects(f.d.intent(user,{productId:'auto',productType:2}), /REMINDERS_UNAVAILABLE/);
  f.d.config.sandboxCheckout=true;
  assert.equal(f.d.capabilities().renewalReminders,false);
  assert.equal(f.d.capabilities().sandboxCheckout,true);
  await f.d.intent(user,{productId:'auto',productType:2});
  f.d.purchaseProvider.config.environment='NORMAL';
  assert.equal(f.d.capabilities().sandboxCheckout,undefined);
  await assert.rejects(f.d.intent(user,{productId:'auto',productType:2}), /REMINDERS_UNAVAILABLE/);
  f.d.purchaseProvider.config.environment='SANDBOX'; f.d.purchaseProvider.sandboxReady=false;
  await assert.rejects(f.d.intent(user,{productId:'auto',productType:2}), /REMINDERS_UNAVAILABLE/);
});
test('NEXT v3 PS256 JWT and subscription content are valid; HTTP success alone is rejected', async () => {
  const { privateKey, publicKey } = generateKeyPairSync('rsa', { modulusLength: 2048 });
  let code = '80000000', sent;
  const sender = new HuaweiPushSender({ projectId: '12345678', verified: true,
    credential: { project_id: '12345678', key_id: 'key', sub_account: 'service', private_key: privateKey.export({ type: 'pkcs8', format: 'pem' }) },
    fetcher: async (url, request) => { sent = { url, request }; return { ok: true, json: async () => ({ code, requestId: 'request' }) }; } });
  const body = { deliveryId: 'a'.repeat(64), renewalAt: Date.now() + 7 * DAY, amount: 500, currency: 'CNY', userId: user };
  assert.equal((await sender.send('device', body)).requestId, 'request');
  assert.match(sent.url, /\/v3\/12345678\/messages:send$/);
  const jwt = sent.request.headers.Authorization.slice(7), [head, payload, signature] = jwt.split('.');
  assert.equal(JSON.parse(Buffer.from(head, 'base64url')).alg, 'PS256');
  assert.equal(JSON.parse(Buffer.from(payload, 'base64url')).iss, 'service');
  assert.equal(verify('sha256', Buffer.from(head + '.' + payload), { key: publicKey,
    padding: constants.RSA_PKCS1_PSS_PADDING, saltLength: 32 }, Buffer.from(signature, 'base64url')), true);
  const message = JSON.parse(sent.request.body).payload.notification;
  assert.equal(message.category, 'ACCOUNT'); assert.equal(message.profileId, user);
  assert.match(message.body, /5.00/); assert.match(message.body, /延长一个月/);
  assert.equal(message.clickAction.data.deliveryId, body.deliveryId);
  code = '80300001'; await assert.rejects(sender.send('device', body), /PUSH_UNAVAILABLE/);
});

function receiptFixture(f) {
  const key = Buffer.from('callback-secret-material-32-bytes'), username = 'callback-user';
  const callback = createPushReceiptCallback(f.d, { username, secret: key.toString('base64'), packageName: 'com.test' });
  const event = (deliveryId, result = 0, requestId = 'accepted') => {
    const timestamp = String(f.d.clock()), nonce = randomUUID();
    const signature = createHmac('sha256', key).update(timestamp + nonce + username).digest('base64');
    return { headers: { 'X-HUAWEI-CALLBACK-ID': 'timestamp=' + timestamp + '; nonce=' + nonce + '; value=' + signature },
      body: JSON.stringify({ statuses: [{ token: 'push-token', pushType: 0, appPackageName: 'com.test', biTag: deliveryId,
        requestId, deliveryStatus: { result, timestamp: f.d.clock() } }] }) };
  };
  return { callback, event };
}
test('V2 device receipt requires Huawei HMAC, rejects replay/tampering, and records delivery separately from display', async () => {
  const f = await fixture(); await f.d.reminders.run();
  const { callback, event } = receiptFixture(f);
  const delivery = (await f.store.list('renewal-deliveries:' + user))[0], request = event(delivery.deliveryId);
  await assert.rejects(callback({ ...request, headers: {} }), /INVALID_PUSH_RECEIPT/);
  const tampered = structuredClone(request); tampered.headers['X-HUAWEI-CALLBACK-ID'] = tampered.headers['X-HUAWEI-CALLBACK-ID'].replace('value=', 'value=x');
  await assert.rejects(callback(tampered), /INVALID_PUSH_RECEIPT/);
  assert.equal((await callback(request)).code, '0'); await callback(request);
  const updated = await f.store.get(delivery.id);
  assert.equal(updated.providerDeliveryResult, 0); assert.ok(updated.deliveredAt);
  assert.equal(updated.displayStatus, 'UNKNOWN', 'delivery is not proof of rendered UI');
  f.advance(300001); await assert.rejects(callback(request), /INVALID_PUSH_RECEIPT/);
});
test('transient device failure schedules retry; notification-disabled receipt disables further device sends', async () => {
  const f = await fixture(); await f.d.reminders.run();
  const { callback, event } = receiptFixture(f), delivery = f.sends[0].deliveryId;
  await callback(event(delivery, 14)); await f.d.reminders.run(); assert.equal(f.sends.length, 1);
  f.advance(900000); await f.d.reminders.run(); assert.equal(f.sends.length, 2);
  await callback(event(delivery, 6)); assert.equal(await f.d.reminders.available(user), false);
});
test('receipt arriving before acceptance is persisted is merged by matching request ID', async () => {
  const f = await fixture(), { callback, event } = receiptFixture(f);
  f.d.reminders.sender.send = async (_, value) => { await callback(event(value.deliveryId, 0, 'early')); return { requestId: 'early' }; };
  await f.d.reminders.run();
  const [delivery] = await f.store.list('renewal-deliveries:' + user);
  assert.equal(delivery.providerDeliveryResult, 0); assert.ok(delivery.deliveredAt);
});
