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
assert.equal(policy.resolveGraphicsBackend('zink', 'default'), 'zink');
assert.equal(policy.resolveGraphicsBackend('virgl', 'dxvk9'), 'dxvk9');
assert.equal(policy.resolveGraphicsBackend('zink', policy.APP_OPENGL_VIRGL_COMPAT), policy.APP_OPENGL_VIRGL_COMPAT);
assert.equal(policy.normalizeGraphicsBackend(policy.APP_OPENGL_VIRGL_COMPAT), 'dxvk_1_10');
assert.deepEqual(Array.from(policy.resolveAppOpenGLEnvironment(policy.APP_OPENGL_VIRGL_COMPAT)),
  ['MESA_GL_VERSION_OVERRIDE=3.2FC', 'MESA_GLSL_VERSION_OVERRIDE=150']);
assert.equal(policy.resolveGraphicsBackend('virgl', policy.APP_OPENGL_VIRGL_33), policy.APP_OPENGL_VIRGL_33);
assert.equal(policy.normalizeGraphicsBackend(policy.APP_OPENGL_VIRGL_33), 'dxvk_1_10');
assert.equal(policy.APP_GRAPHICS_BACKEND_IDS.includes(policy.APP_OPENGL_VIRGL_33), false);
assert.equal(policy.isVirglOpenGLCompatibility(policy.APP_OPENGL_VIRGL_33), true);
assert.deepEqual(Array.from(policy.resolveAppOpenGLEnvironment(policy.APP_OPENGL_VIRGL_33)),
  ['MESA_GL_VERSION_OVERRIDE=3.3FC', 'MESA_GLSL_VERSION_OVERRIDE=330']);
for (const backend of policy.GRAPHICS_BACKEND_IDS.concat(['default'])) {
  assert.deepEqual(Array.from(policy.resolveAppOpenGLEnvironment(backend)), []);
}
const old = models.createDefaultAppSettings('old');
delete old.graphicsBackend;
delete old.steamOpenGL33;
assert.equal(models.cloneAppSettings(old).graphicsBackend, 'default');
assert.equal(models.cloneAppSettings(old).steamOpenGL33, false);
old.steamOpenGL33 = true;
old.graphicsBackend = 'virgl';
const restored = models.cloneAppSettings(JSON.parse(JSON.stringify(old)));
assert.equal(restored.graphicsBackend, 'virgl');
assert.equal(restored.steamOpenGL33, true);
assert.equal(old.graphicsBackend, 'virgl');
old.graphicsBackend = 'invalid';
assert.equal(models.cloneAppSettings(old).graphicsBackend, 'default');

assert.deepEqual(Array.from(policy.resolveZinkPresentationEnvironment('wined3d_zink')), ['WINEHUA_ZINK_PRESENT=1']);
for (const backend of ['wined3d', 'dxvk_legacy_d3d9', 'dxvk_legacy', 'invalid']) {
  assert.deepEqual(Array.from(policy.resolveZinkPresentationEnvironment(backend)), ['WINEHUA_ZINK_PRESENT=0']);
}
const source = fs.readFileSync(path.join(root, 'service/WineEngineService.ets'), 'utf8');
const start = source.indexOf('  private selectedGraphicsMode(): string {');
const end = source.indexOf('  private ensureProductDirectCapability()', start);
assert(start >= 0 && end > start);
const program = `class Engine {
  private appGraphicsBackend = 'default';
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
  AppSettingsStore: {getInstance: () => ({getRenderMode: () => system})},
  AppStorage: {get: () => undefined, setOrCreate: () => {}},
  testNapi: {getHostGpuName: () => 'Maleoon 910'},
  hilog: {info: () => {}},
};
vm.runInNewContext(compiled.outputText, context);
const engine = new context.exports.Engine();
assert.equal(engine.selectHostGraphicsBackendForExecutable('game-a.exe', 'virgl'), 'dxvk_legacy');
assert.equal(engine.selectHostGraphicsBackendForExecutable('game-a.exe', policy.APP_OPENGL_VIRGL_COMPAT), 'wined3d');
assert.equal(engine.selectHostGraphicsBackendForExecutable('game-a.exe', policy.APP_OPENGL_VIRGL_33), 'wined3d');
assert.equal(engine.selectHostGraphicsBackendForExecutable('game-b.exe'), 'dxvk_legacy_d3d9');
system = 'virgl';
assert.equal(engine.selectHostGraphicsBackendForExecutable('game-b.exe'), 'dxvk_legacy');
assert.equal(engine.selectHostGraphicsBackendForExecutable('game-a.exe', 'dxvk_1_10'), 'dxvk_legacy_d3d9');
assert.equal(engine.selectHostGraphicsBackendForExecutable('game-a.exe', 'dxvk_2_6'), 'dxvk_legacy_d3d9');
assert.equal(engine.selectHostGraphicsBackendForExecutable('game-b.exe', 'invalid'), 'dxvk_legacy');
assert.deepEqual(Array.from(policy.GRAPHICS_PROFILE_IDS),
  ['dxvk_1_10', 'dxvk_2_6', 'virgl', 'zink', policy.VKD3D_DIRECT]);
assert.equal(policy.graphicsProfileButton('dxvk9'), 'dxvk_1_10');
assert.equal(policy.showVirglOpenGLCompatibility('Maleoon 910', 'default'), true);
assert.equal(policy.showVirglOpenGLCompatibility('Maleoon 935', 'default'), false);
assert.equal(policy.showVirglOpenGLCompatibility('Maleoon 935', 'default', false), true);
assert.equal(policy.showVirglOpenGLCompatibility('Maleoon 935', policy.APP_OPENGL_VIRGL_33), true);
assert.equal(engine.usesMobileGL('dxvk_2_6'), true);
assert.equal(engine.usesMobileGL('virgl'), false);
assert.equal(engine.usesMobileGL('zink'), false);
context.testNapi.getHostGpuName = () => 'Maleoon 935';
assert.equal(engine.selectHostGraphicsBackendForExecutable('game-a.exe', 'dxvk_2_6'), 'dxvk_modern_2_6');
assert.equal(engine.selectHostGraphicsBackendForExecutable('dx12.exe', policy.VKD3D_DIRECT), policy.VKD3D_DIRECT);
system = policy.VKD3D_VENUS;
assert.equal(engine.selectHostGraphicsBackendForExecutable('dx12.exe'), policy.VKD3D_VENUS);
system = 'virgl';
assert.equal(engine.selectHostGraphicsBackendForExecutable('pal4.exe', 'dxvk9'), 'dxvk_legacy_d3d9');
assert.equal(engine.selectHostGraphicsBackendForExecutable('pal4.exe', 'zink'), 'wined3d_zink');
assert.equal(engine.selectHostGraphicsBackendForExecutable('game-b.exe'), 'dxvk_legacy');
for (const backend of ['dxvk9', 'zink', policy.APP_OPENGL_VIRGL_COMPAT, policy.APP_OPENGL_VIRGL_33]) {
  old.graphicsBackend = backend;
  assert.equal(models.cloneAppSettings(JSON.parse(JSON.stringify(old))).graphicsBackend, backend);
}
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
  const sandbox = {
    exports: {},
    EngineState: {STOPPED: 'stopped', ERROR: 'error'},
    WineEngineService: {getInstance: () => ({getState: () => 'ready'})},
    GameModeService: {begin() {}, finish() {}},
  };
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
