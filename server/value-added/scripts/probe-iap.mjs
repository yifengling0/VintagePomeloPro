// Read-only provider query. No grant, server delivery confirmation, client finish, or payment is performed.
import { readFile, writeFile } from 'node:fs/promises';
import { HuaweiPurchaseProvider } from '../src/huawei-providers.mjs';
import { requireThat } from '../src/errors.mjs';
import { loadPkixRuntime } from '../src/pkix-runtime.mjs';
import { pathToFileURL } from 'node:url';
import { resolve } from 'node:path';
import { protectedPem } from '../src/main.mjs';
const env = process.env;
requireThat(env.VP_IAP_ENVIRONMENT === 'SANDBOX' && env.VP_IAP_PROBE_RECEIPT_FILE && env.VP_IAP_VERIFICATION_FILE,
  'SANDBOX_PROBE_NOT_CONFIGURED');
const config = { appId: env.VP_APP_ID, packageName: env.VP_PACKAGE_NAME,
  issuerId: env.VP_IAP_ISSUER_ID, keyId: env.VP_IAP_KEY_ID, environment: env.VP_IAP_ENVIRONMENT,
  privateKeyPem: protectedPem(env.VP_IAP_PRIVATE_KEY_BASE64, env.VP_IAP_PRIVATE_KEY_PEM) || await readFile(env.VP_IAP_PRIVATE_KEY_FILE, 'utf8'),
  rootPem: protectedPem(env.VP_IAP_ROOT_CA_BASE64, env.VP_IAP_ROOT_CA_PEM) || await readFile(env.VP_IAP_ROOT_CA_FILE),
  products: JSON.parse(await readFile(env.VP_PRODUCT_CATALOG ?? new URL('../config/product-catalog.json', import.meta.url), 'utf8')),
  ...await loadPkixRuntime(env.VP_IAP_RUNTIME_DIRECTORY ? pathToFileURL(resolve(env.VP_IAP_RUNTIME_DIRECTORY) + '/') : undefined) };
const provider = new HuaweiPurchaseProvider(config);
const receipt = await readFile(env.VP_IAP_PROBE_RECEIPT_FILE, 'utf8');
const type = JSON.parse(receipt).type;
await provider.verify(receipt, type);
await writeFile(env.VP_IAP_VERIFICATION_FILE, JSON.stringify({ providerFingerprint: provider.fingerprint(),
  queryAndSignatureVerified: true, environment: 'SANDBOX', checkedAt: new Date().toISOString() }, null, 2),
  { flag: 'wx', mode: 0o600 });
console.log('Sandbox latest-order query and certificate/JWS verification passed. Receipt was not logged or retained.');
