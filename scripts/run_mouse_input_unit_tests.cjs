const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(path.resolve(process.argv[2]));
const root = path.resolve(__dirname, '../entry/src/main/ets');
const calls = [];
const scrollCalls = [];
const native = { sendPointerEvent: (...args) => calls.push(args), findToplevelAt: () => 10,
  sendScrollEvent: (...args) => scrollCalls.push(args),
  sendKeyEvent() {} };
const models = { DisplayMode: { DESKTOP: 'desktop' } };
const queriedDevices = [];
const hilog = { info() {}, warn() {} };
const inputDevice = { getDeviceInfoSync(id) {
  queriedDevices.push(id);
  if (id === 99) throw new Error('device disappeared');
  return { sources: [id === 15 ? 'touchpad' : 'mouse'] };
} };
const cache = new Map();
const globals = { MouseAction: { Press: 1, Release: 2, Move: 3 },
  MouseButton: { Left: 1, Right: 2, Middle: 4 }, setTimeout, clearTimeout };
function compile(text, dependencies) {
  const exports = {};
  const js = ts.transpileModule(text, { compilerOptions: {
    target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS } }).outputText;
  vm.runInNewContext(js, { exports, ...globals, require: dependencies });
  return exports;
}
function load(name) {
  if (cache.has(name)) return cache.get(name);
  const result = compile(fs.readFileSync(path.join(root, 'service', name + '.ets'), 'utf8'), dep => {
    if (dep === 'libentry.so') return { default: native };
    if (dep === '../model/AppModels') return models;
    if (dep === '@kit.InputKit') return { inputDevice };
    if (dep === '@kit.PerformanceAnalysisKit') return { hilog };
    return load(dep.slice(2));
  });
  cache.set(name, result);return result;
}
const dispatcher = load('InputDispatcher').InputDispatcher.getInstance();
const InputDeviceMapper = load('InputDeviceMapper').InputDeviceMapper;
const mapper = InputDeviceMapper.getInstance();
for (const [id, scale] of [[17, 2.5], [15, .625], [0, 2.5], [-1, 2.5], [99, 2.5]]) {
  const motion = mapper.calibrateRawMotion(id, 1, -2);
  assert.equal(motion.rawDx, scale);
  assert.equal(motion.rawDy, -2 * scale);
}
assert.deepEqual(queriedDevices, [17, 15, 99], 'invalid IDs need no device IPC');
mapper.calibrateRawMotion(17, 2, 0);
assert.deepEqual(queriedDevices, [17, 15, 99], 'device lookup stays off the hot path');
assert.equal(mapper.calibrateRawMotion(0, 0, 0).rawDx, 0, 'zero raw preserves native fallback');
dispatcher.setSession({ sessionId: 'test', toplevelId: 10, active: true, displayMode: 'desktop' });
dispatcher.pointerMove('hardware', 400, 300, false, { rawDx: 2.5, rawDy: -.625, fromMouse: true });
assert.deepEqual(calls.pop(), [10, 3, 400, 300, 0, 2.5, -.625, true]);
dispatcher.pointerDown('hardware', 400, 300, 272, { rawDx: 0, rawDy: 0, fromMouse: true });
assert.deepEqual(calls.pop(), [10, 1, 400, 300, 272, 0, 0, true]);
dispatcher.pointerUp('hardware', 400, 300, 272);
dispatcher.pointerDown('touch', 450, 350, 272);
assert.equal(calls.at(-1)[7], false, 'touch taps keep absolute targeting');
dispatcher.pointerUp('touch', 450, 350, 272);
dispatcher.pointerMove('hardware', 400, 300, false, { rawDx: 0, rawDy: 0, fromMouse: true });
assert.deepEqual(calls.pop(), [10, 3, 400, 300, 0, 0, 0, true]);
function method(file, name) {
  const text = fs.readFileSync(path.join(root, file), 'utf8');
  const start = text.indexOf('  private ' + name + '(');
  assert.ok(start >= 0);
  return text.slice(start, text.indexOf('\n  }', start) + 4);
}
const handler = method('pages/InputOverlay.ets', 'handleDirectMouse');
// Bind the same module names used by the real handler. Calibrated device input
// must survive the entire page -> dispatcher -> router -> NAPI chain.
const js = ts.transpileModule('export class Page {\n' + handler + '\n}', { compilerOptions: {
  target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS } }).outputText;
