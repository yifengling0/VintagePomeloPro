import test from 'node:test';
import assert from 'node:assert/strict';
import { generateKeyPairSync, verify, createHash } from 'node:crypto';
import { HuaweiAccountProvider, iapAuthorization, verifyHuaweiJws } from '../src/huawei-providers.mjs';
import { addCalendarMonth, deriveMembership, MEMBERSHIP_POLICY } from '../src/membership-policy.mjs';

test('Account accepts only provider subject, rejects NSP_STATUS HTTP header even with plausible JSON', async () => {
  const old = globalThis.fetch;
  let calls = 0;
  globalThis.fetch = async () => ++calls === 1 ? new Response(JSON.stringify({token_type:'Bearer',access_token:'temporary'})) :
    new Response(JSON.stringify({openID:'must-not-authenticate'}), {headers:{NSP_STATUS:'6'}});
  try { await assert.rejects(new HuaweiAccountProvider({clientId:'c',clientSecret:'s'}).exchange('code'),/INVALID_IDENTITY/); }
  finally { globalThis.fetch = old; }
});
test('IAP request JWT signs exact body digest with ES256 R||S and bounded validity', () => {
  const keys = generateKeyPairSync('ec',{namedCurve:'prime256v1'}), body='{"purchaseOrderId":"one","purchaseToken":"two"}';
  const jwt = iapAuthorization({keyId:'kid',issuerId:'iss',appId:'app',
    privateKeyPem:keys.privateKey.export({type:'pkcs8',format:'pem'})},body,1800000000000);
  const [h,p,s] = jwt.split('.'), claims=JSON.parse(Buffer.from(p,'base64url'));
  assert.equal(claims.digest,createHash('sha256').update(body).digest('hex'));
  assert.equal(claims.aud,'iap-v1'); assert.equal(claims.exp-claims.iat,300);
  assert.ok(verify('sha256',Buffer.from(h+'.'+p),{key:keys.publicKey,dsaEncoding:'ieee-p1363'},Buffer.from(s,'base64url')));
  assert.throws(()=>verifyHuaweiJws('eyJhbGciOiJub25lIn0.e30.AAAA','not a root'),/INVALID_JWS/);
});
test('calendar-month-v1 clamps month ends, preserves UTC time, stacks distinct orders, and recomputes refunds', () => {
  assert.equal(new Date(addCalendarMonth(Date.parse('2024-01-31T03:04:05.006Z'))).toISOString(),'2024-02-29T03:04:05.006Z');
  assert.equal(new Date(addCalendarMonth(Date.parse('2025-01-31T03:04:05.006Z'))).toISOString(),'2025-02-28T03:04:05.006Z');
  const now=Date.parse('2026-01-20T00:00:00Z');
  const one={orderId:'a',purchasedAt:Date.parse('2026-01-01T00:00:00Z'),status:'purchased',verifiedAt:now,policyVersion:MEMBERSHIP_POLICY};
  const two={...one,orderId:'b',purchasedAt:Date.parse('2026-01-10T00:00:00Z')};
  const both=deriveMembership({a:one,b:two},now,300000);
  assert.equal(new Date(both.expiresAt).toISOString(),'2026-03-01T00:00:00.000Z');
  assert.equal(new Date(deriveMembership({a:{...one,status:'revoked'},b:two},now,300000).expiresAt).toISOString(),
    '2026-02-10T00:00:00.000Z');
  assert.equal(deriveMembership({a:one,b:two},now+300001,300000).active,false);
});
