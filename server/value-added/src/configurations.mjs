import { createHash, randomUUID } from 'node:crypto';
import { requireThat, ServiceError } from './errors.mjs';
import { CONFIGURATION_BYTES, CONFIGURATION_SCOPE, validateCloudSettings } from './configuration-policy.mjs';
export const CONFIG_CHUNK_BYTES = 262144;
export const CONFIG_BYTES = CONFIGURATION_BYTES;
const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const uid = value => typeof value === 'string' && /^[a-f0-9-]{36}$/.test(value);
const sha = value => typeof value === 'string' && /^[a-f0-9]{64}$/.test(value);
const device = value => ['tablet','desktop'].includes(value);
const indexKey = user => 'config-index:' + user;
const jobKey = id => 'config-job:' + id;
const fresh = () => ({ heads: {}, jobs: [], retired: [], objects: {} });
export function validateConfigurationManifest(value) {
  requireThat(value?.scope === CONFIGURATION_SCOPE && Array.isArray(value.members) && value.members.length === 1 &&
    value.members[0]?.name === 'manifest.json', 'CONFIG_SCOPE_UNSUPPORTED');
  requireThat(value?.formatVersion === 1 && device(value.deviceClass) && Array.isArray(value.members) &&
    value.members.length >= 1 && value.members.length <= 257, 'CONFIG_INVALID');
  let total = 0, parts = 0; const names = new Set();
  for (const member of value.members) {
    requireThat(member && /^(manifest\.json|assets\/[a-f0-9]{64}\.[a-z0-9]{1,8})$/.test(member.name) && !names.has(member.name) &&
      Number.isSafeInteger(member.size) && member.size > 0 && member.size <= (member.name === 'manifest.json' ? 8388608 : 16777216) &&
      sha(member.sha256) && Array.isArray(member.chunks) && member.chunks.length === Math.ceil(member.size / CONFIG_CHUNK_BYTES), 'CONFIG_INVALID');
    names.add(member.name); total += member.size; parts += member.chunks.length;
    member.chunks.forEach((chunk,index) => requireThat(chunk && sha(chunk.sha256) && chunk.size === Math.min(CONFIG_CHUNK_BYTES,member.size-index*CONFIG_CHUNK_BYTES), 'CONFIG_INVALID'));
  }
  requireThat(names.has('manifest.json') && total <= CONFIG_BYTES && parts <= 512 && total === value.totalBytes, 'CONFIG_QUOTA', 413);
  return JSON.parse(JSON.stringify(value));
}
const chunks = manifest => {
  const map = new Map(); for (const m of manifest.members) for (const c of m.chunks) {
    requireThat(!map.has(c.sha256) || map.get(c.sha256).size === c.size, 'CONFIG_INVALID'); map.set(c.sha256,c);
  } return [...map.values()];
};
/** One immutable complete head per device class. Deduplication and leases are scoped to the verified account. */
export class Configurations {
  constructor(domain, objects, clock=Date.now) { this.domain=domain; this.store=domain.store; this.objects=objects; this.clock=clock; }
  async gate(user,membership=true) { this.domain.gate('configuration'); if(membership)await this.domain.requireMembership(user); }
  owned(job,user) { requireThat(job?.userId===user,'CONFIG_NOT_FOUND',404); return job; }
  async current(user,type) {
    await this.gate(user); requireThat(device(type),'CONFIG_INVALID');
      const index=await this.store.get(indexKey(user))??fresh(), head=index.heads[type];
      if (!head) return { head:null,quotaBytes:CONFIG_BYTES };
      const job=this.owned(await this.store.get(jobKey(head.id)),user);
      requireThat(job.status==='published','CONFIG_INCOMPLETE',503);
      return {head,manifest:job.manifest,quotaBytes:CONFIG_BYTES};
  }
  async lease(user,id) {
    await this.gate(user);requireThat(uid(id),'CONFIG_INVALID');
    return this.store.transaction(async tx=>{const index=await tx.get(indexKey(user)),job=this.owned(await tx.get(jobKey(id)),user);
      requireThat(job.status==='published'&&(Object.values(index.heads).some(h=>h.id===id)||index.retired.includes(id)&&job.pinnedUntil>this.clock()),'CONFIG_NOT_FOUND',404);
      job.pinnedUntil=Math.max(job.pinnedUntil??0,this.clock()+600000);tx.put(jobKey(id),job,'config-jobs:'+user);return {status:'LEASED'};});
  }
  async begin(user,{baseRevision,manifest}) {
    await this.gate(user); const clean=validateConfigurationManifest(manifest);
    requireThat(Number.isSafeInteger(baseRevision)&&baseRevision>=0,'CONFIG_INVALID');
    await this.cleanup(user);
    const id=randomUUID();
    return this.store.transaction(async tx=>{
      const index=await tx.get(indexKey(user))??fresh(), head=index.heads[clean.deviceClass];
      if ((head?.revision??0)!==baseRevision) return {status:'CONFLICT',head:head??null};
      requireThat(index.jobs.length<2 && Object.keys(index.objects).length<1536,'CONFIG_BUSY',409);
      const specifications=chunks(clean), physical=Object.values(index.objects).reduce((n,o)=>n+o.size,0);
      const missing=specifications.filter(c=>index.objects[c.sha256]?.status!=='stored');
      // Existing full backups may exceed the new cap. Allow one bounded settings
      // replacement so old resources can retire without discarding the current head.
      let legacy = false;
      for (const head of Object.values(index.heads)) if ((await tx.get(jobKey(head.id)))?.manifest.scope !== CONFIGURATION_SCOPE) legacy = true;
      requireThat(physical+missing.reduce((n,c)=>n+c.size,0)<=(legacy ? 3*64*1024*1024+CONFIG_BYTES : 3*CONFIG_BYTES),'CONFIG_QUOTA',409);
      const job={id,userId:user,baseRevision,manifest:clean,status:'uploading',expiresAt:this.clock()+3600000,pinnedUntil:0};
      index.jobs.push(id);
      for (const chunk of specifications) {
        const object=index.objects[chunk.sha256];
        if (!object) index.objects[chunk.sha256]={size:chunk.size,status:'reserved',until:0};
        else requireThat(object.size===chunk.size && !['deleting','uncertain'].includes(object.status),'CONFIG_RECONCILIATION_REQUIRED',409);
      }
      tx.put(indexKey(user),index,'config-index'); tx.put(jobKey(id),job,'config-jobs:'+user);
      return {status:'READY',id,missing:missing.map(c=>c.sha256)};
    });
  }
  async upload(user,id,{sha256,data}) {
    await this.gate(user); requireThat(uid(id)&&sha(sha256)&&typeof data==='string'&&data.length<=349528&&/^[A-Za-z0-9+/]*={0,2}$/.test(data),'CONFIG_INVALID');
    const bytes=Buffer.from(data,'base64'); requireThat(bytes.toString('base64')===data&&hash(bytes)===sha256,'CONFIG_HASH_MISMATCH');
    const attempt=randomUUID(), key='configurations/'+user+'/'+sha256;
    const stored=await this.store.transaction(async tx=>{
      const job=this.owned(await tx.get(jobKey(id)),user),index=await tx.get(indexKey(user));
      validateConfigurationManifest(job.manifest);
      requireThat(job.status==='uploading'&&job.expiresAt>this.clock(),'CONFIG_UPLOAD_EXPIRED',409);
      const expected=chunks(job.manifest).find(c=>c.sha256===sha256);requireThat(expected&&expected.size===bytes.length,'CONFIG_INVALID');
      const object=index.objects[sha256];if(object.status==='stored')return true;
      // A timed-out writer is quarantined; its key is never freed by TTL alone.
      requireThat(object.status==='reserved','CONFIG_CHUNK_BUSY',409);
      object.status='writing';object.attempt=attempt;object.until=this.clock()+120000;
      tx.put(indexKey(user),index,'config-index');return false;
    });
    if(stored)return {status:'STORED'};
    try { await this.objects.put(key,bytes,sha256); }
    catch(error) { await this.store.transaction(async tx=>{const index=await tx.get(indexKey(user));if(index.objects[sha256]?.attempt===attempt){index.objects[sha256].status='uncertain';tx.put(indexKey(user),index,'config-index');}});throw new ServiceError('CONFIG_RECONCILIATION_REQUIRED',503); }
    await this.store.transaction(async tx=>{const index=await tx.get(indexKey(user)),object=index.objects[sha256];
      requireThat(object?.attempt===attempt&&object.status==='writing','CONFIG_INCOMPLETE',409);
      object.status='stored';object.until=0;delete object.attempt;tx.put(indexKey(user),index,'config-index');});
    return {status:'STORED'};
  }
  async member(user,manifest,name) {
    const spec=manifest.members.find(m=>m.name===name);requireThat(spec,'CONFIG_INCOMPLETE');
    const result=Buffer.alloc(spec.size);let offset=0;
    for(const part of spec.chunks){const bytes=await this.objects.read('configurations/'+user+'/'+part.sha256,part.size);
      requireThat(hash(bytes)===part.sha256,'CONFIG_HASH_MISMATCH',503);bytes.copy(result,offset);offset+=bytes.length;}
    requireThat(hash(result)===spec.sha256,'CONFIG_HASH_MISMATCH',503);return result;
  }
  async finalize(user,id) {
    await this.gate(user);requireThat(uid(id),'CONFIG_INVALID');
    const pending=this.owned(await this.store.get(jobKey(id)),user);
    if(pending.status==='published')return {status:'PUBLISHED',head:pending.head};
    validateConfigurationManifest(pending.manifest);
    requireThat(pending.status==='uploading'&&pending.expiresAt>this.clock(),'CONFIG_UPLOAD_EXPIRED',409);
    const index=await this.store.get(indexKey(user));
    requireThat(chunks(pending.manifest).every(c=>index.objects[c.sha256]?.status==='stored'),'CONFIG_INCOMPLETE',409);
    let value;try{value=JSON.parse((await this.member(user,pending.manifest,'manifest.json')).toString('utf8'));}catch(error){if(error instanceof ServiceError)throw error;throw new ServiceError('CONFIG_INVALID');}
    validateCloudSettings(value);
    const assets=new Map(value.assets.map(a=>[a.path,a]));
    requireThat(assets.size+1===pending.manifest.members.length,'CONFIG_INCOMPLETE');
    for(const member of pending.manifest.members.filter(m=>m.name!=='manifest.json')){
      const asset=assets.get(member.name);requireThat(asset&&asset.sha256===member.sha256&&asset.size===member.size,'CONFIG_INCOMPLETE');
      await this.member(user,pending.manifest,member.name);
    }
    return this.store.transaction(async tx=>{
      const job=this.owned(await tx.get(jobKey(id)),user),index=await tx.get(indexKey(user)),type=job.manifest.deviceClass,current=index.heads[type];
      if(job.status==='published')return {status:'PUBLISHED',head:job.head};
      requireThat(job.status==='uploading'&&job.expiresAt>this.clock()&&chunks(job.manifest).every(c=>index.objects[c.sha256]?.status==='stored'),'CONFIG_INCOMPLETE',409);
      if((current?.revision??0)!==job.baseRevision)return {status:'CONFLICT',head:current??null};
      if(current)index.retired.push(current.id);
      const head={id,revision:job.baseRevision+1,sha256:hash(JSON.stringify(job.manifest)),totalBytes:job.manifest.totalBytes,createdAt:this.clock()};
      job.status='published';job.head=head;index.heads[type]=head;index.jobs=index.jobs.filter(j=>j!==id);
      tx.put(jobKey(id),job,'config-jobs:'+user);tx.put(indexKey(user),index,'config-index');return {status:'PUBLISHED',head};
    });
  }
  async read(user,id,sha256) {
    await this.gate(user);requireThat(uid(id)&&sha(sha256),'CONFIG_INVALID');
    const job=this.owned(await this.store.get(jobKey(id)),user),index=await this.store.get(indexKey(user));
    requireThat(job.status==='published'&&(Object.values(index.heads).some(h=>h.id===id)||index.retired.includes(id)&&job.pinnedUntil>this.clock()),'CONFIG_NOT_FOUND',404);
    const part=chunks(job.manifest).find(c=>c.sha256===sha256);requireThat(part,'CONFIG_NOT_FOUND',404);
    const bytes=await this.objects.read('configurations/'+user+'/'+sha256,part.size);requireThat(hash(bytes)===sha256,'CONFIG_HASH_MISMATCH',503);
    return {sha256,data:bytes.toString('base64')};
  }
  async cleanup(user) {
    await this.gate(user,false);
    const deletions=await this.store.transaction(async tx=>{
      const index=await tx.get(indexKey(user));if(!index)return [];
      const keep=[];
      for(const id of [...index.jobs,...index.retired]){
        const job=this.owned(await tx.get(jobKey(id)),user);
        if(job.status==='uploading'&&job.expiresAt>this.clock()||job.status==='published'&&job.pinnedUntil>this.clock())keep.push(id);
        else tx.delete(jobKey(id));
      }
      index.jobs=index.jobs.filter(id=>keep.includes(id));index.retired=index.retired.filter(id=>keep.includes(id));
      const referenced=new Set();
      for(const id of [...Object.values(index.heads).map(h=>h.id),...keep]){
        const job=this.owned(await tx.get(jobKey(id)),user);chunks(job.manifest).forEach(c=>referenced.add(c.sha256));
      }
      const deleting=[];
      for(const [digest,object]of Object.entries(index.objects)){
        if(!referenced.has(digest)&&['reserved','stored','deleting'].includes(object.status)){object.status='deleting';deleting.push(digest);}
      }
      tx.put(indexKey(user),index,'config-index');return deleting;
    });
    for(const digest of deletions){await this.objects.delete('configurations/'+user+'/'+digest);await this.store.transaction(async tx=>{
      const index=await tx.get(indexKey(user));requireThat(index.objects[digest]?.status==='deleting','CONFIG_INCOMPLETE',503);
      delete index.objects[digest];tx.put(indexKey(user),index,'config-index');});}
    return {status:'CLEANED'};
  }
  async abort(user,id) {
    await this.gate(user,false);requireThat(uid(id),'CONFIG_INVALID');
    await this.store.transaction(async tx=>{const job=this.owned(await tx.get(jobKey(id)),user);
      if(job.status==='uploading'){job.expiresAt=0;tx.put(jobKey(id),job,'config-jobs:'+user);}});
    return this.cleanup(user);
  }
}
