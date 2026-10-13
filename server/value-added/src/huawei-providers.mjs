import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { execFileSync } from 'node:child_process';
import { createHash, createPrivateKey, randomUUID, sign, verify, X509Certificate } from 'node:crypto';
import { addCalendarMonth, MEMBERSHIP_POLICY } from './membership-policy.mjs';
import { subscriptionFact } from './subscription.mjs';
import { requireThat } from './errors.mjs';

async function fetchJson(url, options) {
  const response = await fetch(url, { ...options, signal: AbortSignal.timeout(15000), redirect: 'error' });
  requireThat(response.ok, 'PROVIDER_UNAVAILABLE', 503);
  requireThat(!response.headers.has('NSP_STATUS'), 'INVALID_IDENTITY', 401);
  const text = await response.text();
  requireThat(Buffer.byteLength(text) < 524288, 'INVALID_PROVIDER_RESPONSE', 502);
  return JSON.parse(text);
}
export class HuaweiAccountProvider {
  constructor({ clientId, clientSecret }) {
    this.clientId = clientId; this.clientSecret = clientSecret;
    this.ready = !!clientId && !!clientSecret;
  }
  async exchange(code) {
    requireThat(this.ready, 'NOT_CONFIGURED', 503);
    const token = await fetchJson('https://oauth-login.cloud.huawei.com/oauth2/v3/token', {
      method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams({ grant_type: 'authorization_code', client_id: this.clientId,
        client_secret: this.clientSecret, code }) });
    requireThat(token.token_type === 'Bearer' && typeof token.access_token === 'string' && !token.error, 'INVALID_IDENTITY', 401);
    const user = await fetchJson('https://account.cloud.huawei.com/rest.php?nsp_svc=GOpen.User.getInfo', {
      method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams({ access_token: token.access_token, getNickName: '0' }) });
    requireThat(!Object.hasOwn(user, 'NSP_STATUS') && typeof user.openID === 'string' && user.openID.length > 0 &&
      user.openID.length < 1024, 'INVALID_IDENTITY', 401);
    // Subject comes from Huawei over TLS after exchanging this application's code. Tokens are discarded.
    return { subject: 'huawei:' + user.openID, audience: this.clientId };
  }
}

