import test from 'node:test';
import assert from 'node:assert/strict';
import { CloudSaves } from '../src/cloud-saves.mjs';
import { CHUNK_BYTES, hash, validateSaveManifest } from '../src/save-manifest.mjs';
import { MemoryStore } from './memory-store.mjs';
const user='a'.repeat(64), other='b'.repeat(64);
function manifest(bytes=Buffer.from('save')){return {schemaVersion:1,totalBytes:bytes.length,
 files:[{relativePath:'save.dat',size:bytes.length,offset:0,sha256:hash(bytes)}],
 chunks:bytes.length?[{index:0,size:bytes.length,sha256:hash(bytes)}]:[]};}
function fixture(){const store=new MemoryStore(),data=new Map();let now=1000000,active=true,puts=0;
 const objects={ready:true,put:async(k,v)=>{puts++;data.set(k,Buffer.from(v));},delete:async k=>{data.delete(k);},download:k=>({url:'https://objects.example/'+k,headers:{},expiresAt:now+60000})};
 const domain={store,requireMembership:async()=>{if(!active)throw Error('MEMBERSHIP_REQUIRED');}};
 return {store,data,objects,saves:new CloudSaves(domain,objects,()=>now),advance:n=>now+=n,revoke:()=>active=false,puts:()=>puts};}
async function start(f,revision=0,slot){slot??=await f.saves.createSlot(user,{label:'My game'});
 return {slot,...await f.saves.begin(user,{slotId:slot.slotId,baseRevision:revision,manifest:manifest()})};}
