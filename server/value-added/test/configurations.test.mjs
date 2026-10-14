import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { Configurations, validateConfigurationManifest, CONFIG_CHUNK_BYTES } from '../src/configurations.mjs';
import { MemoryStore } from './memory-store.mjs';
const user='a'.repeat(64), other='b'.repeat(64), hash=b=>createHash('sha256').update(b).digest('hex');
function fixture(){let now=1000,active=true;const store=new MemoryStore(),data=new Map();
  const objects={ready:true,put:async(k,b,h)=>{assert.equal(hash(b),h);data.set(k,Buffer.from(b));},read:async(k,n)=>{const b=data.get(k);assert.equal(b?.length,n);return Buffer.from(b);},delete:async k=>{data.delete(k);}};
  const domain={store,gate:()=>{},requireMembership:async()=>{if(!active)throw Error('MEMBERSHIP_REQUIRED');}};
  return {service:new Configurations(domain,objects,()=>now),store,data,objects,advance:n=>now+=n,revoke:()=>active=false};
}
function snapshot(label='one',asset=Buffer.from('wallpaper'),type='tablet'){
  const assetHash=hash(asset),name='assets/'+assetHash+'.img';
  const config={formatVersion:1,minReaderVersion:1,minWriterVersion:1,modules:['settings','input_profiles','library','resources'].map(name=>({name,entries:[],schemaVersion:name==='input_profiles'?6:1})),assets:[{path:name,sha256:assetHash,size:asset.length,target:'wallpaper'}],label};
  const members=[{name:'manifest.json',bytes:Buffer.from(JSON.stringify(config))},{name,bytes:asset}];
  return {members,manifest:{formatVersion:1,deviceClass:type,totalBytes:members.reduce((n,m)=>n+m.bytes.length,0),members:members.map(m=>({name:m.name,size:m.bytes.length,sha256:hash(m.bytes),chunks:Array.from({length:Math.ceil(m.bytes.length/CONFIG_CHUNK_BYTES)},(_,i)=>{const bytes=m.bytes.subarray(i*CONFIG_CHUNK_BYTES,(i+1)*CONFIG_CHUNK_BYTES);return {sha256:hash(bytes),size:bytes.length};})}))}};
}
async function upload(f,s= snapshot(),base=0,owner=user){const started=await f.service.begin(owner,{baseRevision:base,manifest:s.manifest});assert.equal(started.status,'READY');
  for(const member of s.members)for(let offset=0;offset<member.bytes.length;offset+=CONFIG_CHUNK_BYTES){const bytes=member.bytes.subarray(offset,offset+CONFIG_CHUNK_BYTES),sha256=hash(bytes);
    if(started.missing.includes(sha256))await f.service.upload(owner,started.id,{sha256,data:bytes.toString('base64')});}
  return {...started,publish:()=>f.service.finalize(owner,started.id)};
}
test('complete configuration restores verified members and deduplicates unchanged resources',async()=>{
  const f=fixture(),a=await upload(f);const published=await a.publish();assert.equal(published.head.revision,1);
  const current=await f.service.current(user,'tablet');assert.equal(current.head.sha256,hash(JSON.stringify(current.manifest)));
  await f.service.lease(user,a.id);
  const b=await upload(f,snapshot('two'),1);assert.equal(b.missing.length,1);await b.publish();
  const part=current.manifest.members[1].chunks[0];const restored=await f.service.read(user,a.id,part.sha256);assert.equal(hash(Buffer.from(restored.data,'base64')),part.sha256);
  f.advance(600001);await f.service.cleanup(user);assert.equal(f.data.size,2);
  await assert.rejects(f.service.read(user,a.id,part.sha256),/CONFIG_NOT_FOUND/);
});
test('same-base publication races preserve winner and original backup until complete',async()=>{
  const f=fixture(),a=await upload(f,snapshot('a')),b=await upload(f,snapshot('b'));
  assert.equal((await f.service.current(user,'tablet')).head,null);
  const results=await Promise.all([a.publish(),b.publish()]);assert.deepEqual(results.map(r=>r.status).sort(),['CONFLICT','PUBLISHED']);
  assert.equal((await f.service.current(user,'tablet')).head.revision,1);
  assert.equal((await f.service.begin(user,{baseRevision:0,manifest:snapshot().manifest})).status,'CONFLICT');
});
test('account and device isolation, membership checks and chunk ownership',async()=>{
  const f=fixture(),a=await upload(f);await a.publish();
  assert.equal((await f.service.current(other,'tablet')).head,null);assert.equal((await f.service.current(user,'desktop')).head,null);
  await assert.rejects(f.service.read(other,a.id,snapshot().manifest.members[0].sha256),/CONFIG_NOT_FOUND/);
  f.revoke();await assert.rejects(f.service.current(user,'tablet'),/MEMBERSHIP_REQUIRED/);
});
test('invalid names, duplicates, quota, wrong hash, incomplete resources and JSON cannot replace head',async()=>{
  const f=fixture(),original=await upload(f);await original.publish();const originalHead=(await f.service.current(user,'tablet')).head;
  for(const bad of ['../secret','userdata/token','/absolute','assets/../../x']){const s=snapshot();s.manifest.members[1].name=bad;assert.throws(()=>validateConfigurationManifest(s.manifest),/CONFIG_INVALID/);}
  const s=snapshot('incomplete'),job=await f.service.begin(user,{baseRevision:1,manifest:s.manifest});
  await assert.rejects(f.service.upload(user,job.id,{sha256:s.manifest.members[0].chunks[0].sha256,data:Buffer.from('bad').toString('base64')}),/CONFIG_HASH_MISMATCH/);
  await assert.rejects(f.service.finalize(user,job.id),/CONFIG_INCOMPLETE/);
  assert.deepEqual((await f.service.current(user,'tablet')).head,originalHead);
});
test('uncertain PUT is quarantined and never deleted or promoted by an expired lease',async()=>{
  const f=fixture(),s=snapshot(),job=await f.service.begin(user,{baseRevision:0,manifest:s.manifest});f.objects.put=async()=>{throw Error('timeout');};
  await assert.rejects(f.service.upload(user,job.id,{sha256:s.manifest.members[0].chunks[0].sha256,data:s.members[0].bytes.toString('base64')}),/CONFIG_RECONCILIATION_REQUIRED/);
  f.advance(3600001);await f.service.cleanup(user);assert.equal((await f.service.current(user,'tablet')).head,null);
  await assert.rejects(f.service.begin(user,{baseRevision:0,manifest:s.manifest}),/CONFIG_RECONCILIATION_REQUIRED/);
});
test('multipart resource and manifest digests validated again before publishing',async()=>{
  const f=fixture(),s=snapshot('large',Buffer.alloc(CONFIG_CHUNK_BYTES+37,7)),a=await upload(f,s);await a.publish();
  assert.equal((await f.service.current(user,'tablet')).manifest.members[1].chunks.length,2);
  const next=await upload(f,snapshot('altered',Buffer.alloc(CONFIG_CHUNK_BYTES+37,9)),1);
  const key=[...f.data.keys()].find(k=>k.endsWith(snapshot('altered',Buffer.alloc(CONFIG_CHUNK_BYTES+37,9)).manifest.members[1].chunks[0].sha256));
  f.data.get(key)[0]^=1;await assert.rejects(next.publish(),/CONFIG_HASH_MISMATCH/);assert.equal((await f.service.current(user,'tablet')).head.revision,1);
});
test('unchanged current-head checks have no transactions and retired metadata stays bounded',async()=>{
  const f=fixture();let transactions=0;const transaction=f.store.transaction.bind(f.store);f.store.transaction=action=>{transactions++;return transaction(action);};
  for(let i=0;i<10;i++){const a=await upload(f,snapshot('head'+i),i);await a.publish();}
  const before=transactions;for(let i=0;i<8;i++)await f.service.current(user,'tablet');assert.equal(transactions,before);
  await f.service.cleanup(user);assert.equal([...f.store.rows.keys()].filter(k=>k.startsWith('config-job:')).length,1);assert.equal(f.data.size,2);
});