// A small bounded DER reader only walks the certificate extension sequence. It does not search arbitrary bytes.
function derNode(bytes, at, end = bytes.length) {
  requireThat(at >= 0 && at + 2 <= end, 'INVALID_CERTIFICATE');
  const tag = bytes[at++]; let length = bytes[at++];
  if (length & 128) {
    const count = length & 127;
    requireThat(count > 0 && count <= 4 && at + count <= end && bytes[at] !== 0, 'INVALID_CERTIFICATE');
    length = 0;
    for (let i = 0; i < count; i++) length = length * 256 + bytes[at++];
    requireThat(length >= 128, 'INVALID_CERTIFICATE');
  }
  requireThat(at + length <= end, 'INVALID_CERTIFICATE');
  return { tag, start: at, end: at + length };
}
function children(bytes, node) {
  const result = []; let at = node.start;
  while (at < node.end) { const child = derNode(bytes, at, node.end); result.push(child); at = child.end; }
  return result;
}
export function hasHuaweiIapExtension(certificate) {
  const bytes = certificate.raw;
  const sequence = derNode(bytes, 0);
  requireThat(sequence.tag === 48 && sequence.end === bytes.length, 'INVALID_CERTIFICATE');
  const tbs = children(bytes, sequence)[0];
  const wrapper = children(bytes, tbs).find(node => node.tag === 163);
  if (!wrapper) return false;
  const extensionSequence = children(bytes, wrapper)[0];
  // DER OID 1.3.6.1.4.1.2011.2.415.1.1.
  const expected = Buffer.from('2b060104018f5b02831f0101', 'hex');
  return children(bytes, extensionSequence).some(extension => {
    const fields = children(bytes, extension), oid = fields[0];
    const nonCritical = fields[1]?.tag !== 1 || bytes[fields[1].start] === 0;
    return nonCritical && oid.tag === 6 && bytes.subarray(oid.start, oid.end).equals(expected);
  });
}
function verifyPkix(chain, root, now, opensslPath = 'openssl') {
  // OpenSSL's PKIX validator covers path length, CA signing usage and unknown critical extensions.
  // Certificates are public data; temporary files contain no account/payment secret and are always removed.
  const directory = mkdtempSync(join(tmpdir(), 'vp-iap-chain-'));
  try {
    writeFileSync(join(directory, 'root.pem'), root.toString(), { mode: 0o600 });
    writeFileSync(join(directory, 'intermediate.pem'), chain[1].toString(), { mode: 0o600 });
    writeFileSync(join(directory, 'leaf.pem'), chain[0].toString(), { mode: 0o600 });
    execFileSync(opensslPath, ['verify', '-no-CAfile', '-no-CApath', '-no-CAstore', '-trusted', join(directory, 'root.pem'),
      '-untrusted', join(directory, 'intermediate.pem'), '-verify_depth', '2', '-auth_level', '2', '-purpose', 'any',
      '-attime', String(Math.floor(now / 1000)), join(directory, 'leaf.pem')], { timeout: 5000, stdio: 'pipe' });
  } catch { requireThat(false, 'INVALID_CERTIFICATE_CHAIN'); }
  finally { rmSync(directory, { recursive: true, force: true }); }
  const bytes = chain[0].raw, tbs = children(bytes, derNode(bytes, 0))[0];
  const wrapper = children(bytes, tbs).find(node => node.tag === 163);
  const extensions = children(bytes, children(bytes, wrapper)[0]);
  for (const extension of extensions) {
    const fields = children(bytes, extension), oid = fields[0];
    if (bytes.subarray(oid.start, oid.end).equals(Buffer.from('551d0f', 'hex'))) {
      const value = fields[fields.length - 1], usage = derNode(bytes, value.start, value.end);
      requireThat(usage.tag === 3 && usage.end - usage.start >= 2 && (bytes[usage.start + 1] & 128) !== 0,
        'INVALID_CERTIFICATE_KEY_USAGE');
    }
  }
}
export function verifyHuaweiJws(jws, trustedRootPem, now = Date.now(), opensslPath = 'openssl') {
  requireThat(typeof jws === 'string' && jws.length < 131072, 'INVALID_JWS');
  const parts = jws.split('.');
  requireThat(parts.length === 3 && parts.every(part => /^[A-Za-z0-9_-]+$/.test(part)), 'INVALID_JWS');
  const header = JSON.parse(Buffer.from(parts[0], 'base64url').toString('utf8'));
  requireThat(header.alg === 'ES256' && header.typ === 'JWT' && Array.isArray(header.x5c) && header.x5c.length === 3 &&
    !header.crit && !header.jku && !header.x5u, 'INVALID_JWS');
  const chain = header.x5c.map(value => {
    requireThat(typeof value === 'string' && value.length < 16384, 'INVALID_CERTIFICATE');
    return new X509Certificate(Buffer.from(value, 'base64'));
  });
  const root = new X509Certificate(trustedRootPem);
  requireThat(chain[2].fingerprint256 === root.fingerprint256 && root.ca && root.verify(root.publicKey), 'UNTRUSTED_CERTIFICATE');
  for (const cert of [...chain, root]) {
    requireThat(Date.parse(cert.validFrom) <= now && Date.parse(cert.validTo) > now, 'EXPIRED_CERTIFICATE');
  }
  requireThat(!chain[0].ca && chain[1].ca && chain[0].checkIssued(chain[1]) && chain[0].verify(chain[1].publicKey) &&
    chain[1].checkIssued(root) && chain[1].verify(root.publicKey) && hasHuaweiIapExtension(chain[0]), 'INVALID_CERTIFICATE_CHAIN');
  verifyPkix(chain, root, now, opensslPath);
  requireThat(chain[0].publicKey.asymmetricKeyType === 'ec' &&
    chain[0].publicKey.asymmetricKeyDetails?.namedCurve === 'prime256v1', 'INVALID_JWS');
  const signature = Buffer.from(parts[2], 'base64url');
  requireThat(signature.length === 64 && verify('sha256', Buffer.from(parts[0] + '.' + parts[1]),
    { key: chain[0].publicKey, dsaEncoding: 'ieee-p1363' }, signature), 'INVALID_JWS');
  return JSON.parse(Buffer.from(parts[1], 'base64url').toString('utf8'));
}
const segment = object => Buffer.from(JSON.stringify(object)).toString('base64url');
export function iapAuthorization(config, body, now = Date.now()) {
  const header = segment({ alg: 'ES256', typ: 'JWT', kid: config.keyId });
  const iat = Math.floor(now / 1000);
  const payload = segment({ iss: config.issuerId, aud: 'iap-v1', iat, exp: iat + 300, aid: config.appId,
    digest: createHash('sha256').update(body).digest('hex') });
  const unsigned = header + '.' + payload;
  return unsigned + '.' + sign('sha256', Buffer.from(unsigned),
    { key: createPrivateKey(config.privateKeyPem), dsaEncoding: 'ieee-p1363' }).toString('base64url');
}

