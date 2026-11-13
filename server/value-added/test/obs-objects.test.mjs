import test from 'node:test';
import assert from 'node:assert/strict';
import { ObsObjects } from '../src/obs-objects.mjs';
import { hash } from '../src/save-manifest.mjs';
import { probeObs } from '../scripts/probe-obs.mjs';
const key='saves/'+'a'.repeat(64)+'/11111111-1111-1111-1111-111111111111/0';
function fixture(){const objects=new Map(),signatures=[];
 const signer={createSignedUrlSync:args=>{signatures.push(args);return {SignedUrl:'https://private.obs.cn-north-4.myhuaweicloud.com/'+(args.Key??'')+'?'+(args.SpecialParam??'signature=mock'),ActualSignedRequestHeaders:args.Headers??{}};}};
 const transport=async(url,options={})=>{assert.equal(options.redirect,'error');assert.ok(options.signal);
   const u=new URL(url),k=u.pathname.slice(1),method=options.method??'GET';
   if(!u.search)return new Response('denied',{status:403});
   if(u.search==='?publicAccessBlock')return new Response('<PublicAccessBlockConfiguration>'+['BlockPublicAcls','IgnorePublicAcls','BlockPublicPolicy','RestrictPublicBuckets'].map(key=>'<'+key+'>true</'+key+'>').join('')+'</PublicAccessBlockConfiguration>');
   if(u.search==='?bucketStatus')return new Response('<BucketStatus><IsPublic>false</IsPublic></BucketStatus>');
   if(u.search==='?versioning')return new Response('<VersioningConfiguration xmlns="http://obs.myhuaweicloud.com/doc/2015-06-30/"/>');
   if(method==='PUT'){if(hash(options.body)!==options.headers['x-obs-content-sha256'])return new Response('bad checksum',{status:400});
     objects.set(k,Buffer.from(options.body));return new Response('',{status:200});}
   if(method==='DELETE'){objects.delete(k);return new Response(null,{status:204});}
   const bytes=objects.get(k);return bytes?new Response(bytes,{headers:{'content-length':String(bytes.length)}}):new Response('missing',{status:404});
 };
 return {adapter:new ObsObjects(signer,{bucket:'private',endpoint:'https://obs.cn-north-4.myhuaweicloud.com',fingerprint:'fingerprint'},transport),objects,signatures};}
test('OBS uses signer-only + normal transport, server-verified same bytes, no ETag trust',async()=>{
 const {adapter,objects,signatures}=fixture();
 const bytes=Buffer.from('save');await adapter.put(key,bytes,hash(bytes));assert.equal(objects.get(key).toString(),'save');
 assert.equal(signatures[0].Method,'PUT');assert.equal(signatures[0].Headers['x-obs-content-sha256'],hash(bytes));
 assert.equal(signatures[1].Method,'GET');assert.equal(signatures[0].Expires,60);
 await assert.rejects(adapter.put(key,bytes,'0'.repeat(64)),/SAVE_HASH_MISMATCH/);
 await assert.rejects(adapter.put('../outside',bytes,hash(bytes)),/INVALID_OBJECT_KEY/);
 await adapter.delete(key);assert.equal(objects.size,0);
});
test('OBS operator probe truly tests privacy, checksum rejection, versioning, deletion',async()=>{
 const f=fixture();const evidence=await probeObs(f.adapter);assert.equal(f.objects.size,0);await f.adapter.enable(evidence);assert.equal(f.adapter.ready,true);
 assert.match(f.adapter.download(key).url,/^https:/);
 await assert.rejects(f.adapter.enable({...evidence,versioningDisabled:false}),/OBS_NOT_VERIFIED/);
});
test('OBS refuses wrong hosts, oversized/truncated reads and altered stored bytes',async()=>{
 const f=fixture();f.adapter.signer.createSignedUrlSync=()=>({SignedUrl:'https://attacker.example/',ActualSignedRequestHeaders:{}});
 assert.throws(()=>f.adapter.sign('GET',key),/INVALID_OBJECT_URL/);
 const g=fixture();g.adapter.transport=async()=>new Response('too long',{headers:{'content-length':'4'}});
 await assert.rejects(g.adapter.read(key,4),/OBJECT_READ_FAILED/);
 const h=fixture();h.adapter.transport=async(_url,options)=>options.method==='PUT'?new Response(''):new Response('evil',{headers:{'content-length':'4'}});
 await assert.rejects(h.adapter.put(key,Buffer.from('save'),hash('save')),/SAVE_HASH_MISMATCH/);
});

test('OBS gate refuses public saves prefix and stale reports after policy or versioning changes',async()=>{
 const f=fixture(),original=f.adapter.transport;
 f.adapter.transport=async(url,options)=>!new URL(url).search&&new URL(url).pathname.startsWith('/saves/')?
  new Response('public data',{status:200}):original(url,options);
 await assert.rejects(probeObs(f.adapter),/OBS_BUCKET_NOT_PRIVATE/);assert.equal(f.objects.size,0);
 const g=fixture(),evidence=await probeObs(g.adapter),safe=g.adapter.transport;
 g.adapter.transport=async(url,options)=>new URL(url).search==='?bucketStatus'?
  new Response('<BucketStatus><IsPublic>true</IsPublic></BucketStatus>'):safe(url,options);
 await assert.rejects(g.adapter.enable(evidence),/OBS_BUCKET_NOT_PRIVATE/);assert.equal(g.adapter.ready,false);
 g.adapter.transport=async(url,options)=>new URL(url).search==='?versioning'?
  new Response('<VersioningConfiguration><Status>Enabled</Status></VersioningConfiguration>'):safe(url,options);
 await assert.rejects(g.adapter.enable(evidence),/OBS_VERSIONING_NOT_DISABLED/);assert.equal(g.adapter.ready,false);
 g.adapter.transport=async(url,options)=>new URL(url).search==='?publicAccessBlock'?
  new Response('<PublicAccessBlockConfiguration/>'):safe(url,options);
 await assert.rejects(g.adapter.enable(evidence),/OBS_BUCKET_SECURITY_UNKNOWN/);assert.equal(g.adapter.ready,false);
});

test('bad-SHA probe holds MD5, type and bytes constant',async()=>{
 const f=fixture();await probeObs(f.adapter);
 const puts=f.signatures.filter(s=>s.Method==='PUT');assert.equal(puts.length,2);
 assert.equal(puts[0].Headers['Content-MD5'],puts[1].Headers['Content-MD5']);
 assert.equal(puts[0].Headers['Content-Type'],puts[1].Headers['Content-Type']);
 assert.notEqual(puts[0].Headers['x-obs-content-sha256'],puts[1].Headers['x-obs-content-sha256']);
});
