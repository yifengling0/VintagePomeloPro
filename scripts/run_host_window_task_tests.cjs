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
  // Exercise the production registry: dispatch confirmation cannot reveal
  // Steam; only a matching game's real toplevel completes the foreground handoff.
  const g = new Registry();
  const gameRaises = [];
  g.setActivator(async (task, current) => { gameRaises.push(task.toplevelId); return current(); });
  g.markLauncherForeground();
  g.expectSteamGame('695630');
  g.registerSteamGame('deck-game', '695630');
  event(g, 90, 'created', {sessionId:'helper'}, true);
  await drain(); assert.deepEqual(gameRaises, [], 'Steam/helper cannot satisfy Play intent');
  event(g, 91, 'created', {sessionId:'deck-game'});
  await drain(); assert.deepEqual(gameRaises, [91]);
  assert.equal(g.isSteamGameForeground(), true, 'Real game restores the Wine desktop from Deck');
  event(g, 92, 'created', {sessionId:'deck-game'});
  await drain(); assert.deepEqual(gameRaises, [91], 'No repeated restore for replacement windows');

  g.markLauncherForeground(); g.expectSteamGame('695630');
  event(g, 93, 'created'); // Window races ahead of broker registration.
  g.associateSession(93, 'early-game');
  g.registerSteamGame('early-game', '695630');
  await drain(); assert.deepEqual(gameRaises, [91,93]);

  g.markLauncherForeground(); g.expectSteamGame('695630');
  g.markLauncherForeground();
  g.registerSteamGame('cancelled', '695630');
  event(g, 94, 'created', {sessionId:'cancelled'});
  await drain(); assert.deepEqual(gameRaises, [91,93], 'Returning home cancels pending Play focus');

  g.expectSteamGame('695630'); g.noteInteraction(90);
  g.registerSteamGame('interaction', '695630');
  event(g, 95, 'created', {sessionId:'interaction'});
  await drain(); assert.deepEqual(gameRaises, [91,93], 'An explicit interaction wins');

  g.markLauncherForeground(); const stale = g.expectSteamGame('695630');
  g.expectSteamGame('1746030'); g.cancelExpectedSteamGame(stale);
  g.registerSteamGame('wrong-app', '695630'); event(g, 96, 'created', {sessionId:'wrong-app'});
  await drain(); assert.deepEqual(gameRaises, [91,93], 'Other AppID cannot satisfy newer intent');
  g.registerSteamGame('new-app', '1746030'); event(g, 97, 'created', {sessionId:'new-app'});
  await drain(); assert.deepEqual(gameRaises, [91,93,97], 'Older failure cannot cancel newer Play');

  g.markLauncherForeground(); g.expectSteamGame('695630'); g.pendingSteamLaunch.expiresAt=0;
  g.registerSteamGame('expired', '695630'); event(g, 98, 'created', {sessionId:'expired'});
  await drain(); assert.deepEqual(gameRaises, [91,93,97], 'Expired intent never steals focus');

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
  assert.equal(calls.length, 1, 'Window destruction alone cannot prove game exit');
  r.finishSteamGame('game-a'); await drain();
  assert.equal(calls.length, 2, 'Confirmed foreground game exit returns to native Steam');
  event(r, 2, 'created', { sessionId: 'game-b' });
  r.registerSteamGame('game-b');
  assert.notEqual(r.snapshot()[1].key, oldKey, 'ID reuse requires a new identity');
  assert.equal(await r.activate(oldKey), false);
  r.noteInteraction(1);
  event(r, 2, 'destroyed'); r.finishSteamGame('game-b'); await drain();
  assert.equal(calls.length, 2, 'Background game exit must not steal focus');

  r.registerSteamGame('replacement');
  event(r, 20, 'created', { sessionId: 'replacement' });
  event(r, 20, 'destroyed'); await drain();
  event(r, 21, 'created', { sessionId: 'replacement' }); await drain();
  assert.equal(calls.length, 2, 'Splash replacement cannot bounce back to Steam');
  r.finishSteamGame('replacement'); await drain();
  assert.equal(calls.length, 2, 'Process end before window removal must wait for teardown');
  event(r, 21, 'destroyed'); await drain();
  assert.equal(calls.length, 3, 'Both process and window teardown return exactly once');

  r.registerSteamGame('multi');
  event(r, 22, 'created', { sessionId: 'multi' });
  event(r, 23, 'created', { sessionId: 'multi' });
  event(r, 23, 'destroyed'); await drain();
  assert.equal(calls.length, 3, 'Closing one of multiple game windows never restores Steam');
  event(r, 22, 'destroyed'); r.finishSteamGame('multi'); await drain();
  assert.equal(calls.length, 4);

  r.registerSteamGame('home');
  event(r, 24, 'created', { sessionId: 'home' });
  event(r, 24, 'destroyed'); r.markLauncherForeground();
  r.finishSteamGame('home'); await drain();
  assert.equal(calls.length, 4, 'Returning to Deck while a game exits wins');
  r.markDesktopForeground();
  r.registerSteamGame('unknown');
  event(r, 25, 'created', { sessionId: 'unknown' });
  event(r, 25, 'destroyed'); r.finishSteamGame('unknown', false); await drain();
  assert.equal(calls.length, 4, 'Missing process state is not a confirmed exit');

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
  // Clicking the already-active window must cancel a delayed raise of another
  // task too; active identity need not change to express a foreground choice.
  let releaseOther;
  r.setActivator(async (task, current) => {
    await new Promise(resolve => { releaseOther = resolve; });
    return current();
  });
  const other = r.activate(r.snapshot().find(t => t.toplevelId === 6).key);
  await drain();
  r.noteInteraction(1);
  releaseOther();
  assert.equal(await other, false, 'Interaction with active Steam cancels a pending game raise');
  const revision = r.foregroundRevision();
  r.markLauncherForeground();
  assert.notEqual(r.foregroundRevision(), revision, 'Home invalidates foreground intent captured before cold startup');
  const before = notifications;
  r.unsubscribe(listener); r.reset();
  assert.equal(notifications, before);
  assert.equal(r.snapshot().length, 0);
  console.log('Host task lifecycle, provenance, minimize/restore, modal focus, late association, ID reuse, exit focus and async switching: PASS');
})().catch(error => { console.error(error); process.exitCode = 1; });
