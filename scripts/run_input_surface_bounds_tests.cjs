const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(path.resolve(process.argv[2]));
const root = path.resolve(__dirname, '../entry/src/main/ets');
const exports_ = {};
const releases = [];
const dispatcher = { setSession() {}, releaseAll: why => releases.push(why) };
vm.runInNewContext(ts.transpileModule(fs.readFileSync(path.join(root,
  'service/InputOverlayWindowCoordinator.ets'), 'utf8'), { compilerOptions: {
  target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS
} }).outputText, { exports: exports_, setTimeout() {}, require(id) {
  const deps = { '@kit.ArkUI': {}, './LogService': { hilog: { info() {}, warn() {}, error() {} } },
    '../model/AppModels': { DisplayMode: { DESKTOP: 'desktop' } },
    './InputDispatcher': { InputDispatcher: { getInstance: () => dispatcher } } };
  assert.ok(id in deps, id); return deps[id];
} });
const { InputOverlayWindowCoordinator: Coordinator, InputOverlaySurface: Surface } = exports_;
const coordinator = Coordinator.getInstance();
let parentRect = { left: 0, top: 0, width: 2560, height: 1600 };
let actualRect = {};
let deferResize;
let holdResize = false;
const operations = [];
const overlay = {
  resize: async (width, height) => {
    operations.push(['resize', width, height]); actualRect.width = width; actualRect.height = height;
    if (holdResize) { holdResize = false; await new Promise(resolve => { deferResize = resolve; }); }
  },
  moveWindowToGlobal: async (left, top) => {
    operations.push(['move', left, top]); Object.assign(actualRect, { left, top });
  },
  getWindowProperties: () => ({ windowRect: actualRect }),
  setWindowTouchable: async () => {}, showWindow: async () => {},
  destroyWindow: async () => {}, raiseToAppTop: async () => {}
};
coordinator.overlayWindow = overlay;
coordinator.parentWindow = { getWindowProperties: () => ({ windowRect: parentRect }) };
coordinator.snapshot.visible = true;
const surface = new Surface();
function report(key, left, top, width, height, owner = surface) {
  coordinator.updateSurfaceBounds(owner, key, left, top, width, height);
}
function expectRect(left, top, width, height) {
  assert.deepEqual(actualRect, { left, top, width, height });
}
async function main() {
  // Mini independent game: no desktop cut-edge; decor is measured, not guessed.
  coordinator.surfaceKey = 'window-2';
  coordinator.snapshot.session.toplevelId = 2;
  coordinator.setActiveToplevel(1);
  assert.equal(coordinator.getSnapshot().session.toplevelId, 2, 'late desktop rebind cannot steal game input');
  report('window-2', 0, 105, 2560, 1430);
  await coordinator.boundsOp;
  expectRect(0, 105, 2560, 1430);
  // Overlay-local touch pixels and a virtual cursor now share the visible hotspot.
  for (const [x, y] of [[0, 0], [1280, 715], [2500, 1400]]) {
    assert.equal(actualRect.left + x, parentRect.left + surface.left + x);
    assert.equal(actualRect.top + y, parentRect.top + surface.top + y);
  }
  // Restore/move uses the live parent origin while retaining surface-local bounds.
  parentRect = { left: 211, top: 73, width: 2560, height: 1600 };
  await coordinator.syncBounds(); expectRect(211, 178, 2560, 1430);
  // Rotation, fullscreen and a different DPI deliver actual physical bounds.
  report('window-2', 18, 54, 1564, 2406);
  await coordinator.boundsOp; expectRect(229, 127, 1564, 2406);
  report('window-2', 0, 0, 1600, 2560);
  await coordinator.boundsOp; expectRect(211, 73, 1600, 2560);
  report('window-2', NaN, 0, 1, 1);
  report('window-2', 0, 0, 0, 1);
  await coordinator.syncBounds(); expectRect(211, 73, 1600, 2560);
  // Background desktop relayout cannot overwrite a foreground game's bounds.
  const desktop = new Surface();
  report('desktop', 80, 0, 2400, 1600, desktop);
  await coordinator.boundsOp; expectRect(211, 73, 1600, 2560);
  coordinator.surfaceKey = 'desktop';
  await coordinator.syncBounds(); expectRect(291, 73, 2400, 1600);
  // An old page's teardown cannot remove its replacement measurement.
  const replacement = new Surface();
  report('desktop', 0, 42, 2560, 1516, replacement);
  coordinator.releaseSurfaceBounds(desktop);
  await coordinator.boundsOp; expectRect(211, 115, 2560, 1516);
  // Delayed old resize cannot reposition an overlay after switching sessions.
  holdResize = true;
  const stale = coordinator.syncBounds();
  await Promise.resolve(); await Promise.resolve();
  assert.equal(typeof deferResize, 'function');
  const beforeSwitch = operations.length;
  coordinator.surfaceKey = 'window-2';
  const latest = coordinator.syncBounds();
  deferResize(); await Promise.all([stale, latest]);
  assert.deepEqual(operations.slice(beforeSwitch), [['resize', 1600, 2560], ['move', 211, 73]]);
  // Hiding wins over an in-flight bounds update and keeps the input noninteractive.
  holdResize = true;
  const resizing = coordinator.syncBounds();
  await Promise.resolve(); await Promise.resolve();
  await coordinator.setVisible(false);
  deferResize(); await resizing;
  expectRect(-10000, -10000, 1600, 2560);
  assert.ok(releases.includes('overlay-hidden'));
  console.log('Input surface bounds: Mini decor, cut-edge, cursor/touch alignment, move, rotation, fullscreen, stale owner, async switch/hide PASS');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
