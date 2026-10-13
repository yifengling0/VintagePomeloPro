import test from 'node:test';
import assert from 'node:assert/strict';
import { createCloudFunction } from '../src/cloud-function.mjs';

const envelope = (path, method = 'GET', body = '') => ({ protocol: 1, path, method, body, authorization: '' });
test('cloud envelope preserves auth, membership gates and server-derived identity', async () => {
  const calls = [];
  const call = createCloudFunction({
    capabilities: () => ({ account: true, profiles: true, preferences: true, cloudSaves: true }),
    authenticate: async token => { assert.equal(token, 'a'.repeat(43)); return 'server-owner'; },
    putPreferences: async (...args) => { calls.push(args); return { status: 'SAVED', current: { revision: 2 } }; }
  });
  assert.equal(JSON.parse((await call(envelope('/v1/capabilities'))).body).cloudSaves, false);
  const request = envelope('/v1/settings/tablet', 'PUT', JSON.stringify({ baseRevision: 1, payload: {}, userId: 'forged' }));
  assert.equal((await call(request)).status, 401);
  assert.equal(calls.length, 0);
  request.authorization = 'Bearer ' + 'a'.repeat(43);
  const result = await call(request);
  assert.equal(result.status, 200);
  assert.equal(result.protocol, 1);
  assert.equal(calls[0][0], 'server-owner');
  assert.equal(calls[0][1], 'tablet');
  assert.equal(JSON.parse(result.body).current.revision, 2);
});
test('reject malformed envelopes, oversized UTF-8 and unqualified save transport before domain access', async () => {
  let accesses = 0;
  const call = createCloudFunction({ authenticate: () => { accesses++; }, capabilities: () => { accesses++; } });
  const invalid = [null, [], {}, { ...envelope('/v1/capabilities'), protocol: 2 },
    envelope('https://attacker.test/v1/capabilities'), envelope('/v1/../capabilities'),
    envelope('/v1/capabilities?x=1'), { ...envelope('/v1/capabilities'), headers: {} },
    { ...envelope('/v1/capabilities'), authorization: {} }, envelope('/v1/capabilities', 'GET', '{}')];
  for (const value of invalid) assert.equal((await call(value)).status, 400);
  assert.equal((await call(envelope('/v1/auth/exchange', 'POST', '中'.repeat(200000)))).status, 413);
  assert.equal((await call(envelope('/v1/saves/uploads', 'POST', '{}'))).status, 503);
  assert.equal(accesses, 0);
});
test('HTTP trigger unwraps bounded SDK data and ignores outer authorization', async () => {
  let calls = 0;
  const call = createCloudFunction({ capabilities: () => { calls++; return { account: true }; } });
  const request = envelope('/v1/capabilities');
  for (const body of [request, JSON.stringify(request)]) {
    const result = await call({ body, headers: { authorization: 'Bearer forged' }, httpMethod: 'POST' });
    assert.equal(result.status, 200);
    assert.equal(JSON.parse(result.body).account, true);
  }
  assert.equal(calls, 2);
  assert.equal((await call({body:'{'})).status, 400);
  assert.equal((await call({body:JSON.stringify(request),isBase64Encoded:true})).status, 400);
  assert.equal((await call({body:'x'.repeat(768*1024+1)})).status, 413);
  assert.equal((await call({body:envelope('/v1/settings/tablet','PUT','{}'),headers:{authorization:'Bearer forged'}})).status, 401);
});
test('provider exceptions are redacted and service errors preserve their status', async () => {
  const call = createCloudFunction({ begin: async () => { throw Error('secret-provider-response'); } });
  const result = await call(envelope('/v1/auth/begin', 'POST', '{}'));
  assert.deepEqual(result, { protocol: 1, status: 500, body: '{"code":"SERVICE_ERROR"}' });
  assert.equal((await call(envelope('/v1/auth/begin', 'POST', '{'))).status, 400);
});
test('warm instances do not bypass request rate limits', async () => {
  const call = createCloudFunction({ capabilities: () => ({ account: false }) });
  for (let index = 0; index < 300; index++) assert.equal((await call(envelope('/v1/capabilities'))).status, 200);
  assert.equal((await call(envelope('/v1/capabilities'))).status, 429);
});
test('Huawei entry calls its single-argument callback once even when service initialization fails', async () => {
  const { myHandler } = await import('../handler.cjs');
  let callbacks = 0;
  await new Promise((resolve, reject) => {
    const returned = myHandler(envelope('/v1/capabilities'), {}, (...args) => {
      try {
        callbacks++;
        assert.equal(args.length, 1);
        assert.equal(args[0].protocol, 1);
        assert.ok([200, 503].includes(args[0].status));
        resolve();
      } catch (error) { reject(error); }
    });
    assert.equal(returned, undefined);
  });
  assert.equal(callbacks, 1);
});