const exported = {};
vm.runInNewContext(js, { exports: exported, ...globals, vp2px: x => x * 2,
  mapMouseButton: () => 272, InputDispatcher: load('InputDispatcher').InputDispatcher,
  InputDeviceMapper });
const overlay = new exported.Page();
overlay.eventGate = { shouldHandleMouse: () => true };
overlay.hideForExternalInput = () => {};
overlay.mouseLeftReleaseTimer = -1;
const event = { action: 3, windowX: 200, windowY: 150, deviceId: 17,
  rawDeltaX: 1, rawDeltaY: -2, stopPropagation() {} };
overlay.handleDirectMouse(event);
assert.deepEqual(calls.pop(), [10, 3, 400, 300, 0, 2.5, -5, true]);
overlay.handleDirectMouse({ ...event, deviceId: 15 });
assert.deepEqual(calls.pop(), [10, 3, 400, 300, 0, .625, -1.25, true]);
for (const deviceId of [undefined, 0, -1, 99]) {
  overlay.handleDirectMouse({ ...event, deviceId });
  assert.deepEqual(calls.pop(), [10, 3, 400, 300, 0, 2.5, -5, true],
    'overlay must retain main mouse calibration when device identity is unavailable');
}
overlay.handleDirectMouse({ ...event, rawDeltaX: undefined, rawDeltaY: undefined });
assert.deepEqual(calls.pop(), [10, 3, 400, 300, 0, 0, 0, true]);
overlay.handleDirectMouse({ ...event, action: 1 });
assert.equal(calls.at(-1)[7], true);
dispatcher.releaseAll('test');
// Replay five seconds worth of report counts at common mouse polling rates.
// This is an unpaced host replay, not a measurement of Bluetooth latency or
// device FPS. Raw displacement must survive even when screen coordinates wrap.
const queryCountBeforeReplay = queriedDevices.length;
for (const reportsPerSecond of [125, 500, 1000]) {
  for (const [deviceId, scale] of [[17, 2.5], [15, .625]]) {
    calls.length = 0;
    let expectedX = 0, expectedY = 0;
    const reportCount = reportsPerSecond * 5;
    for (let index = 0; index < reportCount; index++) {
      const dx = index % 17 < 12 ? .5 : -.25;
      const dy = index % 11 < 7 ? -.125 : .375;
      expectedX += dx * scale;
      expectedY += dy * scale;
      overlay.handleDirectMouse({ ...event, deviceId,
        windowX: 200 + index % 64, windowY: 150 + index % 32,
        rawDeltaX: dx, rawDeltaY: dy });
    }
    assert.equal(calls.length, reportCount, 'every physical motion reaches NAPI');
    assert.equal(calls.reduce((total, call) => total + call[5], 0), expectedX,
      'small horizontal deltas survive high report counts');
    assert.equal(calls.reduce((total, call) => total + call[6], 0), expectedY,
      'small vertical deltas survive high report counts');
    assert.ok(calls.every(call => call[7] === true), 'replay retains hardware source');
    console.log(`mouse raw replay: ${reportsPerSecond}/s equivalent, ${reportCount} reports, device=${deviceId} PASS`);
  }
}
assert.equal(queriedDevices.length, queryCountBeforeReplay,
  'continuous motion never repeats synchronous device queries');
