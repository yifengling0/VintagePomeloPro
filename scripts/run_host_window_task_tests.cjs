#!/usr/bin/env node
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(path.resolve(process.argv[2]));
const file = path.resolve(__dirname, '../entry/src/main/ets/service/HostWindowTasks.ets');
const output = ts.transpileModule(fs.readFileSync(file, 'utf8'), {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS }
}).outputText;
const exportsObject = {};
vm.runInNewContext(output, { exports: exportsObject });
const Registry = exportsObject.HostWindowTasks;
const event = (r, id, name, data = {}, native = false) => r.onEvent(id, name, JSON.stringify(data), native);
const drain = async () => { for (let n = 0; n < 12; ++n) await Promise.resolve(); };

(async () => {
  const r = new Registry();
  let notifications = 0;
  const listener = () => ++notifications;
  r.subscribe(listener);
  event(r, 1, 'created', { clientPid: 100, sessionId: '' }, true);
  event(r, 1, 'title', { title: 'Steam' });
  r.registerSteamGame('game-a'); // Launch registration can precede the window.
  event(r, 2, 'created', { clientPid: 200, sessionId: 'game-a' });
  event(r, 2, 'title', { title: 'Steam' }); // A title must never establish native provenance.
  assert.equal(r.snapshot()[0].source, 'steam-linux');
  assert.equal(r.snapshot()[1].source, 'steam-game');
  event(r, 2, 'minimized');
  assert.equal(r.snapshot().length, 2);
  assert.equal(r.snapshot()[1].minimized, true);
  const copy = r.snapshot()[1]; copy.title = 'modified';
  assert.equal(r.snapshot()[1].title, 'Steam');
  event(r, 3, 'modal', { modal: 1, owner: 2 });
  event(r, 3, 'created', { sessionId: 'game-a' });
  event(r, 4, 'modal', { modal: 1, owner: 3 });
  event(r, 4, 'created', { sessionId: 'game-a' });
  event(r, 5, 'desktop_root'); event(r, 5, 'created');
  assert.equal(r.snapshot().length, 2, 'Modal and desktop surfaces are not independent tasks');
  assert.equal(r.focusTarget(2), 4);
  event(r, 4, 'destroyed'); assert.equal(r.focusTarget(2), 3);
  event(r, 3, 'destroyed'); assert.equal(r.focusTarget(2), 2);

  const calls = [];
  r.setActivator(async (task, current) => { calls.push(task.key); return current(); });
  const oldKey = r.snapshot()[1].key;
  assert.equal(await r.activate(oldKey), true);
  assert.equal(r.snapshot()[1].minimized, false);
  event(r, 2, 'destroyed'); await drain();
  assert.equal(calls.length, 2, 'Foreground game exit returns to native Steam');
  event(r, 2, 'created', { sessionId: 'game-b' });
  r.registerSteamGame('game-b');
  assert.notEqual(r.snapshot()[1].key, oldKey, 'ID reuse requires a new identity');
  assert.equal(await r.activate(oldKey), false);
  r.noteInteraction(1);
  event(r, 2, 'destroyed'); await drain();
  assert.equal(calls.length, 2, 'Background game exit must not steal focus');

  event(r, 6, 'created', { clientPid: 300 });
  r.registerSteamGame('late'); r.associateSession(6, 'late');
  assert.equal(r.snapshot().find(t => t.toplevelId === 6).source, 'steam-game');
  let release;
  const raised = [];
  r.setActivator(async (task, current) => {
    if (task.toplevelId === 6) await new Promise(resolve => { release = resolve; });
    if (!current()) return false;
    raised.push(task.toplevelId); return true;
  });
  const pending = r.activate(r.snapshot().find(t => t.toplevelId === 6).key);
  await drain();
  const latest = r.activate(r.snapshot()[0].key);
  release();
  assert.equal(await pending, false);
  assert.equal(await latest, true);
  assert.deepEqual(raised, [1], 'A stale show completion cannot raise above the latest request');
  const before = notifications;
  r.unsubscribe(listener); r.reset();
  assert.equal(notifications, before);
  assert.equal(r.snapshot().length, 0);
  console.log('Host task lifecycle, provenance, minimize/restore, modal focus, late association, ID reuse, exit focus and async switching: PASS');
})().catch(error => { console.error(error); process.exitCode = 1; });
