const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(path.resolve(process.argv[2]));
const source = path.resolve(__dirname, '../entry/src/main/ets/service/WineImeCoordinator.ets');
const code = ts.transpileModule(fs.readFileSync(source, 'utf8'), {compilerOptions: {
  target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS}}).outputText;
let pc = true, active = false, focused = 10, callback, armed = false;
const commits = [], preedits = [], keys = [], calls = [], handlers = new Map();
let pendingAttach = null, rejectNext = false;
const foreignHandler = () => {};
handlers.set('insertText', new Set([foreignHandler]));
const controller = {
  async attach(show, config) {
    calls.push(['attach', show, config.windowId]);
    if (rejectNext) {rejectNext = false; throw Error('test failure');}
    if (pendingAttach) await pendingAttach;
  },
  async detach() {calls.push(['detach']);},
  on(event, fn) {if (!handlers.has(event)) handlers.set(event, new Set()); handlers.get(event).add(fn);},
  off(event, fn) {assert.equal(typeof fn, 'function', 'off must preserve other UI listeners'); handlers.get(event)?.delete(fn);}
};
const native = {
  setImeCallback(fn) {assert.equal(callback, undefined, 'single native callback'); callback = fn;},
  wineTextInputSetArmed(value) {armed = value; if (!value) active = false;},
  wineTextInputActive: () => active,
  wineTextInputFocusedToplevel: () => focused,
  wineTextInputCommit: text => commits.push(text),
  wineTextInputPreedit: text => preedits.push(text),
  sendKeyEvent: (...event) => keys.push(event)
};
const exportsObject = {};
vm.runInNewContext(code, {exports: exportsObject, require(id) {
  if (id === '@kit.IMEKit') return {inputMethod: {getController: () => controller,
    TextInputType: {TEXT: 1}, EnterKeyType: {NEWLINE: 2, DONE: 3}}};
  if (id === '@kit.ArkUI') return {window: {WindowEventType: {WINDOW_ACTIVE: 2, WINDOW_INACTIVE: 3, WINDOW_HIDDEN: 4, WINDOW_DESTROYED: 7}}};
  if (id === 'libentry.so') return {default: native};
  if (id === '../common/DeviceCapabilityPolicy') return {DeviceCapabilityPolicy: {usesManagedWineWindows: () => pc}};
  if (id === './LogService') return {hilog: {info(){},warn(){}}};
  throw Error('Unexpected dependency '+id);
}});
const ime = exportsObject.WineImeCoordinator.getInstance();
function host(id) {
  const listeners = new Set();
  return {getWindowProperties: () => ({id}),
    on(event, fn) {assert.equal(event, 'windowEvent'); listeners.add(fn);},
    off(event, fn) {assert.equal(event, 'windowEvent'); assert.equal(typeof fn, 'function'); listeners.delete(fn);},
    emit(event) {for (const fn of [...listeners]) fn(event);}, listeners};
}
const hostA = host(100), hostB = host(200);
const emit = (event, ...args) => {for (const fn of [...(handlers.get(event)||[])]) fn(...args);};
const count = () => calls.filter(x => x[0] === 'attach').length;
const settle = () => ime.operations;
const tick = () => new Promise(setImmediate);
(async () => {
  ime.foreground(10, hostA, true); await settle();
  assert.equal(ime.hasAttachedSession(), false);
  assert.equal(armed, true); assert.equal(count(), 0, 'no caret: do not intercept gameplay keyboard');
  active = true; callback(1, 1, 2, 3, 4); await settle();
  assert.deepEqual(calls.at(-1), ['attach', false, 100], 'hardware IME binds the actual host without touch keyboard');
  assert.equal(ime.hasAttachedSession(), true, 'attached IME delivers Enter through sendFunctionKey; a raw orphan key-up cannot synthesize a second press');
  emit('setPreviewText', 'zhongwen', {start: 0,end: 8});
  emit('insertText', '中文'); emit('finishTextPreview');
  assert.deepEqual(commits, ['中文']); assert.deepEqual(preedits, ['zhongwen','']);
  emit('deleteLeft', 2); emit('deleteRight', 1); emit('sendFunctionKey', {enterKeyType: 2});
  assert.deepEqual(keys, [[10,14,true],[10,14,false],[10,14,true],[10,14,false],
    [10,111,true],[10,111,false],[10,28,true],[10,28,false]]);
  const before = keys.length; emit('deleteLeft', NaN); emit('deleteLeft', -3);
  assert.equal(keys.length, before);
  const oldInsert = [...handlers.get('insertText')].find(fn => fn !== foreignHandler);
  focused = 20; ime.foreground(20, hostB, true); ime.background(hostA); await settle();
  assert.deepEqual(calls.at(-1), ['attach', false, 200]);
  oldInsert('stale'); emit('insertText', '新窗');
  assert.deepEqual(commits, ['中文','新窗'], 'late old callback cannot type into the new window');
  callback(0, 0, 0, 0, 0); await settle();
  emit('insertText', '当前焦点');
  assert.equal(commits.at(-1), '当前焦点', 'late leave notification uses current native focus');
  ime.setKeyboardVisible(true); await settle();
  assert.deepEqual(calls.at(-1), ['attach', true, 200]);
  ime.setKeyboardVisible(false); await settle();
  assert.equal(armed, true, 'hiding overlay keyboard cannot disarm a PC hardware IME');
  ime.foreground(0, hostA, false); await settle();
  assert.equal(armed, false, 'leaving automatic host does not leave text input armed');
  assert.equal(handlers.get('insertText').size, 1); assert(handlers.get('insertText').has(foreignHandler));
  pc = false; ime.foreground(0, hostA, true); await settle();
  const n = calls.length; ime.foreground(0, hostA, true); await settle();
  assert.equal(calls.length, n, 'compare effective policy rather than raw automatic request');
  ime.setKeyboardVisible(true); active = true; focused = 31; callback(1); await settle();
  assert.deepEqual(calls.at(-1), ['attach', true, 100]);
  emit('insertText', '桌面子窗'); assert.equal(commits.at(-1), '桌面子窗');
  ime.setKeyboardVisible(false); await settle();
  pc = true; focused = 10; ime.foreground(10, hostA, true); active = true;
  let resolveAttach; pendingAttach = new Promise(resolve => resolveAttach = resolve);
  callback(1); await tick();
  focused = 20; ime.foreground(20, hostB, true);
  resolveAttach(); pendingAttach = null; await settle();
  assert.deepEqual(calls.at(-1), ['attach', false, 200], 'in-flight attach must retire before new host binds');
  assert.equal(handlers.get('insertText').size, 2, 'only one live bridge callback');
  ime.background(hostB); await settle();
  assert.equal(handlers.get('insertText').size, 1); assert.equal(armed, false);
  rejectNext = true; ime.foreground(20, hostB, true); active = true; callback(1); await settle();
  callback(1); await settle(); emit('insertText','恢复'); assert.equal(commits.at(-1),'恢复');
  active = false; callback(0); await settle(); emit('insertText','ignored');
  assert.equal(commits.at(-1),'恢复');
  ime.observeWindow(10, hostA, true); ime.observeWindow(20, hostB, true);
  ime.observeWindow(10, hostA, true); assert.equal(hostA.listeners.size, 1);
  focused = 10; hostA.emit(2); active = true; callback(1); await settle();
  assert.deepEqual(calls.at(-1), ['attach', false, 100]);
  focused = 20; hostB.emit(2); hostA.emit(3); active = true; callback(1); await settle();
  assert.deepEqual(calls.at(-1), ['attach', false, 200], 'visible PC windows must transfer IME on focus without a new Ability foreground');
  emit('insertText', 'focus switched'); assert.equal(commits.at(-1), 'focus switched');
  ime.unobserveWindow(hostA); assert.equal(hostA.listeners.size, 0);
  assert.equal(armed, true, 'old focus observer teardown cannot disarm the current host');
  hostB.emit(7); await settle(); assert.equal(hostB.listeners.size, 0); assert.equal(armed, false);
  console.log('Wine IME host identity, window focus switching, Chinese composition, precise deletes, async ownership, keyboard policy and recovery PASS');
})().catch(error => {console.error(error);process.exitCode=1;});
