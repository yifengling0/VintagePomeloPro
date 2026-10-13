import test from 'node:test';
import assert from 'node:assert/strict';
import { generateKeyPairSync, verify } from 'node:crypto';
import { HuaweiPurchaseProvider } from '../src/huawei-providers.mjs';
import { ValueAddedDomain } from '../src/domain.mjs';
import { MemoryStore } from './memory-store.mjs';
import { subscriptionFact } from '../src/subscription.mjs';

const product={id:'monthly',type:2,kind:'membership',subscriptionGroupId:'members',policyVersion:'huawei-subscription-v1',
  prices:{CNY:[500]},promotionalPrices:{vip_2026_year_end:{CNY:[200]}}};
test('saved custom offer is signed with official claims, owner, nonce, five-minute expiry and ES256', () => {
  const pair=generateKeyPairSync('ec',{namedCurve:'prime256v1'});
  const provider=new HuaweiPurchaseProvider({appId:'app',issuerId:'issuer',keyId:'kid',products:[product],
    privateKeyPem:pair.privateKey.export({type:'pkcs8',format:'pem'})});provider.ready=true;
  const owner='a'.repeat(64),now=1791850000000;
  const signed=provider.signPromotion(product.id,'vip_2026_year_end',owner,now),parts=signed.jwsRepresentation.split('.');
  const header=JSON.parse(Buffer.from(parts[0],'base64url')),payload=JSON.parse(Buffer.from(parts[1],'base64url')),data=JSON.parse(payload.data);
  assert.deepEqual(header,{alg:'ES256',typ:'JWT',kid:'kid'});assert.equal(payload.aud,'iap-v1');assert.equal(payload.aid,'app');
  assert.equal(payload.iat,Math.floor(now/1000));assert.equal(payload.exp-payload.iat,300);
  assert.deepEqual(data.productItem,{applicationId:'app',productId:'monthly'});
  assert.equal(data.offerInfo.offerId,'vip_2026_year_end');assert.equal(data.offerInfo.applicationUserName,owner);
  assert.ok(verify('sha256',Buffer.from(parts[0]+'.'+parts[1]),{key:pair.publicKey,dsaEncoding:'ieee-p1363'},Buffer.from(parts[2],'base64url')));
  const second=JSON.parse(JSON.parse(Buffer.from(provider.signPromotion(product.id,'vip_2026_year_end',owner,now).jwsRepresentation.split('.')[1],'base64url')).data);
  assert.notEqual(second.offerInfo.nonce,data.offerInfo.nonce);
  for(const bad of ['unknown','__proto__'])assert.throws(()=>provider.signPromotion(product.id,bad,owner,now),/UNKNOWN_OFFER/);
  provider.ready=false;assert.throws(()=>provider.signPromotion(product.id,'vip_2026_year_end',owner,now),/NOT_CONFIGURED/);
});
test('only configured offers can enter purchase intent; standard subscription and coffee retain their original path', async () => {
  let signs=0;
  const domain=new ValueAddedDomain({store:new MemoryStore(),accountProvider:{ready:true},purchaseProvider:{ready:true,
    signPromotion:(id,offer,user,now)=>{signs++;return {promotionalOfferId:offer,applicationUserName:user,jwsRepresentation:'opaque'};}},
    config:{products:[product,{id:'coffee',type:0,kind:'coffee'}],purchaseBindingSecret:'x'.repeat(32)},clock:()=>1791850000000});
  const user='a'.repeat(64);
  for(const offer of ['unknown','__proto__',''])await assert.rejects(domain.intent(user,
    {productId:'monthly',productType:2,promotionalOfferId:offer}),/UNKNOWN_OFFER/);
  await assert.rejects(domain.intent(user,{productId:'coffee',productType:0,promotionalOfferId:'vip_2026_year_end'}),/UNKNOWN_OFFER/);
  const signed=await domain.intent(user,{productId:'monthly',productType:2,promotionalOfferId:'vip_2026_year_end'});
  assert.equal(signed.promotionalPurchase.promotionalOfferId,'vip_2026_year_end');assert.equal(signs,1);
  assert.equal((await domain.intent(user,{productId:'monthly',productType:2})).promotionalPurchase,undefined);
  assert.equal((await domain.intent(user,{productId:'coffee',productType:0})).promotionalPurchase,undefined);
});
test('known ¥2 discount and later ¥5 renewal keep official expiry; unknown discount cannot grant membership', () => {
  const now=Date.parse('2027-02-01T00:00:00+08:00');
  const candidate={productId:'monthly',subscriptionId:'sub',purchaseToken:'token',subGroupId:'members',subGroupGenerationId:'gen',developerPayload:'bound'};
  const order={...candidate,applicationId:'app',environment:'SANDBOX',productType:'2',purchaseOrderId:'paid',duration:'P1M',
    purchaseTime:Date.parse('2026-12-20T00:00:00+08:00'),signedTime:now,finishStatus:'1',currency:'CNY',price:200,
    offerTypeCode:'2',offerId:'vip_2026_year_end'};
  const groupFor=lastPurchaseOrder=>({applicationId:'app',packageName:'com.test',environment:'SANDBOX',subGroupId:'members',
    lastSubscriptionStatus:{...candidate,status:'1',expiresTime:now+3600000,lastPurchaseOrder,
      renewalInfo:{productId:'monthly',environment:'SANDBOX',subGroupGenerationId:'gen',autoRenewStatusCode:'1',hasInBillingRetryPeriod:false}}});
  const config={appId:'app',packageName:'com.test',environment:'SANDBOX',products:[product]};
  for(const paid of [order,{...order,price:500,offerId:undefined,offerTypeCode:undefined}])
    assert.equal(subscriptionFact(groupFor(paid),candidate,config,now).expiresAt,now+3600000);
  for(const bad of [{price:199},{offerId:'unknown'},{offerId:'__proto__'},{offerId:undefined},{offerTypeCode:'1'},{currency:'USD'}])
    assert.throws(()=>subscriptionFact(groupFor({...order,...bad}),candidate,config,now),/ORDER_PRICE_MISMATCH/);
});
