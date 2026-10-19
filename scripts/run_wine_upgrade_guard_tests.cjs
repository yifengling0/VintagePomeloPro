// Run production upgrade detection against a temporary, real prefix filesystem.
const assert = require('node:assert/strict');
const nodefs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(process.argv[2]);
const root = path.resolve(__dirname, '..');
const base = nodefs.mkdtempSync(path.join(os.tmpdir(), 'vp-wine-upgrade-'));
const sandbox = '/data/storage/el2/base/files';
const resolve = name => base + name.substring(sandbox.length);
const marker = base + '/.winehua-upgrade-reset-required';
const legacy = 'content=299899902898b989;wine=1cb1ac93e4;box64=0411b38569';
const modern = 'content=current;wine-valve=cd547f7a0e;fex=86ff33bbe2;arch=arm64-v8a:aarch64';
let failWrite = false;
const fs = {
  OpenMode: { READ_ONLY: 'r', CREATE: 1, WRITE_ONLY: 2, TRUNC: 4 },
  accessSync: name => nodefs.existsSync(resolve(name)),
  readTextSync: name => nodefs.readFileSync(resolve(name), 'utf8'),
  statSync: name => nodefs.statSync(resolve(name)),
  openSync: (name, mode) => ({ fd: nodefs.openSync(resolve(name), mode === 'r' ? 'r' : 'w') }),
  closeSync: file => nodefs.closeSync(file.fd),
  readSync: (fd, buffer, options) => nodefs.readSync(fd, Buffer.from(buffer), 0, options.length, options.offset ?? 0),
  writeSync: (fd, content) => failWrite ? 0 : nodefs.writeSync(fd, content),
  renameSync: (from, to) => nodefs.renameSync(resolve(from), resolve(to)),
  unlinkSync: name => nodefs.unlinkSync(resolve(name))
};
function load(relative, dependencies) {
  const exports = {};
  const compiled = ts.transpileModule(nodefs.readFileSync(path.join(root, relative), 'utf8'), {
    compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS }
  }).outputText;
  vm.runInNewContext(compiled, { exports, require: name => {
    assert(name in dependencies, name); return dependencies[name];
  }, Uint8Array, ArrayBuffer, DataView });
  return exports;
}
const rules = load('entry/src/main/ets/model/WineUpgradeRules.ets', {});
const { WineUpgradeGuard, WINE_UPGRADE_RESET_MESSAGE } = load(
  'entry/src/main/ets/service/WineUpgradeGuard.ets', {
    '@kit.CoreFileKit': { fileIo: fs }, '../model/WineUpgradeRules': rules
  });
const bundled = new Uint8Array(Buffer.from(modern));
function write(relative, data) {
  const target = base + '/' + relative;
  nodefs.mkdirSync(path.dirname(target), { recursive: true });
  nodefs.writeFileSync(target, data);
}
function clear() {
  for (const name of nodefs.readdirSync(base)) nodefs.rmSync(base + '/' + name, { recursive: true, force: true });
}
function prefix(runtime, version, machine) {
  if (runtime) write('wine/.winehua-runtime-version', runtime);
  if (version) write('.wine/.winehua-prefix-runtime-version', version);
  write('.wine/system.reg', 'WINE REGISTRY Version 2');
  if (machine) {
    const pe = Buffer.alloc(262);
    pe.writeUInt16LE(0x5a4d, 0); pe.writeUInt32LE(256, 60);
    pe.writeUInt32LE(0x00004550, 256); pe.writeUInt16LE(machine, 260);
    write('.wine/drive_c/windows/system32/ntdll.dll', pe);
  }
}
const source = nodefs.readFileSync(path.join(root, 'entry/src/main/ets/service/WineEngineService.ets'), 'utf8');
function method(signature) {
  const start = source.indexOf('  ' + signature);
  assert(start >= 0, signature);
  return source.slice(start, source.indexOf('\n  }', start) + 4);
}
const EngineState = { STOPPED: 'stopped', PREPARING: 'preparing', READY: 'ready', SWITCHING: 'switching', ERROR: 'error' };
let dialogs = [], routes = [], answer, wiped = true, reinstalls = 0, failReinstall = false;
const promptAction = { showDialog(options) {
  dialogs.push(options);
  return new Promise(resolve => { answer = resolve; });
} };
const router = { async pushUrl(route) { routes.push(route); } };
const hilog = { error() {}, warn() {} }, DOMAIN = 0, TAG = 'test';
const testNapi = { resetWinePrefix() {
  if (wiped) nodefs.rmSync(base + '/.wine', { recursive: true, force: true });
  return wiped;
} };
const AppSettingsStore = { getInstance: () => ({ getGlobalSettings: () => ({ preserveUserDataOnReset: false }) }) };
const RESET_KEEP_DIR = '/unused';
const removeRecursive = () => {};
class Fields {
  context = {}; state = EngineState.STOPPED; upgradeGuard = new WineUpgradeGuard();
  upgradeWarningPromise = null; transitions = []; waiterResults = [];
  getBundledRuntimeVersion() { return bundled; }
  setState(state, message) { this.state = state; this.transitions.push(state); this.message = message; }
  finishWaiters(ready) { this.waiterResults.push(ready); }
  async stopSession() { this.state = EngineState.STOPPED; }
  async forceRefreshRuntime() {
    ++reinstalls;
    if (failReinstall) throw new Error('reinstall failed');
    write('wine/.winehua-runtime-version', modern);
  }
  selectedD3DBackend() { throw new Error('Blocked launch reached native preparation'); }
}
eval(ts.transpileModule('class UpgradeEngine extends Fields {\n' +
  ['async startSession(', 'private blockLegacyWineUpgrade(', 'async wipeEnvironment(', 'async resetPrefix(']
    .map(method).join('\n') + '\n}\nglobalThis.UpgradeEngine = UpgradeEngine;',
  { compilerOptions: { target: ts.ScriptTarget.ES2020 } }).outputText);
