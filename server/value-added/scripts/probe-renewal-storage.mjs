// Explicit test-zone writes only; never imported by the deployed function.
import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';
import { writeFile } from 'node:fs/promises';
import { connectCloudDb } from '../src/clouddb-store.mjs';
const env = process.env;
assert.equal(env.VP_PROBE_ALLOW_WRITES, 'I_APPROVE_TEST_RECORDS');
assert.equal(env.VP_AGC_ZONE, 'PomeloAccountTest', 'Probe is restricted to the test zone');
assert.ok(env.VP_RENEWAL_STORAGE_EVIDENCE_FILE);
const store = await connectCloudDb({ credentialFile: env.VP_AGC_CREDENTIAL_FILE, zoneName: env.VP_AGC_ZONE });
const scope = 'config-probe-renewal:' + randomUUID();
const keys = Array.from({length:300}, (_, n) => scope + ':' + String(n).padStart(3,'0'));
let cursor = '', seen = [];
let cleaned = false;
try {
for (let offset = 0; offset < 300; offset += 25) await store.transaction(async tx => {
  for (let n = offset; n < offset + 25; n++) tx.put(keys[n], { n }, scope);
});
do {
  const page = await store.page(scope, cursor, 25);
  if (!page.length) break;
  for (const row of page) { assert.ok(row.key > cursor); cursor = row.key; seen.push(row.value.n); }
} while (seen.length <= 300);
assert.deepEqual(seen, Array.from({ length: 300 }, (_, n) => n));
} finally {
  for (let offset = 0; offset < 300; offset += 25) await store.transaction(async tx => {
    for (const key of keys.slice(offset,offset+25)) tx.delete(key);
  });
  assert.deepEqual(await store.page(scope, '', 25), []);
  cleaned = true;
}
await writeFile(env.VP_RENEWAL_STORAGE_EVIDENCE_FILE, JSON.stringify({ fingerprint: store.fingerprint,
  testedAt: Date.now(), paginationVerified: true, rows: seen.length, probeRowsRemoved: cleaned }, null, 2), { flag: 'wx', mode: 0o600 });
console.log('CloudDB renewal pagination: 300 ordered rows verified; isolated probe records removed.');
