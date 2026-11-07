import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash, generateKeyPairSync } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { deriveMembership, LIFETIME_EXPIRES_AT, LIFETIME_POLICY, MEMBERSHIP_POLICY, SUBSCRIPTION_POLICY } from '../src/membership-policy.mjs';
import { HuaweiPurchaseProvider } from '../src/huawei-providers.mjs';
import { ValueAddedDomain } from '../src/domain.mjs';
import { membershipProductsVerified } from '../src/main.mjs';
import { checkMembershipRelease } from '../scripts/check-membership-release.mjs';
import { MemoryStore } from './memory-store.mjs';

const now = Date.parse('2026-11-07T00:00:00+08:00');
const lifetime = { id: 'VIP', type: 1, kind: 'membership', purchasesEnabled: true,
  policyVersion: LIFETIME_POLICY, prices: { CNY: [3800] },
  promotionalPrices: { time_limit: { CNY: [2800] } } };
const owner = 'a'.repeat(64);
const orderKey = orderId => 'order:' + createHash('sha256').update('app\n' + orderId).digest('hex');
function fixture() {
  let time = now;
  const store = new MemoryStore();
  const provider = { ready: true, verify: async receipt => JSON.parse(receipt) };
  const config = { appId: 'app', products: [lifetime], purchaseBindingSecret: 'x'.repeat(32) };
  const makeDomain = () => new ValueAddedDomain({ store, accountProvider: { ready: true }, purchaseProvider: provider,
    config, clock: () => time });
  return { store, provider, domain: makeDomain(), makeDomain, advance: ms => { time += ms; } };
}
async function purchase(domain, orderId = 'lifetime-order') {
  const intent = await domain.intent(owner, { productId: 'VIP', productType: 1 });
  return { appId: 'app', productId: 'VIP', productType: 1, orderId, purchaseToken: 'synthetic-token',
    developerPayload: intent.developerPayload, quantity: 1, status: 'purchased', needsFinish: true,
    purchasedAt: now - 1000, expiresAt: LIFETIME_EXPIRES_AT };
}
const verify = (domain, fact) => domain.verify(owner, { purchaseData: JSON.stringify(fact), productType: fact.productType });

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

