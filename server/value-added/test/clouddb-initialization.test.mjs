import test from 'node:test';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';

test('the actual pinned SDK can reconnect after service initialization fails without duplicating its global client', async () => {
  // The vendor SDK owns a credential refresh interval. Isolate it so it cannot
  // keep the test runner alive after all assertions have completed.
  const script=`
  import assert from 'node:assert/strict';
  import {connectCloudDb} from ${JSON.stringify(new URL('../src/clouddb-store.mjs', import.meta.url).href)};
  const credential={type:'project_client_id',project_id:'12345678',region:'CN',client_id:'fixture-client',client_secret:'fixture-secret'};
  const connect=()=>connectCloudDb({credentialJson:JSON.stringify(credential),zoneName:'FixtureZone'});
  const first=await connect();
  const second=await connect();
  assert.equal(first.fingerprint,second.fingerprint);
  assert.equal(second.ready,false, 'SDK reuse cannot grant verification readiness');
  credential.project_id='87654321';
  await assert.rejects(connect(),/CLOUDDB_CLIENT_CONFLICT/,'another project must not silently reuse the warm client');
  process.exit(0);
  `;
  const child=spawnSync(process.execPath,['--input-type=module','-e',script],{encoding:'utf8',timeout:10000});
  assert.equal(child.status,0,child.error?.message ?? child.stderr);
});
