import test from 'node:test';
import assert from 'node:assert/strict';
import { addCalendarMonth } from '../src/membership-policy.mjs';
import { ValueAddedDomain } from '../src/domain.mjs';
import { MemoryStore } from './memory-store.mjs';
const make = () => {
  let now = 1800000000000;
  const store = new MemoryStore();
  const provider = { ready: true, verify: async receipt => JSON.parse(receipt) };
  const d = new ValueAddedDomain({ store, accountProvider: { ready: true,
    exchange: async code => ({ subject: code, audience: 'client' }) }, purchaseProvider: provider,
    config: { clientId: 'client', appId: 'app', identitySecret: 'test-identity'.repeat(4),
      purchaseBindingSecret: 'test-binding'.repeat(4),
      products: [{ id: 'month', type: 3, kind: 'membership' }, { id: 'coffee', type: 0, kind: 'coffee' }] },
    clock: () => now });
  return { d, store, provider, now: () => now, advance: ms => { now += ms; } };
};
const login = async (d, code = 'alice') => { const a = await d.begin(); return d.exchange({ ...a, code, clientId: 'client' }); };
async function receipt(d, userId, now, productId = 'month', orderId = 'order') {
  const productType = productId === 'month' ? 3 : 0;
  const binding = await d.intent(userId, { productId, productType });
  return { appId: 'app', productId, productType, orderId, purchaseToken: 'provider-token',
    developerPayload: binding.developerPayload, quantity: 1, status: 'purchased', needsFinish: true,
    purchasedAt: now - 1000, expiresAt: addCalendarMonth(now - 1000) };
}
const verify = (d, id, fact) => d.verify(id, { purchaseData: JSON.stringify(fact), productType: fact.productType });
const profile = { id: 'custom', name: 'Custom', schemaVersion: 6, cursorSpeed: 1, overlayOpacity: 80,
  gamepadMappings: [], elements: [] };

