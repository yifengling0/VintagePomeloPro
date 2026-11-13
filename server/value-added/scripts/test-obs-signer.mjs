// Actual locked OBS signer, synthetic credentials, mocked transport. No cloud request or real credential.
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { resolve } from 'node:path';
import { ObsObjects } from '../src/obs-objects.mjs';
import { hash } from '../src/save-manifest.mjs';
const require=createRequire(process.env.VP_TEST_MODULE_ROOT?resolve(process.env.VP_TEST_MODULE_ROOT,'package.json'):new URL('../package.json',import.meta.url));
const xml=require('fast-xml-parser');
xml.XMLParser.prototype.parse=()=>{throw Error('XML parser must not be reached by signer-only adapter');};
xml.XMLBuilder.prototype.build=()=>{throw Error('XML builder must not be reached by signer-only adapter');};
const ObsClient=require('esdk-obs-nodejs');
assert.equal(require('esdk-obs-nodejs/package.json').version,'3.26.2');
const signer=new ObsClient({access_key_id:'SYNTHETIC_TEST_AK',secret_access_key:'synthetic-test-only-secret',
 server:'https://obs.cn-north-4.myhuaweicloud.com',signature:'obs',ssl_verify:true});
const rows=new Map();let calls=0;
const adapter=new ObsObjects(signer,{bucket:'private',endpoint:'https://obs.cn-north-4.myhuaweicloud.com',fingerprint:'test'},async(url,options)=>{
 calls++;const target=new URL(url);assert.equal(target.hostname,'private.obs.cn-north-4.myhuaweicloud.com');
 assert.ok(target.searchParams.has('Signature'));assert.equal(options.redirect,'error');assert.ok(options.signal);
 const headers=new Map(Object.entries(options.headers).map(([k,v])=>[k.toLowerCase(),v]));
 if(options.method==='PUT'){
  assert.equal(headers.get('x-obs-content-sha256'),hash(options.body));assert.equal(headers.get('content-type'),'application/octet-stream');
  assert.ok(headers.get('content-md5'));rows.set(target.pathname,Buffer.from(options.body));return new Response('');
 }
 if(options.method==='DELETE'){rows.delete(target.pathname);return new Response(null,{status:204});}
 const bytes=rows.get(target.pathname);return new Response(bytes,{headers:{'content-length':String(bytes.length)}});
});
const key='saves/'+'a'.repeat(64)+'/11111111-1111-1111-1111-111111111111/0',bytes=Buffer.from('signer-test');
await adapter.put(key,bytes,hash(bytes));await adapter.delete(key);assert.equal(calls,3);assert.equal(rows.size,0);
console.log('PASS: actual OBS 3.26.2 signer with verified-byte PUT/GET/DELETE; XMLParser/XMLBuilder execution forbidden and unreachable');
