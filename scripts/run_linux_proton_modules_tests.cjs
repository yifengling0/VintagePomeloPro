const assert=require('node:assert/strict'),fs=require('node:fs'),os=require('node:os'),path=require('node:path'),vm=require('node:vm'),crypto=require('node:crypto');
const ts=require(path.resolve(process.argv[2]));
const root=fs.mkdtempSync(path.join(os.tmpdir(),'vpp-module-'));
const hash=b=>crypto.createHash('sha256').update(b).digest('hex');
const write=(p,b)=>{fs.mkdirSync(path.dirname(p),{recursive:true});fs.writeFileSync(p,b);};
const backend=Buffer.from('GE Unix winevulkan');
const archive=Buffer.from('verified compressed missing originals');
const payload={
 'system32/concrt140.dll':Buffer.from('original ARM64X dynamic module'),
 'system32/d3d11.dll':Buffer.from('original builtin d3d11'),
 'system32/user.dll':Buffer.from('original builtin with user override'),
 'system32/link.dll':Buffer.from('original builtin with user link'),
 'system32/drivers/test.sys':Buffer.from('original driver'),
 'syswow64/dynamic.dll':Buffer.from('original x86 module'),
 'system32/base.dll':Buffer.from('original base module'),
 'system32/icuuc68.dll':Buffer.from('original ARM64 ICU'),
 'syswow64/icuuc68.dll':Buffer.from('original x86 ICU')
};
const commonControls='microsoft.windows.common-controls_6595b64144ccf1df_6.0.2600.2982_none_deadbeef';
for(const arch of ['arm64','x86']){payload[`winsxs/${arch}_${commonControls}/comctl32.dll`]=Buffer.from('original '+arch+' v6');payload[`winsxs/manifests/${arch}_${commonControls}.manifest`]=Buffer.from('original '+arch+' manifest');}
const files=Object.entries(payload).map(([p,b])=>{const arch=p.startsWith('system32/')?'aarch64-windows':'i386-windows';const original=p.startsWith('winsxs/')?'files/share/default_pfx_arm64/drive_c/windows/'+p:'files/lib/wine/'+(p.endsWith('icuuc68.dll')?'icu/':'')+arch+'/'+path.basename(p);return {path:p,original,source:p.endsWith('base.dll')?'package/'+original:'archive',bytes:b.length,sha256:hash(b)};});
files.find(f=>f.path==='system32/user.dll').builtin=true;
const dir=directory=>({files:files.filter(f=>f.original.startsWith(directory+'/')).length,bytes:files.filter(f=>f.original.startsWith(directory+'/')).reduce((n,f)=>n+f.bytes,0),membersSha256:hash(directory)});
const manifest={schemaVersion:1,architecture:'aarch64-linux-gnu',id:hash('module support'),backend:{path:'files/lib/wine/aarch64-unix/winevulkan.so',bytes:backend.length,sha256:hash(backend)},files,
 closure:{schema:1,assemblies:{files:4,bytes:files.filter(f=>f.path.startsWith('winsxs/')).reduce((n,f)=>n+f.bytes,0),membersSha256:hash('sxs')},directories:{'files/lib/wine/aarch64-windows':dir('files/lib/wine/aarch64-windows'),'files/lib/wine/i386-windows':dir('files/lib/wine/i386-windows'),'files/lib/wine/icu/aarch64-windows':dir('files/lib/wine/icu/aarch64-windows'),'files/lib/wine/icu/i386-windows':dir('files/lib/wine/icu/i386-windows'),'files/lib/wine/aarch64-unix':{files:1,bytes:backend.length,membersSha256:hash(backend)}}},
 archive:{bytes:archive.length,sha256:hash(archive)}};
