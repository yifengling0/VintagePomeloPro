const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(path.resolve(process.argv[2]));

function load(file, deps) {
  const exports = {};
  const source = fs.readFileSync(path.join(__dirname, '../entry/src/main/ets', file), 'utf8');
  const compiled = ts.transpileModule(source, { compilerOptions: {
    target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS
  } }).outputText;
  vm.runInNewContext(compiled, { exports, Set, Map, Promise, require(id) {
    assert.ok(id in deps, `unexpected dependency ${id}`); return deps[id];
  } });
  return exports;
}

const { InputOverlayWindowCoordinator } = load('service/InputOverlayWindowCoordinator.ets', {
  '@kit.ArkUI': {}, './LogService': {},
  '../model/AppModels': { DisplayMode: { DESKTOP: 'desktop' } },
  './InputDispatcher': {}, './AppSettingsStore': {}
});
const coordinator = InputOverlayWindowCoordinator.getInstance();
const { DeckLaunchFeedbackService } = load('service/DeckLaunchFeedbackService.ets', {
  './InputOverlayWindowCoordinator': { InputOverlayWindowCoordinator }
});
const feedback = DeckLaunchFeedbackService.getInstance();

async function main() {
  const observed = [];
  const listener = value => observed.push(value);
  feedback.subscribe(listener);
  assert.equal(observed[0], null, 'late-mounted host subscribes to current state');
  const first = feedback.begin({ title: 'Steam', cover: '', message: 'Preparing' });
  await coordinator.blockingOp;
  assert.equal(coordinator.getSnapshot().blocking, true);
  await coordinator.setEngineBusy(true);
  await coordinator.setEngineBusy(false);
  assert.equal(coordinator.getSnapshot().blocking, true, 'Wine READY cannot uncover an unfinished Steam launch');

  const snapshot = feedback.snapshot(); snapshot.title = 'mutated';
  assert.equal(feedback.snapshot().title, 'Steam', 'observers do not mutate the active request');
  const second = feedback.begin({ title: 'Game', cover: '/cover.png', message: 'Starting' });
  feedback.update(first, 'stale phase'); feedback.finish(first);
  assert.equal(feedback.snapshot().title, 'Game', 'old completion cannot dismiss a newer mask');
  feedback.update(second, 'Opening window');
  let lateState;
  const late = value => { lateState = value; };
  feedback.subscribe(late);
  assert.equal(lateState.message, 'Opening window', 'new input window receives the current stage');
  feedback.unsubscribe(late);
  const count = observed.length;
  feedback.update(second, 'Opening window');
  assert.equal(observed.length, count, 'unchanged polling does not repaint the mask');

  await coordinator.setEngineBusy(true);
  feedback.finish(second);
  await coordinator.blockingOp;
  assert.equal(feedback.snapshot(), null);
  assert.equal(coordinator.getSnapshot().blocking, true, 'mask dismissal cannot unblock an initializing engine');
  await coordinator.setEngineBusy(false);
  assert.equal(coordinator.getSnapshot().blocking, false);
  feedback.update(second, 'late result'); feedback.finish(second);
  assert.equal(feedback.snapshot(), null, 'completed feedback cannot reappear');
  feedback.unsubscribe(listener);

  // Queue a delayed native operation, then change reasons while it is in flight.
  // Window operations must drain in order and end in the latest combined state.
  let finishShow;
  const shows = [];
  coordinator.overlayWindow = {
    showWindow: async () => { shows.push('show'); await new Promise(resolve => { finishShow = resolve; }); },
    setWindowTouchable: async value => { shows.push(value); }
  };
  coordinator.raise = async () => {};
  const engineOn = coordinator.setEngineBusy(true);
  await Promise.resolve(); await Promise.resolve();
  assert.equal(typeof finishShow, 'function');
  const launchOn = coordinator.setLaunchBusy(true);
  const engineOff = coordinator.setEngineBusy(false);
  const launchOff = coordinator.setLaunchBusy(false);
  finishShow();
  await Promise.all([engineOn, launchOn, engineOff, launchOff]);
  assert.equal(coordinator.getSnapshot().blocking, false, 'late show cannot restore a stale blocking reason');
  assert.equal(shows.filter(value => value === 'show').length, 1, 'one native show for overlapping reasons');
  console.log('Deck launch feedback: stale completions, cross-window subscription, engine/launch overlap and queued window updates PASS');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
