// Isolated storage/protocol probe. Does not validate user login or purchase entitlement.
import { createHash, randomBytes } from 'node:crypto';
import { writeFile } from 'node:fs/promises';
import { connectCloudDb } from '../src/clouddb-store.mjs';
import { ConfigurationDatabaseObjects } from '../src/configuration-database-objects.mjs';
import { Configurations, CONFIG_CHUNK_BYTES } from '../src/configurations.mjs';
import assert from 'node:assert/strict';
const hash=bytes=>createHash('sha256').update(bytes).digest('hex');
const owner=hash(randomBytes(32)), keys=new Set(); let store;
try {
  store=await connectCloudDb({credentialFile:process.env.VP_CONFIG_PROBE_CREDENTIAL,zoneName:process.env.VP_CONFIG_PROBE_ZONE});
  await store.loadVerification(process.env.VP_CONFIG_PROBE_VERIFICATION);
  const objects=new ConfigurationDatabaseObjects(store);
  const service=new Configurations({store,gate(){},requireMembership:async()=>{}},objects);
  async function publish(label,base) {
    const value={formatVersion:1,minReaderVersion:1,minWriterVersion:1,writtenByAppVersion:'1.4.8',requiredFeatures:['cloud-settings-v1'],
      modules:['settings','input_profiles','library','resources'].map(name=>({name,schemaVersion:name==='input_profiles'?6:1,entries:[]})),
      assets:[],probe:label};
    const members=[{name:'manifest.json',bytes:Buffer.from(JSON.stringify(value))}];
    const manifest={formatVersion:1,scope:'user-settings',deviceClass:'tablet',totalBytes:members.reduce((n,m)=>n+m.bytes.length,0),members:members.map(m=>({
      name:m.name,size:m.bytes.length,sha256:hash(m.bytes),chunks:Array.from({length:Math.ceil(m.bytes.length/CONFIG_CHUNK_BYTES)},(_,i)=>{
        const bytes=m.bytes.subarray(i*CONFIG_CHUNK_BYTES,(i+1)*CONFIG_CHUNK_BYTES);return {sha256:hash(bytes),size:bytes.length};})}))};
    keys.add('config-index:'+owner);
    const begun=await service.begin(owner,{baseRevision:base,manifest});assert.equal(begun.status,'READY');keys.add('config-job:'+begun.id);
    for (const member of members) for(let offset=0;offset<member.bytes.length;offset+=CONFIG_CHUNK_BYTES) {
      const bytes=member.bytes.subarray(offset,offset+CONFIG_CHUNK_BYTES),digest=hash(bytes);keys.add('config-object:'+owner+':'+digest);
      if(begun.missing.includes(digest))await service.upload(owner,begun.id,{sha256:digest,data:bytes.toString('base64')});
    }
    const result=await service.finalize(owner,begun.id);assert.equal(result.status,'PUBLISHED');return {manifest,result,missing:begun.missing.length};
  }
  const first=await publish('first',0),second=await publish('second',1);assert.equal(first.missing,1);assert.equal(second.missing,1);
  const current=await service.current(owner,'tablet');assert.equal(current.head.revision,2);
  for (const part of second.manifest.members[0].chunks) {
    const read=await service.read(owner,current.head.id,part.sha256);assert.equal(hash(Buffer.from(read.data,'base64')),part.sha256);
  }
  const report={checkedAt:new Date().toISOString(),isolatedOwner:true,fullChunkBytes:CONFIG_CHUNK_BYTES,
    realCloudDbWriteRead:true,completeAtomicPublication:true,revision:current.head.revision,settingsOnly:true,resourceChunksUploaded:0,
    identityAndMembershipValidation:'not tested by this isolated probe'};
  await store.transaction(async tx=>{for(const key of keys)await tx.get(key);for(const key of keys)tx.delete(key);});
  for(const key of keys)assert.equal(await store.get(key),null);report.realCloudDbDelete=true;
  if(process.env.VP_CONFIG_PROBE_REPORT)await writeFile(process.env.VP_CONFIG_PROBE_REPORT,JSON.stringify(report,null,2)+'\n');
  console.log(JSON.stringify(report));
  process.exit(0);
} catch(error) {
  console.error('Configuration CloudDB probe failed: '+(/^[A-Z_0-9]{1,64}$/.test(error.code??error.message)?error.code??error.message:'PROBE_FAILED'));
  process.exitCode=1;
}
