// Runs the actual pinned AGC SDK through a local HTTPS mock. No AGC credentials or remote service are used.
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { mkdtempSync, writeFileSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { execFileSync } from 'node:child_process';
import { createServer } from 'node:https';
import { CloudDbStore, ValueAddedRecord, LOCK_KEY } from '../src/clouddb-store.mjs';
const root = process.env.VP_TEST_MODULE_ROOT ? resolve(process.env.VP_TEST_MODULE_ROOT, 'package.json') :
  new URL('../package.json', import.meta.url);
const require = createRequire(root), directory = mkdtempSync(join(tmpdir(), 'vp-agc-transport-'));
const originalCwd = process.cwd();
let server;
try {
  process.chdir(directory);
  process.env.NO_PROXY = '127.0.0.1'; process.env.no_proxy = '127.0.0.1';
  execFileSync('openssl',['req','-x509','-newkey','ec','-pkeyopt','ec_paramgen_curve:prime256v1','-nodes',
    '-keyout','key.pem','-out','cert.pem','-days','1','-subj','/CN=127.0.0.1','-addext','subjectAltName=IP:127.0.0.1'],
    {cwd:directory,stdio:'pipe'});
  const agc = require('@agconnect/common-server'), sdk = require('@agconnect/database-server');
  assert.equal(require('axios/package.json').version, '0.34.0');
  assert.match(require('uuid').v1(), /^[a-f0-9-]{36}$/);
  const rows = new Map([[LOCK_KEY,{key:LOCK_KEY,scope:'internal',json:'{"revision":0}',naturalbase_version:'1'}]]);
  let posts = 0, transactions = 0;
  server = createServer({key:readFileSync('key.pem'),cert:readFileSync('cert.pem')}, async (request,response)=>{
    const chunks=[];for await(const chunk of request)chunks.push(chunk);
    const raw=Buffer.concat(chunks).toString(), data=raw?JSON.parse(raw):null;
    response.setHeader('Content-Type','application/json');
    if(request.url==='/fail'){response.writeHead(409);response.end(JSON.stringify({errorCodeV2:'synthetic-conflict'}));return;}
    if(request.url==='/slow'){setTimeout(()=>response.end('{}'),200);return;}
    assert.equal(request.headers.authorization,'Bearer synthetic-local-token');
    assert.match(request.headers['x-request-id'],/^[a-f0-9]{32}$/);
    if(request.url.endsWith('/queries')){
      const key=data.queryConditions.find(condition=>condition.fieldName==='key').value;
      response.end(JSON.stringify({data:rows.has(key)?[rows.get(key)]:[]}));return;
    }
    if(request.url.endsWith('/transaction')){
      transactions++;assert.ok(data.needVerifyObjectsList.length);
      for(const operation of data.transactionList)for(const record of operation.objects)
        rows.set(record.key,{...record,naturalbase_version:String(Number(rows.get(record.key)?.naturalbase_version??0)+1)});
      response.end('{}');return;
    }
    posts++;response.end(JSON.stringify({method:request.method,data}));
  });
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  const origin='https://127.0.0.1:'+server.address().port;
  const client=agc.getHttpClient({casFile:[join(directory,'cert.pem')],timeout:1000});
  const result=await client.post(origin+'/echo',{nested:{value:7}},null,{Authorization:'Bearer synthetic-local-token'});
  assert.deepEqual(result.data,{method:'POST',data:{nested:{value:7}}});
  await assert.rejects(client.get(origin+'/fail',null,{}),error=>error.response.status===409&&error.response.data.errorCodeV2==='synthetic-conflict');
  const fast=agc.getHttpClient({casFile:[join(directory,'cert.pem')],timeout:20});
  await assert.rejects(fast.get(origin+'/slow',null,{}),error=>['ECONNABORTED','ETIMEDOUT'].includes(error.code));
  const credential={getRegion:()=> 'CN',getClientId:()=> 'synthetic-client',getProjectId:()=> 'synthetic-project',
    getAccessToken:async()=> 'synthetic-local-token'};
  const zone=new sdk.CloudDBZone({getCredential:()=>credential},new sdk.CloudDBZoneConfig('SyntheticZone'));
  zone.requestSender.httpClient=client;
  zone.requestSender.requestTemplate.url=origin+'/';zone.requestSender.requestTemplate.backupUrl=origin+'/';
  const store=new CloudDbStore(zone,sdk.CloudDBZoneQuery,'synthetic-fingerprint');
  assert.deepEqual(await store.get(LOCK_KEY),{revision:0});
  await store.transaction(async tx=>{
    assert.equal(await tx.get('test:order'),null);
    tx.put('test:order',{owner:'test',count:1},'test');
  });
  assert.deepEqual(await store.get('test:order'),{owner:'test',count:1});
  assert.equal((await store.get(LOCK_KEY)).revision,1);assert.equal(transactions,1);assert.equal(posts,1);
  console.log('PASS: actual AGC SDK + axios 0.34.0 / uuid 11.1.1 HTTPS request, auth/requestId, response/error/timeout, query and transaction serialization');
} catch(error) {console.error(error);process.exitCode=1;}
finally {
  if(server){server.closeAllConnections();await new Promise(resolve=>server.close(resolve));}
  process.chdir(originalCwd);rmSync(directory,{recursive:true,force:true});
}
// Official SDK logging timers may outlive requests; all connections/test files are already closed above.
process.exit(process.exitCode??0);
