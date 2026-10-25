const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const crypto = require('node:crypto');
const vm = require('node:vm');
const ts = require(path.resolve(process.argv[2]));
const home = fs.mkdtempSync(path.join(os.tmpdir(), 'vpp-proton-test-'));
const base = home + '/steamohos-native', root = base + '/proton-probe', scratch = base + '/proton-component-install';
const hash = data => crypto.createHash('sha256').update(data).digest('hex');
const write = (name, text) => { fs.mkdirSync(path.dirname(name), {recursive:true}); fs.writeFileSync(name,text); };
let files = {}, manifest, hashes = 0, decompressions = 0, lowSpace = false, failActivation = false, badZip = false;
const archive = Buffer.from('verified component archive');
const archivePath = home + '/embedded.zip'; write(archivePath, archive);
const makeManifest = version => {
  files = {};
  const names = ['package/files/lib/wine/aarch64-unix/wine','hmosdeck','libproton_probe.so',
    'prefix/drive_c/proton-x86-locale.exe','prefix/drive_c/dxvk26/d3d11.dll','prefix/drive_c/dxvk26/dxgi.dll',
    'graphics/libproton_egl.so','graphics/libproton_vulkan.so','fonts/tahoma.ttf','locale/zh_CN.UTF-8/LC_CTYPE',
    'licenses/NOTICE.txt','audio-direct','manual-launch.version','runtime-modules.list','prefix-modules.list',
    ...Array.from({length:8}, (_,i) => `package/files/lib/${i}.dll`)];
  for (const name of names) files[name] = Buffer.from(`${version}:${name}`);
  const rows = names.map(name => ({path:name,bytes:files[name].length,sha256:hash(files[name]),mode:384}));
  manifest = {schemaVersion:1,id:hash(version),bytes:archive.length,sha256:hash(archive),
    installedBytes:rows.reduce((n,f)=>n+f.bytes,0),files:rows};
};
const exists = name => fs.existsSync(name);
const deps = {
  '@kit.AbilityKit': {}, '@kit.CoreFileKit': {fileIo: {
    accessSync: exists, mkdirSync: (name, recursive) => fs.mkdirSync(name,{recursive}),
    lstatSync: name => {const s=fs.lstatSync(name);return {size:s.size,mtime:s.mtimeMs,isFile:()=>s.isFile(),isDirectory:()=>s.isDirectory(),isSymbolicLink:()=>s.isSymbolicLink()};},
    statSync: value => typeof value === 'number' ? fs.fstatSync(value) : fs.statSync(value),
    listFileSync: name=>fs.readdirSync(name), unlinkSync: name=>fs.unlinkSync(name),rmdirSync:name=>fs.rmdirSync(name),
    renameSync: (from,to) => {
      if (failActivation && from===scratch+'/new/graphics') { failActivation=false;throw new Error('injected activation failure'); }
      fs.renameSync(from,to);
    },
    OpenMode: {CREATE:fs.constants.O_CREAT,WRITE_ONLY:fs.constants.O_WRONLY,TRUNC:fs.constants.O_TRUNC,NOFOLLOW:fs.constants.O_NOFOLLOW},
    openSync: (name,flags) => ({fd:fs.openSync(name,flags)}),closeSync:file=>fs.closeSync(file.fd),fsyncSync:fd=>fs.fsyncSync(fd),
    readSync: (fd, buffer, options) => fs.readSync(fd,Buffer.from(buffer),0,options.length,options.offset),
    writeSync: (fd,buffer)=>fs.writeSync(fd,Buffer.from(buffer))
  },statfs:{getFreeSize:async()=>lowSpace?0:1e12}},
  '@kit.BasicServicesKit': {zlib:{decompressFile:async(_archive,destination)=>{
    decompressions++;
    for (const [name,data] of Object.entries(files)) write(destination+'/'+name,data);
    if(badZip) write(destination+'/hmosdeck','corrupt');
  }}},
  './ConfigurationFiles': {ConfigurationFiles: {
    ensure: name=>{fs.mkdirSync(name,{recursive:true});assert.ok(!fs.lstatSync(name).isSymbolicLink());},
    text: data=>Buffer.from(data).toString(),json:write,
    fileHash:async name=>{hashes++;return hash(fs.readFileSync(name));}
  }},
  './ManagedSmokePayloadService': {ensureSmokeDirectory:()=>true,readSmokeText:name=>exists(name)?fs.readFileSync(name,'utf8'):''}
};
const exportsObject={};
const source=fs.readFileSync(path.join(__dirname,'../entry/src/main/ets/service/LinuxGnuProtonComponent.ets'),'utf8')
  .replace('/data/storage/el2/base/files/steamohos-native',base);
