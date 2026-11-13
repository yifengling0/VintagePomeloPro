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
function snapshot(label='one',asset=null,type='tablet'){
  if (!asset) {
    const config={formatVersion:1,minReaderVersion:1,minWriterVersion:1,requiredFeatures:['cloud-settings-v1'],
      modules:['settings','input_profiles','library','resources'].map(name=>({name,entries:[],schemaVersion:name==='input_profiles'?6:1})),assets:[],label};
    const bytes=Buffer.from(JSON.stringify(config)),digest=hash(bytes);
    return {members:[{name:'manifest.json',bytes}],manifest:{formatVersion:1,scope:'user-settings',deviceClass:type,totalBytes:bytes.length,
      members:[{name:'manifest.json',size:bytes.length,sha256:digest,chunks:[{sha256:digest,size:bytes.length}]}]}};
  }
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
test('settings restore verified JSON and retire replaced configurations',async()=>{
  const f=fixture(),a=await upload(f);const published=await a.publish();assert.equal(published.head.revision,1);
  const current=await f.service.current(user,'tablet');assert.equal(current.head.sha256,hash(JSON.stringify(current.manifest)));
  await f.service.lease(user,a.id);
  const b=await upload(f,snapshot('two'),1);assert.equal(b.missing.length,1);await b.publish();
  const part=current.manifest.members[0].chunks[0];const restored=await f.service.read(user,a.id,part.sha256);assert.equal(hash(Buffer.from(restored.data,'base64')),part.sha256);
  f.advance(600001);await f.service.cleanup(user);assert.equal(f.data.size,1);
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
  for(const bad of ['../secret','userdata/token','/absolute','assets/../../x']){const s=snapshot();s.manifest.members[0].name=bad;assert.throws(()=>validateConfigurationManifest(s.manifest),/CONFIG_SCOPE_UNSUPPORTED/);}
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
test('resource uploads and oversized JSON are refused before reserving storage',async()=>{
  const f=fixture(),s=snapshot('large',Buffer.alloc(CONFIG_CHUNK_BYTES+37,7));
  await assert.rejects(f.service.begin(user,{baseRevision:0,manifest:s.manifest}),/CONFIG_SCOPE_UNSUPPORTED/);
  const large=snapshot();large.manifest.totalBytes=CONFIG_CHUNK_BYTES+1;large.manifest.members[0].size=CONFIG_CHUNK_BYTES+1;
  large.manifest.members[0].chunks=[{sha256:'a'.repeat(64),size:CONFIG_CHUNK_BYTES},{sha256:'b'.repeat(64),size:1}];
  assert.throws(()=>validateConfigurationManifest(large.manifest),/CONFIG_QUOTA/);assert.equal(f.store.rows.size,0);
});
test('configuration digest is checked again before publication',async()=>{
  const f=fixture(),a=await upload(f);await a.publish();
  const altered=snapshot('altered'),next=await upload(f,altered,1);
  const key=[...f.data.keys()].find(k=>k.endsWith(altered.manifest.members[0].chunks[0].sha256));
  f.data.get(key)[0]^=1;await assert.rejects(next.publish(),/CONFIG_HASH_MISMATCH/);assert.equal((await f.service.current(user,'tablet')).head.revision,1);
});
test('unchanged current-head checks have no transactions and retired metadata stays bounded',async()=>{
  const f=fixture();let transactions=0;const transaction=f.store.transaction.bind(f.store);f.store.transaction=action=>{transactions++;return transaction(action);};
  for(let i=0;i<10;i++){const a=await upload(f,snapshot('head'+i),i);await a.publish();}
  const before=transactions;for(let i=0;i<8;i++)await f.service.current(user,'tablet');assert.equal(transactions,before);
  await f.service.cleanup(user);assert.equal([...f.store.rows.keys()].filter(k=>k.startsWith('config-job:')).length,1);assert.equal(f.data.size,1);
});

test('library metadata, font records and wallpaper references cannot be hidden in JSON',async()=>{
  for(const change of [
    value=>value.modules[2].entries.push({store:'vintage_pomelo_pro',key:'library_path_v1',value:'/games'}),
    value=>value.modules[3].entries.push({store:'pomelo_config_resources',key:'fonts_v1',value:'[]'}),
    value=>value.modules[0].entries.push({store:'vintage_pomelo_pro',key:'global_settings_v1',value:'{"pageBackgroundUri":"/local/image"}'})
  ]) {
    const f=fixture(),s=snapshot(),value=JSON.parse(s.members[0].bytes);change(value);
    const bytes=Buffer.from(JSON.stringify(value)),digest=hash(bytes);s.members[0].bytes=bytes;s.manifest.totalBytes=bytes.length;
    Object.assign(s.manifest.members[0],{size:bytes.length,sha256:digest,chunks:[{sha256:digest,size:bytes.length}]});
    const job=await upload(f,s);await assert.rejects(job.publish(),/CONFIG_SCOPE_UNSUPPORTED/);
    assert.equal((await f.service.current(user,'tablet')).head,null);
  }
});

test('an old pending upload cannot upload resources or publish after the scope change',async()=>{
  const f=fixture(),s=snapshot('old pending',Buffer.from('font bytes')),id='11111111-1111-4111-8111-111111111111';
  await f.store.transaction(async tx=>{
    tx.put('config-job:'+id,{id,userId:user,manifest:s.manifest,status:'uploading',expiresAt:999999},'config-jobs:'+user);
    tx.put('config-index:'+user,{heads:{},jobs:[id],retired:[],objects:{}},'config-index');
  });
  const bytes=s.members[1].bytes;
  await assert.rejects(f.service.upload(user,id,{sha256:hash(bytes),data:bytes.toString('base64')}),/CONFIG_SCOPE_UNSUPPORTED/);
  await assert.rejects(f.service.finalize(user,id),/CONFIG_SCOPE_UNSUPPORTED/);assert.equal(f.data.size,0);
});
