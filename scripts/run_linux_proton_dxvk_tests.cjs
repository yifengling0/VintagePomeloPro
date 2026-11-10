const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const vm = require('node:vm');
const crypto = require('node:crypto');
const ts = require(path.resolve(process.argv[2]));
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'vpp-dxvk-'));
const hash = b => crypto.createHash('sha256').update(b).digest('hex');
const write = (p,b) => {fs.mkdirSync(path.dirname(p),{recursive:true});fs.writeFileSync(p,b);};
const names = ['libproton_probe.so','libproton_vulkan.so','libvulkan_virtio.so','nsiproxy.so','LICENSE.dxvk',
 'dlls/arm64x/d3d11.dll','dlls/arm64x/dxgi.dll','dlls/i386/d3d11.dll','dlls/i386/dxgi.dll',
 'pe/vulkan-1.dll','pe/winevulkan.dll','pe/d3dcompiler_47.dll'];
const backend = Buffer.from('GE Unix bridge');
const blobs = Object.fromEntries(names.map(n=>[n,Buffer.from('source-built:'+n)]));
const manifest = {schemaVersion:1,architecture:'aarch64-linux-gnu',id:hash('support'),
 backend:{path:'files/lib/wine/aarch64-unix/winevulkan.so',bytes:backend.length,sha256:hash(backend)},
 files:names.map(n=>({path:n,bytes:blobs[n].length,sha256:hash(blobs[n])}))};
manifest.network={wineRevision:'c53678ab22a85f0fad3588dd34f0452a463bd015',unixCallSlots:7,
 backend:{path:'files/lib/wine/aarch64-unix/nsiproxy.so',bytes:backend.length,sha256:hash(backend)},
 ntdll:{path:'files/lib/wine/aarch64-unix/ntdll.so',bytes:backend.length,sha256:hash(backend)}};
manifest.installedBytes=manifest.files.reduce((n,f)=>n+f.bytes,0);
for(const row of [manifest.backend,manifest.network.backend,manifest.network.ntdll])write(root+'/package/'+row.path,backend);
write(root+'/home/save','unchanged');write(root+'/prefix/user.reg','unchanged registry');
let failTarget = '', failOpen = '', copies = 0, space = 1e12;
const openFds = new Set();
const deps = {
 '@kit.AbilityKit':{},
 '@kit.CoreFileKit':{fileIo:{
  accessSync:fs.existsSync,lstatSync:p=>{let s;try{s=fs.lstatSync(p);}catch(e){if(e.code==='ENOENT')e.code=13900002;throw e;}return {size:s.size,isFile:()=>s.isFile(),isSymbolicLink:()=>s.isSymbolicLink()};},
  OpenMode:{READ_ONLY:fs.constants.O_RDONLY,WRITE_ONLY:fs.constants.O_WRONLY,CREATE:fs.constants.O_CREAT,TRUNC:fs.constants.O_TRUNC,NOFOLLOW:fs.constants.O_NOFOLLOW,DIR:fs.constants.O_DIRECTORY},
  openSync:(p,f)=>{if(p===failOpen){failOpen='';throw new Error('open interrupted');}const fd=fs.openSync(p,f);openFds.add(fd);return {fd};},closeSync:f=>{fs.closeSync(f.fd);openFds.delete(f.fd);},fsyncSync:fs.fsyncSync,
  readSync:(fd,b)=>fs.readSync(fd,Buffer.from(b),0,b.byteLength,null),writeSync:(fd,b)=>fs.writeSync(fd,Buffer.from(b)),
  unlinkSync:fs.unlinkSync,renameSync:(a,b)=>{if(b===failTarget){failTarget='';throw new Error('interrupted');}fs.renameSync(a,b);}
 },statfs:{getFreeSize:async()=>space}},
 './ConfigurationFiles':{ConfigurationFiles:{text:b=>Buffer.from(b).toString(),json:write,
  write:(p,b)=>write(p,Buffer.from(b)),hash:async b=>hash(Buffer.from(b)),fileHash:async p=>hash(fs.readFileSync(p)),
  ensure:p=>{fs.mkdirSync(p,{recursive:true});assert.ok(!fs.lstatSync(p).isSymbolicLink());}}},
 './ManagedSmokePayloadService':{readSmokeText:p=>fs.existsSync(p)?fs.readFileSync(p,'utf8'):''}
};
const source=fs.readFileSync(path.join(__dirname,'../entry/src/main/ets/service/LinuxProtonDxvk.ets'),'utf8')
 .replaceAll('/data/storage/el2/base/files/steamohos-native/proton-probe',root);
