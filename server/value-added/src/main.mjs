import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { isAbsolute, resolve } from 'node:path';
import { connectObs } from './obs-objects.mjs';
import { connectCloudDb } from './clouddb-store.mjs';
import { HuaweiAccountProvider, HuaweiPurchaseProvider } from './huawei-providers.mjs';
import { ValueAddedDomain } from './domain.mjs';
import { createApiServer } from './http-server.mjs';
import { requireThat, ServiceError } from './errors.mjs';
import { LIFETIME_POLICY, MEMBERSHIP_POLICY, SUBSCRIPTION_POLICY } from './membership-policy.mjs';
import { loadPkixRuntime } from './pkix-runtime.mjs';
import { HuaweiPushSender } from './huawei-push.mjs';
const packageRoot = fileURLToPath(new URL('../', import.meta.url));
const configFile = path => isAbsolute(path) ? path : resolve(packageRoot, path);
const optionalFile = async path => path ? readFile(configFile(path), 'utf8') : '';
export function membershipProductsVerified(products, release) {
  const available = products.filter(p => p.kind === 'membership' && p.purchasesEnabled !== false);
  return available.length > 0 && available.every(p =>
    (p.type === 1 && p.policyVersion === LIFETIME_POLICY && release?.lifetimeProductId === p.id) ||
    (p.type === 3 && p.policyVersion === MEMBERSHIP_POLICY && release?.oneTimeProductId === p.id) ||
    (p.type === 2 && p.policyVersion === SUBSCRIPTION_POLICY && release?.membershipProductId === p.id));
}
export function protectedPem(encoded, literal = '') {
  if (!encoded) return literal;
  requireThat(typeof encoded === 'string' && encoded.length <= 32768 &&
    /^[A-Za-z0-9+/]+={0,2}$/.test(encoded), 'INVALID_CREDENTIAL_CONFIGURATION', 503);
  const bytes = Buffer.from(encoded, 'base64');
  requireThat(bytes.toString('base64') === encoded, 'INVALID_CREDENTIAL_CONFIGURATION', 503);
  return bytes.toString('utf8');
}
// AGC bounds each environment value; preserve the PEM using three base64 chunks.
export function protectedPushCredential(json, env = {}) {
  const credential = JSON.parse(json || '{}');
  const chunks = [env.VP_PUSH_PRIVATE_KEY_BASE64_1, env.VP_PUSH_PRIVATE_KEY_BASE64_2, env.VP_PUSH_PRIVATE_KEY_BASE64_3];
  if (chunks.some(value => value !== undefined)) {
    requireThat(!credential.private_key && chunks.every(value => typeof value === 'string' && value.length > 0 && value.length <= 1800),
      'INVALID_CREDENTIAL_CONFIGURATION', 503);
    credential.private_key = protectedPem(chunks.join(''));
  }
  return credential;
}