test('lifetime offer is signed for the configured nonconsumable product', () => {
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

test('lifetime type1 purchase intent accepts its configured promotion and rejects type3', async () => {
  let signs = 0;
  const domain = new ValueAddedDomain({ store: new MemoryStore(), accountProvider: { ready: true },
    purchaseProvider: { ready: true, signPromotion: (id, offer, user) => {
      signs++;
      assert.equal(id, 'VIP');
      assert.equal(user, owner);
      return { promotionalOfferId: offer, applicationUserName: user, jwsRepresentation: 'opaque' };
    } },
    config: { products: [lifetime], purchaseBindingSecret: 'x'.repeat(32) }, clock: () => now });
  const intent = await domain.intent(owner, { productId: 'VIP', productType: 1, promotionalOfferId: 'time_limit' });
  assert.equal(intent.promotionalPurchase.promotionalOfferId, 'time_limit');
  assert.equal(domain.verifyBinding(intent.developerPayload, owner, 'VIP'), true);
  assert.equal(signs, 1);
  for (const promotionalOfferId of ['unknown', '__proto__']) {
    await assert.rejects(domain.intent(owner, { productId: 'VIP', productType: 1, promotionalOfferId }), /UNKNOWN_OFFER/);
  }
  await assert.rejects(domain.intent(owner, { productId: 'VIP', productType: 3, promotionalOfferId: 'time_limit' }), /UNKNOWN_PRODUCT/);
  assert.equal((await domain.intent(owner, { productId: 'VIP', productType: 1 })).promotionalPurchase, undefined);
  assert.equal(signs, 1);
});

test('MemoryStore type1 purchase persists one permanent entitlement across duplicates and a new domain instance', async () => {
  const { domain, store, makeDomain, advance, provider } = fixture();
  const fact = await purchase(domain);
  const results = await Promise.all(Array.from({ length: 8 }, () => verify(domain, fact)));
  assert.ok(results.every(result => result.status === 'GRANTED' && result.finishAllowed && result.productType === 1));
  assert.equal((await store.list('orders:' + owner)).length, 1);
  const stored = await store.get(orderKey(fact.orderId));
  assert.equal(stored.productType, 1);
  assert.equal(stored.policyVersion, LIFETIME_POLICY);
  assert.equal(stored.expiresAt, LIFETIME_EXPIRES_AT);
  const persisted = await store.get('entitlement:' + owner);
  assert.equal(Object.keys(persisted.orders).length, 1);
  assert.equal(persisted.orders[orderKey(fact.orderId)].policyVersion, LIFETIME_POLICY);
  assert.deepEqual(await verify(domain, { ...fact, needsFinish: false }), { status: 'ALREADY_FINISHED', finishAllowed: false });

  advance(100 * 366 * 86400000);
  provider.verify = async () => { throw Error('LIFETIME_MUST_NOT_REQUERY'); };
  const restored = await makeDomain().entitlement(owner);
  assert.equal(restored.active, true);
  assert.equal(restored.lifetime, true);
  assert.equal(restored.expiresAt, LIFETIME_EXPIRES_AT);
  assert.equal(restored.refreshRequired, false);
});

test('MemoryStore lifetime refund/revocation cannot be resurrected by a purchased replay', async () => {
  for (const status of ['refunded', 'revoked']) {
    const { domain, store } = fixture();
    const fact = await purchase(domain);
    await verify(domain, fact);
    assert.deepEqual(await verify(domain, { ...fact, status }), { status: status.toUpperCase(), finishAllowed: false });
    assert.equal((await domain.entitlement(owner)).active, false);
    const snapshot = structuredClone(store.rows);
    await assert.rejects(verify(domain, fact), /ORDER_REVOKED/);
    assert.deepEqual(store.rows, snapshot);
    assert.equal((await domain.entitlement(owner)).active, false);
  }
});

test('type3 VIP facts are rejected and an existing same-orderId type3 row is never migrated to type1', async () => {
  const { domain, store } = fixture();
  const fact = await purchase(domain);
  await assert.rejects(verify(domain, { ...fact, productType: 3 }), /UNKNOWN_PRODUCT/);
  assert.equal(store.rows.size, 0);
  const key = orderKey(fact.orderId);
  const bindingHash = createHash('sha256').update(fact.developerPayload).digest('hex');
  const historical = { userId: owner, appId: fact.appId, productId: fact.productId, productType: 3,
    bindingHash, orderId: fact.orderId, status: 'purchased', purchasedAt: fact.purchasedAt,
    expiresAt: LIFETIME_EXPIRES_AT, policyVersion: LIFETIME_POLICY };
  await store.transaction(async tx => {
    tx.put(key, historical, 'orders:' + owner);
    tx.put('entitlement:' + owner, { orders: { [key]: { orderId: fact.orderId, status: 'purchased',
      purchasedAt: fact.purchasedAt, policyVersion: LIFETIME_POLICY, verifiedAt: now } } }, 'entitlements');
  });
  const snapshot = structuredClone(store.rows);
  await assert.rejects(verify(domain, fact), /ORDER_OWNER_IMMUTABLE/);
  assert.deepEqual(store.rows, snapshot);
  assert.equal((await store.get(key)).productType, 3);
});

test('release readiness requires the verified type1/lifetime SKU and never accepts crossed policies', () => {
  const release = { membershipProductId: 'vip_monthly_auto', lifetimeProductId: 'VIP', oneTimeProductId: 'VIP' };
  assert.equal(membershipProductsVerified([lifetime], release), true);
  assert.equal(membershipProductsVerified([lifetime], {}), false);
  assert.equal(membershipProductsVerified([lifetime], { ...release, lifetimeProductId: 'other' }), false);
  for (const invalid of [{ ...lifetime, type: 3 }, { ...lifetime, policyVersion: MEMBERSHIP_POLICY },
    { ...lifetime, policyVersion: SUBSCRIPTION_POLICY }, { ...lifetime, policyVersion: undefined }]) {
    assert.equal(membershipProductsVerified([invalid], release), false);
  }
});

function releaseFixture() {
  return {
    config: { accountClientId: 'synthetic-app', membershipOperatorName: 'Synthetic Operator',
      membershipOneTimeProductId: 'vip_monthly_once', membershipOneTimePurchases: false,
      membershipLifetimeProductId: 'VIP', membershipLifetimePurchases: true,
      membershipProductId: 'vip_monthly_auto', membershipSubscriptionGroupId: 'synthetic-members' },
    products: [{ id: 'vip_monthly_once', type: 3, kind: 'membership', purchasesEnabled: false,
      policyVersion: MEMBERSHIP_POLICY, prices: { CNY: [500] } }, lifetime,
    { id: 'vip_monthly_auto', type: 2, kind: 'membership', policyVersion: SUBSCRIPTION_POLICY,
      subscriptionGroupId: 'synthetic-members', prices: { CNY: [500] } }],
    release: { operatorName: 'Synthetic Operator', appId: 'synthetic-app', environment: 'NORMAL',
      lifetimeProductId: 'VIP', membershipProductId: 'vip_monthly_auto', normalPaymentVerified: true,
      backupAccepted: true, supportVerified: true, cancelInAppVerified: true, agreementsReadVerified: true, testedAt: now - 1000 },
    reminder: { appId: 'synthetic-app', projectId: 'synthetic-project', notificationCategory: 'ACCOUNT',
      timerActive: true, deviceDisplayed: true, deepLinkOpened: true, accountCategoryEligible: true,
      receiptVerified: true, paginationVerified: true, advanceNoticeVerified: true, cancelStopsVerified: true,
      permissionClosedVerified: true, dedupVerified: true, retryAlarmVerified: true, testedAt: now - 1000 },
    storage: { paginationVerified: true, rows: 300, fingerprint: 'synthetic-test-only', testedAt: now - 1000 }, now
  };
}

test('release checker accepts only type1 lifetime with complete synthetic evidence and fails closed otherwise', () => {
  const valid = releaseFixture();
  assert.deepEqual(checkMembershipRelease(valid), []);
  for (const field of ['release', 'reminder', 'storage']) {
    assert.ok(checkMembershipRelease({ ...valid, [field]: {} }).length > 0, field + ' evidence cannot be absent');
  }
  assert.ok(checkMembershipRelease({ ...valid, release: { ...valid.release, lifetimeProductId: undefined } })
    .includes('NORMAL lifetime SKU not verified'));
  for (const patch of [{ type: 3 }, { policyVersion: MEMBERSHIP_POLICY }, { policyVersion: SUBSCRIPTION_POLICY }, { purchasesEnabled: false }]) {
    const products = valid.products.map(p => p.id === 'VIP' ? { ...p, ...patch } : p);
    assert.ok(checkMembershipRelease({ ...valid, products }).includes('Lifetime VIP product missing or not enabled'));
  }
  const wrongSubscription = valid.products.map(p => p.type === 2 ? { ...p, policyVersion: LIFETIME_POLICY } : p);
  assert.ok(checkMembershipRelease({ ...valid, products: wrongSubscription }).includes('Monthly subscription group/price mismatch'));
  assert.ok(checkMembershipRelease({ ...valid, release: { ...valid.release, environment: 'SANDBOX' } }).length > 0);
  assert.ok(checkMembershipRelease({ ...valid, storage: { ...valid.storage, testedAt: now + 1 } }).length > 0);
});

test('release CLI without verification records exits nonzero and never writes real evidence', () => {
  const script = fileURLToPath(new URL('../scripts/check-membership-release.mjs', import.meta.url));
  const catalog = new URL('../config/product-catalog.json', import.meta.url);
  const before = readFileSync(catalog);
  const result = spawnSync(process.execPath, [script], { encoding: 'utf8', timeout: 10000, maxBuffer: 65536 });
  assert.equal(result.error, undefined);
  assert.equal(result.status, 1);
  for (const message of ['NORMAL release evidence missing', 'Push/timer/device evidence missing', 'CloudDB pagination evidence missing']) {
    assert.ok(result.stderr.includes(message), message);
  }
  assert.ok(!result.stdout.includes('checks PASS'));
  assert.deepEqual(readFileSync(catalog), before);
});

test('package evidence whitelist accepts lifetimeProductId without relaxing required flags or unknown-field rejection', () => {
  const script = fileURLToPath(new URL('../scripts/package-cloud-function.py', import.meta.url));
  // Compile only the existing whitelist assignment and rejection guard, never the
  // packager entry point: no real evidence, dependencies, output directory or ZIP.
  const contract = String.raw`
import ast, json, sys
from pathlib import Path
tree = ast.parse(Path(sys.argv[1]).read_text())
loop = next(node for node in ast.walk(tree) if isinstance(node, ast.For)
    and isinstance(node.target, ast.Tuple)
    and [getattr(part, 'id', None) for part in node.target.elts] == ['path', 'name', 'fields'])
fields = ast.literal_eval(loop.iter.elts[0].elts[2])
branch = next(node for node in loop.body if isinstance(node, ast.If))
index = next(i for i, node in enumerate(branch.body) if isinstance(node, ast.Assign)
    and any(isinstance(target, ast.Name) and target.id == 'allowed' for target in node.targets))
assert isinstance(branch.body[index + 1], ast.If)
validation = compile(ast.Module(body=branch.body[index:index + 2], type_ignores=[]), sys.argv[1], 'exec')
report = json.load(sys.stdin)
def accepts(value):
    try:
        exec(validation, {'report': value, 'fields': fields, 'name': 'membership-release.json'})
        return True
    except SystemExit:
        return False
assert report['lifetimeProductId'] == 'VIP'
assert accepts(report), 'valid lifetime evidence rejected'
assert accepts({key: value for key, value in report.items() if key != 'lifetimeProductId'}), 'legacy evidence rejected'
assert not accepts(dict(report, unexpectedField='not-allowed')), 'unknown field accepted'
for field in fields:
    assert not accepts(dict(report, **{field: False})), 'false required flag accepted: ' + field
    assert not accepts({key: value for key, value in report.items() if key != field}), 'missing required flag accepted: ' + field
print('PASS: lifetime packaging evidence whitelist and fail-closed guards; no archive created')
`;
  const result = spawnSync('python3', ['-I', '-c', contract, script], {
    input: JSON.stringify(releaseFixture().release), encoding: 'utf8', timeout: 10000, maxBuffer: 65536
  });
  assert.equal(result.error, undefined);
  assert.equal(result.status, 0, result.stderr);
  assert.match(result.stdout, /PASS: lifetime packaging evidence whitelist/);
});
