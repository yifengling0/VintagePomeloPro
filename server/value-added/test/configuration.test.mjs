import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { protectedPem, protectedPushCredential, membershipProductsVerified } from '../src/main.mjs';
import { loadPkixRuntime, PKIX_FILES } from '../src/pkix-runtime.mjs';

test('single-line protected PEM configuration preserves newlines and rejects malformed or noncanonical input', () => {
  const pem = '-----BEGIN PRIVATE KEY-----\nfixture\n-----END PRIVATE KEY-----\n';
  assert.equal(protectedPem(Buffer.from(pem).toString('base64')), pem);
  assert.equal(protectedPem(undefined, pem), pem);
  for (const bad of ['a', 'YQ', '====', 'YQ==\n', 'A'.repeat(32769)]) {
    assert.throws(() => protectedPem(bad), /INVALID_CREDENTIAL_CONFIGURATION/);
  }
});
test('chunked push credentials preserve the complete key and reject partial or conflicting deployments', () => {
  const pem = '-----BEGIN PRIVATE KEY-----\n' + 'key-fixture'.repeat(300) + '\n-----END PRIVATE KEY-----\n';
  const base64 = Buffer.from(pem).toString('base64');
  const env = Object.fromEntries([1, 2, 3].map((n) => ['VP_PUSH_PRIVATE_KEY_BASE64_' + n, base64.slice((n - 1) * 1800, n * 1800)]));
  const json = JSON.stringify({ project_id: '12345678', key_id: 'fixture', sub_account: 'service' });
  assert.equal(protectedPushCredential(json, env).private_key, pem);
  for (const missing of [1, 2, 3]) {
    const partial = { ...env }; delete partial['VP_PUSH_PRIVATE_KEY_BASE64_' + missing];
    assert.throws(() => protectedPushCredential(json, partial), /INVALID_CREDENTIAL_CONFIGURATION/);
  }
  assert.throws(() => protectedPushCredential(JSON.stringify({ private_key: pem }), env), /INVALID_CREDENTIAL_CONFIGURATION/);
  assert.deepEqual(protectedPushCredential(json), JSON.parse(json));
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