export async function loadService(env = process.env) {
  requireThat(Number(process.versions.node.split('.')[0]) >= 22, 'NODE_RUNTIME_NOT_SUPPORTED', 503);
  const products = JSON.parse(await optionalFile(env.VP_PRODUCT_CATALOG ?? './config/product-catalog.json') || '[]');
  requireThat(Array.isArray(products) && products.length <= 20, 'INVALID_CATALOG');
  const ids = new Set();
  for (const product of products) {
    requireThat(/^[a-zA-Z0-9._-]{1,128}$/.test(product.id) && !ids.has(product.id) &&
      ((product.type === 0 && product.kind === 'coffee') || (product.kind === 'membership' &&
        ((product.type === 1 && product.policyVersion === LIFETIME_POLICY) ||
        (product.type === 3 && product.policyVersion === MEMBERSHIP_POLICY) ||
        (product.type === 2 && product.policyVersion === SUBSCRIPTION_POLICY &&
          /^[a-zA-Z0-9._-]{1,128}$/.test(product.subscriptionGroupId))))) &&
      (product.purchasesEnabled === undefined || typeof product.purchasesEnabled === 'boolean') &&
      product.prices && Object.keys(product.prices).length > 0,
      'INVALID_CATALOG');
    ids.add(product.id);
  }
  const config = { clientId: env.VP_ACCOUNT_CLIENT_ID ?? '', appId: env.VP_APP_ID ?? '', products,
    identitySecret: env.VP_IDENTITY_HMAC_SECRET ?? '', purchaseBindingSecret: env.VP_PURCHASE_HMAC_SECRET ?? '',
    isolatePurchaseEnvironments: true };
  const keysReady = config.identitySecret.length >= 32 && config.purchaseBindingSecret.length >= 32;
  const accountProvider = new HuaweiAccountProvider({ clientId: config.clientId, clientSecret: env.VP_ACCOUNT_CLIENT_SECRET });
  accountProvider.ready = accountProvider.ready && keysReady;
  const purchaseProvider = new HuaweiPurchaseProvider({ appId: config.appId, packageName: env.VP_PACKAGE_NAME,
    // The official signed receipt selects the environment. An old SANDBOX deployment
    // setting must never force AppGallery users through a tester-only SDK check.
    issuerId: env.VP_IAP_ISSUER_ID, keyId: env.VP_IAP_KEY_ID, environment: 'AUTO',
    privateKeyPem: protectedPem(env.VP_IAP_PRIVATE_KEY_BASE64, env.VP_IAP_PRIVATE_KEY_PEM) || await optionalFile(env.VP_IAP_PRIVATE_KEY_FILE),
    rootPem: protectedPem(env.VP_IAP_ROOT_CA_BASE64, env.VP_IAP_ROOT_CA_PEM) || await optionalFile(env.VP_IAP_ROOT_CA_FILE),
    products, ...await loadPkixRuntime() });
  let store = { ready: false, get: async () => { throw new ServiceError('NOT_CONFIGURED', 503); } };
  if ((env.VP_AGC_CREDENTIAL_FILE || env.VP_AGC_CREDENTIAL_JSON || env.PROJECT_CREDENTIAL) && env.VP_AGC_ZONE) {
    store = await connectCloudDb({ credentialFile: env.VP_AGC_CREDENTIAL_FILE,
      credentialJson: env.VP_AGC_CREDENTIAL_JSON || env.PROJECT_CREDENTIAL, zoneName: env.VP_AGC_ZONE });
    if (env.VP_CLOUDDB_VERIFICATION_FILE) await store.loadVerification(configFile(env.VP_CLOUDDB_VERIFICATION_FILE));
  }
  // Checkout requires working credentials and storage; each payment is still verified
  // against Huawei. Optional deployment evidence controls paid sync, not the first sale.
  if (keysReady && accountProvider.ready && store.ready === true && products.length) {
    purchaseProvider.enableCommerce();
  }
  let iapEvidence = '';
  try { iapEvidence = await readFile(configFile(env.VP_IAP_VERIFICATION_FILE ?? './config/iap-verification.json'), 'utf8'); }
  catch (error) { if (error.code !== 'ENOENT') throw error; }
  if (keysReady && accountProvider.ready && products.length && iapEvidence) {
    const evidence = JSON.parse(iapEvidence);
    if (evidence.environment === 'NORMAL' && evidence.queryAndSignatureVerified === true &&
      evidence.providerFingerprint === purchaseProvider.fingerprint('NORMAL')) purchaseProvider.ready = true;
  }
  let objects={ready:false};
  if(env.VP_OBS_ENDPOINT&&env.VP_OBS_BUCKET&&env.VP_OBS_CREDENTIAL_FILE){
    objects=await connectObs(env);
    if(env.VP_OBS_VERIFICATION_FILE)await objects.enable(JSON.parse(await readFile(configFile(env.VP_OBS_VERIFICATION_FILE),'utf8')));
  }
  config.enforceCheckoutReadiness = true;
  config.membershipReleaseReady = purchaseProvider.commerceReady &&
    products.some(product => product.kind === 'membership' && product.purchasesEnabled !== false);
  const pushEvidence = env.VP_RENEWAL_VERIFICATION_FILE ? JSON.parse(await optionalFile(env.VP_RENEWAL_VERIFICATION_FILE)) : {};
  const credential = protectedPushCredential(env.VP_PUSH_CREDENTIAL_JSON || await optionalFile(env.VP_PUSH_CREDENTIAL_FILE), env);
  const pushSender = new HuaweiPushSender({ credential, projectId: env.VP_PUSH_PROJECT_ID ?? '', receiptId: env.VP_PUSH_RECEIPT_ID ?? '',
    verified: !!env.VP_PUSH_RECEIPT_ID && (pushEvidence.appId === config.appId && pushEvidence.projectId === env.VP_PUSH_PROJECT_ID &&
      pushEvidence.timerActive === true && pushEvidence.deviceDisplayed === true && pushEvidence.deepLinkOpened === true &&
      pushEvidence.notificationCategory === 'ACCOUNT' && pushEvidence.accountCategoryEligible === true && pushEvidence.receiptVerified === true &&
      pushEvidence.paginationVerified === true && pushEvidence.advanceNoticeVerified === true &&
      pushEvidence.cancelStopsVerified === true && pushEvidence.permissionClosedVerified === true &&
      pushEvidence.dedupVerified === true && pushEvidence.retryAlarmVerified === true) });
  const domain = new ValueAddedDomain({ store, accountProvider, purchaseProvider, config, objects, pushSender });
  if (store.ready) {
    await domain.reminders.timerHealthy();
  }
  return domain;
}
if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const domain = await loadService();
  const port = Number(process.env.PORT ?? 8080);
  requireThat(Number.isInteger(port) && port > 0 && port < 65536, 'INVALID_PORT');
  createApiServer(domain).listen(port, '127.0.0.1', () => {
    console.log('Value-added API listening on loopback; HTTPS reverse proxy required.');
    console.log(JSON.stringify(domain.capabilities()));
  });
}
