const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),os=require('node:os');
const crypto=require('node:crypto'),vm=require('node:vm'),ts=require(process.argv[2]);
const temp=fs.mkdtempSync(path.join(os.tmpdir(),'steam-supervisor-')),base=temp+'/private',root=temp+'/steam';
const release=root+'/releases/'+'a'.repeat(64),name='steamohos_gnu_steam_session_hap',target=release+'/runtime/'+name;
const raw=path.join(__dirname,'../entry/src/main/resources/rawfile');
const hash=d=>crypto.createHash('sha256').update(d).digest('hex');
fs.mkdirSync(base,{recursive:true});fs.mkdirSync(release+'/runtime',{recursive:true});
const pointer=base+'/external-active.path';fs.writeFileSync(pointer,release+'\n');
let writes=0,corrupt=false,manifest=null;
const deps={
 '@kit.CoreFileKit':{fileIo:{accessSync:fs.existsSync,lstatSync:fs.lstatSync,renameSync:fs.renameSync}},
 './ConfigurationFiles':{ConfigurationFiles:{text:d=>Buffer.from(d).toString(),read:p=>fs.readFileSync(p),
   hash:async d=>hash(d),fileHash:async p=>hash(fs.readFileSync(p)),write:(p,d)=>{writes++;fs.writeFileSync(p,d);}}}
};
const code=ts.transpileModule(fs.readFileSync(path.join(__dirname,'../entry/src/main/ets/service/LinuxSteamClientSupervisor.ets'),'utf8')
 .replaceAll('/data/storage/el2/base/files/steamohos-native',base),
 {compilerOptions:{target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.CommonJS}}).outputText;
const api={};vm.runInNewContext(code,{exports:api,require:id=>{assert.ok(id in deps,id);return deps[id];}});
const context={resourceManager:{getRawFileContent:async p=>manifest&&p.endsWith('manifest.json')?Buffer.from(JSON.stringify(manifest)):
 corrupt&&p.endsWith(name)?Buffer.from('broken'):fs.readFileSync(path.join(raw,p))}};
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
 const posix='libsteamohos_posix_hap.so',posixTarget=release+'/runtime/'+posix;
 const posixManifest=JSON.parse(fs.readFileSync(path.join(raw,'linux-steam/bootstrap/posix-manifest.json')));
 const beforePosix=writes;
 await api.ensureLinuxSteamPosixSupport(context,root);
 assert.equal(hash(fs.readFileSync(posixTarget)),posixManifest.sha256);
 await api.ensureLinuxSteamPosixSupport(context,root);
 assert.equal(writes,beforePosix+1,'HAP POSIX support is separately verified and reused');
 assert.deepEqual(fs.readFileSync(target),payload,'Cloud adapter cannot replace the session supervisor');
 fs.renameSync(release+'/runtime',release+'/runtime-old');fs.symlinkSync(release+'/runtime-old',release+'/runtime');
 await assert.rejects(api.ensureLinuxSteamClientSupervisor(context,root),/目录不安全/);
 assert.equal(fs.readFileSync(release+'/runtime/user-file','utf8'),'preserve');
 console.log('PASS: packaged Steam supervisor identity, cached reuse, corrupt repair, atomic replacement, foreign release and symlink guards');
}finally{fs.rmSync(temp,{recursive:true,force:true});}})().catch(e=>{console.error(e);process.exitCode=1;});
