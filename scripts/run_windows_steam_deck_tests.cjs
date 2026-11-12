// Run the production Deck model and route methods without mounting ArkUI.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(path.resolve(process.argv[2]));
const root = path.join(__dirname, '../entry/src/main/ets');
function load(relative, deps = {}, globals = {}) {
  const out = {};
  const code = ts.transpileModule(fs.readFileSync(path.join(root, relative), 'utf8'), {
    compilerOptions: {target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS}
  }).outputText;
  vm.runInNewContext(code, {exports: out, Set, Map, ...globals, require(id) {
    assert.ok(id in deps, id); return deps[id];
  }});
  return out;
}
const rules = load('steam/SteamLocalRules.ets');
const {windowsDeckSteamGames} = load('model/DeckSteamLibrary.ets', {
  './AppModels': {LaunchKind: {STEAM_GAME: 'steam-game'}}, '../steam/SteamLocalRules': rules
});
const windows = {id: 'steam-default-3590', title: '仙剑奇侠传', source: 'download', directory: '/private/中文 游戏',
  coverPath: '/private/cover.jpg', backgroundPath: '/private/hero.jpg', available: true,
  displayModeOverride: 'desktop', launchTarget: {kind: 'steam-game', executable: '/private/Steam/steam.exe',
    arguments: ['-custom'], workingDirectory: '/private/Steam'}, steam: {appId: '3590', state: 'installed'}};
const [card] = windowsDeckSteamGames([windows, windows]);
assert.equal(card.key, 'windows-3590'); assert.equal(card.windowsCard, windows);
assert.equal(card.directory, windows.directory); assert.equal(card.cover, windows.coverPath);
assert.equal(card.hero, windows.backgroundPath); assert.equal('key' in windows, false);
assert.equal(windowsDeckSteamGames([]).length, 0);
assert.equal(windowsDeckSteamGames([windows, windows]).length, 1);
for (const invalid of [
  {...windows, available: false}, {...windows, steam: undefined},
  {...windows, steam: {...windows.steam, state: 'installing'}},
  {...windows, steam: {...windows.steam, state: 'install-request'}},
  {...windows, steam: {...windows.steam, appId: '228980'}},
  {...windows, launchTarget: {...windows.launchTarget, kind: 'steam-client'}},
  ...['0', '-1', '../3590', '4294967296', '3590;cmd'].map(appId => ({...windows, steam: {...windows.steam, appId}}))
]) assert.equal(windowsDeckSteamGames([invalid]).length, 0, 'only installed, available Windows game cards are playable');
assert.equal(rules.isSteamAppId('4294967295'), true);
assert.equal(rules.isSteamAppId('4294967296'), false);
const other = {...windows, id: 'steam-default-7', title: 'Banana', steam: {...windows.steam, appId: '7'}};
assert.equal(JSON.stringify(windowsDeckSteamGames([windows, other]).map(g => g.name)), JSON.stringify(['Banana', '仙剑奇侠传']));
console.log('Windows Deck catalog: Unicode/private paths, original settings, AppID bounds, empty/uninstall, installed filters, dedupe and sort PASS');

