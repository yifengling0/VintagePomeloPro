// Production menu orchestration and toolbar hit geometry, with no device input.
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const ts = require(path.resolve(process.argv[2]));
const models = require(path.resolve(process.argv[3]));
const root = path.join(__dirname, '../entry/src/main/ets');
function methods(file, names) {
  const source = fs.readFileSync(path.join(root, file), 'utf8');
  return names.map(name => {
    const found = source.match(new RegExp('  private (?:async )?' + name + '\\([\\s\\S]*?\\n  \\}'));
    assert.ok(found, name); return found[0];
  }).join('\n');
}
function load(source, globals) {
  const exports = {};
  vm.runInNewContext(ts.transpileModule('export class Subject {\n'+source+'\n}', {
    compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS }
  }).outputText, { exports, ...globals });
  return new exports.Subject();
}
const menuMethods = methods('pages/InputOverlay.ets', [
  'openClipboardMenu', 'closeClipboardMenu', 'copySelectedText', 'pasteSystemText'
]);
function deferred() { let resolve; const promise = new Promise(r => { resolve = r; }); return {promise, resolve}; }
function fixture() {
  const available = deferred(), prepared = deferred(), keys = [], captures = [], toasts = [];
  let consumer = null, top = 70, queries = 0;
  const page = load(menuMethods, {
    X11ClipboardService: { systemTextAvailable: () => { ++queries; return available.promise; },
      prepareSystemPaste: () => prepared.promise },
    InputRouter: { getInstance: () => ({ getActiveToplevel: () => top, sendWindowKey: (...args) => keys.push(args) }) },
    InputDispatcher: { getInstance: () => ({ releaseAll() {} }) },
    GamepadManager: { getInstance: () => ({ setUiCapture: value => captures.push(value) }) },
    GamePanelInput: { attach: value => { consumer = value; }, detach: value => { if (value === consumer) consumer = null; } }
  });
  Object.assign(page, { snapshot: { visible: true, session: { sessionId: 'game' } },
    clipboardRequest: 0, clipboardInput: null, clipboardOpen: false,
    getUIContext: () => ({ getPromptAction: () => ({ showToast: value => toasts.push(value.message) }) }) });
  return { page, keys, captures, toasts, available, prepared,
    top: value => { top = value; }, consumer: () => consumer, queries: () => queries };
}
(async () => {
  {
    const f = fixture(); const request = f.page.openClipboardMenu(30, 90);
    assert.equal(f.page.clipboardChecking, true); assert.equal(f.page.clipboardAvailable, false);
    f.available.resolve(true); await request;
    assert.equal(f.page.clipboardOpen, true); assert.equal(f.page.clipboardAvailable, true);
    f.consumer()(2070); assert.equal(f.page.clipboardOpen, false);
    assert.deepEqual(f.captures, [true, false]); assert.equal(f.consumer(), null);
  }
  {
    const f = fixture(); const request = f.page.openClipboardMenu(0, 0);
    f.page.closeClipboardMenu(); f.available.resolve(true); await request;
    assert.equal(f.page.clipboardAvailable, false, 'Dismissed menu ignores a late clipboard query');
    f.page.snapshot.visible = false; await f.page.openClipboardMenu(0, 0);
    assert.equal(f.queries(), 1, 'Invisible sessions cannot open the menu');
  }
  {
    const f = fixture(); const request = f.page.openClipboardMenu(0, 0);
    f.available.resolve(false); await request;
    assert.equal(f.page.clipboardOpen, true); assert.equal(f.page.clipboardAvailable, false);
    f.page.copySelectedText();
    assert.deepEqual(f.keys.map(x => x.slice(1)), [
      ['clipboard-copy',29,true], ['clipboard-copy',46,true], ['clipboard-copy',46,false], ['clipboard-copy',29,false]
    ]);
    f.page.closeClipboardMenu();
  }
  for (const change of ['none', 'session', 'window', 'hidden']) {
    const f = fixture(); const paste = f.page.pasteSystemText();
    if (change === 'session') f.page.snapshot.session.sessionId = 'replacement';
    if (change === 'window') f.top(71);
    if (change === 'hidden') f.page.snapshot.visible = false;
    f.prepared.resolve(true); await paste;
    assert.equal(f.keys.length, change === 'none' ? 4 : 0, 'Paste must retain its original visible session and window');
    if (change === 'none') assert.deepEqual(f.keys.map(x => x.slice(1)), [
      ['clipboard-paste',29,true], ['clipboard-paste',47,true], ['clipboard-paste',47,false], ['clipboard-paste',29,false]
    ]);
  }
  {
    const f = fixture(); const paste = f.page.pasteSystemText(); f.prepared.resolve(false); await paste;
    assert.equal(f.keys.length, 0); assert.equal(f.toasts.length, 1);
  }
  const geometry = methods('components/VirtualInputOverlay.ets', ['resolveToolbarVertical', 'isAutoHideMode',
    'toolbarWidth', 'toolbarHeight', 'toolbarOriginX', 'toolbarOriginY', 'clampToolbarX', 'clampToolbarY',
    'applyConfiguredToolbarPosition', 'toolbarActionAt', 'rectContains']);
  for (const [w,h] of [[320,640], [360,800], [800,360], [1280,800]]) for (const edge of [0,1]) {
    const bar = load(geometry, { ControlBarDirection: models.ControlBarDirection });
    Object.assign(bar, { overlayW: w, overlayH: h, toolbarExpanded: true, toolbarAutoHidden: false,
      controlBarDirection: models.ControlBarDirection.AUTO, controlBarPositionX: edge, controlBarPositionY: edge });
    bar.applyConfiguredToolbarPosition();
    assert.ok(bar.toolbarX >= 0 && bar.toolbarX+bar.toolbarWidth() <= w);
    assert.ok(bar.toolbarY >= 0 && bar.toolbarY+bar.toolbarHeight() <= h);
    const vertical = bar.resolveToolbarVertical();
    assert.equal(bar.toolbarActionAt(bar.toolbarX+(vertical?22:282), bar.toolbarY+(vertical?282:26)), 'clipboard');
  }
  console.log('PASS clipboard menu: metadata visibility, dismissal/late results, Escape/capture, target-safe copy/paste and phone/tablet toolbar hit geometry');
})().catch(error => { console.error(error); process.exitCode = 1; });