export class HuaweiPurchaseProvider {
  constructor(config) { this.config = config; this.ready = false; this.sandboxReady = false; }
  checkConfiguration() {
    let version = '';
    try { version = execFileSync(this.config.opensslPath ?? 'openssl', ['version'], { encoding: 'utf8', timeout: 5000, stdio: ['ignore', 'pipe', 'pipe'] }); }
    catch { requireThat(false, 'PKIX_NOT_CONFIGURED', 503); }
    requireThat(/^OpenSSL 3\./.test(version), 'PKIX_NOT_CONFIGURED', 503);
    const key = createPrivateKey(this.config.privateKeyPem);
    requireThat(key.asymmetricKeyType === 'ec' && key.asymmetricKeyDetails?.namedCurve === 'prime256v1',
      'NOT_CONFIGURED', 503);
    this.fingerprint();
  }
  enableSandboxTesting() {
    requireThat(this.config.environment === 'SANDBOX', 'SANDBOX_REQUIRED', 503);
    this.checkConfiguration();
    this.sandboxReady = true;
  }
  enableAfterProbe(evidence) {
    this.checkConfiguration();
    requireThat(evidence?.providerFingerprint === this.fingerprint() && evidence.queryAndSignatureVerified === true,
      'IAP_NOT_VERIFIED', 503);
    this.ready = true;
  }
  fingerprint() {
    const c = this.config;
    requireThat(c.appId && c.packageName && c.issuerId && c.keyId && c.privateKeyPem && c.rootPem &&
      ['SANDBOX','NORMAL'].includes(c.environment), 'NOT_CONFIGURED', 503);
    return createHash('sha256').update(JSON.stringify({ appId: c.appId, packageName: c.packageName,
      issuerId: c.issuerId, keyId: c.keyId, root: new X509Certificate(c.rootPem).fingerprint256,
      environment: c.environment, products: c.products, verifierDigest: c.verifierDigest ?? 'system-openssl3' })).digest('hex');
  }
  signPromotion(productId, offerId, userId, now = Date.now()) {
    requireThat(this.ready || this.sandboxReady && this.config.environment === 'SANDBOX', 'NOT_CONFIGURED', 503);
    const product = this.config.products.find(p => p.id === productId && p.type === 2);
    requireThat(Object.hasOwn(product?.promotionalPrices ?? {}, offerId) && /^[a-f0-9]{64}$/.test(userId), 'UNKNOWN_OFFER');
    const c = this.config, iat = Math.floor(now / 1000);
    const data = JSON.stringify({ productItem: { applicationId: c.appId, productId },
      offerInfo: { offerId, applicationUserName: userId, nonce: randomUUID() } });
    const unsigned = segment({ alg: 'ES256', typ: 'JWT', kid: c.keyId }) + '.' +
      segment({ iss: c.issuerId, aud: 'iap-v1', iat, exp: iat + 300, aid: c.appId, data });
    return { promotionalOfferId: offerId, applicationUserName: userId,
      jwsRepresentation: unsigned + '.' + sign('sha256', Buffer.from(unsigned),
        { key: createPrivateKey(c.privateKeyPem), dsaEncoding: 'ieee-p1363' }).toString('base64url') };
  }
  async verify(purchaseData, productType) {
    // The untrusted order JWS is verified before using its IDs; provider's latest order is verified a second time.
    const data = JSON.parse(purchaseData);
    requireThat(data.type === productType, 'INVALID_ORDER');
    let candidate;
    if (productType === 2 && typeof data.jwsSubscriptionStatus === 'string') {
      requireThat(data.jwsPurchaseOrder === undefined, 'INVALID_ORDER');
      const group = verifyHuaweiJws(data.jwsSubscriptionStatus, this.config.rootPem, Date.now(), this.config.opensslPath);
      candidate = group.lastSubscriptionStatus?.lastPurchaseOrder;
      // CURRENT_ENTITLEMENT returns a signed subscription group, not a purchase-order JWS.
      // Validate its group, package, order and renewal bindings before using any provider IDs.
      subscriptionFact(group, candidate, this.config, Date.now());
    } else {
      requireThat(data.jwsSubscriptionStatus === undefined, 'INVALID_ORDER');
      candidate = verifyHuaweiJws(data.jwsPurchaseOrder, this.config.rootPem, Date.now(), this.config.opensslPath);
    }
    requireThat(candidate.applicationId === this.config.appId && candidate.environment === this.config.environment &&
      candidate.productType === String(productType), 'INVALID_ORDER');
    requireThat(typeof candidate.purchaseOrderId === 'string' && typeof candidate.purchaseToken === 'string', 'INVALID_ORDER');
    const body = JSON.stringify({ purchaseOrderId: candidate.purchaseOrderId, purchaseToken: candidate.purchaseToken });
    const endpoint = productType === 2 ? '/subscription/harmony/v1/application/subscription/status/query' :
      '/order/harmony/v1/application/order/status/query';
    const response = await fetchJson('https://iap.cloud.huawei.com' + endpoint, {
      method: 'POST', headers: { 'Content-Type': 'application/json; charset=UTF-8',
        Authorization: 'Bearer ' + iapAuthorization(this.config, body) }, body });
    if (productType === 2) {
      requireThat(response.responseCode === '0' && response.jwsSubGroupStatus, 'ORDER_NOT_VERIFIED', 502);
      return subscriptionFact(verifyHuaweiJws(response.jwsSubGroupStatus, this.config.rootPem, Date.now(), this.config.opensslPath),
        candidate, this.config, Date.now());
    }
    requireThat(response.responseCode === '0' && response.jwsPurchaseOrder, 'ORDER_NOT_VERIFIED', 502);
    const order = verifyHuaweiJws(response.jwsPurchaseOrder, this.config.rootPem, Date.now(), this.config.opensslPath);
    requireThat(order.applicationId === this.config.appId && order.environment === this.config.environment &&
      order.purchaseOrderId === candidate.purchaseOrderId && order.purchaseToken === candidate.purchaseToken &&
      order.productType === String(productType) && order.productId === candidate.productId, 'INVALID_ORDER');
    const product = this.config.products.find(value => value.id === order.productId && value.type === productType);
    requireThat(product, 'UNKNOWN_PRODUCT');
    requireThat(product.type === 0 || (product.type === 3 && product.policyVersion === MEMBERSHIP_POLICY),
      'MEMBERSHIP_POLICY_UNKNOWN', 503);
    requireThat(Number.isFinite(order.purchaseTime) && Number.isFinite(order.signedTime) && order.signedTime <= Date.now() + 60000 &&
      typeof order.currency === 'string' && order.currency.length === 3 && Number.isFinite(order.price) && order.price >= 0,
      'INVALID_ORDER');
    requireThat(Array.isArray(product.prices?.[order.currency]) && product.prices[order.currency].includes(order.price),
      'ORDER_PRICE_MISMATCH');
    const revoked = order.revocationTime !== undefined || order.purchaseOrderRevocationReasonCode !== undefined;
    // A non-renewing monthly pass is a separate grant anchored to its verified purchase time, never a local increment.
    requireThat(revoked || order.needFinish === false || order.needFinish === true ||
      ['1','2'].includes(order.finishStatus), 'ORDER_FINISH_UNKNOWN');
    const needsFinish = order.needFinish !== false && order.finishStatus !== '1' &&
      (order.needFinish === true || order.finishStatus === '2');
    const expiresAt = product.kind === 'membership' ? addCalendarMonth(order.purchaseTime) : 0;
    return { appId: order.applicationId, orderId: order.purchaseOrderId, productId: order.productId,
      productType, purchaseToken: order.purchaseToken, developerPayload: order.developerPayload,
      quantity: order.quantity === undefined ? 1 : order.quantity,
      status: revoked ? 'revoked' : 'purchased', needsFinish,
      purchasedAt: order.purchaseTime, expiresAt };
  }
}
