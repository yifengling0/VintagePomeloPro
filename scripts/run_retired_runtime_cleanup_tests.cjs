// Run the production allowlist cleanup against real files and links.
const assert = require('node:assert/strict');
const nodefs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const ts = require(process.argv[2]);
const source = nodefs.readFileSync(path.join(__dirname,
  '../entry/src/main/ets/service/WineEngineService.ets'), 'utf8');
const methods = source.slice(source.indexOf('  private retireLinuxRuntimeBridge('),
  source.indexOf('  private async ensureRuntimeInstalled('));
assert.ok(source.indexOf('    this.retireLinuxRuntimeBridge();') >
  source.indexOf('  private async installRuntime('), 'cleanup is on the actual upgrade path');
const base = nodefs.mkdtempSync(path.join(os.tmpdir(), 'vp-retired-runtime-'));
const WINE_ROOT = base + '/wine';
const fs = {accessSync:nodefs.existsSync,lstatSync:nodefs.lstatSync,unlinkSync:nodefs.unlinkSync};
const pathExists = nodefs.existsSync, hilog = {info(){}}, DOMAIN = 0, TAG = 'test';
eval(ts.transpileModule('class Cleanup {' + methods + '}\nglobalThis.Cleanup = Cleanup;',
  {compilerOptions:{target:ts.ScriptTarget.ES2020}}).outputText);
const service = new globalThis.Cleanup();
function write(name, value='keep') {
  nodefs.mkdirSync(path.dirname(name), {recursive:true});nodefs.writeFileSync(name,value);
}
const retired = ['bin/i386-windows/lsteamclient.dll',
  'bin/aarch64-windows/lsteamclient.dll', 'linux-steam-bridge.json'];
try {
  service.retireLinuxRuntimeBridge(); // fresh installation
  const keep = ['.wine/user.reg', 'linux-steam/steamapps/common/game/save',
    'linux-proton/prefix/user.reg', 'wine/share/wine/fonts/custom.ttf',
    'wine/bin/aarch64-windows/kernelbase.dll',
    'wine/bin/aarch64-windows/custom.dll', 'windows-steam/userdata/save'];
  keep.forEach(name=>write(base+'/'+name));
  retired.forEach(name=>write(WINE_ROOT+'/'+name,'old bridge'));
  service.retireLinuxRuntimeBridge();service.retireLinuxRuntimeBridge();
  retired.forEach(name=>assert.equal(nodefs.existsSync(WINE_ROOT+'/'+name),false));
  keep.forEach(name=>assert.equal(nodefs.readFileSync(base+'/'+name,'utf8'),'keep'));
  // Neither a leaf link nor a parent link may cause outside files to be unlinked.
  write(base+'/outside/lsteamclient.dll');
  nodefs.symlinkSync(base+'/outside/lsteamclient.dll',WINE_ROOT+'/bin/i386-windows/lsteamclient.dll');
  nodefs.rmSync(WINE_ROOT+'/bin/aarch64-windows',{recursive:true});
  nodefs.symlinkSync(base+'/outside',WINE_ROOT+'/bin/aarch64-windows');
  service.retireLinuxRuntimeBridge();
  assert.equal(nodefs.lstatSync(WINE_ROOT+'/bin/i386-windows/lsteamclient.dll').isSymbolicLink(),true);
  assert.equal(nodefs.readFileSync(base+'/outside/lsteamclient.dll','utf8'),'keep');
  // A failed unlink must be visible and retryable, not marked as migrated.
  write(WINE_ROOT+'/linux-steam-bridge.json','retry');
  fs.unlinkSync=()=>{throw Error('permission denied');};
  assert.throws(()=>service.retireLinuxRuntimeBridge(),/permission denied/);
  fs.unlinkSync=nodefs.unlinkSync;service.retireLinuxRuntimeBridge();
  assert.equal(nodefs.existsSync(WINE_ROOT+'/linux-steam-bridge.json'),false);
  nodefs.rmSync(WINE_ROOT,{recursive:true});
  nodefs.symlinkSync(base+'/outside',WINE_ROOT);
  service.retireLinuxRuntimeBridge();
  assert.equal(nodefs.readFileSync(base+'/outside/lsteamclient.dll','utf8'),'keep');
  console.log('Retired runtime cleanup PASS: exact managed files, retry, idempotence, symlink and user-data preservation');
} finally {nodefs.rmSync(base,{recursive:true,force:true});}
