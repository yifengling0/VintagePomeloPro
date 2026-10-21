const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),os=require('node:os'),crypto=require('node:crypto'),vm=require('node:vm');
const ts=require(path.resolve(process.argv[2]));
const root=fs.mkdtempSync(path.join(os.tmpdir(),'hmosdeck-managed-bridge-'));
let failRename=0, renames=0;
const fileIo={accessSync:fs.existsSync,statSync:fs.statSync,mkdirSync:(p)=>fs.mkdirSync(p,{recursive:true}),
 copyFileSync:fs.copyFileSync,renameSync(a,b){if(++renames===failRename)throw Error('interrupted');fs.renameSync(a,b);},unlinkSync:fs.unlinkSync,
 OpenMode:{READ_ONLY:'r'},openSync:p=>({fd:fs.openSync(p,'r')}),closeSync:f=>fs.closeSync(f.fd),
 readSync:(fd,buffer,options)=>fs.readSync(fd,new Uint8Array(buffer),0,options.length,options.offset)};
const digest=p=>crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex');
const exportsObject={};
const deps={'@kit.CoreFileKit':{fileIo},'./ConfigurationFiles':{ConfigurationFiles:{
 read(p,max){assert.ok(fs.statSync(p).size<=max);return fs.readFileSync(p,'utf8');},text:x=>x,fileHash:async p=>digest(p)}}};
const file=path.join(__dirname,'../entry/src/main/ets/service/ManagedLinuxSteamBridgeService.ets');
vm.runInNewContext(ts.transpileModule(fs.readFileSync(file,'utf8'),{compilerOptions:{target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.CommonJS}}).outputText,
 {exports:exportsObject,require:id=>{assert.ok(id in deps,id);return deps[id];}});
const service=exportsObject.ManagedLinuxSteamBridgeService;
function fixture(name){
 const runtime=path.join(root,name,'wine'),prefix=path.join(root,name,'prefix'),files=[];
 for(const arch of ['i386','aarch64'])for(const module of ['kernelbase','lsteamclient']){
  const rel=`bin/${arch}-windows/${module}.dll`,source=path.join(runtime,rel),data=Buffer.alloc(1024,3);
  if(module==='kernelbase')data.write('Wine builtin DLL',64);
  fs.mkdirSync(path.dirname(source),{recursive:true});fs.writeFileSync(source,data);
  files.push({path:rel,bytes:data.length,sha256:digest(source)});
 }
 const marker=path.join(runtime,'linux-steam-bridge.json');fs.writeFileSync(marker,JSON.stringify({schemaVersion:1,files}));
 return {runtime,prefix,files,marker,target:record=>path.join(prefix,'drive_c/windows',record.path.includes('/i386-')?'syswow64':'system32',path.basename(record.path))};
}
(async()=>{
 try{
  await service.synchronize(path.join(root,'no-marker'),path.join(root,'untouched'));
  assert.equal(fs.existsSync(path.join(root,'untouched')),false);
  const f=fixture('fresh');await service.synchronize(f.runtime,f.prefix);
  for(const record of f.files)assert.equal(digest(f.target(record)),record.sha256);
  renames=0;await service.synchronize(f.runtime,f.prefix);assert.equal(renames,0,'unchanged prefix does not rewrite DLLs');
  const bad=fixture('incomplete');fs.unlinkSync(path.join(bad.runtime,bad.files[3].path));
  await assert.rejects(service.synchronize(bad.runtime,bad.prefix),/校验失败/);assert.equal(fs.existsSync(bad.prefix),false,'incomplete pair never changes prefix');
  const corrupt=fixture('corrupt');fs.writeFileSync(path.join(corrupt.runtime,corrupt.files[3].path),Buffer.alloc(1024,5));
  await assert.rejects(service.synchronize(corrupt.runtime,corrupt.prefix),/校验失败/);assert.equal(fs.existsSync(corrupt.prefix),false);
  const custom=fixture('custom');const target=custom.target(custom.files[0]);fs.mkdirSync(path.dirname(target),{recursive:true});fs.writeFileSync(target,Buffer.alloc(1024,9));
  await assert.rejects(service.synchronize(custom.runtime,custom.prefix),/自定义/);assert.deepEqual(fs.readFileSync(target),Buffer.alloc(1024,9));
  const interrupted=fixture('interrupted');renames=0;failRename=2;
  await assert.rejects(service.synchronize(interrupted.runtime,interrupted.prefix),/interrupted/);
  failRename=0;await service.synchronize(interrupted.runtime,interrupted.prefix);
  for(const record of interrupted.files)assert.equal(digest(interrupted.target(record)),record.sha256);
  const pathEscape=fixture('paths');pathEscape.files[3].path='../../user.reg';fs.writeFileSync(pathEscape.marker,JSON.stringify({schemaVersion:1,files:pathEscape.files}));
  await assert.rejects(service.synchronize(pathEscape.runtime,pathEscape.prefix),/路径无效/);
  console.log('Managed Steam bridge: fresh/update/idempotence, complete-pair validation, corruption, custom kernelbase, interrupted migration and path boundaries PASS');
 }finally{fs.rmSync(root,{recursive:true,force:true});}
})().catch(error=>{console.error(error);process.exitCode=1;});