test('cloud saves verify bytes, atomically reserve quota, publish once and isolate owners',async()=>{
 const f=fixture(),a=await start(f);await assert.rejects(f.saves.upload(other,a.jobId,0,Buffer.from('save')),/SAVE_NOT_FOUND/);
 await f.saves.upload(user,a.jobId,0,Buffer.from('save'));await f.saves.upload(user,a.jobId,0,Buffer.from('save'));assert.equal(f.puts(),1);
 const result=await f.saves.finalize(user,a.jobId);assert.equal(result.status,'PUBLISHED');
 assert.equal((await f.saves.finalize(user,a.jobId)).slot.revision,1);
 let index=await f.saves.list(user);assert.equal(index.usedBytes,4);assert.equal(index.reservedBytes,0);
 assert.equal((await f.saves.download(user,a.slot.slotId,a.jobId)).manifest.totalBytes,4);
 await assert.rejects(f.saves.download(other,a.slot.slotId,a.jobId),/SAVE_NOT_FOUND/);
 await f.saves.deleteVersion(user,a.slot.slotId,a.jobId,{baseRevision:1});
 await f.saves.deleteVersion(user,a.slot.slotId,a.jobId,{baseRevision:1});
 index=await f.saves.list(user);assert.equal(index.usedBytes,0);assert.equal(f.data.size,0);
});
test('cloud save same-base conflict preserves winner, loser quota released once',async()=>{
 const f=fixture(),a=await start(f),b=await start(f,0,a.slot);
 await Promise.all([f.saves.upload(user,a.jobId,0,Buffer.from('save')),f.saves.upload(user,b.jobId,0,Buffer.from('save'))]);
 const results=await Promise.all([f.saves.finalize(user,a.jobId),f.saves.finalize(user,b.jobId)]);
 assert.deepEqual(results.map(r=>r.status).sort(),['CONFLICT','PUBLISHED']);
 const loser=results[0].status==='CONFLICT'?a:b;
 await Promise.all([f.saves.abort(user,loser.jobId),f.saves.abort(user,loser.jobId)]);
 const index=await f.saves.list(user);assert.equal(index.usedBytes,4);assert.equal(index.reservedBytes,0);assert.equal(f.data.size,1);
});
test('multi-instance concurrent reservations and transfers share CloudDB budgets',async()=>{
 const f=fixture(),slot=await f.saves.createSlot(user,{label:'game'});
 const saves2=new CloudSaves({store:f.store,requireMembership:async()=>{}},f.objects,()=>1000000);
 const result=await Promise.allSettled(Array.from({length:8},(_,n)=>(n%2?saves2:f.saves).begin(user,{slotId:slot.slotId,baseRevision:0,manifest:manifest()})));
 assert.equal(result.filter(r=>r.status==='fulfilled').length,2);
 assert.equal((await f.saves.list(user)).reservedBytes,8);
 const jobs=result.filter(r=>r.status==='fulfilled').map(r=>r.value.jobId);
 await f.saves.acquireChunk(user,jobs[0],0);await saves2.acquireChunk(user,jobs[1],0);
 await assert.rejects(f.saves.acquireChunk(user,jobs[0],0),/SAVE_CHUNK_BUSY/);
 await assert.rejects(f.saves.abort(user,jobs[0]),/SAVE_CLEANUP_PENDING/);
 f.advance(3600001);await f.saves.cleanup(user);assert.equal((await f.saves.list(user)).reservedBytes,0);
});
test('bad hash, incomplete/failing uploads, revoked membership cannot publish',async()=>{
 const f=fixture(),a=await start(f);
 await assert.rejects(f.saves.upload(user,a.jobId,0,Buffer.from('evil')),/SAVE_HASH_MISMATCH/);
 await assert.rejects(f.saves.finalize(user,a.jobId),/SAVE_INCOMPLETE/);
 f.advance(120001);f.objects.put=async()=>{throw Error('offline');};
 await assert.rejects(f.saves.upload(user,a.jobId,0,Buffer.from('save')),/SAVE_RECONCILIATION_REQUIRED/);
 assert.equal((await f.saves.list(user)).reservedBytes,4);
 f.revoke();await assert.rejects(f.saves.finalize(user,a.jobId),/MEMBERSHIP_REQUIRED/);
 f.advance(120001);await assert.rejects(f.saves.abort(user,a.jobId),/SAVE_RECONCILIATION_REQUIRED/);
 assert.equal((await f.store.get('save-index:'+user)).reservedBytes,4);
});
test('manifest rejects traversal, prefix/case/Unicode collisions and inconsistent chunks',()=>{
 const base=manifest();for(const name of ['../save','/save','c:save','a\\b','a/../b','e\u0301']){
  const bad=structuredClone(base);bad.files[0].relativePath=name;assert.throws(()=>validateSaveManifest(bad));}
 for(const pair of [['a','a/b'],['A','a'],['a/b','a']]){
  const bad=manifest(Buffer.alloc(0));bad.files=[...pair.map(relativePath=>({relativePath,size:0,offset:0,sha256:hash('')}))];
  assert.throws(()=>validateSaveManifest(bad),/SAVE_PATH_COLLISION/);}
 const bad=manifest();bad.chunks[0].size=CHUNK_BYTES+1;assert.throws(()=>validateSaveManifest(bad));
 assert.equal(validateSaveManifest(manifest(Buffer.alloc(0))).chunks.length,0);
});

test('expired acquired body lease cannot write after abort; uncertain in-flight PUT is quarantined',async()=>{
 const f=fixture(),a=await start(f),lease=await f.saves.acquireChunk(user,a.jobId,0);
 f.advance(120001);await f.saves.abort(user,a.jobId);
 await assert.rejects(f.saves.upload(user,a.jobId,0,Buffer.from('save'),lease),/SAVE_UPLOAD_EXPIRED/);
 assert.equal(f.puts(),0);assert.equal(f.data.size,0);
 const b=await start(f);let resume,entered;
 const started=new Promise(resolve=>entered=resolve),paused=new Promise(resolve=>resume=resolve);
 f.objects.put=async(key,bytes)=>{entered();await paused;f.data.set(key,Buffer.from(bytes));};
 const sending=f.saves.upload(user,b.jobId,0,Buffer.from('save'));await started;
 f.advance(3600001);await assert.rejects(f.saves.abort(user,b.jobId),/SAVE_RECONCILIATION_REQUIRED/);
 assert.equal((await f.saves.list(user)).reservedBytes,4);
 resume();await sending;await f.saves.abort(user,b.jobId);assert.equal(f.data.size,0);
 assert.equal((await f.saves.list(user)).reservedBytes,0);
});
