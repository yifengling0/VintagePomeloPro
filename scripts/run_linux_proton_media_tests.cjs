const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const vm = require('node:vm');
const crypto = require('node:crypto');
const ts = require(path.resolve(process.argv[2]));
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'proton-media-'));
const raw = path.join(__dirname,'../entry/src/main/resources/rawfile');
const hash = data => crypto.createHash('sha256').update(data).digest('hex');
const original=JSON.parse(fs.readFileSync(path.join(raw,'linux-proton/media/manifest.json')));
const manifest=JSON.parse(JSON.stringify(original));
const backend=Buffer.from('locked GE backend fixture');
manifest.backend={...manifest.backend,bytes:backend.length,sha256:hash(backend)};
function write(p,b){fs.mkdirSync(path.dirname(p),{recursive:true});fs.writeFileSync(p,b);}
write(path.join(root,'package',manifest.backend.path),backend);
write(root+'/prefix/user.reg','preserved registry');write(root+'/home/save','preserved save');
let hashes=0,copies=0,lowSpace=false,badManifest=null,failRename=false;
const deps={
 '@kit.AbilityKit':{},
 '@kit.CoreFileKit':{fileIo:{
  accessSync:fs.existsSync,lstatSync:p=>{const s=fs.lstatSync(p);return {size:s.size,mtime:s.mtimeMs,isFile:()=>s.isFile(),isSymbolicLink:()=>s.isSymbolicLink()};},
  OpenMode:{CREATE:fs.constants.O_CREAT,WRITE_ONLY:fs.constants.O_WRONLY,TRUNC:fs.constants.O_TRUNC,NOFOLLOW:fs.constants.O_NOFOLLOW},
  openSync:(p,f)=>({fd:fs.openSync(p,f)}),closeSync:f=>fs.closeSync(f.fd),fsyncSync:fs.fsyncSync,
  readSync:(fd,b,o)=>fs.readSync(fd,Buffer.from(b),0,o.length,o.offset),writeSync:(fd,b)=>fs.writeSync(fd,Buffer.from(b)),
  renameSync:(from,to)=>{if(failRename){failRename=false;throw new Error('injected interruption');}fs.renameSync(from,to);},unlinkSync:fs.unlinkSync
 },statfs:{getFreeSize:async()=>lowSpace?0:1e12}},
 './ConfigurationFiles':{ConfigurationFiles:{
  ensure:p=>{fs.mkdirSync(p,{recursive:true});assert.ok(!fs.lstatSync(p).isSymbolicLink());},
  text:b=>Buffer.from(b).toString(),fileHash:async p=>{hashes++;return hash(fs.readFileSync(p));},json:write
 }},
 './ManagedSmokePayloadService':{readSmokeText:p=>fs.existsSync(p)?fs.readFileSync(p,'utf8'):''}
};
const source=fs.readFileSync(path.join(__dirname,'../entry/src/main/ets/service/LinuxProtonMedia.ets'),'utf8').replaceAll('/data/storage/el2/base/files/steamohos-native/proton-probe',root);
const code=ts.transpileModule(source,{compilerOptions:{target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.CommonJS}}).outputText;
const api={};vm.runInNewContext(code,{exports:api,setTimeout:f=>setImmediate(f),require:id=>{assert.ok(id in deps,id);return deps[id];}});
const context={resourceManager:{getRawFileContent:async()=>Buffer.from(JSON.stringify(badManifest??manifest)),
 getRawFdSync:name=>{copies++;const p=path.join(raw,name);return {fd:fs.openSync(p,'r'),offset:0,length:fs.statSync(p).size};},closeRawFdSync:()=>{}}};
// ResourceManager owns descriptor lifetime; emulate closeRawFdSync explicitly.
const descriptors=new Map();const get=context.resourceManager.getRawFdSync;
context.resourceManager.getRawFdSync=name=>{const r=get(name);descriptors.set(name,r.fd);return r;};
context.resourceManager.closeRawFdSync=name=>{fs.closeSync(descriptors.get(name));descriptors.delete(name);};
(async()=>{try{
 await api.ensureLinuxProtonMedia(context);assert.equal(copies,manifest.files.length);
 const prior=hashes;await api.ensureLinuxProtonMedia(context);assert.equal(copies,manifest.files.length);assert.equal(hashes,prior+1,'warm launch hashes backend only');
 const codec=root+'/libavcodec.so.62';fs.writeFileSync(codec,'corrupt');await api.ensureLinuxProtonMedia(context);assert.equal(copies,manifest.files.length+1);
 fs.unlinkSync(codec);lowSpace=true;await assert.rejects(api.ensureLinuxProtonMedia(context),/空间不足/);lowSpace=false;
 failRename=true;await assert.rejects(api.ensureLinuxProtonMedia(context),/interruption/);assert.ok(!fs.existsSync(codec+'.pending'));
 await api.ensureLinuxProtonMedia(context);assert.ok(fs.existsSync(codec));
 fs.unlinkSync(codec);fs.symlinkSync(root+'/home/save',codec);await assert.rejects(api.ensureLinuxProtonMedia(context),/不安全/);fs.unlinkSync(codec);
 badManifest={...manifest,files:manifest.files.map((f,i)=>i?f:{...f,path:'../prefix/user.reg'})};await assert.rejects(api.ensureLinuxProtonMedia(context),/文件无效/);badManifest=null;
 badManifest={...manifest,files:manifest.files.map((f,i)=>i?f:{...f,sha256:'0'.repeat(64)})};
 const first=manifest.files[0];const firstPath=first.path.startsWith('files/lib/aarch64-linux-gnu/')?root+'/'+path.basename(first.path):root+'/media/'+first.path;
 if(fs.existsSync(firstPath))fs.unlinkSync(firstPath);await assert.rejects(api.ensureLinuxProtonMedia(context),/校验失败/);badManifest=null;
 write(path.join(root,'package',manifest.backend.path),'incompatible');await assert.rejects(api.ensureLinuxProtonMedia(context),/不匹配/);
 assert.equal(fs.readFileSync(root+'/prefix/user.reg','utf8'),'preserved registry');assert.equal(fs.readFileSync(root+'/home/save','utf8'),'preserved save');
 assert.equal(descriptors.size,0);
 console.log('PASS: real GNU media payload, cold/warm install, repair, partial failure, space, links, path/hash/ABI rejection and user data preservation');
}finally{for(const fd of descriptors.values())fs.closeSync(fd);fs.rmSync(root,{recursive:true,force:true});}})().catch(e=>{console.error(e);process.exitCode=1;});