test('one-use authorization, expiry, logout and cross-account session isolation', async () => {
  const { d, advance } = make(); const attempt = await d.begin();
  const session = await d.exchange({ ...attempt, code: 'alice', clientId: 'client' });
  await assert.rejects(d.exchange({ ...attempt, code: 'bob', clientId: 'client' }), /AUTH_STATE_MISMATCH/);
  assert.equal(await d.authenticate(session.token), session.userId);
  await d.logout(session.token); await assert.rejects(d.authenticate(session.token), /SIGN_IN_REQUIRED/);
  const other = await login(d, 'bob'); assert.notEqual(other.userId, session.userId);
  const expired = await d.begin(); advance(300001);
  await assert.rejects(d.exchange({ ...expired, code: 'bob', clientId: 'client' }), /AUTH_STATE_MISMATCH/);
  advance(1800000); await assert.rejects(d.authenticate(other.token), /SIGN_IN_REQUIRED/);
});
test('state mismatch does not consume someone else’s valid attempt', async () => {
  const { d } = make(); const a = await d.begin();
  await assert.rejects(d.exchange({ ...a, state: 'wrong', code: 'alice', clientId: 'client' }), /AUTH_STATE_MISMATCH/);
  assert.ok((await d.exchange({ ...a, code: 'alice', clientId: 'client' })).userId);
});
test('concurrent duplicate verification grants one fact, never adds membership duration', async () => {
  const { d, store, now } = make(); const { userId } = await login(d);
  const fact = await receipt(d, userId, now());
  const results = await Promise.all(Array.from({ length: 12 }, () => verify(d, userId, fact)));
  assert.ok(results.every(x => x.finishAllowed));
  assert.equal((await store.list('orders:' + userId)).length, 1);
  assert.equal(Object.keys((await store.get('entitlement:' + userId)).orders).length, 1);
  assert.equal((await d.entitlement(userId)).expiresAt, fact.expiresAt);
  const bob = await login(d, 'bob');
  await assert.rejects(verify(d, bob.userId, fact), /ORDER_BINDING_MISMATCH/);
});
test('coffee never grants membership; wrong application, SKU and quantity rejected', async () => {
  const { d, now } = make(); const { userId } = await login(d); const fact = await receipt(d,userId,now(),'coffee');
  assert.equal((await verify(d,userId,fact)).finishAllowed, true);
  assert.equal((await d.entitlement(userId)).active, false);
  await assert.rejects(verify(d,userId,{...fact,appId:'wrong'}),/ORDER_BINDING_MISMATCH/);
  await assert.rejects(verify(d,userId,{...fact,productId:'arbitrary'}),/UNKNOWN_PRODUCT/);
  await assert.rejects(verify(d,userId,{...fact,quantity:2}),/ORDER_BINDING_MISMATCH/);
});
test('refund, stale verification, expiry and pending orders never unlock', async () => {
  const { d, now, advance } = make(); const { userId } = await login(d); const fact = await receipt(d,userId,now());
  assert.equal((await verify(d,userId,{...fact,status:'pending'})).finishAllowed,false);
  assert.equal((await d.entitlement(userId)).active,false);
  await verify(d,userId,fact); assert.equal((await d.entitlement(userId)).active,true);
  advance(300001); assert.equal((await d.entitlement(userId)).active,false);
  await verify(d,userId,fact); assert.equal((await d.entitlement(userId)).active,true);
  await verify(d,userId,{...fact,status:'refunded'}); assert.equal((await d.entitlement(userId)).active,false);
  await assert.rejects(verify(d,userId,{...fact,status:'expired'}),/ORDER_REVOKED/);
  await assert.rejects(verify(d,userId,fact),/ORDER_REVOKED/);
  assert.equal((await d.entitlement(userId)).active,false);
});
test('same-base profile race keeps winner; stale write gets remote branch', async () => {
  const { d, now } = make(); const { userId } = await login(d); await verify(d,userId,await receipt(d,userId,now()));
  const first = await d.putProfile(userId,'tablet','custom',{baseRevision:0,payload:profile});
  assert.equal(first.current.revision,1);
  const results = await Promise.all(['Local A','Local B'].map(name => d.putProfile(userId,'tablet','custom',
    {baseRevision:1,payload:{...profile,name}})));
  assert.deepEqual(results.map(x=>x.status).sort(),['CONFLICT','SAVED']);
  assert.deepEqual(results[0].current, results[1].current);
  assert.equal((await d.putProfile(userId,'desktop','custom',{baseRevision:0,payload:profile})).current.revision,1);
  const bob = await login(d,'bob'); await assert.rejects(d.getProfile(bob.userId,'tablet','custom'),/MEMBERSHIP_REQUIRED/);
  for(const field of ['token','elementsPhone','elementsTablet','elementsDesktop','absolutePath']) {
    await assert.rejects(d.putProfile(userId,'tablet','custom',{baseRevision:2,payload:{...profile,[field]:'secret'}}),/INVALID_PROFILE/);
  }
});
test('transaction error has no partial order/entitlement writes; unverified store fails closed',async()=>{
  const {d,store}=make();
  await assert.rejects(store.transaction(async tx=>{tx.put('order:x',{},'orders');throw Error('interrupted');}),/interrupted/);
  assert.equal(await store.get('order:x'),null);
  store.ready=false; assert.equal(d.capabilities().commerce,false); await assert.rejects(d.begin(),/NOT_CONFIGURED/);
});
test('the 257th distinct profile is rejected atomically while an existing profile remains writable', async () => {
  const {d,now}=make(); const {userId}=await login(d); await verify(d,userId,await receipt(d,userId,now()));
  for(let i=0;i<256;i++) await d.putProfile(userId,'tablet','p'+i,{baseRevision:0,payload:{...profile,id:'p'+i}});
  await assert.rejects(d.putProfile(userId,'tablet','p256',{baseRevision:0,payload:{...profile,id:'p256'}}),/PROFILE_LIMIT/);
  assert.equal((await d.listProfiles(userId,'tablet')).profiles.length,256);
  assert.equal((await d.putProfile(userId,'tablet','p0',{baseRevision:1,payload:{...profile,id:'p0'}})).status,'SAVED');
});
test('already finished store orders restore entitlement without requesting another finish',async()=>{
  const {d,now}=make(); const {userId}=await login(d); const fact=await receipt(d,userId,now());
  const result=await verify(d,userId,{...fact,needsFinish:false});
  assert.equal(result.finishAllowed,false); assert.equal(result.status,'ALREADY_FINISHED');
  assert.equal((await d.entitlement(userId)).active,true);
});
test('retiring a product rejects new intents but still verifies and restores its historical orders', async () => {
  const {d,now}=make(); const {userId}=await login(d);
  const fact=await receipt(d,userId,now());
  d.config.products.find(p=>p.id==='month').purchasesEnabled=false;
  await assert.rejects(d.intent(userId,{productId:'month',productType:3}), /PRODUCT_NOT_FOR_SALE/);
  await verify(d,userId,{...fact,needsFinish:false});
  assert.equal((await d.entitlement(userId)).active,true);
  assert.equal((await d.entitlement(userId)).expiresAt,fact.expiresAt);
});


