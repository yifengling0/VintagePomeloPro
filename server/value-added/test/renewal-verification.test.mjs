import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { loadRenewalVerification } from '../src/main.mjs';

test('bundled renewal report loads without a redundant environment variable; explicit override wins', async () => {
  const root = await mkdtemp(join(tmpdir(), 'vp-renewal-report-'));
  try {
    await mkdir(join(root, 'config'));
    assert.deepEqual(await loadRenewalVerification(undefined, root), {});
    const bundled = { appId: 'app', receiptVerified: false };
    await writeFile(join(root, 'config/renewal-verification.json'), JSON.stringify(bundled));
    assert.deepEqual(await loadRenewalVerification(undefined, root), bundled);
    await writeFile(join(root, 'override.json'), '{"appId":"override"}');
    assert.deepEqual(await loadRenewalVerification('./override.json', root), { appId: 'override' });
    assert.deepEqual(await loadRenewalVerification(join(root, 'override.json'), root), { appId: 'override' });
    await assert.rejects(loadRenewalVerification('missing.json', root), { code: 'ENOENT' });
    for (const invalid of ['invalid JSON', 'null', '[]']) {
      await writeFile(join(root, 'config/renewal-verification.json'), invalid);
      await assert.rejects(loadRenewalVerification(undefined, root));
    }
  } finally { await rm(root, { recursive: true, force: true }); }
});