(async () => {
  const guard = new WineUpgradeGuard();
  assert.equal(guard.needsReset(bundled), false, 'Fresh installation');
  write('wine/.winehua-runtime-version', legacy);
  assert.equal(guard.needsReset(bundled), false, 'Old runtime with no prefix can install normally');
  prefix(legacy, legacy, 0x8664);
  assert.equal(guard.needsReset(bundled), true, 'Released 1.4.4 installation');
  assert.equal(nodefs.existsSync(marker), true, 'Requirement persists before runtime changes');
  write('wine/.winehua-runtime-version', modern);
  nodefs.rmSync(base + '/.wine', { recursive: true, force: true });
  assert.equal(new WineUpgradeGuard().needsReset(bundled), true, 'Interrupted reset and new process do not dismiss the warning');
  clear(); prefix(modern, legacy, 0xaa64);
  assert.equal(guard.needsReset(bundled), true, 'Updated runtime with legacy prefix marker');
  clear(); prefix(modern, modern, 0x8664);
  assert.equal(guard.needsReset(bundled), true, 'Both markers updated but AMD64 core DLL remains');
  clear(); prefix('', '', 0x8664);
  assert.equal(guard.needsReset(bundled), true, 'Unmarked legacy prefix');
  clear(); prefix(modern, modern.replace('aarch64', 'x86_64'), 0xaa64);
  assert.equal(guard.needsReset(bundled), true, 'Prefix architecture changed');
  clear(); prefix(modern.replace('content=current', 'content=older'), modern.replace('cd547f7a0e', 'earlierpin'), 0xaa64);
  assert.equal(guard.needsReset(bundled), false, 'Normal Proton runtime or audio update');
  assert.equal(nodefs.existsSync(marker), false);
  clear(); prefix(modern, modern, 0xaa64);
  assert.equal(guard.needsReset(bundled), false, 'Current compatible prefix');
  clear(); prefix(legacy, legacy, 0x8664); failWrite = true;
  assert.throws(() => guard.needsReset(bundled), /persist/);
  assert.equal(nodefs.existsSync(marker), false, 'A partial write cannot acknowledge an upgrade');
  failWrite = false;

  const engine = new UpgradeEngine();
  assert.equal(await engine.startSession('desktop', 'desktop', '/games'), false);
  assert.deepEqual(engine.transitions, [EngineState.PREPARING, EngineState.ERROR], 'Deferred programs receive a terminal failure');
  assert.deepEqual(engine.waiterResults, [false]);
  assert.equal(dialogs.length, 1);
  assert.equal(await engine.startSession('desktop', 'desktop', '/games'), false, 'ERROR retry remains blocked');
  assert.equal(dialogs.length, 1, 'Concurrent attempts share one reminder');
  answer({ index: 0 }); await engine.upgradeWarningPromise;
  assert.equal(routes.length, 0); assert.equal(guard.needsReset(bundled), true, 'Cancel leaves the gate active');
  engine.blockLegacyWineUpgrade(); answer({ index: 1 }); await engine.upgradeWarningPromise;
  assert.equal(routes[0].url, 'pages/SystemSettings');
  assert.equal(routes[0].params.section, 'engine');
  assert.match(WINE_UPGRADE_RESET_MESSAGE, /1\.4\.4/);
  assert.match(WINE_UPGRADE_RESET_MESSAGE, /重置 Wine prefix/);
  assert.match(WINE_UPGRADE_RESET_MESSAGE, /备份.*卸载/);
  assert.equal(guard.needsReset(bundled), true, 'Opening settings does not acknowledge reset');

  wiped = false;
  await assert.rejects(engine.resetPrefix(false, ''), /reset Wine prefix/);
  assert.equal(reinstalls, 0); assert.equal(guard.needsReset(bundled), true, 'Failed native wipe keeps the gate');
  wiped = true; failReinstall = true;
  await assert.rejects(engine.resetPrefix(false, ''), /reinstall failed/);
  assert.equal(guard.needsReset(bundled), true, 'Failed runtime reinstall keeps the gate');
  failReinstall = false;
  await engine.resetPrefix(false, '');
  assert.equal(nodefs.existsSync(marker), false);
  assert.equal(guard.needsReset(bundled), false, 'Completed reset releases the launch gate');
  assert.equal(engine.state, EngineState.STOPPED);
  console.log('PASS: 1.4.4 detection, interrupted updates, legacy core DLLs, compatible updates, launch blocking, reminder routing and reset failures/recovery');
})().catch(error => { console.error(error); process.exitCode = 1; }).finally(() => {
  nodefs.rmSync(base, { recursive: true, force: true });
});
