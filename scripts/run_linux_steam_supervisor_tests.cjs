const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),os=require('node:os');
const crypto=require('node:crypto'),vm=require('node:vm'),ts=require(process.argv[2]);
const temp=fs.mkdtempSync(path.join(os.tmpdir(),'steam-supervisor-')),base=temp+'/private',root=temp+'/steam';
const release=root+'/releases/'+'a'.repeat(64),name='steamohos_gnu_steam_session_hap',target=release+'/runtime/'+name;
const raw=path.join(__dirname,'../entry/src/main/resources/rawfile');
const hash=d=>crypto.createHash('sha256').update(d).digest('hex');
const core=JSON.parse(fs.readFileSync(path.join(raw,'linux-steam/bootstrap/native-guest-core-x86_64/manifest.json')));
const driver=JSON.parse(fs.readFileSync(path.join(raw,'linux-steam/bootstrap/native-vulkan-driver/manifest.json')));
// Exercise filesystem deployment with a small valid ELF fixture; source archive
// identities and full payloads are checked by the component/package verifier.
const driverElf=Buffer.alloc(1024);driverElf.set([127,69,76,70,2,1]);driverElf[18]=183;
const driverFiles=Object.keys(driver.files);
const driverFixture={...driver,files:Object.fromEntries(driverFiles.map(n=>[n,{bytes:driverElf.length,sha256:hash(driverElf)}]))};
fs.mkdirSync(base+'/proton-probe/graphics',{recursive:true});
for(const n of driverFiles)fs.writeFileSync(base+'/proton-probe/graphics/'+n,driverElf);

fs.mkdirSync(base,{recursive:true});fs.mkdirSync(release+'/runtime',{recursive:true});
const pointer=base+'/external-active.path';fs.writeFileSync(pointer,release+'\n');
let writes=0,corrupt=false,manifest=null,damagedResource='';
const deps={
 '@kit.CoreFileKit':{fileIo:{accessSync:fs.existsSync,lstatSync:fs.lstatSync,renameSync:fs.renameSync,mkdirSync:fs.mkdirSync}},
 './ConfigurationFiles':{ConfigurationFiles:{text:d=>Buffer.from(d).toString(),read:p=>fs.readFileSync(p),
   hash:async d=>hash(d),fileHash:async p=>hash(fs.readFileSync(p)),write:(p,d)=>{writes++;fs.writeFileSync(p,d);}}}
};
const code=ts.transpileModule(fs.readFileSync(path.join(__dirname,'../entry/src/main/ets/service/LinuxSteamClientSupervisor.ets'),'utf8')
 .replaceAll('/data/storage/el2/base/files/steamohos-native',base),
 {compilerOptions:{target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.CommonJS}}).outputText;
const api={};vm.runInNewContext(code,{exports:api,require:id=>{assert.ok(id in deps,id);return deps[id];}});
const context={resourceManager:{getRawFileContent:async p=>p==='linux-steam/bootstrap/native-vulkan-driver/manifest.json'?Buffer.from(JSON.stringify(driverFixture)):
 manifest&&p.endsWith('manifest.json')?Buffer.from(JSON.stringify(manifest)):
 (corrupt&&p.endsWith(name)||damagedResource&&p.endsWith('/'+damagedResource))?Buffer.from('broken'):fs.readFileSync(path.join(raw,p))}};
