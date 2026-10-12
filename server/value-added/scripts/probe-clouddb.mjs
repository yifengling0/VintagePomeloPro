// Run only against an authorized, non-live AGC zone before enabling purchases. This writes test records.
import assert from 'node:assert/strict';
import { readFile, writeFile } from 'node:fs/promises';
import { randomUUID } from 'node:crypto';
import { connectCloudDb, SDK_VERSION, LOCK_KEY, ValueAddedRecord } from '../src/clouddb-store.mjs';
import { requireThat } from '../src/errors.mjs';
const env = process.env;
requireThat(env.VP_PROBE_ALLOW_WRITES === 'I_APPROVE_TEST_RECORDS' && env.VP_CLOUDDB_VERIFICATION_FILE,
  'PROBE_REQUIRES_EXPLICIT_TEST_ZONE_APPROVAL');
const store = await connectCloudDb({ credentialFile: env.VP_AGC_CREDENTIAL_FILE, zoneName: env.VP_AGC_ZONE });
const pendingFile = env.VP_CLOUDDB_VERIFICATION_FILE + '.pending';
if (process.argv.includes('--expect-disconnected')) {
  const report = JSON.parse(await readFile(pendingFile, 'utf8'));
  assert.equal(report.fingerprint, store.fingerprint);
  await assert.rejects(store.get(LOCK_KEY));
  await assert.rejects(store.transaction(async tx => tx.put('probe:offline', { bad: true }, 'probe')));
  report.disconnectedFailsClosed = true;
  await writeFile(env.VP_CLOUDDB_VERIFICATION_FILE, JSON.stringify(report, null, 2), { mode: 0o600, flag: 'wx' });
  console.log('Disconnected access rejected. Restore network before starting the service.');
} else {
  assert.ok(await store.get(LOCK_KEY), 'Pre-create the lock row once in the stopped zone; never upsert it on startup.');
  const prefix = 'probe:' + randomUUID();
  const key = prefix + ':cas';
  await store.transaction(async tx => tx.put(key, { revision: 0 }, 'probe'));
  let arrivals = 0, release;
  const barrier = new Promise(resolve => { release = resolve; });
  const firstAttempt = [true, true];
  const cas = await Promise.all([0,1].map(index => store.transaction(async tx => {
    const value = await tx.get(key);
    if (value.revision !== 0) return 'CONFLICT';
    // Both first attempts must read revision 0 before either can commit. SDK retries skip the barrier.
    if (firstAttempt[index]) {
      firstAttempt[index] = false; arrivals++; if (arrivals === 2) release();
      await Promise.race([barrier, new Promise((_, reject) => setTimeout(() => reject(Error('CAS overlap not observed')), 10000))]);
    }
    tx.put(key, { revision: 1 }, 'probe'); return 'SAVED';
  })));
  assert.equal(arrivals, 2);
  assert.deepEqual(cas.sort(), ['CONFLICT','SAVED']);
  const order = prefix + ':order', entitlement = prefix + ':entitlement';
  const grant = owner => store.transaction(async tx => {
    const old = await tx.get(order), oldEntitlement = await tx.get(entitlement);
    if (old) { assert.equal(old.owner, owner); return old; }
    await new Promise(resolve => setTimeout(resolve, 100));
    tx.put(order, { owner, grants: 1 }, 'probe');
    tx.put(entitlement, { count: (oldEntitlement?.count ?? 0) + 1 }, 'probe');
    return { owner, grants: 1 };
  });
  await Promise.all([grant('owner-a'), grant('owner-a')]);
  assert.equal((await store.get(entitlement)).count, 1);
  await assert.rejects(grant('owner-b'));
  assert.equal((await store.get(order)).owner, 'owner-a');
  await assert.rejects(store.transaction(async tx => {
    tx.put(prefix + ':rollback-order', {}, 'probe');
    tx.put(prefix + ':rollback-entitlement', {}, 'probe');
    throw Error('deliberate rollback');
  }));
  assert.equal(await store.get(prefix + ':rollback-order'), null);
  assert.equal(await store.get(prefix + ':rollback-entitlement'), null);
  // Force rejection of a multi-object transaction by the real CloudDB server, not by the callback.
  // An unknown object type passes the SDK's structural model checks but must fail at the service.
  class AbsentProbeType extends ValueAddedRecord { getClassName() { return 'AbsentProbe' + prefix.replace(/[^a-zA-Z0-9]/g, ''); } }
  const rollbackKey = prefix + ':server-rollback';
  const lockBefore = await store.get(LOCK_KEY);
  let sentTransaction = false, remoteRejection = false;
  const sender = store.zone.requestSender;
  assert.ok(sender && typeof sender.sendPostRequest === 'function', 'SDK transport changed; do not waive this gate.');
  const originalSend = sender.sendPostRequest;
  sender.sendPostRequest = function (...args) {
    if (args[2]?.transactionList) sentTransaction = true;
    return originalSend.apply(this, args).catch(error => {
      if (args[2]?.transactionList && error?.response?.status && error?.response?.data?.errorCodeV2) remoteRejection = true;
      throw error;
    });
  };
  let rejected = false;
  try {
    const result = await Promise.race([store.zone.runTransaction({ apply: async tx => {
      const [lock] = await tx.executeQuery(store.query(LOCK_KEY));
      const revision = JSON.parse(lock.json).revision;
      tx.executeUpsert([new ValueAddedRecord(LOCK_KEY, 'internal', JSON.stringify({revision: revision + 1})),
        new ValueAddedRecord(rollbackKey, 'probe', '{}')]);
      tx.executeUpsert([new AbsentProbeType(prefix + ':invalid', 'probe', '{}')]);
      return true;
    } }), new Promise((_, reject) => { const timer = setTimeout(() => reject(Error('probe timed out')), 30000); timer.unref(); })]);
    rejected = result !== true;
  } catch { rejected = true; }
  finally { sender.sendPostRequest = originalSend; }
  assert.ok(sentTransaction && remoteRejection && rejected, 'Must observe an attempted server transaction and rejection.');
  assert.equal(await store.get(rollbackKey), null, 'Server rejected transaction must not partially write records.');
  assert.deepEqual(await store.get(LOCK_KEY), lockBefore, 'Rejected transaction must not advance its lock row.');
  const report = { fingerprint: store.fingerprint, sdkVersion: SDK_VERSION, checkedAt: new Date().toISOString(),
    sameBaseConflict: true, duplicateOrder: true, callbackAbort: true, serverRollback: true, ownerImmutable: true, disconnectedFailsClosed: false };
  await writeFile(pendingFile, JSON.stringify(report, null, 2), { mode: 0o600, flag: 'wx' });
  console.log('Online transaction checks passed. No launch gate yet: block AGC egress and run --expect-disconnected.');
}
