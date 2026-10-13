import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { isAbsolute, resolve } from 'node:path';
import { connectObs } from './obs-objects.mjs';
import { connectCloudDb } from './clouddb-store.mjs';
import { HuaweiAccountProvider, HuaweiPurchaseProvider } from './huawei-providers.mjs';
import { ValueAddedDomain } from './domain.mjs';
import { createApiServer } from './http-server.mjs';
import { requireThat, ServiceError } from './errors.mjs';
import { MEMBERSHIP_POLICY, SUBSCRIPTION_POLICY } from './membership-policy.mjs';
import { loadPkixRuntime } from './pkix-runtime.mjs';
const packageRoot = fileURLToPath(new URL('../', import.meta.url));
const configFile = path => isAbsolute(path) ? path : resolve(packageRoot, path);
const optionalFile = async path => path ? readFile(configFile(path), 'utf8') : '';
export function protectedPem(encoded, literal = '') {
  if (!encoded) return literal;
  requireThat(typeof encoded === 'string' && encoded.length <= 32768 &&
    /^[A-Za-z0-9+/]+={0,2}$/.test(encoded), 'INVALID_CREDENTIAL_CONFIGURATION', 503);
  const bytes = Buffer.from(encoded, 'base64');
  requireThat(bytes.toString('base64') === encoded, 'INVALID_CREDENTIAL_CONFIGURATION', 503);
  return bytes.toString('utf8');
}
export async function loadService(env = process.env) {
  requireThat(Number(process.versions.node.split('.')[0]) >= 22, 'NODE_RUNTIME_NOT_SUPPORTED', 503);
  const products = JSON.parse(await optionalFile(env.VP_PRODUCT_CATALOG ?? './config/product-catalog.json') || '[]');
  requireThat(Array.isArray(products) && products.length <= 20, 'INVALID_CATALOG');
  const ids = new Set();
  for (const product of products) {
    requireThat(/^[a-zA-Z0-9._-]{1,128}$/.test(product.id) && !ids.has(product.id) &&
      ((product.type === 0 && product.kind === 'coffee') || (product.kind === 'membership' &&
        ((product.type === 3 && product.policyVersion === MEMBERSHIP_POLICY) ||
        (product.type === 2 && product.policyVersion === SUBSCRIPTION_POLICY &&
          /^[a-zA-Z0-9._-]{1,128}$/.test(product.subscriptionGroupId))))) &&
      product.prices && Object.keys(product.prices).length > 0,
      'INVALID_CATALOG');
    ids.add(product.id);
  }
  const config = { clientId: env.VP_ACCOUNT_CLIENT_ID ?? '', appId: env.VP_APP_ID ?? '', products,
    identitySecret: env.VP_IDENTITY_HMAC_SECRET ?? '', purchaseBindingSecret: env.VP_PURCHASE_HMAC_SECRET ?? '' };
  const keysReady = config.identitySecret.length >= 32 && config.purchaseBindingSecret.length >= 32;
  const accountProvider = new HuaweiAccountProvider({ clientId: config.clientId, clientSecret: env.VP_ACCOUNT_CLIENT_SECRET });
  accountProvider.ready = accountProvider.ready && keysReady;
  const purchaseProvider = new HuaweiPurchaseProvider({ appId: config.appId, packageName: env.VP_PACKAGE_NAME,
    issuerId: env.VP_IAP_ISSUER_ID, keyId: env.VP_IAP_KEY_ID, environment: env.VP_IAP_ENVIRONMENT,
    privateKeyPem: protectedPem(env.VP_IAP_PRIVATE_KEY_BASE64, env.VP_IAP_PRIVATE_KEY_PEM) || await optionalFile(env.VP_IAP_PRIVATE_KEY_FILE),
    rootPem: protectedPem(env.VP_IAP_ROOT_CA_BASE64, env.VP_IAP_ROOT_CA_PEM) || await optionalFile(env.VP_IAP_ROOT_CA_FILE),
    products, ...await loadPkixRuntime() });
  let store = { ready: false, get: async () => { throw new ServiceError('NOT_CONFIGURED', 503); } };
  if ((env.VP_AGC_CREDENTIAL_FILE || env.VP_AGC_CREDENTIAL_JSON || env.PROJECT_CREDENTIAL) && env.VP_AGC_ZONE) {
    store = await connectCloudDb({ credentialFile: env.VP_AGC_CREDENTIAL_FILE,
      credentialJson: env.VP_AGC_CREDENTIAL_JSON || env.PROJECT_CREDENTIAL, zoneName: env.VP_AGC_ZONE });
    if (env.VP_CLOUDDB_VERIFICATION_FILE) await store.loadVerification(configFile(env.VP_CLOUDDB_VERIFICATION_FILE));
  }
  if (keysReady && accountProvider.ready && products.length && env.VP_IAP_VERIFICATION_FILE) {
    purchaseProvider.enableAfterProbe(JSON.parse(await readFile(configFile(env.VP_IAP_VERIFICATION_FILE), 'utf8')));
  }
  let objects={ready:false};
  if(env.VP_OBS_ENDPOINT&&env.VP_OBS_BUCKET&&env.VP_OBS_CREDENTIAL_FILE){
    objects=await connectObs(env);
    if(env.VP_OBS_VERIFICATION_FILE)await objects.enable(JSON.parse(await readFile(configFile(env.VP_OBS_VERIFICATION_FILE),'utf8')));
  }
  return new ValueAddedDomain({ store, accountProvider, purchaseProvider, config, objects });
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