async function routes() {
  const home = fs.readFileSync(path.join(root, 'components/hmosdeck/DeckHome.ets'), 'utf8');
  const a = home.indexOf('  private async openLocal(');
  const b = home.indexOf('  private async refreshLibrary(');
  const fragment = home.slice(a, home.indexOf('  @Builder', a)) +
    home.slice(b, home.indexOf('  private filteredLocal(', b));
  const out = {}, events = [];
  let installed = true, failed = false;
  const service = {installed: () => installed, async open(action) {events.push(['client', action]);}};
  vm.runInNewContext(ts.transpileModule('export class Router {\n' + fragment + '\n}', {
    compilerOptions: {target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS}
  }).outputText, {exports: out, SteamClientService: {getInstance: () => service},
    promptAction: {showToast: value => events.push(['toast', value.message])}});
  const router = new out.Router();
  router.games = [card]; router.libraryTab = 'steam'; router.query = '仙剑';
  assert.equal(router.filtered().length, 1);
  router.query = 'absent'; assert.equal(router.filtered().length, 0);
  router.query = ''; router.libraryTab = 'local'; assert.equal(router.filtered().length, 0);
  router.onLocalSettings = app => events.push(['settings', app]);
  router.onLocalLaunch = async app => {if (failed) throw Error('prepare failed'); events.push(['windows', app]);};
  router.localCover = app => app.coverPath; router.localBuiltin = () => undefined;
  router.beginLaunch = async (_title, _cover, _message, _builtin, action) => action();
  router.manageComponents = () => events.push(['components']);
  await router.openGame(card); assert.equal(events.pop()[1], windows, 'Deck preserves the launch descriptor and Wine settings');
  await router.openLocal({...windows, available: false}); assert.equal(events.pop()[0], 'settings');
  router.busy = true; await router.openGame(card); await router.open('library'); assert.equal(events.length, 0);
  router.busy = false; installed = false; await router.open('library'); assert.equal(events.pop()[0], 'components');
  installed = true;
  for (const action of ['library', 'downloads', 'account', 'store', 'desktop', 'bigPicture']) {
    await router.open(action); assert.equal(JSON.stringify(events.pop()), JSON.stringify(['client', action]));
  }
  failed = true; await assert.rejects(router.openGame(card), /prepare failed/); failed = false;
  router.onRefresh = async () => {await Promise.resolve(); events.push(['scan-windows']);};
  router.refresh = () => events.push(['render']);
  await router.refreshLibrary();
  assert.equal(JSON.stringify(events), JSON.stringify([['scan-windows'], ['render']]));
  const {deckLayout} = load('model/DeckLayout.ets');
  for (const [width, height] of [[320,640], [360,800], [412,915], [600,960], [800,360], [1280,800]]) {
    const layout = deckLayout(width, height);
    assert.ok(layout.contentWidth > 0 && layout.contentHeight > 0);
    assert.ok(Math.abs(layout.coverHeight / 1.5 * layout.columns + (layout.columns - 1) * layout.gap +
      2 * layout.inset - layout.contentWidth) < 0.001, 'phone/tablet cards fit without horizontal overflow');
  }
  console.log('Windows Deck routes: empty install, game settings, busy guard, six client actions, failed launch, refresh ordering and phone/tablet geometry PASS');
}
async function panel() {
  const source = fs.readFileSync(path.join(root, 'components/SteamClientPanel.ets'), 'utf8');
  const start = source.indexOf('  private async scanGames(');
  const out = {}, events = [];
  let fail = false, complete;
  const catalog = {async scan() {events.push('scan'); if (fail) throw Error('scan failed'); await new Promise(resolve => {complete = resolve;});}};
  vm.runInNewContext(ts.transpileModule('export class Panel {\n' + source.slice(start, source.indexOf('  private openComponents', start)) + '\n}', {
    compilerOptions: {target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS}
  }).outputText, {exports: out, AppCatalogService: {getInstance: () => catalog}});
  const panel = new out.Panel(); panel.onChanged = () => events.push('changed');
  const scan = panel.scanGames(); await panel.scanGames();
  assert.equal(events.length, 1, 'duplicate refresh is coalesced'); complete(); await scan;
  assert.equal(events[1], 'changed'); assert.equal(panel.scanning, false);
  fail = true; await panel.scanGames(); assert.equal(panel.message, 'scan failed'); assert.equal(panel.scanning, false);
  panel.busy = true; const count = events.length; await panel.scanGames(); assert.equal(events.length, count);
  console.log('Steam components: manual scan executes the real catalog, duplicate scan/import guard and failure recovery PASS');
}
(async () => {await routes(); await panel();})().catch(error => {console.error(error); process.exitCode = 1;});