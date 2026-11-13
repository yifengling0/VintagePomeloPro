import test from 'node:test';
import assert from 'node:assert/strict';
import { createHmac, randomUUID } from 'node:crypto';
import { createPushReceiptFunction, createFunctionGraphReceiptEntry } from '../src/push-receipt-function.mjs';
import { createPushReceiptCallback } from '../src/push-receipts.mjs';
import { MemoryStore } from './memory-store.mjs';

function fixture() {
  const store = new MemoryStore(), now = Date.now(), key = Buffer.alloc(32, 1), username = 'fixture';
  const callback = createPushReceiptCallback({ store, clock: () => now },
    { username, secret: key.toString('base64'), packageName: 'com.test' });
  const event = (body = '{"statuses":[]}', base64 = false, at = now) => {
    const nonce = randomUUID();
    return { httpMethod: 'POST', path: '/push/receipts', isBase64Encoded: base64,
      body: base64 ? Buffer.from(body).toString('base64') : body,
      headers: { 'Content-Type': 'application/json', 'X-Huawei-Callback-Id': 'timestamp=' + at + '; nonce=' + nonce +
        '; value=' + createHmac('sha256', key).update(String(at) + nonce + username).digest('base64') } };
  };
  return { store, now, callback, event };
}

test('HTTPS gateway contract preserves Huawei HMAC, decodes base64 once, and rejects unrelated requests', async () => {
  const f = fixture(), run = createPushReceiptFunction(async () => f.callback);
  for (const base64 of [false, true]) {
    const result = await run(f.event(undefined, base64));
    assert.equal(result.statusCode, 200);
    assert.equal(result.isBase64Encoded, false);
    assert.equal(JSON.parse(result.body).code, '0');
  }
  assert.equal((await f.store.list('push-receipt-nonces')).length, 2);
  for (const request of [null, { ...f.event(), httpMethod: 'GET' }, { ...f.event(), path: '/v1/capabilities' }]) {
    assert.equal((await run(request)).statusCode, 404);
  }
  assert.equal((await run({ ...f.event(), headers: {} })).statusCode, 403);
  assert.equal((await run(f.event(undefined, false, f.now - 300001))).statusCode, 403);
  for (const body of ['null', '{', '[]', '{}', '"text"', '{"statuses":[null]}']) assert.equal((await run(f.event(body))).statusCode, 400);
  const forged = f.event(); forged.headers['X-Huawei-Callback-Id'] = forged.headers['X-Huawei-Callback-Id'].replace(/value=./, 'value=!');
  assert.equal((await run(forged)).statusCode, 403);
  const duplicate = f.event(); duplicate.headers['x-huawei-callback-id'] = duplicate.headers['X-Huawei-Callback-Id'];
  assert.equal((await run(duplicate)).statusCode, 403);
});

test('malformed and oversized gateway bodies fail before CloudDB initialization', async () => {
  const f = fixture(); let initialized = 0;
  const run = createPushReceiptFunction(async () => { initialized++; return f.callback; });
  for (const base64 of [false, true]) assert.equal((await run(f.event('x'.repeat(65537), base64))).statusCode, 413);
  for (const body of ['!!!=', 'e30', '/w==']) {
    assert.equal((await run({ ...f.event(), body, isBase64Encoded: true })).statusCode, 400);
  }
  const compressed = f.event(); compressed.headers['Content-Encoding'] = 'gzip';
  assert.equal((await run(compressed)).statusCode, 415);
  assert.equal(initialized, 0);
});

test('initialization and storage failures are retryable and redacted; warm calls share successful initialization', async () => {
  const f = fixture(); let initialized = 0;
  const run = createPushReceiptFunction(async () => {
    if (++initialized === 1) throw new Error('sensitive-credentials-and-sdk-error');
    return f.callback;
  });
  const unavailable = await run(f.event());
  assert.equal(unavailable.statusCode, 503);
  assert.ok(!JSON.stringify(unavailable).includes('sensitive'));
  const results = await Promise.all([run(f.event()), run(f.event())]);
  assert.ok(results.every(result => result.statusCode === 200));
  assert.equal(initialized, 2);
  const failedStore = createPushReceiptFunction(async () => async () => { throw new Error('private-storage-detail'); });
  assert.equal((await failedStore(f.event())).statusCode, 503);
});

test('FunctionGraph callback uses error/result exactly once and returns no Promise', async () => {
  const f = fixture(), entry = createFunctionGraphReceiptEntry(async () => f.callback);
  let calls = 0;
  await new Promise((resolve, reject) => {
    assert.equal(entry(f.event(undefined, true), {}, (...args) => {
      try { calls++; assert.equal(args.length, 2); assert.equal(args[0], null);
        assert.equal(typeof args[1], 'string'); assert.equal(JSON.parse(args[1]).statusCode, 200); resolve(); }
      catch (error) { reject(error); }
    }), undefined);
  });
  assert.equal(calls, 1);
});
