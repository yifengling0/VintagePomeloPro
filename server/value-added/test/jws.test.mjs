import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { execFileSync } from 'node:child_process';
import { X509Certificate, sign, createHash } from 'node:crypto';
import {verifyHuaweiJws as verifyJws,hasHuaweiIapExtension,HuaweiPurchaseProvider} from '../src/huawei-providers.mjs';
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
  console.log('PASS: synthetic JWS valid/alg/signature/OID/noncritical/root/expiry/chain/tamper + provider request digest/latest status/app/token/environment/price/finish/refund');
} finally {globalThis.fetch=oldFetch;rmSync(dir,{recursive:true,force:true});}