calls.length = 0;
// Execute the actual .onMouse callbacks of the alternate presentation paths.
// A device/source correction must behave the same through all four entrances.
function mouseCallback(file) {
  const source = fs.readFileSync(path.join(root, file), 'utf8');
  const match = /^(\s*)\.onMouse\(\((event|ev): MouseEvent\) => \{/m.exec(source);
  assert.ok(match, file);
  const start = match.index + match[0].length;
  const end = source.indexOf('\n' + match[1] + '})', start);
  assert.ok(end > start, file);
  const method = 'export class Page { handle(' + match[2] + ': MouseEvent) {' +
    source.slice(start, end) + '\n} }';
  return compile(method, () => { throw new Error('unexpected dependency'); }).Page;
}
// Callbacks refer to these names directly, as they do inside their pages.
Object.assign(globals, { AxisAction: { NONE: 0, BEGIN: 1, UPDATE: 2, END: 3, CANCEL: 4 }, vp2px: x => x * 2, mapMouseButton: () => 272,
  InputDeviceMapper, testNapi: native, hilog, DOMAIN: 0,
  WineWindowManager: { getInstance: () => ({ shouldStartMoving: () => false, getDesktopRootId: () => 1 }) },
  InputDispatcher: load('InputDispatcher').InputDispatcher });
const AxisPage = compile('export class Page {\n' + method('pages/InputOverlay.ets', 'handleDirectAxis') +
  '\n}', () => {}).Page;
const AxisEventGate = load('InputEventGate').AxisEventGate;
const axisPage = new AxisPage();
axisPage.axisEventGate = new AxisEventGate();
calls.length = 0;
const axisEvent = { action: 2, windowX: 350, windowY: 210, x: 2, y: 3, scrollStep: 0,
  getVerticalAxisValue: () => .25, getHorizontalAxisValue: () => -15 };
axisPage.handleDirectAxis(axisEvent);
assert.deepEqual(scrollCalls.splice(0), [[10, 0, .25, -1, 700, 420], [10, 1, -15, -1, 700, 420]]);
assert.equal(calls.length, 0, 'Hardware wheel must not emit an absolute motion before axis (Xwayland XI device switch)');
assert.deepEqual({ ...dispatcher.lastPointer() }, { x: 700, y: 420, valid: true });
axisPage.handleDirectAxis(axisEvent);
assert.deepEqual(scrollCalls.splice(0), [[10, 0, .25, -1, 700, 420], [10, 1, -15, -1, 700, 420]]);
assert.equal(calls.length, 0, 'Stationary repeated wheels keep the same XI scroll device');
calls.length = 0;
axisPage.handleDirectAxis({ ...axisEvent, getVerticalAxisValue: () => 0, getHorizontalAxisValue: () => 0 });
assert.equal(calls.length + scrollCalls.length, 0, 'Zero axis events cannot change focus or inject input');
for (const [file, target] of [['pages/WineWindow.ets', 20], ['pages/WinePopup.ets', 30],
                             ['components/DesktopLayer.ets', 10]]) {
  const source = fs.readFileSync(path.join(root, file), 'utf8');
  const match = /^(\s*)\.onAxisEvent\(\((event|ev)\?: AxisEvent\) => \{/m.exec(source);
  assert.ok(match, file);
  const start = match.index + match[0].length;
  const end = source.indexOf('\n' + match[1] + '})', start);
  const Page = compile('export class Page { handle(' + match[2] + '?: AxisEvent) {' +
    source.slice(start, end) + '\n} }', () => {}).Page;
  const page = new Page();
  Object.assign(page, { toplevelId: 20, popupId: 30, axisEventGate: new AxisEventGate(), noteHardwareActivity() {} });
  page.handle(axisEvent);
  assert.deepEqual(scrollCalls.splice(0), [[target, 0, .25, -1, 4, 6], [target, 1, -15, -1, 4, 6]], file);
  assert.equal(calls.length, 0, file + ': axis cannot become mouse-button input');
}
for (const action of [3, 4]) {
  calls.length = 0; axisPage.handleDirectAxis({ ...axisEvent, action });
  assert.equal(calls.length + scrollCalls.length, 0, 'Axis END/CANCEL must not replay retained deltas');
}
const axisGate = new AxisEventGate();
assert.equal(axisGate.shouldHandle(2, 100000000001, 10, 0, 1, 3), true);
assert.equal(axisGate.shouldHandle(2, 100000000001, 10, 0, 1, 3), false, 'same system axis sample is injected once');
assert.equal(axisGate.shouldHandle(2, 100000000002, 10, 0, 1, 3), true, 'adjacent real high-rate samples survive');
assert.equal(axisGate.shouldHandle(2, 100000000002, -10, 0, 1, 3), true, 'reverse scrolling is preserved');
assert.equal(axisGate.shouldHandle(3, 100000000002, -10, 0, 1, 3), false, 'terminal event never replays delta');
assert.equal(axisGate.shouldHandle(2, 0, 10, 0, 1, 3), true, 'unknown timestamps are not guessed');
console.log('Physical axis callbacks: continuous delta, current pointer, zero event, no click across all presentations PASS');
for (const [file, target] of [['pages/WineWindow.ets', 20],
                             ['pages/WinePopup.ets', 30],
                             ['components/DesktopLayer.ets', 10]]) {
  const Page = mouseCallback(file);
  const page = new Page();
  Object.assign(page, { toplevelId: 20, popupId: 30, floating: false,
    leftBtnTl: 0, rightBtnTl: 0, midBtnTl: 0,
    eventGate: { shouldHandleMouse: () => true }, noteHardwareActivity() {}, hoverLogCounter: 0 });
  for (const [deviceId, scale] of [[17, 2.5], [15, .625], [undefined, 2.5], [0, 2.5]]) {
    page.handle({ ...event, x: 200, y: 150, deviceId });
    const call = calls.pop();
    assert.deepEqual(call.slice(0, 4), [target, 3, 400, 300], file + ': unchanged absolute coordinates');
    assert.deepEqual(call.slice(5), [scale, -2 * scale, true], file + ': one shared calibration');
  }
}
const virtualMethods = ['shouldShowVirtualCursor', 'handleOverlayMouse', 'handleOverlayTouch',
  'handleTouchpad', 'handleToolbarButtonTouch', 'handleToolbarButtonMouse'];
Object.assign(globals, { TouchType: { Down: 0, Up: 1, Move: 2, Cancel: 3 } });
const VirtualPage = compile('export class VirtualInputOverlay {\n' + virtualMethods.map(name =>
  method('components/VirtualInputOverlay.ets', name)).join('\n') + '\n}', () => {}).VirtualInputOverlay;
const virtual = new VirtualPage();
let forwardedMouse = 0;
Object.assign(virtual, { pointerEventGate: new (load('InputEventGate').InputEventGate)(),
  virtualCursorActive: false, touchpadModeVisual: true, showCursor: true,
  cursorVpX: 200, cursorVpY: 150, hideToolbar: true, editMode: false,
  interactive: true, overlayControlsActive: () => false,
  keyboardShieldContains: () => false, onUnhandledMouse() { forwardedMouse++; },
  vcInit: true, vcX: 400, vcY: 300, ensureScreenPx() {}, startToolbarAutoHide() {} });
assert.equal(virtual.shouldShowVirtualCursor(), false, 'startup must not draw a stationary virtual arrow');
const finger = { id: 1, x: 100, y: 100 };
const touch = { source: 2, sourceTool: 1, type: 0,
  touches: [finger], changedTouches: [finger], stopPropagation() {} };
virtual.handleOverlayTouch(touch);
assert.equal(virtual.shouldShowVirtualCursor(), true, 'real touchpad input owns its indicator');
virtual.handleOverlayMouse({ ...event, source: 1, sourceTool: 3 });
assert.equal(forwardedMouse, 1, 'physical motion still reaches the game');
assert.equal(virtual.shouldShowVirtualCursor(), false, 'physical mouse hides the virtual arrow');
virtual.handleOverlayTouch({ ...touch, source: 1, sourceTool: 3 });
assert.equal(virtual.shouldShowVirtualCursor(), false, 'mouse-synthesized touch cannot resurrect the arrow');
virtual.handleOverlayTouch(touch);
virtual.handleOverlayMouse({ ...event, source: 2, sourceTool: 1 });
assert.equal(forwardedMouse, 1, 'touch-synthesized mouse is not forwarded twice');
assert.equal(virtual.shouldShowVirtualCursor(), true, 'synthetic mouse cannot steal touch ownership');
virtual.handleToolbarButtonMouse({ ...event, source: 1, sourceTool: 3 }, () => {});
assert.equal(virtual.shouldShowVirtualCursor(), false, 'toolbar hardware input also hides the indicator');
virtual.handleToolbarButtonTouch({ ...touch, source: 1, sourceTool: 3 }, () => {
  throw new Error('mouse touch copy must not toggle the toolbar');
});
const visibility = method('service/WineWindowManager.ets', 'canHideCursor');
const policyExports = {};
vm.runInNewContext(ts.transpileModule('export class Policy {\n' + visibility + '\n}', {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS } }).outputText,
  { exports: policyExports, DeviceCapabilityPolicy: { usesManagedWineWindows: () => false } });
const policy = new policyExports.Policy();
Object.assign(policy, { isDesktopMode: true, desktopAbilityRunning: true, desktopAbilityForeground: true,
  desktopLauncherVisible: true, desktopRootId: 1 });
assert.equal(policy.canHideCursor(10), true, 'foreground tablet desktop ignores launcher flag');
assert.equal(policy.canHideCursor(1), false, 'desktop shell stays visible');
assert.equal(policy.canHideCursor(0), false, 'unidentified surface stays visible');
policy.desktopAbilityForeground = false;
assert.equal(policy.canHideCursor(10), false, 'background desktop restores cursor');
policy.desktopAbilityRunning = false;
assert.equal(policy.canHideCursor(10), false, 'launcher preview stays visible');
policy.desktopLauncherVisible = false;
assert.equal(policy.canHideCursor(10), true, 'embedded full desktop still supports hiding');
console.log('mouse_input: hardware, touch, calibration, all entrances, cursor ownership and visibility PASS');


// Reproduce first-use and cross-panel scrolling without an intervening click.
Object.assign(globals, { InputRouter: load('InputRouter').InputRouter });
const DirectPage = compile('export class Page {\n' + ['handleDirectTouch', 'directSource',
  'midpointX', 'midpointY', 'resetDirectTapSequence', 'scrollThresholdPx'].map(name =>
    method('pages/InputOverlay.ets', name)).join('\n') + '\n}', () => {}).Page;
const direct = new DirectPage();
Object.assign(direct, { directTouches: new Map(), eventGate: { shouldHandleTouch: () => true },
  settings: { virtualMouseConfig: { mouseSensitivity: 100 } }, TAP_SLOP_PX: 48, TAP_TIMEOUT_MS: 450,
  injectDirectLeftTap() { throw new Error('two-finger scroll must not click'); } });
function fingers(type, x, y) {
  const points = [0, 1].map(id => ({ id, windowX: x + id * 40, windowY: y,
    x: x + id * 40, y }));
  return { type, touches: type === 1 || type === 3 ? [] : points,
    changedTouches: points, stopPropagation() {} };
}
function assertDirectGesture(x, y, terminal) {
  const anchor = [(x + 20) * 2, y * 2];
  calls.length = 0; scrollCalls.length = 0;
  direct.handleDirectTouch(fingers(0, x, y));
  assert.equal(calls.length + scrollCalls.length, 0, 'two-finger down has no click or motion');
  direct.handleDirectTouch(fingers(2, x, y - 70));
  assert.ok(scrollCalls.length > 0, 'two-finger move reaches the scroll path');
  assert.ok(scrollCalls.every(call => call[4] === anchor[0] && call[5] === anchor[1]),
    'Direct two-finger target must use this gesture start, not the old cursor or moving fingers');
  const count = scrollCalls.length;
  direct.handleDirectTouch(fingers(terminal, x, y - 70));
  assert.equal(scrollCalls.length, count, 'up/cancel never replays scroll');
  assert.equal(calls.length, 0, 'scroll cannot inject clicks or absolute mouse motion');
}
dispatcher.rememberPointer(50, 50);
assertDirectGesture(400, 250, 1);
assertDirectGesture(100, 300, 3);
assert.equal(direct.directTouches.size, 0);

// Touchpad gestures keep their existing virtual cursor target; finger location
// must only select a target in direct-touch mode.
const PadPage = compile('export class Page {\n' + ['handleTouchpadTouch', 'touchpadMidpoint'].map(name =>
  method('pages/InputOverlay.ets', name)).join('\n') + '\n}', () => {}).Page;
const pad = new PadPage();
Object.assign(pad, { eventGate: { shouldHandleTouch: () => true }, touchpadFingers: new Map(),
  settings: { virtualMouseConfig: { mouseSensitivity: 100 } }, cursorX: 100, cursorY: 80,
  ensureTouchpadCursor() {}, showCursorAt() {}, extendPendingTouchpadTap() {}, releaseTouchpadDrag() {},
  touchpadDragInterval: () => 400, touchpadPendingTapTimer: -1, touchpadLastTapAt: 0 });
dispatcher.rememberPointer(200, 160);
calls.length = 0; scrollCalls.length = 0;
pad.handleTouchpadTouch(fingers(0, 400, 300));
pad.handleTouchpadTouch(fingers(2, 400, 260));
assert.ok(scrollCalls.length > 0);
assert.ok(scrollCalls.every(call => call[4] === 200 && call[5] === 160),
  'touchpad scrolling remains at the virtual cursor');
assert.equal(calls.length, 0);

// Execute the legacy desktop touch callback as well: the scroll anchor must
// not follow moving fingers across panels and keep switching Xwayland devices.
const desktopSource = fs.readFileSync(path.join(root, 'components/DesktopLayer.ets'), 'utf8');
const desktopMatch = /^(\s*)\.onTouch\(\(ev: TouchEvent\) => \{/m.exec(desktopSource);
assert.ok(desktopMatch);
const desktopStart = desktopMatch.index + desktopMatch[0].length;
const desktopEnd = desktopSource.indexOf('\n' + desktopMatch[1] + '})', desktopStart);
assert.ok(desktopEnd > desktopStart);
Object.assign(globals, { DesktopLayer: { SCROLL_STEP_PX: 40 } });
const DesktopTouch = compile('export class Page { handle(ev: TouchEvent) {' +
  desktopSource.slice(desktopStart, desktopEnd) + '\n} }', () => {}).Page;
const desktopTouch = new DesktopTouch();
Object.assign(desktopTouch, { floating: false, eventGate: { shouldHandleTouch: () => true },
  twoFingerActive: false, touchPending: false, isDragging: false });
calls.length = 0; scrollCalls.length = 0;
desktopTouch.handle(fingers(0, 400, 300));
desktopTouch.handle(fingers(2, 430, 250));
assert.ok(scrollCalls.length > 0);
assert.ok(scrollCalls.every(call => call[4] === 840 && call[5] === 600),
  'legacy desktop two-finger scroll stays anchored at gesture start');
assert.equal(calls.length, 0);
console.log('two_finger_scroll: initial target, panel change, fixed anchor, terminal events, touchpad cursor PASS');

// The direct touch/mouse surface uses the same slots as the visible toolbar.
const Toolbar = compile('export class Toolbar {\n' +
  ['rectContains', 'toolbarActionAt'].map(name => method('components/VirtualInputOverlay.ets', name)).join('\n') +
  '\n}', () => {}).Toolbar;
const toolbar = new Toolbar();
Object.assign(toolbar, { toolbarExpanded: true, toolbarAutoHidden: false,
  toolbarOriginX: () => 100, toolbarOriginY: () => 200, resolveToolbarVertical: () => false });
for (const floating of [false, true]) {
  toolbar.desktopFloatingFullscreen = floating;
  for (const vertical of [false, true]) {
    toolbar.resolveToolbarVertical = () => vertical;
    const actions = ['toolbar', floating ? 'home' : 'launcher', 'editor', 'controls', 'mouse', 'keyboard'];
    for (let slot = 0; slot < actions.length; slot++) {
      const x = 100 + (vertical ? 22 : 22 + slot * 44);
      const y = 200 + (vertical ? (slot === 0 ? 20 : 40 + (slot - 1) * 44 + 22) : 26);
      assert.equal(toolbar.toolbarActionAt(x, y), actions[slot], `toolbar slot ${slot} vertical=${vertical} floating=${floating}`);
    }
  }
}
console.log('Toolbar: native/embedded launcher, horizontal/vertical action slots PASS');
