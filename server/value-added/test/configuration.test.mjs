import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { protectedPem, membershipProductsVerified } from '../src/main.mjs';
import { loadPkixRuntime, PKIX_FILES } from '../src/pkix-runtime.mjs';

test('single-line protected PEM configuration preserves newlines and rejects malformed or noncanonical input', () => {
  const pem = '-----BEGIN PRIVATE KEY-----\nfixture\n-----END PRIVATE KEY-----\n';
  assert.equal(protectedPem(Buffer.from(pem).toString('base64')), pem);
  assert.equal(protectedPem(undefined, pem), pem);
  for (const bad of ['a', 'YQ', '====', 'YQ==\n', 'A'.repeat(32769)]) {
    assert.throws(() => protectedPem(bad), /INVALID_CREDENTIAL_CONFIGURATION/);
  }
});
test('bundled PKIX runtime rejects modified binary before executing or changing permissions', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'vp-pkix-integrity-'));
  const url = pathToFileURL(directory + '/');
  try {
    assert.deepEqual(await loadPkixRuntime(url), {});
    const files = Object.fromEntries(PKIX_FILES.map(name => [name, '0'.repeat(64)]));
    await writeFile(join(directory, 'manifest.json'), JSON.stringify({ protocol: 1, opensslVersion: '3.5.9', files }));
    await writeFile(join(directory, 'openssl-verify'), '#!/bin/sh\nexit 0\n');
    await assert.rejects(loadPkixRuntime(url), /PKIX_NOT_CONFIGURED/);
  } finally { await rm(directory, { recursive: true, force: true }); }
});
test('release evidence covers every offered membership SKU while retired SKUs remain recoverable', () => {
  const products = [{id:'auto',type:2,kind:'membership'}, {id:'once',type:3,kind:'membership',purchasesEnabled:false}];
  assert.equal(membershipProductsVerified(products, {membershipProductId:'auto'}), true);
  assert.equal(membershipProductsVerified(products, {oneTimeProductId:'once'}), false);
  products[1].purchasesEnabled = true;
  assert.equal(membershipProductsVerified(products, {membershipProductId:'auto'}), false);
  assert.equal(membershipProductsVerified(products, {membershipProductId:'auto',oneTimeProductId:'once'}), true);
  assert.equal(membershipProductsVerified([], {}), false);
});
