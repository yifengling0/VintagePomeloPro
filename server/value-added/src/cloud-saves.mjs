import { randomUUID } from 'node:crypto';
import { requireThat, ServiceError } from './errors.mjs';
import { ACCOUNT_BYTES, hash, validateSaveManifest } from './save-manifest.mjs';
const TTL = 60 * 60 * 1000;
const LEASE = 120000;
const uuid = value => typeof value === 'string' && /^[a-f0-9-]{36}$/.test(value);
const indexKey = user => 'save-index:' + user;
const jobKey = id => 'save-job:' + id;
const freshIndex = () => ({ slots: [], jobs: [], usedBytes: 0, reservedBytes: 0 });
/** Object storage never decides authority. All owner/quota/head transitions are one CloudDB transaction. */
export class CloudSaves {
  constructor(domain, objects, clock = Date.now) { this.domain=domain; this.store=domain.store; this.objects=objects; this.clock=clock; }
  async gate(user, membership=true) {
    requireThat(this.store.ready && this.objects.ready, 'NOT_CONFIGURED', 503);
    if (membership) await this.domain.requireMembership(user);
  }
  owned(job,user) { requireThat(job && job.userId === user, 'SAVE_NOT_FOUND',404); return job; }
  async retireVersions(tx, user, index, versions) {
    for (const version of versions) {
      const job = this.owned(await tx.get(jobKey(version.versionId)), user);
      requireThat(job.status === 'published', 'STORAGE_INVARIANT', 503);
      job.status = 'deleting'; job.wasPublished = true;
      if (!index.jobs.includes(job.id)) index.jobs.push(job.id);
      tx.put(jobKey(job.id), job, 'save-jobs:' + user);
    }
  }
  async list(user) {
    await this.gate(user,false);
    // Migrate old indexes atomically; retain quota until the retired objects are actually deleted.
    await this.store.transaction(async tx => {
      const index = await tx.get(indexKey(user)); if (!index) return;
      for (const slot of index.slots) {
        if (slot.versions.length <= 1) continue;
        await this.retireVersions(tx, user, index, slot.versions.slice(1));
        slot.versions = slot.versions.slice(0, 1); slot.revision++;
      }
      tx.put(indexKey(user), index, 'save-index');
    });
    try { await this.cleanup(user); } catch (_) { /* Retired objects remain charged and retryable. */ }
    const index=await this.store.get(indexKey(user))??freshIndex();
    return {slots:index.slots,usedBytes:index.usedBytes,reservedBytes:index.reservedBytes,quotaBytes:ACCOUNT_BYTES}; }
  async createSlot(user,{label}) {
    await this.gate(user); requireThat(typeof label==='string' && label.trim().length>0 && label.length<=80 && !/[\u0000-\u001f]/.test(label),'INVALID_SAVE_SLOT');
    const slotId=randomUUID();
    return this.store.transaction(async tx=>{const index=await tx.get(indexKey(user))??freshIndex();
      requireThat(index.slots.length<16,'SAVE_SLOT_LIMIT',409);
      const slot={slotId,label:label.trim(),revision:0,versions:[]}; index.slots.push(slot);
      tx.put(indexKey(user),index,'save-index');return slot;});
  }
  async begin(user,{slotId,baseRevision,manifest}) {
    await this.gate(user); requireThat(uuid(slotId)&&Number.isSafeInteger(baseRevision)&&baseRevision>=0,'INVALID_SAVE_SLOT');
    const clean=validateSaveManifest(manifest), id=randomUUID(), now=this.clock();
    return this.store.transaction(async tx=>{const index=await tx.get(indexKey(user))??freshIndex();
      const slot=index.slots.find(s=>s.slotId===slotId); requireThat(slot,'SAVE_NOT_FOUND',404);
      if(slot.revision!==baseRevision)return {status:'CONFLICT',slot};
      requireThat(index.jobs.length<2 && index.usedBytes+index.reservedBytes+clean.totalBytes<=ACCOUNT_BYTES,'SAVE_QUOTA',409);
      const job={id,userId:user,slotId,baseRevision,manifest:clean,status:'uploading',expiresAt:now+TTL, chunks:{}};
      index.jobs.push(id);index.reservedBytes+=clean.totalBytes;
      tx.put(jobKey(id),job,'save-jobs:'+user);tx.put(indexKey(user),index,'save-index');
      return {status:'READY',jobId:id,expiresAt:job.expiresAt};});
  }
  async uploadSpec(user,id,part) {
    await this.gate(user); requireThat(uuid(id)&&Number.isInteger(part),'INVALID_SAVE_CHUNK');
    const job=this.owned(await this.store.get(jobKey(id)),user);
    requireThat(job.status==='uploading' && !job.cancelled && job.expiresAt>this.clock(),'SAVE_UPLOAD_EXPIRED',409);
    const chunk=job.manifest.chunks[part];requireThat(chunk,'INVALID_SAVE_CHUNK');return chunk;
  }
  async acquireChunk(user,id,part) {
    const expected=await this.uploadSpec(user,id,part), attempt=randomUUID(), now=this.clock();
    return this.store.transaction(async tx=>{const job=this.owned(await tx.get(jobKey(id)),user);
      const global=await tx.get('save-upload-budget')??{leases:[]};
      requireThat(job.status==='uploading'&&!job.cancelled&&job.expiresAt>now,'SAVE_UPLOAD_EXPIRED',409);
      const existing=job.chunks[part];if(existing?.status==='stored')return {stored:true,expected};
      requireThat(!existing || (existing.status==='receiving'&&existing.leaseUntil<=now),'SAVE_CHUNK_BUSY',409);
      global.leases=global.leases.filter(lease=>lease.until>now);
      requireThat(global.leases.length<4 && global.leases.filter(lease=>lease.userId===user).length<2,'SAVE_TRANSFER_BUSY',429);
      const key='saves/'+user+'/'+id+'/'+part;
      global.leases.push({attempt,userId:user,until:now+LEASE});
      job.chunks[part]={status:'receiving',attempt,leaseUntil:now+LEASE,key};
      tx.put(jobKey(id),job,'save-jobs:'+user);tx.put('save-upload-budget',global,'internal-save');
      return {stored:false,attempt,key,expected};});
  }
  async upload(user,id,part,bytes,acquired=null) {
    const lease=acquired??await this.acquireChunk(user,id,part), expected=lease.expected;
    requireThat(Buffer.isBuffer(bytes)&&bytes.length===expected.size&&hash(bytes)===expected.sha256,'SAVE_HASH_MISMATCH');
    if(lease.stored)return {status:'STORED'};
    // Only these same verified bytes are transmitted. Clients never receive write credentials.
    // An ambiguous timeout retains quota and the lease instead of incorrectly declaring cleanup complete.
    await this.store.transaction(async tx=>{const job=this.owned(await tx.get(jobKey(id)),user),chunk=job.chunks[part];
      requireThat(job.status==='uploading'&&!job.cancelled&&job.expiresAt>this.clock()&&chunk?.status==='receiving'&&
        chunk.attempt===lease.attempt&&chunk.leaseUntil>this.clock(),'SAVE_UPLOAD_EXPIRED',409);
      chunk.status='writing';tx.put(jobKey(id),job,'save-jobs:'+user);});
    try{await this.objects.put(lease.key,bytes,expected.sha256);}
    catch(error){
      // A transport failure cannot prove the remote PUT will never commit later. Quarantine
      // this job/key and retain its quota; never turn a TTL into evidence of deletion safety.
      await this.store.transaction(async tx=>{const job=this.owned(await tx.get(jobKey(id)),user);
        if(job.chunks[part]?.attempt===lease.attempt){job.chunks[part].status='uncertain';tx.put(jobKey(id),job,'save-jobs:'+user);}});
      throw new ServiceError('SAVE_RECONCILIATION_REQUIRED',503);
    }
    await this.store.transaction(async tx=>{const job=this.owned(await tx.get(jobKey(id)),user);
      const global=await tx.get('save-upload-budget');
      requireThat(job.status==='uploading'&&job.chunks[part]?.status==='writing'&&job.chunks[part]?.attempt===lease.attempt,'SAVE_UPLOAD_EXPIRED',409);
      job.chunks[part]={status:'stored',key:lease.key};
      global.leases=global.leases.filter(item=>item.attempt!==lease.attempt);
      tx.put(jobKey(id),job,'save-jobs:'+user);tx.put('save-upload-budget',global,'internal-save');});
    return {status:'STORED'};
  }
  async finalize(user,id) {
    await this.gate(user);requireThat(uuid(id),'INVALID_SAVE_SLOT');
    const result = await this.store.transaction(async tx=>{const job=this.owned(await tx.get(jobKey(id)),user);
      const index=await tx.get(indexKey(user));const slot=index?.slots.find(s=>s.slotId===job.slotId);
      requireThat(slot,'SAVE_NOT_FOUND',404);
      if(job.status==='published')return {status:'PUBLISHED',slot};
      requireThat(job.status==='uploading'&&!job.cancelled&&job.expiresAt>this.clock(),'SAVE_UPLOAD_EXPIRED',409);
      requireThat(job.manifest.chunks.every(c=>job.chunks[c.index]?.status==='stored'),'SAVE_INCOMPLETE',409);
      if(slot.revision!==job.baseRevision)return {status:'CONFLICT',slot};
      await this.retireVersions(tx,user,index,slot.versions);
      slot.revision++;slot.versions=[{versionId:id,createdAt:this.clock(),totalBytes:job.manifest.totalBytes}];
      job.status='published';index.jobs=index.jobs.filter(value=>value!==id);
      index.reservedBytes-=job.manifest.totalBytes;index.usedBytes+=job.manifest.totalBytes;
      tx.put(jobKey(id),job,'save-jobs:'+user);tx.put(indexKey(user),index,'save-index');
      return {status:'PUBLISHED',slot};});
    try { await this.cleanup(user); } catch (_) { /* Publishing succeeded; object cleanup is retryable. */ }
    return result;
  }
  async download(user,slotId,id) {
    await this.gate(user);requireThat(uuid(slotId)&&uuid(id),'INVALID_SAVE_SLOT');
    const index=await this.store.get(indexKey(user));const slot=index?.slots.find(s=>s.slotId===slotId);
    requireThat(slot?.versions.some(v=>v.versionId===id),'SAVE_NOT_FOUND',404);
    const job=this.owned(await this.store.get(jobKey(id)),user);
    requireThat(job.status==='published'&&job.slotId===slotId,'SAVE_NOT_FOUND',404);
    return {versionId:id,manifest:job.manifest,downloads:job.manifest.chunks.map(chunk=>({index:chunk.index,
      ...this.objects.download(job.chunks[chunk.index].key)}))};
  }
  async downloadTicket(user,slotId,id,part) {
    requireThat(Number.isInteger(part)&&part>=0&&part<64,'INVALID_SAVE_CHUNK');
    const plan=await this.download(user,slotId,id);
    requireThat(plan.downloads[part],'INVALID_SAVE_CHUNK');return plan.downloads[part];
  }
  async abort(user,id) {
    await this.gate(user,false);requireThat(uuid(id),'INVALID_SAVE_SLOT');
    await this.store.transaction(async tx=>{const job=this.owned(await tx.get(jobKey(id)),user);
      if(job.status==='uploading'){job.cancelled=true;tx.put(jobKey(id),job,'save-jobs:'+user);}});
    return this.remove(user,id,false);
  }
  async deleteVersion(user,slotId,id,{baseRevision}) {
    await this.gate(user,false);requireThat(uuid(slotId)&&uuid(id)&&Number.isSafeInteger(baseRevision),'INVALID_SAVE_SLOT');
    return this.remove(user,id,true,slotId,baseRevision);
  }
  async remove(user,id,published,slotId='',baseRevision=-1) {
    const now=this.clock();
    const job=await this.store.transaction(async tx=>{const job=this.owned(await tx.get(jobKey(id)),user);
      const index=await tx.get(indexKey(user));
      if(job.status==='deleted')return job;
      if(published){
        const slot=index.slots.find(s=>s.slotId===slotId);requireThat(slot&&job.slotId===slotId,'SAVE_NOT_FOUND',404);
        if(job.status!=='deleting'){
          requireThat(job.status==='published'&&slot.revision===baseRevision,'SAVE_CONFLICT',409);
          slot.versions=slot.versions.filter(v=>v.versionId!==id);slot.revision++;
          if(!index.jobs.includes(id))index.jobs.push(id);
        }
      }else requireThat(['uploading','deleting'].includes(job.status),'SAVE_ALREADY_PUBLISHED',409);
      // A live writer might still succeed after cancellation. Defer cleanup until its bounded request has expired.
      requireThat(!Object.values(job.chunks).some(c=>c.status==='writing'||c.status==='uncertain'),'SAVE_RECONCILIATION_REQUIRED',409);
      requireThat(Object.values(job.chunks).every(c=>c.status!=='receiving'||c.leaseUntil<=now),'SAVE_CLEANUP_PENDING',409);
      job.status='deleting';job.wasPublished=job.wasPublished??published;
      tx.put(jobKey(id),job,'save-jobs:'+user);tx.put(indexKey(user),index,'save-index');return job;});
    if(job.status==='deleted')return {status:'DELETED'};
    // Deletes target only server-generated keys owned by this job, never a client-provided prefix or path.
    for(const chunk of Object.values(job.chunks))await this.objects.delete(chunk.key);
    await this.store.transaction(async tx=>{const current=this.owned(await tx.get(jobKey(id)),user);
      const index=await tx.get(indexKey(user));if(current.status==='deleted')return;
      requireThat(current.status==='deleting','STORAGE_INVARIANT',503);
      if(current.wasPublished){index.usedBytes-=current.manifest.totalBytes;index.jobs=index.jobs.filter(value=>value!==id);}
      else {index.reservedBytes-=current.manifest.totalBytes;index.jobs=index.jobs.filter(value=>value!==id);}
      requireThat(index.usedBytes>=0&&index.reservedBytes>=0,'STORAGE_INVARIANT',503);
      current.status='deleted';tx.put(jobKey(id),current,'save-jobs:'+user);tx.put(indexKey(user),index,'save-index');});
    return {status:'DELETED'};
  }
  async cleanup(user) {
    await this.gate(user,false);const index=await this.store.get(indexKey(user))??freshIndex();let pending=0,reconciliation=false;
    for(const id of index.jobs){const job=this.owned(await this.store.get(jobKey(id)),user);
      if(job.expiresAt<=this.clock()||job.status==='deleting'||job.cancelled){
        try{await this.abort(user,id);}catch(error){if(!(error instanceof ServiceError))throw error;pending++;if(error.code==='SAVE_RECONCILIATION_REQUIRED')reconciliation=true;}
      }else pending++;}
    return {status:reconciliation?'RECONCILIATION_REQUIRED':pending?'CLEANUP_PENDING':'CLEANED',pending};
  }
}
