const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const vm = require('node:vm');
const crypto = require('node:crypto');
const ts = require(process.argv[2]);
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'proton-bootstrap-'));
const source = path.join(__dirname, '../entry/src/main/ets/service/LinuxProtonBootstrap.ets');
const raw = path.join(__dirname, '../entry/src/main/resources/rawfile');
const hash = data => crypto.createHash('sha256').update(data).digest('hex');
let writes = 0, corrupt = '', badManifest = null;
const deps = {
  '@kit.AbilityKit': {},
  '@kit.CoreFileKit': {fileIo:{accessSync:fs.existsSync,lstatSync:fs.lstatSync}},
  './ConfigurationFiles': {ConfigurationFiles:{
    ensure:p=>{fs.mkdirSync(p,{recursive:true});assert.ok(!fs.lstatSync(p).isSymbolicLink());},
    text:data=>Buffer.from(data).toString(),
    fileHash:async p=>hash(fs.readFileSync(p)), hash:async data=>hash(data),
    write:(p,data)=>{writes++;fs.writeFileSync(p+'.part',data);fs.renameSync(p+'.part',p);}
  }}
};
const text = fs.readFileSync(source,'utf8').replaceAll('/data/storage/el2/base/files/steamohos-native/proton-probe/bootstrap',root);
const code = ts.transpileModule(text,{compilerOptions:{target:ts.ScriptTarget.ES2020,module:ts.ModuleKind.CommonJS}}).outputText;
const api = {};
vm.runInNewContext(code,{exports:api,require:id=>{assert.ok(id in deps,id);return deps[id];}});
const context = {resourceManager:{getRawFileContent:async name=>{
  if (badManifest && name.endsWith('/manifest.json')) return Buffer.from(JSON.stringify(badManifest));
  return name.endsWith(corrupt) && corrupt ? Buffer.from('corrupt') : fs.readFileSync(path.join(raw,name));
}}};
(async()=>{
  try {
    await api.ensureLinuxProtonBootstrap(context);assert.equal(writes,3);
    await api.ensureLinuxProtonBootstrap(context);assert.equal(writes,3,'second launch must reuse verified files');
    fs.writeFileSync(path.join(root,'psapi.dll'),'broken');
    corrupt = 'psapi.dll';
    await assert.rejects(api.ensureLinuxProtonBootstrap(context),/校验失败/);
    corrupt = '';await api.ensureLinuxProtonBootstrap(context);assert.equal(writes,4);
    fs.unlinkSync(path.join(root,'psapi.dll'));fs.symlinkSync(path.join(root,'lsass.exe'),path.join(root,'psapi.dll'));
    await assert.rejects(api.ensureLinuxProtonBootstrap(context),/不安全/);
    const manifest = JSON.parse(fs.readFileSync(path.join(raw,'linux-proton/bootstrap/manifest.json')));
    badManifest = {...manifest,files:manifest.files.map((f,i)=>i?f:{...f,name:'../user.reg'})};
    await assert.rejects(api.ensureLinuxProtonBootstrap(context),/文件无效/);
    badManifest = {...manifest,files:manifest.files.map(()=>manifest.files[0])};
    await assert.rejects(api.ensureLinuxProtonBootstrap(context),/文件无效/);
    console.log('PASS: real bootstrap payload hashes, reuse, corrupt package/installed repair, symlinks, traversal and duplicate inventory');
  } finally { fs.rmSync(root,{recursive:true,force:true}); }
})().catch(error=>{console.error(error);process.exitCode=1;});