const preferences = {schemaVersion:1,themeMode:'system',uiSkin:'classic',consolePalette:'violet',
  consoleCardLayout:'focus3d',viewMode:'grid',useGlassEffect:true,desktopInputProfileId:'default'};
test('preferences CAS race, payload digest and account/device isolation',async()=>{
  const {d,now}=make();const {userId}=await login(d);await verify(d,userId,await receipt(d,userId,now()));
  assert.equal((await d.getPreferences(userId,'tablet')).current,null);
  const first=await d.putPreferences(userId,'tablet',{baseRevision:0,payload:preferences});
  assert.equal(first.current.revision,1);
  const {createHash}=await import('node:crypto');
  assert.equal(first.current.payloadSha256,createHash('sha256').update(JSON.stringify(first.current.payload)).digest('hex'));
  const race=await Promise.all(['light','dark'].map(themeMode=>d.putPreferences(userId,'tablet',{baseRevision:1,payload:{...preferences,themeMode}})));
  assert.deepEqual(race.map(x=>x.status).sort(),['CONFLICT','SAVED']);assert.deepEqual(race[0].current,race[1].current);
  assert.equal((await d.getPreferences(userId,'desktop')).current,null);
  const bob=await login(d,'bob');await verify(d,bob.userId,await receipt(d,bob.userId,now(),'month','bob-order'));
  assert.equal((await d.getPreferences(bob.userId,'tablet')).current,null);
  await d.putPreferences(bob.userId,'tablet',{baseRevision:0,payload:{...preferences,themeMode:'dark'}});
  assert.equal((await d.getPreferences(userId,'tablet')).current.revision,2);
});
test('settings allowlist excludes paths, runtime knobs, account data and per-game defaults',async()=>{
  const {d,now,advance}=make();const {userId}=await login(d);await verify(d,userId,await receipt(d,userId,now()));
  for(const field of ['token','absolutePath','winePrefix','steamPath','fexOptions','screenResolution'])
    await assert.rejects(d.putPreferences(userId,'tablet',{baseRevision:0,payload:{...preferences,[field]:'private'}}),/INVALID_PREFERENCES/);
  for(const bad of [{themeMode:'invalid'},{useGlassEffect:'true'},{desktopInputProfileId:'app-local-id'},{schemaVersion:2},{desktopInputProfileId:'../x'}])
    await assert.rejects(d.putPreferences(userId,'tablet',{baseRevision:0,payload:{...preferences,...bad}}),/INVALID_PREFERENCES/);
  await assert.rejects(d.putPreferences(userId,'phone',{baseRevision:0,payload:preferences}),/INVALID_PREFERENCES/);
  await assert.rejects(d.putPreferences(userId,'tablet',{baseRevision:-1,payload:preferences}),/INVALID_PREFERENCES/);
  assert.equal((await d.getPreferences(userId,'tablet')).current,null);
  advance(300001);await assert.rejects(d.getPreferences(userId,'tablet'),/MEMBERSHIP_REQUIRED/);
});

test('sandbox checkout accepts only verified facts and keeps paid backup gates closed',async()=>{
  const {d,provider,now}=make();provider.ready=false;provider.sandboxReady=true;provider.config={environment:'SANDBOX'};
  const {userId}=await login(d);assert.equal(d.capabilities().commerce,true);assert.equal(d.capabilities().sandboxCommerce,true);
  assert.equal(d.capabilities().profiles,false);assert.equal(d.capabilities().preferences,false);
  assert.equal((await d.entitlement(userId)).active,false);
  const fact=await receipt(d,userId,now());await verify(d,userId,fact);assert.equal((await d.entitlement(userId)).active,true);
  await assert.rejects(d.getPreferences(userId,'tablet'),/NOT_CONFIGURED/);
  provider.config.environment='NORMAL';assert.equal(d.capabilities().commerce,false);
  await assert.rejects(d.intent(userId,{productId:'month',productType:3}),/NOT_CONFIGURED/);
});
