// Exercise production settings and engine selection, including cross-game reset.
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const ts = require(process.argv[2] || 'typescript');
const root = path.join(__dirname, '../entry/src/main/ets');
const cache = new Map();
function load(file) {
  file = path.resolve(file);
  if (cache.has(file)) return cache.get(file);
  const output = ts.transpileModule(fs.readFileSync(file, 'utf8'), {
    compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020},
    reportDiagnostics: true,
  });
  assert.equal(output.diagnostics.length, 0);
  const exports = {};
  cache.set(file, exports);
  vm.runInNewContext(output.outputText, {exports,
    require: (name) => load(path.resolve(path.dirname(file), name + '.ets'))});
  return exports;
}
const policy = load(path.join(root, 'model/GraphicsBackendSettings.ets'));
const models = load(path.join(root, 'model/AppModels.ets'));
assert.equal(policy.resolveGraphicsBackend('virgl', 'default'), 'virgl');
assert.equal(policy.resolveGraphicsBackend('dxvk_1_10', 'virgl'), 'virgl');
assert.equal(policy.resolveGraphicsBackend('virgl', 'dxvk_2_6'), 'dxvk_2_6');
assert.equal(policy.resolveGraphicsBackend('virgl', 'corrupt'), 'virgl');
assert.equal(policy.resolveGraphicsBackend('corrupt', 'default'), 'dxvk_1_10');
const old = models.createDefaultAppSettings('old');
delete old.graphicsBackend;
assert.equal(models.cloneAppSettings(old).graphicsBackend, 'default');
old.graphicsBackend = 'virgl';
const restored = models.cloneAppSettings(JSON.parse(JSON.stringify(old)));
assert.equal(restored.graphicsBackend, 'virgl');
assert.equal(old.graphicsBackend, 'virgl');
old.graphicsBackend = 'invalid';
assert.equal(models.cloneAppSettings(old).graphicsBackend, 'default');

const source = fs.readFileSync(path.join(root, 'service/WineEngineService.ets'), 'utf8');
const start = source.indexOf('  private selectedD3DBackend(): string {');
const end = source.indexOf('  private ensureProductDirectCapability()', start);
assert(start >= 0 && end > start);
const program = `class Engine {
  private appGraphicsBackend = 'default';
  private hostGraphicsLabExperiment = '';
  private hostGraphicsBackendOverride = '';
  private hostGraphicsBackend = '';
  ${source.slice(start, end)}
}
exports.Engine = Engine;`;
const compiled = ts.transpileModule(program, {
  compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020},
  reportDiagnostics: true,
});
assert.equal(compiled.diagnostics.length, 0);
let system = 'dxvk_1_10';
const context = {exports: {}, ...policy, DOMAIN: 0, TAG: 'test',
  executableUsesVkd3dGraphics: () => false,
  AppSettingsStore: {getInstance: () => ({getRenderMode: () => system})},
  AppStorage: {get: () => undefined, setOrCreate: () => {}},
  testNapi: {getHostGpuName: () => 'Maleoon 910'},
  hilog: {info: () => {}},
};
vm.runInNewContext(compiled.outputText, context);
const engine = new context.exports.Engine();
assert.equal(engine.selectHostGraphicsBackendForExecutable('game-a.exe', 'virgl'), 'wined3d');
assert.equal(engine.selectHostGraphicsBackendForExecutable('game-b.exe'), 'dxvk_legacy');
system = 'virgl';
assert.equal(engine.selectHostGraphicsBackendForExecutable('game-b.exe'), 'wined3d');
assert.equal(engine.selectHostGraphicsBackendForExecutable('game-a.exe', 'dxvk_1_10'), 'dxvk_legacy');
assert.equal(engine.selectHostGraphicsBackendForExecutable('game-a.exe', 'dxvk_2_6'), 'dxvk_legacy');
assert.equal(engine.selectHostGraphicsBackendForExecutable('game-b.exe', 'invalid'), 'wined3d');
async function checkLaunchQueue() {
  const session = fs.readFileSync(path.join(root, 'service/AppSessionService.ets'), 'utf8');
  const begin = session.indexOf('  private launchTail:');
  const end = session.indexOf('  private launchQueued(', begin);
  assert(begin >= 0 && end > begin);
  const body = `class Sessions { ${session.slice(begin, end)} } exports.Sessions = Sessions;`;
  const js = ts.transpileModule(body, {
    compilerOptions: {module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020},
    reportDiagnostics: true,
  });
  assert.equal(js.diagnostics.length, 0);
  const sandbox = {exports: {}};
  vm.runInNewContext(js.outputText, sandbox);
  const sessions = new sandbox.exports.Sessions();
  let release;
  const first = new Promise(resolve => { release = resolve; });
  const entered = [];
  sessions.launchQueued = async (app) => {
    entered.push(app);
    if (app === 'game-a') await first;
    if (app === 'broken') throw new Error('failed startup');
    return app;
  };
  const a = sessions.launch('game-a', {}, '');
  const b = sessions.launch('game-b', {}, '');
  await Promise.resolve();
  assert.deepEqual(entered, ['game-a']);
  release();
  assert.equal(await a, 'game-a');
  assert.equal(await b, 'game-b');
  const broken = sessions.launch('broken', {}, '');
  const later = sessions.launch('later', {}, '');
  await assert.rejects(broken, /failed startup/);
  assert.equal(await later, 'later');
  assert.deepEqual(entered, ['game-a', 'game-b', 'broken', 'later']);
}
checkLaunchQueue().then(() => {
  console.log('PASS: inheritance, persistence, old settings, device fallback, cross-game reset and serialized launches');
}).catch(error => { console.error(error); process.exitCode = 1; });
