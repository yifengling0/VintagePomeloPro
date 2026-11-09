import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { execFileSync } from 'node:child_process';
import { X509Certificate, sign, createHash, verify as verifySignature } from 'node:crypto';
import {verifyHuaweiJws as verifyJws,hasHuaweiIapExtension,HuaweiPurchaseProvider} from '../src/huawei-providers.mjs';
import { LIFETIME_EXPIRES_AT, LIFETIME_POLICY, MEMBERSHIP_POLICY, deriveMembership } from '../src/membership-policy.mjs';
import { ValueAddedDomain } from '../src/domain.mjs';
import { MemoryStore } from './memory-store.mjs';
const verifyHuaweiJws=(jws,root,now)=>verifyJws(jws,root,now??Date.now(),process.env.VP_TEST_OPENSSL_PATH);
const dir=mkdtempSync(join(tmpdir(),'vp-test-jws-')); const oldFetch=globalThis.fetch;
const run=(...args)=>execFileSync('openssl',args,{cwd:dir,stdio:'pipe'});
const pem=name=>readFileSync(join(dir,name+'.pem'),'utf8');
const key=name=>readFileSync(join(dir,name+'.key'),'utf8');
const der=name=>new X509Certificate(pem(name)).raw.toString('base64');
function issue(name,issuer,ext){
  run('ecparam','-name','prime256v1','-genkey','-noout','-out',name+'.key');
  run('req','-new','-key',name+'.key','-subj','/CN=VP Synthetic '+name,'-out',name+'.csr');
  writeFileSync(join(dir,name+'.ext'),ext);
  run('x509','-req','-in',name+'.csr','-CA',issuer+'.pem','-CAkey',issuer+'.key','-CAcreateserial','-days','2','-sha256','-extfile',name+'.ext','-out',name+'.pem');
}
const enc=o=>Buffer.from(JSON.stringify(o)).toString('base64url');
function jws(payload,{leaf='leaf',alg='ES256',chain=[der(leaf),der('intermediate'),der('root')],privateKey=key(leaf)}={}){
 const head=enc({alg,typ:'JWT',x5c:chain}),data=enc(payload),unsigned=head+'.'+data;
 return unsigned+'.'+sign('sha256',Buffer.from(unsigned),{key:privateKey,dsaEncoding:'ieee-p1363'}).toString('base64url');
}
try {
  run('ecparam','-name','prime256v1','-genkey','-noout','-out','root.key');
  run('req','-new','-x509','-key','root.key','-subj','/CN=VP Synthetic Root','-days','3','-sha256','-addext','basicConstraints=critical,CA:TRUE,pathlen:1','-addext','keyUsage=critical,keyCertSign,cRLSign','-out','root.pem');
  issue('intermediate','root','basicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,keyCertSign,cRLSign\n');
  const leafExt='basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\n';
  issue('leaf','intermediate',leafExt+'1.3.6.1.4.1.2011.2.415.1.1=DER:05:00\n');
  issue('wrongoid','intermediate',leafExt+'1.3.6.1.4.1.2011.2.415.1.2=DER:05:00\n');
  issue('criticaloid','intermediate',leafExt+'1.3.6.1.4.1.2011.2.415.1.1=critical,DER:05:00\n');
  issue('unknowncritical','intermediate',leafExt+'1.3.6.1.4.1.2011.2.415.1.1=DER:05:00\n1.2.3.4.5=critical,DER:05:00\n');
  issue('badusage','intermediate','basicConstraints=critical,CA:FALSE\nkeyUsage=critical,keyAgreement\n1.3.6.1.4.1.2011.2.415.1.1=DER:05:00\n');
  assert.throws(()=>verifyHuaweiJws(jws({test:true},{leaf:'unknowncritical'}),pem('root')),/INVALID_CERTIFICATE_CHAIN/);
  assert.throws(()=>verifyHuaweiJws(jws({test:true},{leaf:'badusage'}),pem('root')),/INVALID_CERTIFICATE_KEY_USAGE/);
  const now=Date.now();const payload={environment:'SANDBOX',purchaseOrderId:'synthetic-order',purchaseToken:'synthetic-token',applicationId:'test-app',productId:'month',productType:'3',quantity:1,purchaseTime:now-1000,signedTime:now,currency:'CNY',price:100,developerPayload:'synthetic-binding',needFinish:true,finishStatus:'2'};
  const good=jws(payload);assert.deepEqual(verifyHuaweiJws(good,pem('root')),payload);
  assert.equal(hasHuaweiIapExtension(new X509Certificate(pem('leaf'))),true);
  assert.throws(()=>verifyHuaweiJws(jws(payload,{alg:'HS256'}),pem('root')),/INVALID_JWS/);
  assert.throws(()=>verifyHuaweiJws(jws(payload,{privateKey:key('wrongoid')}),pem('root')),/INVALID_JWS/);
  assert.throws(()=>verifyHuaweiJws(jws(payload,{leaf:'wrongoid'}),pem('root')),/INVALID_CERTIFICATE_CHAIN/);
  assert.throws(()=>verifyHuaweiJws(jws(payload,{leaf:'criticaloid'}),pem('root')),/INVALID_CERTIFICATE_CHAIN/);
  assert.throws(()=>verifyHuaweiJws(good,pem('intermediate')),/UNTRUSTED_CERTIFICATE/);
  assert.throws(()=>verifyHuaweiJws(good,pem('root'),now+4*86400000),/EXPIRED_CERTIFICATE/);
  assert.throws(()=>verifyHuaweiJws(jws(payload,{chain:[der('leaf'),der('root'),der('intermediate')]}),pem('root')),/UNTRUSTED_CERTIFICATE/);
  const parts=good.split('.');parts[1]=enc({...payload,price:0});assert.throws(()=>verifyHuaweiJws(parts.join('.'),pem('root')),/INVALID_JWS/);
  let latest=payload,calls=0;
  globalThis.fetch=async(url,opts)=>{ calls++;assert.equal(url,'https://iap.cloud.huawei.com/order/harmony/v1/application/order/status/query');
    assert.deepEqual(JSON.parse(opts.body),{purchaseOrderId:payload.purchaseOrderId,purchaseToken:payload.purchaseToken});
    const auth=JSON.parse(Buffer.from(opts.headers.Authorization.slice(7).split('.')[1],'base64url').toString());
    assert.equal(auth.digest,createHash('sha256').update(opts.body).digest('hex'));assert.equal(auth.aud,'iap-v1');assert.equal(auth.aid,'test-app');assert.ok(auth.exp-auth.iat<=3600);
    return new Response(JSON.stringify({responseCode:'0',jwsPurchaseOrder:jws(latest)}),{status:200});
  };
  const provider=new HuaweiPurchaseProvider({appId:'test-app',packageName:'test.package',issuerId:'issuer',keyId:'key-id',environment:'SANDBOX',rootPem:pem('root'),privateKeyPem:key('leaf'),opensslPath:process.env.VP_TEST_OPENSSL_PATH,products:[{id:'month',type:3,kind:'membership',policyVersion:'calendar-month-v1',prices:{CNY:[100]}}]});
  provider.enableSandboxTesting();assert.equal(provider.sandboxReady,true);assert.equal(provider.ready,false);
  const normalProvider=new HuaweiPurchaseProvider({...provider.config,environment:'NORMAL'});
  assert.throws(()=>normalProvider.enableSandboxTesting(),/SANDBOX_REQUIRED/);assert.equal(normalProvider.sandboxReady,false);
  const incompleteProvider=new HuaweiPurchaseProvider({...provider.config,rootPem:''});
  assert.throws(()=>incompleteProvider.enableSandboxTesting(),/NOT_CONFIGURED/);assert.equal(incompleteProvider.sandboxReady,false);
  const wrongMode=JSON.stringify({type:3,jwsPurchaseOrder:jws({...payload,environment:'NORMAL'})});
  await assert.rejects(provider.verify(wrongMode,3),/INVALID_ORDER/);assert.equal(calls,0);
  const receipt=JSON.stringify({type:3,jwsPurchaseOrder:good});
  assert.equal((await provider.verify(receipt,3)).needsFinish,true);assert.equal(calls,1);
  latest={...payload,needFinish:false,finishStatus:'1'};assert.equal((await provider.verify(receipt,3)).needsFinish,false);
  latest={...payload,revocationTime:now};assert.equal((await provider.verify(receipt,3)).status,'revoked');
  for(const bad of [{...payload,applicationId:'other-app'},{...payload,purchaseToken:'other-token'},{...payload,environment:'NORMAL'},{...payload,price:200}]) {latest=bad;await assert.rejects(provider.verify(receipt,3));}
  const lifetimeProduct={id:'VIP',type:1,kind:'membership',policyVersion:LIFETIME_POLICY,
    prices:{CNY:[3800]},promotionalPrices:{time_limit:{CNY:[2800]}}};
  const lifetimeProvider=new HuaweiPurchaseProvider({...provider.config,products:[lifetimeProduct]});
  lifetimeProvider.enableSandboxTesting();
  assert.equal(lifetimeProvider.ready,false);
  assert.equal(lifetimeProvider.sandboxReady,true);
  const oldTypeProvider=new HuaweiPurchaseProvider({...lifetimeProvider.config,products:[{...lifetimeProduct,type:3}]});
  assert.notEqual(lifetimeProvider.fingerprint(),oldTypeProvider.fingerprint(),'type1 catalog must invalidate the old type3 probe fingerprint');
  const lifetimeOrder={...payload,productId:'VIP',productType:'1',purchaseOrderId:'synthetic-vip-order',
    purchaseToken:'synthetic-vip-token',price:3800};
  let latestLifetime=lifetimeOrder,lifetimeCalls=0,latestSigningKey=key('leaf');
  globalThis.fetch=async(url,opts)=>{
    lifetimeCalls++;
    assert.equal(url,'https://iap.cloud.huawei.com/order/harmony/v1/application/order/status/query');
    assert.equal(opts.method,'POST');
    assert.deepEqual(JSON.parse(opts.body),{purchaseOrderId:lifetimeOrder.purchaseOrderId,purchaseToken:lifetimeOrder.purchaseToken});
    const [head,body,signature]=opts.headers.Authorization.slice(7).split('.');
    const auth=JSON.parse(Buffer.from(body,'base64url'));
    assert.equal(auth.digest,createHash('sha256').update(opts.body).digest('hex'));
    assert.equal(auth.aud,'iap-v1');assert.equal(auth.aid,'test-app');assert.equal(auth.exp-auth.iat,300);
    assert.ok(verifySignature('sha256',Buffer.from(head+'.'+body),
      {key:new X509Certificate(pem('leaf')).publicKey,dsaEncoding:'ieee-p1363'},Buffer.from(signature,'base64url')));
    return new Response(JSON.stringify({responseCode:'0',jwsPurchaseOrder:jws(latestLifetime,{privateKey:latestSigningKey})}));
  };
  const lifetimeReceipt=JSON.stringify({type:1,jwsPurchaseOrder:jws(lifetimeOrder)});
  const lifetimeFact=await lifetimeProvider.verify(lifetimeReceipt,1);
  assert.equal(lifetimeCalls,1);assert.equal(lifetimeFact.productType,1);
  assert.equal(lifetimeFact.status,'purchased');assert.equal(lifetimeFact.needsFinish,true);
  assert.equal(lifetimeFact.purchasedAt,lifetimeOrder.purchaseTime);assert.equal(lifetimeFact.expiresAt,LIFETIME_EXPIRES_AT);
  latestLifetime={...lifetimeOrder,price:2800,offerId:'time_limit'};
  assert.equal((await lifetimeProvider.verify(lifetimeReceipt,1)).expiresAt,LIFETIME_EXPIRES_AT);
  for(const bad of [{price:2800},{price:2800,offerId:'unknown'},{price:2800,offerId:'__proto__'},
    {price:2799,offerId:'time_limit'},{price:3801},{price:6800},{price:3800,currency:'USD'}]) {
    latestLifetime={...lifetimeOrder,...bad};
    await assert.rejects(lifetimeProvider.verify(lifetimeReceipt,1),/ORDER_PRICE_MISMATCH/);
  }
  const beforeWrongReceipt=lifetimeCalls;
  await assert.rejects(lifetimeProvider.verify(JSON.stringify({type:3,jwsPurchaseOrder:jws({...lifetimeOrder,productType:'3'})}),1),/INVALID_ORDER/);
  await assert.rejects(lifetimeProvider.verify(JSON.stringify({type:1,jwsPurchaseOrder:jws({...lifetimeOrder,productType:'3'})}),1),/INVALID_ORDER/);
  await assert.rejects(lifetimeProvider.verify(JSON.stringify({type:1,jwsPurchaseOrder:jws(lifetimeOrder,{privateKey:key('wrongoid')})}),1),/INVALID_JWS/);
  assert.equal(lifetimeCalls,beforeWrongReceipt,'untrusted/wrong-type receipts must not query even the mocked provider');
  for(const bad of [{productType:'3'},{productId:'other'},{purchaseOrderId:'other'},
    {purchaseToken:'other'},{applicationId:'other'},{environment:'NORMAL'}]) {
    latestLifetime={...lifetimeOrder,...bad};
    await assert.rejects(lifetimeProvider.verify(lifetimeReceipt,1),/INVALID_ORDER/);
  }
  latestLifetime=lifetimeOrder;latestSigningKey=key('wrongoid');
  await assert.rejects(lifetimeProvider.verify(lifetimeReceipt,1),/INVALID_JWS/);
  latestSigningKey=key('leaf');
  for(const [needFinish,finishStatus,expected] of [[false,'1',false],[undefined,'1',false],[undefined,'2',true],[true,'2',true]]) {
    latestLifetime={...lifetimeOrder,needFinish,finishStatus};
    assert.equal((await lifetimeProvider.verify(lifetimeReceipt,1)).needsFinish,expected);
  }
  latestLifetime={...lifetimeOrder,needFinish:undefined,finishStatus:undefined};
  await assert.rejects(lifetimeProvider.verify(lifetimeReceipt,1),/ORDER_FINISH_UNKNOWN/);
  for(const revoked of [{revocationTime:now},{purchaseOrderRevocationReasonCode:'1'}]) {
    latestLifetime={...lifetimeOrder,needFinish:undefined,finishStatus:undefined,...revoked};
    const fact=await lifetimeProvider.verify(lifetimeReceipt,1);
    assert.equal(fact.status,'revoked');assert.equal(fact.needsFinish,false);
  }
  const type3LifetimeOrder={...lifetimeOrder,productType:'3'};
  latestLifetime=type3LifetimeOrder;
  const type3LifetimeReceipt=JSON.stringify({type:3,jwsPurchaseOrder:jws(type3LifetimeOrder)});
  await assert.rejects(lifetimeProvider.verify(type3LifetimeReceipt,3),/UNKNOWN_PRODUCT/);
  for(const invalid of [{type:1,policyVersion:MEMBERSHIP_POLICY},{type:1,policyVersion:'huawei-subscription-v1'},
    {type:3,policyVersion:LIFETIME_POLICY},{type:3,policyVersion:'unknown'}]) {
    const invalidProvider=new HuaweiPurchaseProvider({...lifetimeProvider.config,products:[{...lifetimeProduct,...invalid}]});
    latestLifetime=invalid.type===1?lifetimeOrder:type3LifetimeOrder;
    await assert.rejects(invalidProvider.verify(invalid.type===1?lifetimeReceipt:type3LifetimeReceipt,invalid.type),/MEMBERSHIP_POLICY_UNKNOWN/);
  }

  // Full local path: real signing/verifying provider, synthetic official-format JWS,
  // latest-order fetch mock, persistent domain facts, and the existing membership derivation.
  const lifetimeStore=new MemoryStore(),owner='a'.repeat(64);
  let domainTime=now;
  const lifetimeConfig={appId:'test-app',products:[lifetimeProduct],purchaseBindingSecret:'synthetic-binding-secret'.repeat(3)};
  const makeLifetimeDomain=()=>new ValueAddedDomain({store:lifetimeStore,accountProvider:{ready:true},
    purchaseProvider:lifetimeProvider,config:lifetimeConfig,clock:()=>domainTime});
  const lifetimeDomain=makeLifetimeDomain();
  assert.equal(lifetimeDomain.capabilities().sandboxCommerce,undefined);
  assert.equal(lifetimeDomain.capabilities().profiles,false);
  const intent=await lifetimeDomain.intent(owner,{productId:'VIP',productType:1,promotionalOfferId:'time_limit'});
  const [offerHead,offerBody,offerSignature]=intent.promotionalPurchase.jwsRepresentation.split('.');
  const offer=JSON.parse(Buffer.from(offerBody,'base64url'));
  assert.deepEqual(JSON.parse(offer.data).productItem,{applicationId:'test-app',productId:'VIP'});
  assert.equal(JSON.parse(offer.data).offerInfo.offerId,'time_limit');
  assert.equal(JSON.parse(offer.data).offerInfo.applicationUserName,owner);
  assert.ok(verifySignature('sha256',Buffer.from(offerHead+'.'+offerBody),
    {key:new X509Certificate(pem('leaf')).publicKey,dsaEncoding:'ieee-p1363'},Buffer.from(offerSignature,'base64url')));
  const purchasedLifetime={...lifetimeOrder,price:2800,offerId:'time_limit',developerPayload:intent.developerPayload};
  latestLifetime=purchasedLifetime;
  const boundReceipt=JSON.stringify({type:1,jwsPurchaseOrder:jws(purchasedLifetime)});
  const grant=await lifetimeDomain.verify(owner,{purchaseData:boundReceipt,productType:1});
  assert.equal(grant.status,'GRANTED');assert.equal(grant.finishAllowed,true);assert.equal(grant.productType,1);
  assert.equal(grant.purchaseOrderId,purchasedLifetime.purchaseOrderId);
  const storedOrders=await lifetimeStore.list('orders:'+owner);
  assert.equal(storedOrders.length,1);assert.equal(storedOrders[0].productType,1);
  assert.equal(storedOrders[0].policyVersion,LIFETIME_POLICY);assert.equal(storedOrders[0].expiresAt,LIFETIME_EXPIRES_AT);
  latestLifetime={...purchasedLifetime,needFinish:false,finishStatus:'1'};
  assert.deepEqual(await lifetimeDomain.verify(owner,{purchaseData:boundReceipt,productType:1}),{status:'ALREADY_FINISHED',finishAllowed:false});
  assert.equal((await lifetimeStore.list('orders:'+owner)).length,1);
  domainTime+=100*366*86400000;
  const queriesBeforeRestore=lifetimeCalls;
  const restored=await makeLifetimeDomain().entitlement(owner);
  assert.equal(restored.active,true);assert.equal(restored.lifetime,true);
  assert.equal(restored.expiresAt,LIFETIME_EXPIRES_AT);assert.equal(restored.refreshRequired,false);
  assert.equal(lifetimeCalls,queriesBeforeRestore,'permanent entitlement must not requery');
  const persisted=await lifetimeStore.get('entitlement:'+owner);
  assert.equal(deriveMembership(persisted.orders,domainTime,300000).lifetime,true);
  latestLifetime={...purchasedLifetime,revocationTime:now};
  assert.deepEqual(await lifetimeDomain.verify(owner,{purchaseData:boundReceipt,productType:1}),{status:'REVOKED',finishAllowed:false});
  assert.equal((await lifetimeDomain.entitlement(owner)).active,false);
  const refundedRows=structuredClone(lifetimeStore.rows);
  latestLifetime=purchasedLifetime;
  await assert.rejects(lifetimeDomain.verify(owner,{purchaseData:boundReceipt,productType:1}),/ORDER_REVOKED/);
  assert.deepEqual(lifetimeStore.rows,refundedRows);
  assert.equal((await lifetimeDomain.entitlement(owner)).active,false);
  console.log('PASS: nonconsumable type1 official-format JWS/latest query, CNY 3800 and time_limit 2800 whitelist, wrong type/policy/price/signature, finish/refund, provider->domain->permanent entitlement');

  const subscribed={...payload,productId:'monthly',productType:'2',subscriptionId:'sub',subGroupId:'members',
    subGroupGenerationId:'generation',duration:'P1M',price:500};
  let subGroup={applicationId:'test-app',packageName:'test.package',environment:'SANDBOX',subGroupId:'members',
    lastSubscriptionStatus:{subscriptionId:'sub',subGroupGenerationId:'generation',purchaseToken:payload.purchaseToken,
      status:'1',expiresTime:now+600000,lastPurchaseOrder:subscribed,
      renewalInfo:{productId:'monthly',subGroupGenerationId:'generation',environment:'SANDBOX',
        autoRenewStatusCode:'1',hasInBillingRetryPeriod:false}}};
  globalThis.fetch=async(url,opts)=>{
    assert.equal(url,'https://iap.cloud.huawei.com/subscription/harmony/v1/application/subscription/status/query');
    assert.deepEqual(JSON.parse(opts.body),{purchaseOrderId:payload.purchaseOrderId,purchaseToken:payload.purchaseToken});
    return new Response(JSON.stringify({responseCode:'0',jwsSubGroupStatus:jws(subGroup)}));
  };
  const autoProvider=new HuaweiPurchaseProvider({...provider.config,products:[{id:'monthly',type:2,kind:'membership',
    policyVersion:'huawei-subscription-v1',subscriptionGroupId:'members',prices:{CNY:[500]}}]});
  const autoReceipt=JSON.stringify({type:2,jwsPurchaseOrder:jws(subscribed)});
  assert.equal((await autoProvider.verify(autoReceipt,2)).expiresAt,now+600000);
  const currentEntitlementReceipt=JSON.stringify({type:2,jwsSubscriptionStatus:jws(subGroup)});
  assert.equal((await autoProvider.verify(currentEntitlementReceipt,2)).status,'purchased');
  assert.equal((await autoProvider.verify(currentEntitlementReceipt,2)).developerPayload,subscribed.developerPayload);
  await assert.rejects(autoProvider.verify(JSON.stringify({type:2,jwsSubscriptionStatus:jws(subGroup),
    jwsPurchaseOrder:jws(subscribed)}),2),/INVALID_ORDER/);
  await assert.rejects(autoProvider.verify(JSON.stringify({type:2,jwsSubscriptionStatus:jws({...subGroup,packageName:'wrong'})}),2),/INVALID_SUBSCRIPTION/);
  await assert.rejects(autoProvider.verify(JSON.stringify({type:2,jwsSubscriptionStatus:jws(subGroup,{privateKey:key('wrongoid')})}),2),/INVALID_JWS/);
  await assert.rejects(autoProvider.verify(JSON.stringify({type:2,jwsSubscriptionStatus:jws({...subGroup,lastSubscriptionStatus:undefined})}),2),/INVALID_SUBSCRIPTION/);
  const validGroup=subGroup;
  subGroup={...validGroup,packageName:'another.package'};await assert.rejects(autoProvider.verify(autoReceipt,2),/INVALID_SUBSCRIPTION/);
  subGroup=validGroup;
  globalThis.fetch=async()=>new Response(JSON.stringify({responseCode:'0',jwsSubGroupStatus:jws(subGroup,{privateKey:key('wrongoid')})}));
  await assert.rejects(autoProvider.verify(autoReceipt,2),/INVALID_JWS/);
  const unified = new HuaweiPurchaseProvider({...provider.config,environment:'AUTO'});
  unified.enableCommerce();
  assert.equal(unified.commerceReady,true);assert.equal(unified.ready,false);assert.equal(unified.sandboxReady,false);
  for (const environment of ['NORMAL','SANDBOX']) {
    const candidate={...payload,environment};let official=candidate,queries=0;
    globalThis.fetch=async()=>{queries++;return new Response(JSON.stringify({responseCode:'0',jwsPurchaseOrder:jws(official)}));};
    const receipt=JSON.stringify({type:3,jwsPurchaseOrder:jws(candidate)});
    assert.equal((await unified.verify(receipt,3)).environment,environment);
    official={...candidate,environment:environment==='NORMAL'?'SANDBOX':'NORMAL'};
    await assert.rejects(unified.verify(receipt,3),/INVALID_ORDER/);
    const before=queries;
    await assert.rejects(unified.verify(JSON.stringify({type:3,jwsPurchaseOrder:jws({...candidate,environment:'AUTO'})}),3),/INVALID_ORDER/);
    assert.equal(queries,before);
    const subscription={...subscribed,environment};
    const group={...validGroup,environment,lastSubscriptionStatus:{...validGroup.lastSubscriptionStatus,
      lastPurchaseOrder:subscription,renewalInfo:{...validGroup.lastSubscriptionStatus.renewalInfo,environment}}};
    let officialGroup=group;
    globalThis.fetch=async()=>new Response(JSON.stringify({responseCode:'0',jwsSubGroupStatus:jws(officialGroup)}));
    const unifiedAuto=new HuaweiPurchaseProvider({...autoProvider.config,environment:'AUTO'});
    const autoReceipt=JSON.stringify({type:2,jwsPurchaseOrder:jws(subscription)});
    assert.equal((await unifiedAuto.verify(autoReceipt,2)).environment,environment);
    assert.equal((await unifiedAuto.verify(JSON.stringify({type:2,jwsSubscriptionStatus:jws(group)}),2)).environment,environment);
    officialGroup={...group,environment:environment==='NORMAL'?'SANDBOX':'NORMAL'};
    await assert.rejects(unifiedAuto.verify(autoReceipt,2),/INVALID_SUBSCRIPTION/);
  }
  console.log('PASS: AUTO accepts signed NORMAL and SANDBOX orders/subscriptions and rejects cross-environment latest responses');
  console.log('PASS: synthetic JWS valid/alg/signature/OID/noncritical/root/expiry/chain/tamper + provider request digest/latest status/app/token/environment/price/finish/refund');
} finally {globalThis.fetch=oldFetch;rmSync(dir,{recursive:true,force:true});}