const code=ts.transpileModule(source,{compilerOptions:{target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.CommonJS}}).outputText;
function loadApi(){const api={};vm.runInNewContext(code,{exports:api,setTimeout:f=>setImmediate(f),require:id=>{assert.ok(id in deps,id);return deps[id];}});return api;}
const api=loadApi();
const context={resourceManager:{getRawFileContent:async n=>{
 if(n.endsWith('manifest.json'))return Buffer.from(JSON.stringify(manifest));
 copies++;return blobs[n.replace('linux-proton/dxvk-support/','')];
}}};
const prefix=root+'/prefix/drive_c/windows/';
const builtin=Buffer.alloc(96);builtin.write('MZ');builtin.write('Wine builtin DLL',64);
(async()=>{try{
 await api.ensureLinuxProtonDxvk(context);assert.equal(copies,names.length);
 await api.ensureLinuxProtonDxvk(context);assert.equal(copies,names.length,'unchanged support reused');
 for(const dir of ['system32','syswow64'])for(const dll of ['d3d11.dll','dxgi.dll'])write(prefix+dir+'/'+dll,builtin);
 write(prefix+'syswow64/dxgi.dll','user custom DLL');
 await assert.rejects(api.prepareLinuxProtonDxvkPrefix(),/用户已有 DLL/);
 assert.deepEqual(fs.readFileSync(prefix+'system32/d3d11.dll'),builtin,'group prevalidation prevents partial install');
 write(prefix+'syswow64/dxgi.dll',builtin);
 space=0;
 await assert.rejects(api.prepareLinuxProtonDxvkPrefix(),/空间不足/);
 assert.deepEqual(fs.readFileSync(prefix+'system32/d3d11.dll'),builtin,'space check precedes mutation');
 space=1e12;
 failOpen=root+'/dxvk-prefix-state/old/system32/d3d11.dll.pending';
 await assert.rejects(api.prepareLinuxProtonDxvkPrefix(),/open interrupted/);
 assert.equal(openFds.size,0,'failed output open closes input FD');
 failTarget=prefix+'syswow64/dxgi.dll';
 await assert.rejects(api.prepareLinuxProtonDxvkPrefix(),/interrupted/);
 for(const dir of ['system32','syswow64'])for(const dll of ['d3d11.dll','dxgi.dll'])assert.deepEqual(fs.readFileSync(prefix+dir+'/'+dll),builtin,'interruption rolls back whole group');
 assert.ok(!fs.existsSync(prefix+'system32/vulkan-1.dll'));
 assert.ok(!fs.existsSync(prefix+'syswow64/dxgi.dll.pending'),'failed copy does not leave a pending DLL');
 await api.prepareLinuxProtonDxvkPrefix();
 for(const [arch,dir] of [['arm64x','system32'],['i386','syswow64']])for(const dll of ['d3d11.dll','dxgi.dll'])
  assert.deepEqual(fs.readFileSync(prefix+dir+'/'+dll),blobs['dlls/'+arch+'/'+dll]);
 assert.deepEqual(fs.readFileSync(prefix+'system32/vulkan-1.dll'),blobs['pe/vulkan-1.dll']);
 await api.prepareLinuxProtonDxvkPrefix();assert.ok(!fs.existsSync(root+'/dxvk-prefix-state/journal.json'));
 // Recreate a process death between the DLL rename and receipt/commit. A new
 // service instance must recover the previous receipt and original DLL first.
 const state=root+'/dxvk-prefix-state/';
 const previousReceipt=fs.readFileSync(state+'installed.json','utf8');
 write(state+'old/system32/d3d11.dll',builtin);
 write(state+'installed.json','interrupted receipt');
 write(state+'journal.json',JSON.stringify({committed:false,previousReceipt,changes:[{path:'system32/d3d11.dll',before:hash(builtin),after:hash(blobs['dlls/arm64x/d3d11.dll'])}]}));
 write(prefix+'syswow64/dxgi.dll','user custom DLL');
 await assert.rejects(loadApi().prepareLinuxProtonDxvkPrefix(),/用户已有 DLL/);
 assert.deepEqual(fs.readFileSync(prefix+'system32/d3d11.dll'),builtin,'restart recovered original bytes');
 assert.equal(fs.readFileSync(state+'installed.json','utf8'),previousReceipt,'restart recovered prior ownership');
 write(prefix+'syswow64/dxgi.dll',blobs['dlls/i386/dxgi.dll']);
 await api.prepareLinuxProtonDxvkPrefix();
 // Unknown edits, traversal and symlink targets never become owned by a receipt.
 write(prefix+'system32/d3d11.dll','modified by user');
 await assert.rejects(api.prepareLinuxProtonDxvkPrefix(),/用户已有 DLL/);
 fs.unlinkSync(prefix+'system32/d3d11.dll');fs.symlinkSync(root+'/home/save',prefix+'system32/d3d11.dll');
 await assert.rejects(api.prepareLinuxProtonDxvkPrefix(),/不安全/);
 fs.unlinkSync(prefix+'system32/d3d11.dll');fs.symlinkSync(root+'/home/missing',prefix+'system32/d3d11.dll');
 await assert.rejects(api.prepareLinuxProtonDxvkPrefix(),/不安全/);
 assert.ok(fs.lstatSync(prefix+'system32/d3d11.dll').isSymbolicLink(),'dangling user link is preserved');
 const bad={...manifest,files:manifest.files.map((f,i)=>i?f:{...f,path:'../prefix/user.reg'})};
 assert.throws(()=>api.validateLinuxProtonDxvk(bad),/文件无效/);
 write(root+'/dxvk-prefix-state/journal.json',JSON.stringify({committed:false,previousReceipt:'',changes:[{path:'../../home/save',before:'',after:hash('bad')}]}));
 await assert.rejects(api.prepareLinuxProtonDxvkPrefix(),/路径无效/);
 for(const paired of [manifest.network.backend,manifest.network.ntdll]) {
  write(root+'/package/'+paired.path,'wrong GE version');
  await assert.rejects(api.ensureLinuxProtonDxvk(context),/不匹配/);
  write(root+'/package/'+paired.path,backend);
 }
 assert.throws(()=>api.validateLinuxProtonDxvk({...manifest,network:{...manifest.network,unixCallSlots:8}}),/清单无效/);
 write(root+'/package/'+manifest.backend.path,'wrong GE bridge');
 await assert.rejects(api.ensureLinuxProtonDxvk(context),/不匹配/);
 assert.equal(fs.readFileSync(root+'/home/save','utf8'),'unchanged');
 assert.equal(fs.readFileSync(root+'/prefix/user.reg','utf8'),'unchanged registry');
 assert.equal(openFds.size,0);
 console.log('PASS: support reuse, paired PE widths, group validation, rollback/restart, FD/pending cleanup, unknown DLL/dangling-link/journal protection, GE pairing, save preservation');
}finally{fs.rmSync(root,{recursive:true,force:true});}})().catch(e=>{console.error(e);process.exitCode=1;});
