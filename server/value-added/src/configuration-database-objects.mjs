import { createHash } from 'node:crypto';
import { requireThat } from './errors.mjs';
import { CONFIG_CHUNK_BYTES } from './configurations.mjs';
/** Dedicated bounded configuration objects on the already verified private CloudDB zone.
 * No client database access. This adapter can be replaced by OBS without changing the snapshot protocol.
 */
export class ConfigurationDatabaseObjects {
  constructor(store) { this.store=store; }
  get ready() { return this.store.ready===true; }
  key(key) {
    const match=/^configurations\/([a-f0-9]{64})\/([a-f0-9]{64})$/.exec(key);
    requireThat(match,'INVALID_OBJECT_KEY');return {key:'config-object:'+match[1]+':'+match[2],owner:match[1],hash:match[2]};
  }
  async put(key,bytes,sha256) {
    const parsed=this.key(key); requireThat(this.ready&&Buffer.isBuffer(bytes)&&bytes.length<=CONFIG_CHUNK_BYTES&&
      sha256===parsed.hash&&createHash('sha256').update(bytes).digest('hex')===sha256,'CONFIG_HASH_MISMATCH');
    await this.store.transaction(async tx=>{
      const previous=await tx.get(parsed.key);
      if(previous){requireThat(previous.size===bytes.length&&previous.sha256===sha256&&previous.data===bytes.toString('base64'),'CONFIG_HASH_MISMATCH');return;}
      tx.put(parsed.key,{sha256,size:bytes.length,data:bytes.toString('base64')},'config-objects:'+parsed.owner);
    });
    await this.read(key,bytes.length);
  }
  async read(key,expectedSize) {
    const parsed=this.key(key);requireThat(this.ready&&Number.isSafeInteger(expectedSize)&&expectedSize>0&&expectedSize<=CONFIG_CHUNK_BYTES,'CONFIG_QUOTA');
    const object=await this.store.get(parsed.key);requireThat(object?.sha256===parsed.hash&&object.size===expectedSize&&typeof object.data==='string','CONFIG_INCOMPLETE',503);
    const bytes=Buffer.from(object.data,'base64');requireThat(bytes.length===expectedSize&&bytes.toString('base64')===object.data&&
      createHash('sha256').update(bytes).digest('hex')===parsed.hash,'CONFIG_HASH_MISMATCH',503);return bytes;
  }
  async delete(key) { const parsed=this.key(key);await this.store.transaction(async tx=>{await tx.get(parsed.key);tx.delete(parsed.key);}); }
}
