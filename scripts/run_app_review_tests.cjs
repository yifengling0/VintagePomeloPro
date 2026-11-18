// Execute production timing, persistence and presentation against platform fakes.
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const ts = require(path.resolve(process.argv[2]));
const root = path.join(__dirname, '../entry/src/main/ets');
function compile(source) { return ts.transpileModule(source, {
  compilerOptions: {target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS}
}).outputText; }
const compiled = compile(fs.readFileSync(path.join(root, 'service/AppReviewService.ets'), 'utf8'));
const HOUR = 3600000;
function deferred() { let resolve; const promise = new Promise(r => {resolve = r;}); return {promise, resolve}; }
async function turns() { for (let i = 0; i < 24; ++i) await Promise.resolve(); }
function fixture(type = 'tablet', disk = new Map()) {
  let now = 0, timer, opens = 0, failRead = false, flush = async () => {};
  const cache = new Map(disk), targets = [], shown = [];
  const prefs = {getSync: (key, value) => cache.has(key) ? cache.get(key) : value,
    putSync: (key, value) => cache.set(key, value), async flush() {
      const snapshot = new Map(cache); await flush(); snapshot.forEach((value, key) => disk.set(key, value));
    }};
  const scope = {exports: {}, Promise,
    setInterval(fn, period) { assert.equal(period, 60000); timer = fn; return 1; },
    clearInterval() { timer = undefined; },
    require(name) {
      if (name === '@ohos.data.preferences') return {default: {getPreferencesSync() {
        ++opens; if (failRead) throw Error('unavailable'); return prefs;
      }}};
      if (name === '@ohos.systemDateTime') return {default: {TimeType: {ACTIVE: 1},
        getUptime(kind) { assert.equal(kind, 1); return now; }}};
      if (name === '@ohos.app.ability.wantConstant') return {default: {Flags: {FLAG_START_WITHOUT_TIPS: 1}}};
      if (name === '@kit.AbilityKit') return {bundleManager: {BundleFlag: {GET_BUNDLE_INFO_DEFAULT: 0},
        getBundleInfoForSelf: async () => ({name: 'com.vintage.pomelopro'})}};
      if (name.includes('DeviceCapabilityPolicy')) return {DeviceCapabilityPolicy: {deviceType: () => type}};
      if (name === './LogService') return {hilog: {warn() {}}};
      throw Error(name);
    }};
  vm.runInNewContext(compiled, scope);
  const owner = {async startAbility(want) { targets.push(want); }};
  const service = scope.exports.AppReviewService;
  return {service, disk, owner, targets, shown, opens: () => opens, timer: () => !!timer,
    advance(ms) { now += ms; }, tick() { timer?.(); }, failRead(value) { failRead = value; },
    flush(fn) { flush = fn; },
    foreground(who = owner) { service.foreground(who); }, background(who = owner) { service.background(who); },
    prompt(canShow = () => true, show = async () => { shown.push(disk.get('review_prompt_disabled')); }) {
      return service.maybePrompt(owner, canShow, show);
    }};
}
(async () => {
  for (const type of ['phone', 'tv', 'unknown']) {
    const f = fixture(type); f.foreground(); f.advance(2 * HOUR); await f.prompt(); f.background();
    assert.equal(f.opens(), 0); assert.equal(f.timer(), false); assert.equal(f.shown.length, 0);
  }
  for (const type of ['tablet', '2in1', 'pc']) {
    const f = fixture(type); f.foreground(); f.advance(HOUR - 1); await f.prompt();
    assert.equal(f.shown.length, 0, 'No reminder before an actual hour');
    f.advance(1); await f.prompt(); await f.prompt();
    assert.deepEqual(f.shown, [true], 'Durable once flag precedes the sole display');
    assert.equal(f.timer(), false);
  }
  {
    const f = fixture(), second = {}; f.foreground(); f.advance(20 * 60000); f.foreground(second);
    f.advance(10 * 60000); f.background(); f.advance(10 * 60000); f.background(second); await turns();
    assert.equal(f.disk.get('foreground_usage_ms'), 40 * 60000, 'Overlapping windows count once');
    assert.equal(f.timer(), false); f.advance(7 * HOUR); await f.prompt(); assert.equal(f.shown.length, 0);
    const restarted = fixture('tablet', f.disk); restarted.foreground(); restarted.advance(20 * 60000);
    await restarted.prompt(); assert.equal(restarted.shown.length, 1, 'Usage survives process restart');
    const upgraded = fixture('tablet', f.disk); upgraded.foreground(); upgraded.advance(3 * HOUR); await upgraded.prompt();
    assert.equal(upgraded.shown.length, 0, 'Dismissed reminders remain disabled after restart/upgrade');
  }
  {
    const f = fixture(); f.foreground(); f.advance(HOUR); f.tick(); await turns();
    assert.equal(f.disk.get('foreground_usage_ms'), HOUR, 'Periodic checkpoint survives an abrupt process exit');
    assert.equal(f.timer(), false);
    const restarted = fixture('pc', f.disk); restarted.foreground(); await restarted.prompt();
    assert.equal(restarted.shown.length, 1);
  }
  {
    const f = fixture(), gate = deferred(); let available = true;
    f.foreground(); f.advance(HOUR); f.flush(() => gate.promise);
    const claim = f.prompt(() => available); await turns(); available = false;
    gate.resolve(); await claim; assert.equal(f.shown.length, 0, 'No hidden dialog after late navigation');
    available = true; await f.prompt(() => available); assert.equal(f.shown.length, 1, 'Deferred claim resumes once');
  }
  {
    const f = fixture(), gate = deferred(); f.foreground(); f.advance(HOUR); f.flush(() => gate.promise);
    const claim = f.prompt(); await turns(); f.background(); gate.resolve(); await claim;
    assert.equal(f.shown.length, 0); f.foreground(); await f.prompt(); assert.equal(f.shown.length, 1);
  }
  {
    const f = fixture(), dialog = deferred(); f.foreground(); f.advance(HOUR);
    const first = f.prompt(() => true, async () => { f.shown.push(true); await dialog.promise; });
    await turns(); await f.prompt(); await f.prompt(); assert.equal(f.shown.length, 1, 'Concurrent polling never duplicates a dialog');
    dialog.resolve(); await first;
  }
  {
    const f = fixture(); f.foreground(); f.advance(HOUR); f.flush(async () => { throw Error('disk full'); });
    await f.prompt(); assert.equal(f.shown.length, 0, 'No display before a durable once state');
    f.flush(async () => {}); await f.prompt(); assert.deepEqual(f.shown, [true]);
  }
  {
    const f = fixture(); f.failRead(true); f.foreground(); assert.equal(f.timer(), false);
    f.failRead(false); f.foreground(); f.advance(HOUR); await f.prompt(); assert.equal(f.shown.length, 1);
  }
  for (const invalid of [-1, NaN, Infinity, '3600000']) {
    const f = fixture('tablet', new Map([['foreground_usage_ms', invalid]])); f.foreground(); await f.prompt();
    assert.equal(f.shown.length, 0, 'Invalid saved counters cannot bypass the hour threshold');
  }
  {
    const f = fixture(); assert.equal(await f.service.openReviewPage(f.owner), true);
    assert.equal(f.targets[0].uri, 'market://details?id=com.vintage.pomelopro');
    assert.equal(f.targets[0].bundleName, 'com.huawei.appmarket'); f.targets.length = 0;
    f.owner.startAbility = async want => { f.targets.push(want); if (f.targets.length === 1) throw Error('no market'); };
    assert.equal(await f.service.openReviewPage(f.owner), true);
    assert.equal(f.targets[1].uri, 'https://appgallery.huawei.com/app/detail?id=com.vintage.pomelopro');
    f.owner.startAbility = async () => { throw Error('unavailable'); };
    assert.equal(await f.service.openReviewPage(f.owner), false);
  }
  const index = fs.readFileSync(path.join(root, 'pages/Index.ets'), 'utf8');
  const method = index.match(/  private maybeShowReviewPrompt\([\s\S]*?\n  \}/)[0];
  for (const blocked of ['none', 'hidden', 'transition', 'scan', 'engine', 'detail', 'steam', 'game', 'chrome']) {
    const f = fixture('tablet', new Map([['foreground_usage_ms', HOUR]])); f.foreground();
    const dialogs = [], scope = {exports: {}, AppReviewService: f.service,
      HostWindowTasks: {getInstance: () => ({hasForegroundTask: () => blocked === 'game'})}};
    vm.runInNewContext(compile('export class Page {\n' + method + '\n}'), scope);
    const page = new scope.exports.Page();
    Object.assign(page, {context: f.owner, consolePageVisible: blocked !== 'hidden', catalogInitialized: true,
      transitioning: blocked === 'transition', scanning: blocked === 'scan', consoleDetailVisible: blocked === 'detail',
      steamPop: blocked === 'steam', libraryChromeVisible: () => blocked !== 'chrome', isEngineBusy: () => blocked === 'engine',
      getUIContext: () => ({getPromptAction: () => ({async showDialog(options) { dialogs.push(options); return {index: 0}; }})})});
    page.maybeShowReviewPrompt(); await turns();
    assert.equal(dialogs.length, blocked === 'none' ? 1 : 0, blocked);
    assert.equal(f.targets.length, 0, 'Cancel never opens or submits a market review');
  }
  console.log('PASS app review: hour boundary, tablet/PC-only, multiwindow union, background/restart persistence, durable once, races/failures, library gates and market fallback');
})().catch(error => { console.error(error); process.exitCode = 1; });