const code=ts.transpileModule(source,{compilerOptions:{target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.CommonJS}}).outputText;
vm.runInNewContext(code,{exports:exportsObject,setTimeout:fn=>setImmediate(fn),require:id=>{assert.ok(id in deps,id);return deps[id];}});
const service=exportsObject.LinuxGnuProtonComponent;
const context={resourceManager:{getRawFileContent:async()=>Buffer.from(JSON.stringify(manifest)),
  getRawFdSync:()=>({fd:fs.openSync(archivePath,'r'),length:archive.length,offset:0}),closeRawFdSync:()=>{}}};
(async()=>{
  try {
    makeManifest('v1');
    for(const name of ['../game','home/save','prefix/system.reg','prefix/drive_c/game/save','package/../home/x','/absolute','package//x','package\\x'])
      assert.throws(()=>exportsObject.protonComponentGroup(name));
    const invalid={...manifest,files:[...manifest.files,manifest.files[0]]};
    assert.throws(()=>exportsObject.validateProtonComponent(invalid));
    write(root+'/prefix/user.reg','user registry');write(root+'/home/save','save');
    await Promise.all([service.ensureReady(context),service.ensureReady(context)]);
    assert.equal(decompressions,1);assert.ok(service.ready());
    assert.equal(fs.readFileSync(root+'/prefix/user.reg','utf8'),'user registry');
    assert.equal(fs.readFileSync(root+'/home/save','utf8'),'save');
    const firstHashes=hashes; await service.ensureReady(context);
    assert.equal(hashes,firstHashes);assert.equal(decompressions,1,'repeat startup must reuse receipt');
    service.setSessionActive(true);await service.ensureReady(context);service.setSessionActive(false);
    const previous=fs.readFileSync(root+'/hmosdeck','utf8');const oldReceipt=fs.readFileSync(root+'/component-installed.json','utf8');
    makeManifest('v2');lowSpace=true;
    await assert.rejects(service.ensureReady(context),/空间不足/);lowSpace=false;
    assert.equal(fs.readFileSync(root+'/hmosdeck','utf8'),previous);
    badZip=true; await assert.rejects(service.ensureReady(context),/文件校验失败/);badZip=false;
    assert.equal(fs.readFileSync(root+'/hmosdeck','utf8'),previous);
    failActivation=true;await assert.rejects(service.ensureReady(context),/activation failure/);
    assert.equal(fs.readFileSync(root+'/hmosdeck','utf8'),previous);
    assert.equal(fs.readFileSync(root+'/component-installed.json','utf8'),oldReceipt);
    assert.ok(!exists(scratch+'/transaction.json'));
    await service.ensureReady(context);assert.ok(fs.readFileSync(root+'/hmosdeck','utf8').startsWith('v2:'));
    // Simulate process death between moving an old group and activating a new one.
    fs.mkdirSync(scratch+'/old',{recursive:true});fs.renameSync(root+'/graphics',scratch+'/old/graphics');
    write(scratch+'/transaction.json',JSON.stringify({committed:false,previousReceipt:fs.readFileSync(root+'/component-installed.json','utf8'),changes:[{path:'graphics',hadOld:true}]}));
    await service.ensureReady(context);assert.ok(exists(root+'/graphics/libproton_egl.so'));
    write(root+'/hmosdeck','damaged');await service.ensureReady(context);
    assert.equal(fs.readFileSync(root+'/hmosdeck','utf8'),files.hmosdeck.toString());
    const probeResult=await service.verifyFreshPrefix(context,async()=>{
      assert.ok(!exists(root+'/prefix/user.reg'),'probe must use an empty prefix');
      assert.ok(exists(root+'/prefix/drive_c/proton-x86-locale.exe'),'helper must be delivered automatically');
      write(root+'/prefix/generated.reg','generated only');return 'initialized';
    });
    assert.equal(probeResult,'initialized');assert.ok(!exists(root+'/prefix/generated.reg'));
    await assert.rejects(service.verifyFreshPrefix(context,async()=>{throw new Error('probe child failed');}),/probe child failed/);
    assert.ok(!exists(root+'/prefix-before-install-test'));
    assert.equal(fs.readFileSync(root+'/prefix/user.reg','utf8'),'user registry');
    assert.equal(fs.readFileSync(root+'/home/save','utf8'),'save');
    assert.ok(!exists(scratch+'/component.zip')&&!exists(scratch+'/new')&&!exists(scratch+'/old'));
    console.log('GNU Proton clean install, concurrency, cached startup, space failure, corruption, activation rollback, crash recovery, repair and user data preservation: PASS');
  } finally { fs.rmSync(home,{recursive:true,force:true}); }
})().catch(e=>{console.error(e);process.exitCode=1;});