(async()=>{try{
 fs.writeFileSync(release+'/runtime/user-file','preserve');
 await api.ensureLinuxSteamClientSupervisor(context,root);assert.equal(writes,1);
 await api.ensureLinuxSteamClientSupervisor(context,root);assert.equal(writes,1,'verified startup reuses the exact installed supervisor');
 fs.writeFileSync(target,'old-corrupt');corrupt=true;
 await assert.rejects(api.ensureLinuxSteamClientSupervisor(context,root),/校验失败/);
 assert.equal(fs.readFileSync(target,'utf8'),'old-corrupt','failed validation cannot replace a live file');
 corrupt=false;await api.ensureLinuxSteamClientSupervisor(context,root);assert.equal(writes,2);
 const payload=fs.readFileSync(target);
 fs.unlinkSync(target);fs.symlinkSync(release+'/runtime/user-file',target);
 await assert.rejects(api.ensureLinuxSteamClientSupervisor(context,root),/不安全/);
 fs.unlinkSync(target);fs.writeFileSync(target,payload);
 fs.symlinkSync(release+'/runtime/user-file',target+'.pending');
 await assert.rejects(api.ensureLinuxSteamClientSupervisor(context,root),/不安全/);fs.unlinkSync(target+'.pending');
 fs.writeFileSync(pointer,temp+'/other/releases/'+'a'.repeat(64));
 await assert.rejects(api.ensureLinuxSteamClientSupervisor(context,root),/位置无效/);fs.writeFileSync(pointer,release);
 const real=JSON.parse(fs.readFileSync(path.join(raw,'linux-steam/bootstrap/manifest.json')));
 manifest={...real,name:'../steam'};await assert.rejects(api.ensureLinuxSteamClientSupervisor(context,root),/清单无效/);manifest=null;
 const support=['desktop-manifest.json','posix-manifest.json','fex-manifest.json','native-graphics-manifest.json']
   .map(n=>JSON.parse(fs.readFileSync(path.join(raw,'linux-steam/bootstrap/'+n))));
 const beforePosix=writes;
 await api.ensureLinuxSteamPosixSupport(context,root);
 for(const item of support)assert.equal(hash(fs.readFileSync(release+'/runtime/'+item.name)),item.sha256);
 const repairs=JSON.parse(fs.readFileSync(path.join(raw,'linux-steam/bootstrap/native-game-support/manifest.json')));
 const installed=beforePosix+support.length+Object.keys(core.files).length+driverFiles.length+Object.keys(repairs.files).length+3;
 assert.equal(writes,installed,'companions, guest core, native driver and manifests must be installed');
 for(const [leaf,files] of [['native-guest-core-x86_64',core.files],['native-vulkan-driver',driverFixture.files]])
   for(const [n,row] of Object.entries(files))assert.equal(hash(fs.readFileSync(release+'/runtime/'+leaf+'/'+n)),row.sha256);
 await api.ensureLinuxSteamPosixSupport(context,root);
 assert.equal(writes,installed+3,'verified binary payloads must be reused; only manifests are refreshed');
 for(const [n,row] of Object.entries(repairs.files))assert.equal(hash(fs.readFileSync(release+'/runtime/native-game-support/'+n)),row.sha256);
 for(const item of support){
   const file=release+'/runtime/'+item.name;
   fs.writeFileSync(file,'installed-old');damagedResource=item.name;
   await assert.rejects(api.ensureLinuxSteamPosixSupport(context,root),/校验失败/);
   assert.equal(fs.readFileSync(file,'utf8'),'installed-old','invalid payload cannot replace '+item.name);
   damagedResource='';await api.ensureLinuxSteamPosixSupport(context,root);
   assert.equal(hash(fs.readFileSync(file)),item.sha256);
 }
 for(const [n,row] of Object.entries(repairs.files)) {
   const file=release+'/runtime/native-game-support/'+n;
   fs.writeFileSync(file,'installed-old');damagedResource=n;
   await assert.rejects(api.ensureLinuxSteamPosixSupport(context,root),/校验失败/);
   assert.equal(fs.readFileSync(file,'utf8'),'installed-old');
   damagedResource='';await api.ensureLinuxSteamPosixSupport(context,root);
   assert.equal(hash(fs.readFileSync(file)),row.sha256);
   fs.unlinkSync(file);await api.ensureLinuxSteamPosixSupport(context,root);
   assert.equal(hash(fs.readFileSync(file)),row.sha256,'missing repairs must self-install');
 }
 assert.deepEqual(fs.readFileSync(target),payload,'Cloud adapter cannot replace the session supervisor');
 fs.renameSync(release+'/runtime',release+'/runtime-old');fs.symlinkSync(release+'/runtime-old',release+'/runtime');
 await assert.rejects(api.ensureLinuxSteamClientSupervisor(context,root),/目录不安全/);
 assert.equal(fs.readFileSync(release+'/runtime/user-file','utf8'),'preserve');
 console.log('PASS: packaged Steam supervisor identity, cached reuse, corrupt repair, atomic replacement, foreign release and symlink guards');
}finally{fs.rmSync(temp,{recursive:true,force:true});}})().catch(e=>{console.error(e);process.exitCode=1;});
