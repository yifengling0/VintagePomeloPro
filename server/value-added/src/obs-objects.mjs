import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { CHUNK_BYTES, hash } from './save-manifest.mjs';
import { requireThat, ServiceError } from './errors.mjs';
export const OBS_VERSION='3.26.2';
/** Use the official SDK for LOCAL signing only. Its HTTPS transport disables hostname validation. */
export class ObsObjects {
  constructor(signer, {bucket,endpoint,fingerprint}, transport=fetch) {
    this.signer=signer;this.bucket=bucket;this.endpoint=endpoint;this.fingerprint=fingerprint;this.transport=transport;this.ready=false;
  }
  sign(method,key,headers={}) {
    requireThat(/^(saves\/[a-f0-9]{64}\/[a-f0-9-]{36}\/[0-9]{1,2}|probe\/[a-f0-9-]{36})$/.test(key),'INVALID_OBJECT_KEY');
    const signed=this.signer.createSignedUrlSync({Method:method,Bucket:this.bucket,Key:key,Expires:60,Headers:headers});
    const url=new URL(signed.SignedUrl), endpoint=new URL(this.endpoint);
    requireThat(url.protocol==='https:' && !url.username && !url.password &&
      url.hostname===this.bucket+'.'+endpoint.hostname && url.port===endpoint.port,'INVALID_OBJECT_URL',503);
    return {url:signed.SignedUrl,headers:signed.ActualSignedRequestHeaders??{},expiresAt:Date.now()+60000};
  }
  async request(method,key,headers={},bytes) {
    const signed=this.sign(method,key,headers);
    return this.transport(signed.url,{method,headers:signed.headers,body:bytes,redirect:'error',signal:AbortSignal.timeout(30000)});
  }
  async put(key,bytes,sha256) {
    requireThat(Buffer.isBuffer(bytes)&&bytes.length<=CHUNK_BYTES&&hash(bytes)===sha256,'SAVE_HASH_MISMATCH');
    const response=await this.request('PUT',key,{'Content-Type':'application/octet-stream',
      'Content-MD5':createHash('md5').update(bytes).digest('base64'),'x-obs-content-sha256':sha256},bytes);
    requireThat(response.status===200,'OBJECT_UPLOAD_FAILED',503);await response.body?.cancel();
    // Read the stored bytes back, not client metadata or an ETag claimed to be SHA-256.
    const actual=await this.read(key,bytes.length);requireThat(hash(actual)===sha256,'SAVE_HASH_MISMATCH',503);
  }
  async read(key,expectedSize) {
    requireThat(Number.isSafeInteger(expectedSize)&&expectedSize>=0&&expectedSize<=CHUNK_BYTES,'SAVE_QUOTA');
    const response=await this.request('GET',key);
    requireThat(response.status===200 && Number(response.headers.get('content-length'))===expectedSize,'OBJECT_READ_FAILED',503);
    const result=Buffer.alloc(expectedSize);let offset=0;
    try{for await(const bytes of response.body){requireThat(offset+bytes.length<=expectedSize,'SAVE_QUOTA');result.set(bytes,offset);offset+=bytes.length;}}
    catch{await response.body?.cancel().catch(()=>{});throw new ServiceError('OBJECT_READ_FAILED',503);}
    requireThat(offset===expectedSize,'OBJECT_READ_FAILED',503);return result;
  }
  download(key) {requireThat(this.ready,'NOT_CONFIGURED',503);return this.sign('GET',key);}
  async delete(key) {
    const response=await this.request('DELETE',key);requireThat([204,404].includes(response.status),'OBJECT_DELETE_FAILED',503);await response.body?.cancel();
  }
  async bucketXml(specialParam) {
    requireThat(['versioning','publicAccessBlock','bucketStatus'].includes(specialParam),'INVALID_OBS_CONFIG');
    const signed=this.signer.createSignedUrlSync({Method:'GET',Bucket:this.bucket,SpecialParam:specialParam,Expires:60});
    const url=new URL(signed.SignedUrl),endpoint=new URL(this.endpoint);
    requireThat(url.protocol==='https:'&&!url.username&&!url.password&&url.hostname===this.bucket+'.'+endpoint.hostname&&
      url.port===endpoint.port,'INVALID_OBJECT_URL',503);
    const response=await this.transport(signed.SignedUrl,{method:'GET',headers:signed.ActualSignedRequestHeaders,
      redirect:'error',signal:AbortSignal.timeout(10000)});
    requireThat(response.status===200,'OBS_BUCKET_SECURITY_UNKNOWN',503);
    let xml='';for await(const bytes of response.body){xml+=Buffer.from(bytes).toString('utf8');requireThat(xml.length<=65536,'OBS_BUCKET_SECURITY_UNKNOWN',503);}
    requireThat(!/<!/.test(xml),'OBS_BUCKET_SECURITY_UNKNOWN',503);
    return xml.trim().replace(/^<\?xml[^?]*\?>\s*/,'');
  }
  booleanXml(xml,root,fields) {
    const match=new RegExp('^<'+root+'(?:\\s+[^>]*)?>([\\s\\S]*)</'+root+'>$').exec(xml);
    requireThat(match,'OBS_BUCKET_SECURITY_UNKNOWN',503);let remainder=match[1];const result={};
    for(const name of fields){const pattern=new RegExp('<'+name+'>\\s*(true|false)\\s*</'+name+'>','g');
      const values=[...remainder.matchAll(pattern)];requireThat(values.length===1,'OBS_BUCKET_SECURITY_UNKNOWN',503);
      result[name]=values[0][1]==='true';remainder=remainder.replace(pattern,'');}
    requireThat(!remainder.trim(),'OBS_BUCKET_SECURITY_UNKNOWN',503);return result;
  }
  async checkVersioningDisabled() {
    const xml=await this.bucketXml('versioning');
    requireThat(/^<VersioningConfiguration\b[^>]*(?:\/>|>\s*<\/VersioningConfiguration>)$/.test(xml), 'OBS_VERSIONING_NOT_DISABLED',503);
    return true;
  }
  async checkBucketSecurity() {
    await this.checkVersioningDisabled();
    const fields=['BlockPublicAcls','IgnorePublicAcls','BlockPublicPolicy','RestrictPublicBuckets'];
    const bpa=this.booleanXml(await this.bucketXml('publicAccessBlock'),'PublicAccessBlockConfiguration',fields);
    requireThat(fields.every(field=>bpa[field]===true),'OBS_BPA_REQUIRED',503);
    const status=this.booleanXml(await this.bucketXml('bucketStatus'),'BucketStatus',['IsPublic']);
    requireThat(status.IsPublic===false,'OBS_BUCKET_NOT_PRIVATE',503);return true;
  }
  async enable(evidence) {
    this.ready=false;
    requireThat(evidence.fingerprint===this.fingerprint&&evidence.sdkVersion===OBS_VERSION&&evidence.privateBucket===true&&
      evidence.bpaChecked===true&&evidence.savesNamespacePrivate===true&&evidence.versioningDisabled===true&&
      evidence.roundTripSha256===true&&evidence.badChecksumRejected===true&&evidence.deleteVerified===true,'OBS_NOT_VERIFIED',503);
    // An old write probe cannot attest to current ACL/policy/versioning. Read-only recheck every process start.
    await this.checkBucketSecurity();this.ready=true;
  }
}
export async function connectObs(env) {
  const endpoint=env.VP_OBS_ENDPOINT,bucket=env.VP_OBS_BUCKET;
  requireThat(/^https:\/\/obs\.[a-z0-9-]+\.myhuaweicloud\.com$/.test(endpoint??'')&&/^[a-z0-9][a-z0-9-]{1,61}[a-z0-9]$/.test(bucket??''),'INVALID_OBS_CONFIG');
  // Credentials are deployment-owned files, never app resources or HTTP responses.
  const secret=JSON.parse(await readFile(env.VP_OBS_CREDENTIAL_FILE,'utf8'));
  requireThat(typeof secret.accessKeyId==='string'&&typeof secret.secretAccessKey==='string'&&secret.accessKeyId&&secret.secretAccessKey,'INVALID_OBS_CONFIG');
  const require=createRequire(import.meta.url), ObsClient=require('esdk-obs-nodejs');
  const signer=new ObsClient({access_key_id:secret.accessKeyId,secret_access_key:secret.secretAccessKey,
    security_token:secret.securityToken,server:endpoint,signature:'obs',ssl_verify:true});
  const lock=hash(await readFile(new URL('../package-lock.json',import.meta.url)));
  const fingerprint=hash(JSON.stringify({endpoint,bucket,accessKeyId:secret.accessKeyId,sdk:OBS_VERSION,lock}));
  return new ObsObjects(signer,{bucket,endpoint,fingerprint});
}