manifest.installedBytes=files.reduce((n,f)=>n+f.bytes,0);manifest.archiveInstalledBytes=files.filter(f=>f.source==='archive').reduce((n,f)=>n+f.bytes,0);
manifest.builtinInstalledBytes=files.filter(f=>f.builtin).reduce((n,f)=>n+f.bytes,0);
write(root+'/package/'+manifest.backend.path,backend);write(root+'/'+files.find(f=>f.path.endsWith('base.dll')).source,payload['system32/base.dll']);
write(root+'/save/slot','save bytes');write(root+'/prefix/user.reg','registry bytes');
let space=1e12,renames=0,failRename='',badExtract=false,badSource=false,failOpen='',extracts=0;
const fds=new Set();
const stat=p=>{try{return fs.lstatSync(p);}catch(e){if(e.code==='ENOENT')e.code=13900002;throw e;}};
const deps={
 '@kit.AbilityKit':{},
 '@kit.CoreFileKit':{fileIo:{
  lstatSync:stat,statSync:n=>typeof n==='number'?fs.fstatSync(n):fs.statSync(n),listFileSync:fs.readdirSync,
  OpenMode:{CREATE:fs.constants.O_CREAT,WRITE_ONLY:fs.constants.O_WRONLY,READ_ONLY:fs.constants.O_RDONLY,TRUNC:fs.constants.O_TRUNC,NOFOLLOW:fs.constants.O_NOFOLLOW},
  openSync:(p,f)=>{if(p===failOpen){failOpen='';throw new Error('output open failed');}const fd=fs.openSync(p,f);fds.add(fd);return {fd};},
  closeSync:f=>{fs.closeSync(f.fd);fds.delete(f.fd);},fsyncSync:fs.fsyncSync,
  readSync:(fd,b,o)=>fs.readSync(fd,Buffer.from(b),0,o.length,o.offset),writeSync:(fd,b)=>fs.writeSync(fd,Buffer.from(b)),
  renameSync:(a,b)=>{if(b===failRename){failRename='';throw new Error('interrupted');}fs.renameSync(a,b);renames++;},
  unlinkSync:fs.unlinkSync,rmdirSync:fs.rmdirSync
 },statfs:{getFreeSize:async()=>space}},
 '@kit.BasicServicesKit':{zlib:{decompressFile:async(_a,d)=>{extracts++;for(const f of files.filter(f=>f.source==='archive'))write(d+'/'+f.path,badExtract?Buffer.from('corrupt'):payload[f.path]);}}},
 './ConfigurationFiles':{ConfigurationFiles:{text:b=>Buffer.from(b).toString(),json:write,fileHash:async p=>hash(fs.readFileSync(p)),ensure:p=>{if(!fs.existsSync(p))fs.mkdirSync(p,{recursive:true});assert.ok(fs.lstatSync(p).isDirectory()&&!fs.lstatSync(p).isSymbolicLink(),'unsafe parent');}}}
};
const source=fs.readFileSync(path.join(__dirname,'../entry/src/main/ets/service/LinuxProtonModules.ets'),'utf8').replaceAll('/data/storage/el2/base/files/steamohos-native/proton-probe',root);
const code=ts.transpileModule(source,{compilerOptions:{target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.CommonJS}}).outputText;
function load(){const api={};vm.runInNewContext(code,{exports:api,setTimeout:f=>setImmediate(f),require:id=>{assert.ok(id in deps,id);return deps[id];}});return api;}
const api=load(), prefix=root+'/prefix/drive_c/windows/';
const context={resourceManager:{getRawFileContent:async()=>Buffer.from(JSON.stringify(manifest)),getRawFdSync:()=>{const p=root+'/raw.zip';write(p,archive);const fd=fs.openSync(p,'r');fds.add(fd);return {fd,offset:0,length:archive.length};},closeRawFdSync:()=>{for(const fd of fds){fs.closeSync(fd);fds.delete(fd);}}}};
(async()=>{try{
 space=0;await assert.rejects(api.ensureLinuxProtonModules(context,()=>{}),/空间不足/);assert.equal(renames,0);
 space=1e12;badExtract=true;await assert.rejects(api.ensureLinuxProtonModules(context,()=>{}),/校验失败/);assert.equal(renames,0,'validate all before activation');badExtract=false;
 write(prefix+'system32/user.dll','user custom DLL');
 write(prefix+'system32/d3d11.dll','source-built DXVK');
 fs.symlinkSync(root+'/missing-user-file',prefix+'system32/link.dll');
 failRename=prefix+'syswow64/dynamic.dll';
 await assert.rejects(api.ensureLinuxProtonModules(context,()=>{}),/interrupted/);
 assert.deepEqual(fs.readFileSync(prefix+'system32/concrt140.dll'),payload['system32/concrt140.dll'],'already completed modules are reusable');
 assert.ok(!fs.existsSync(root+'/module-install'),'interrupted scratch cleaned');
 // Retry through a new service instance: no prior receipt is required.
 await load().ensureLinuxProtonModules(context,()=>{});
 assert.deepEqual(fs.readFileSync(prefix+'syswow64/dynamic.dll'),payload['syswow64/dynamic.dll']);
 assert.deepEqual(fs.readFileSync(prefix+'system32/icuuc68.dll'),payload['system32/icuuc68.dll']);
 assert.deepEqual(fs.readFileSync(prefix+'syswow64/icuuc68.dll'),payload['syswow64/icuuc68.dll']);
 for(const f of files.filter(f=>f.path.startsWith('winsxs/')))assert.deepEqual(fs.readFileSync(prefix+f.path),payload[f.path]);
 const noAssembly={...manifest,files:files.filter(f=>!f.path.startsWith('winsxs/'))};assert.throws(()=>api.validateLinuxProtonModules(noAssembly),/不完整/);
 assert.throws(()=>api.validateLinuxProtonModules({...manifest,files:files.map(f=>f.path.startsWith('winsxs/')?{...f,original:'files/share/other/'+f.path}:f)}),/来源无效/);
 assert.deepEqual(fs.readFileSync(prefix+'system32/drivers/test.sys'),payload['system32/drivers/test.sys']);
 assert.deepEqual(fs.readFileSync(prefix+'system32/base.dll'),payload['system32/base.dll']);
 assert.equal(fs.readFileSync(prefix+'system32/user.dll','utf8'),'user custom DLL');
 const builtin=root+'/package/'+files.find(f=>f.builtin).original;
 assert.deepEqual(fs.readFileSync(builtin),payload['system32/user.dll'],'Unix companion receives exact original, never user override');
 fs.unlinkSync(builtin);await api.ensureLinuxProtonModules(context,()=>{});
 assert.deepEqual(fs.readFileSync(builtin),payload['system32/user.dll'],'existing prefix is repaired at builtin search path too');
 write(builtin,'changed original');await assert.rejects(api.ensureLinuxProtonModules(context,()=>{}),/已改变/);
 write(builtin,payload['system32/user.dll']);
 assert.equal(fs.readFileSync(prefix+'system32/d3d11.dll','utf8'),'source-built DXVK','backend group remains regular and untouched');
 assert.ok(fs.lstatSync(prefix+'system32/link.dll').isSymbolicLink(),'dangling user link preserved');
 const before=extracts;await api.ensureLinuxProtonModules(context,()=>{});assert.equal(extracts,before,'warm launch never decompresses');
 // Source nofollow and checksum protections for missing base modules.
 fs.unlinkSync(prefix+'system32/base.dll');const base=root+'/'+files.find(f=>f.path.endsWith('base.dll')).source;
 fs.unlinkSync(base);fs.symlinkSync(root+'/save/slot',base);
 await assert.rejects(api.ensureLinuxProtonModules(context,()=>{}));assert.ok(fs.lstatSync(base).isSymbolicLink());
 fs.unlinkSync(base);write(base,'wrong GE base bytes');await assert.rejects(api.ensureLinuxProtonModules(context,()=>{}),/无效|校验失败/);
 write(base,payload['system32/base.dll']);failOpen=root+'/module-install/module.pending';await assert.rejects(api.ensureLinuxProtonModules(context,()=>{}),/output open failed/);assert.equal(fds.size,0,'input FD closed on output failure');
 await api.ensureLinuxProtonModules(context,()=>{});
 for(const file of [{...files[0],path:'../save/slot'},{...files[0],original:'files/lib/wine/i386-windows/concrt140.dll'},{...files[0],source:'../../save/slot'}])
  assert.throws(()=>api.validateLinuxProtonModules({...manifest,files:[file,...files.slice(1)]}),/无效/);
 assert.throws(()=>api.validateLinuxProtonModules({...manifest,files:[...files,files[0]]}),/无效/);
 write(root+'/package/'+manifest.backend.path,'other GE backend');await assert.rejects(api.ensureLinuxProtonModules(context,()=>{}),/不匹配/);
 assert.equal(fs.readFileSync(root+'/save/slot','utf8'),'save bytes');assert.equal(fs.readFileSync(root+'/prefix/user.reg','utf8'),'registry bytes');assert.equal(fds.size,0);
 console.log('PASS: complete ABI seeding, Unix companion binding/recovery, stage integrity, cold/retry/warm, regular DXVK, user DLL/link/save preservation, nofollow/hash checks, FD cleanup, low disk');
}finally{fs.rmSync(root,{recursive:true,force:true});}})().catch(e=>{console.error(e);process.exitCode=1;});
