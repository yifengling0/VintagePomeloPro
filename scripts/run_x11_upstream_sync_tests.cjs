// Run production routing and services with platform dispatch fakes.
const assert = require('node:assert/strict'), fs = require('node:fs'), vm = require('node:vm'), path = require('node:path');
const ts = require(process.argv[2]);
function load(name, dependencies = {}, globals = {}) {
  const source = fs.readFileSync(path.join(__dirname, '../entry/src/main/ets/', name + '.ets'), 'utf8');
  const compiled = ts.transpileModule(source, { compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS }, reportDiagnostics: true });
  assert(!compiled.diagnostics?.some(d => d.category === ts.DiagnosticCategory.Error), name);
  const context = { exports: {}, require: id => { assert(id in dependencies, id); return dependencies[id]; }, ...globals };
  vm.runInNewContext(compiled.outputText, context); return context.exports;
}
const {resolveOutputScale} = load('model/DisplayScalePolicy');
for (const density of [1, 1.61, 3]) for (const percent of [75, 100, 150]) {
  const g = resolveOutputScale(percent, 1200, 900, density);
  assert.equal(g.scale, percent / 100);
  assert.equal(g.width, Math.round(1200 / (percent / 100)));
  assert.equal(g.height, Math.round(900 / (percent / 100)));
  assert(Math.abs(density * g.boost - g.scale) < 0.000001);
  const wineX = 123; assert(Math.abs(wineX * g.scale / g.scale - wineX) < 0.000001);
}
assert.equal(resolveOutputScale(NaN, 1200, 900, 2).scale, 1);
const {parseSystemOpenTarget: parse} = load('model/SystemOpenTarget');
assert.equal(parse('https://example.com/x?y=1', '/p').uri, 'https://example.com/x?y=1');
assert.equal(parse('http://example.com', '/p').uri, 'http://example.com');
assert.equal(parse('mailto:a@example.com', '/p').uri, 'mailto:a@example.com');
assert.equal(parse('file:///C:/game/a%20b.txt', '/prefix').path, '/prefix/dosdevices/c:/game/a b.txt');
assert.equal(parse('file://localhost/tmp/a.txt', '/prefix').path, '/tmp/a.txt');
for (const input of ['', 'javascript:alert(1)', 'ftp://server/file', 'file://server/file', 'file:///%ZZ', 'https://bad\nvalue', 'file:///a%00b']) assert.equal(parse(input, '/p'), null, input);
const tracker = load('service/InputStateTracker'); const keys = [];
const {InputRouter} = load('service/InputRouter', {
  'libentry.so': {default: {sendKeyEvent: (...args) => keys.push(args), sendPointerEvent() {}}},
  './InputStateTracker': tracker
}, {MouseAction: {Release: 2}});
const router = InputRouter.getInstance(); router.setActiveToplevel(10);
router.sendKey(30, true); router.sendWindowKey(10, 'physical', 30, true);
router.sendWindowKey(11, 'physical', 31, true); router.releaseWindowSource(10, 'physical');
assert.equal(keys.filter(k => k[0] === 10 && k[1] === 30 && !k[2]).length, 0, 'overlay still holds A');
router.releaseWindowSource(11, 'physical');
assert(keys.some(k => k[0] === 11 && k[1] === 31 && !k[2]));
router.sendKey(30, false); assert(keys.some(k => k[0] === 10 && k[1] === 30 && !k[2]));
router.sendWindowKey(10, 'physical', 42, true); router.sendWindowKey(10, 'physical', 42, true);
assert.equal(keys.filter(k => k[1] === 42 && k[2]).length, 2, 'physical repeat preserved');
router.releaseAllKeys(); assert(keys.some(k => k[1] === 42 && !k[2]));
let callback, enabled = false, changed, allow = false, missing = false, failLaunch = false, opened = [], errors = [], closed = 0;
const settings = {getGlobalSettings: () => ({systemOpenEnabled: allow}), subscribeGlobalChanges: f => {changed=f;}};
const {SystemOpenService: service} = load('service/SystemOpenService', {
  '@kit.AbilityKit': {wantConstant: {Flags: {FLAG_AUTH_READ_URI_PERMISSION: 1}}},
  '@kit.CoreFileKit': {fileIo: {OpenMode:{READ_ONLY:0}, openSync: p => {if(missing)throw Error('unmapped'); return {fd:1,path:p};}, statSync: () => ({isFile:()=>true}), closeSync:()=>closed++}, fileUri: {getUriFromPath: () => 'file://com.vintage.pomelopro/data/storage/el2/base/files/a.txt'}},
  './LogService': {hilog:{warn(){}}},
  '@kit.ArkUI': {promptAction: {showToast: x => errors.push(x.message)}},
  'libentry.so': {default:{setSystemOpenEnabled:x=>{enabled=x;}, setOpenTargetCallback:f=>{callback=f;}}},
  './AppSettingsStore': {AppSettingsStore:{getInstance:()=>settings}},
  './WineWindowManager': {WineWindowManager: {getInstance: () => ({openSystemTarget: (uri, flags) => {
    opened.push({uri,flags}); return failLaunch ? Promise.reject(Error('no handler')) : Promise.resolve();
  }})}},
  '../model/SystemOpenTarget': {parseSystemOpenTarget:parse}
});
async function checkForegroundOpenContext() {
  const {WineWindowManager} = load('service/WineWindowManager', {
    '../model/DisplayScalePolicy': {resolveOutputScale}, './InputRouter': {InputRouter},
    '@kit.AbilityKit': {}, '@kit.ArkUI': {}, '@kit.BasicServicesKit': {}, '@kit.InputKit': {},
    './LogService': {hilog: {info(){}, warn(){}}}, 'libentry.so': {default: {}},
    './HostWindowTasks': {}, './InputDispatcher': {},
    '../model/AppModels': {WinePresentationMode: {DESKTOP:'desktop'}},
    './PopupWindowManager': {}, './ModalWindowManager': {}, './FusionWindowManager': {},
    './AppSettingsStore': {}, './DesktopModeService': {}, '../common/DeviceCapabilityPolicy': {}
  });
  const wm=WineWindowManager.getInstance(), accepted=[];
  const carrier=name=>({startAbility:async want=>{accepted.push([name,want]);},
    openLink:async(uri,options)=>{accepted.push([name,{uri,options}]);}});
  wm.uiAbilityContext=carrier('entry');
  const steam=carrier('steam'), game=carrier('game');
  wm.abilityContexts.set(21,steam);wm.noteAbilityForeground(21,true);
  wm.abilityContexts.set(22,game);wm.noteAbilityForeground(22,true);
  wm.noteAbilityForeground(21,true);
  await wm.openSystemTarget('https://example.com',0);
  assert.equal(accepted.at(-1)[0],'steam','HTTP uses most recent foreground carrier');
  assert.equal(accepted.at(-1)[1].options.appLinkingOnly,false);
  wm.noteAbilityForeground(21,false);
  await wm.openSystemTarget('http://example.com',0);
  assert.equal(accepted.at(-1)[0],'game','HTTP follows foreground changes');
  wm.destroyed.add(22);wm.desktopAbilityForeground=true;wm.desktopAbilityCtx=steam;
  await wm.openSystemTarget('file://com.vintage.pomelopro/files/a.txt',1);
  assert.equal(accepted.at(-1)[0],'steam','files exclude destroyed windows and use live desktop');
  assert.equal(accepted.at(-1)[1].flags,1,'file retains read-only URI grant');
  assert.equal(accepted.at(-1)[1].action,'ohos.want.action.viewData');
  await wm.openSystemTarget('mailto:a@example.com',0);
  assert.equal(accepted.at(-1)[1].uri,'mailto:a@example.com');
  steam.openLink=()=>{throw Error('launch rejected');};
  await assert.rejects(wm.openSystemTarget('https://example.com',0),/launch rejected/);
  wm.desktopAbilityForeground=false;
  await wm.startHostAbility({abilityName:'EntryAbility'});
  assert.equal(accepted.at(-1)[0],'entry','host launches retain Entry fallback');
  wm.uiAbilityContext=null;
  await assert.rejects(wm.openSystemTarget('https://example.com',0),/context unavailable/);
}
(async () => {
  await checkForegroundOpenContext();
  service.initialize();
  service.start('/prefix'); assert(!enabled); assert(!callback('https://example.com'));
  allow = true; changed(); assert(enabled); assert(callback('https://example.com')); assert.equal(opened.length, 1);
  missing = true; assert(!callback('file:///C:/a.txt')); assert.equal(opened.length,1, 'unmapped file falls back');
  missing = false; assert(callback('file:///C:/a.txt')); assert.equal(opened.at(-1).flags,1); assert.equal(closed,1);
  failLaunch = true; assert(callback('https://example.com')); await Promise.resolve(); assert.equal(errors.length,1, 'accepted launch failure is visible');
  const old = callback; service.stop(); assert.equal(callback,null); assert(!enabled); assert(!old('https://example.com'), 'old callback cannot open after stop');
  console.log('PASS upstream sync: URI parsing, foreground OPEN context, acceptance/fallback/error/teardown, per-window/source keys, physical repeat');
})().catch(e => {console.error(e);process.exitCode=1;});
