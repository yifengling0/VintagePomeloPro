// Explicit test-zone writes only; never imported by the deployed function.
import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import { writeFile } from 'node:fs/promises';
import { connectCloudDb } from '../src/clouddb-store.mjs';
const env = process.env;
assert.equal(env.VP_PROBE_ALLOW_WRITES, 'I_APPROVE_TEST_RECORDS');
assert.ok(env.VP_RENEWAL_STORAGE_EVIDENCE_FILE);
const store = await connectCloudDb({ credentialFile: env.VP_AGC_CREDENTIAL_FILE, zoneName: env.VP_AGC_ZONE });
const scope = 'probe-renewal:' + randomUUID();
for (let offset = 0; offset < 300; offset += 25) await store.transaction(async tx => {
  for (let n = offset; n < offset + 25; n++) tx.put(scope + ':' + String(n).padStart(3, '0'), { n }, scope);
});
let cursor = '', seen = [];
do {
  const page = await store.page(scope, cursor, 25);
  if (!page.length) break;
  for (const row of page) { assert.ok(row.key > cursor); cursor = row.key; seen.push(row.value.n); }
} while (seen.length <= 300);
assert.deepEqual(seen, Array.from({ length: 300 }, (_, n) => n));
await writeFile(env.VP_RENEWAL_STORAGE_EVIDENCE_FILE, JSON.stringify({ fingerprint: store.fingerprint,
  testedAt: Date.now(), paginationVerified: true, rows: seen.length }, null, 2), { flag: 'wx', mode: 0o600 });
console.log('CloudDB renewal pagination: 300 ordered rows verified. Remove the isolated probe scope through AGC after review.');
