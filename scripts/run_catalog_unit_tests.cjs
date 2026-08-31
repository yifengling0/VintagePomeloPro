#!/usr/bin/env node

const assert = require('node:assert/strict');
const path = require('node:path');

const outputDirectory = process.argv[2];
if (!outputDirectory) {
  throw new Error('compiled model directory is required');
}

const models = require(path.resolve(outputDirectory, 'AppModels.js'));
const rules = require(path.resolve(outputDirectory, 'AppCatalogRules.js'));

assert.equal(rules.catalogFileName('D:/Games/Demo/Game.exe'), 'Game.exe');
for (const name of ['wineboot', 'wineboot.exe', 'C:\\windows\\system32\\WINEBOOT.EXE',
  '@engine/wineboot', '@engine/explorer', 'wineserver', 'winehua_keep.exe']) {
  assert.equal(rules.isWineInfrastructurePath(name), true, name);
}
for (const name of ['Game.exe', 'C:\\games\\wineboot-game.exe', 'winecfg.exe', 'explorer.exe']) {
  assert.equal(rules.isWineInfrastructurePath(name), false, name);
}

assert.equal(rules.catalogNameWithoutExtension('Game.EXE'), 'Game');
assert.equal(
  rules.chooseExecutablePath('/games/Demo', [
    '/games/Demo/setup.exe',
    '/games/Demo/Demo.exe',
    '/games/Demo/bin/helper.exe'
  ]),
  '/games/Demo/Demo.exe'
);
assert.equal(
  rules.chooseExecutablePath('/games/Demo', [
    '/games/Demo/setup.exe',
    '/games/Demo/crashreporter.exe',
    '/games/Demo/Game.exe'
  ]),
  '/games/Demo/Game.exe'
);
assert.equal(
  rules.chooseExecutablePath('/games/Demo', [
    '/games/Demo/Game.exe',
    '/games/Demo/Launcher.exe'
  ]),
  ''
);
assert.equal(
  rules.chooseExecutablePath('/games/Demo', ['/games/Demo/setup.exe']),
  ''
);
assert.equal(
  rules.chooseExecutablePath('/games/palmod2.0', [
    '/games/palmod2.0/palmod.exe',
    '/games/palmod2.0/launcher.exe'
  ]),
  '/games/palmod2.0/palmod.exe'
);
assert.equal(
  rules.chooseExecutablePath('/games/palmod2.0', [
    '/games/palmod2.0/sdlpal.exe',
    '/games/palmod2.0/pal/Pal.exe',
    '/games/palmod2.0/pal/PALS.EXE'
  ]),
  '/games/palmod2.0/sdlpal.exe'
);
assert.equal(
  rules.chooseCoverFileName(['readme.txt', 'folder.png', 'cover.jpg']),
  'cover.jpg'
);
assert.equal(
  rules.chooseCoverFileName(['folder.webp', 'screenshot.png']),
  'folder.webp'
);
assert.equal(rules.shouldRetainMissingCachedCard(false, false, false), false);
assert.equal(rules.shouldRetainMissingCachedCard(true, false, false), true);
assert.equal(rules.shouldRetainMissingCachedCard(true, false, true), false);

assert.equal(models.normalizeLaunchPath(' C:\\Games\\Demo\\Game.EXE '), 'c:/games/demo/game.exe');
assert.equal(
  models.normalizeCatalogLaunchIdentity('/storage/Users/currentUser/Download/com.vintage.pomelopro/games/PAL/PAL2.EXE'),
  'games/pal/pal2.exe'
);
assert.equal(
  models.normalizeCatalogLaunchIdentity('Z:\\games\\PAL\\PAL2.EXE'),
  'games/pal/pal2.exe'
);
assert.equal(models.normalizeCatalogLaunchIdentity('C:\\Games\\PAL\\PAL2.EXE'), 'c:/games/pal/pal2.exe');
assert.equal(
  models.stableAppId(models.AppSource.DOWNLOAD, 'C:\\Games\\Demo\\Game.EXE'),
  models.stableAppId(models.AppSource.DOWNLOAD, 'c:/games/demo/game.exe')
);
const resolvedRunning = models.resolveRunningAppIds([
  { id: 'session-only', launchTarget: { executable: '/games/session.exe' } },
  { id: 'native-only', launchTarget: { executable: 'C:\\Games\\PAL\\PAL.EXE' } },
  { id: 'desktop', launchTarget: { executable: 'explorer.exe' } },
  { id: 'file-manager', launchTarget: { executable: 'EXPLORER.EXE' } }
], new Set(['session-only']), new Set(['c:/games/pal/pal.exe', 'explorer.exe']));
assert.equal(resolvedRunning.has('session-only'), true);
assert.equal(resolvedRunning.has('native-only'), true);
assert.equal(resolvedRunning.has('desktop'), false);
assert.equal(resolvedRunning.has('file-manager'), false);
assert.equal(
  models.resolveDisplayMode(null, models.DisplayMode.DESKTOP),
  models.DisplayMode.DESKTOP
);
assert.deepEqual(Object.values(models.DisplayMode), ['desktop']);
assert.equal(
  models.resolveLaunchDisplayMode(
    models.LaunchKind.WINE_DESKTOP, null, 'single-app'),
  models.DisplayMode.DESKTOP
);
assert.equal(
  models.resolveLaunchDisplayMode(
    models.LaunchKind.WINE_EXE, models.DisplayMode.DESKTOP, models.DisplayMode.DESKTOP),
  models.DisplayMode.DESKTOP
);
assert.equal(
  models.resolveLaunchDisplayMode(
    models.LaunchKind.WINE_SYSTEM, null, models.DisplayMode.DESKTOP),
  models.DisplayMode.DESKTOP
);
assert.equal(
  models.resolveWinePresentationMode(
    models.LaunchKind.WINE_DESKTOP, models.DisplayMode.DESKTOP, true),
  models.WinePresentationMode.DESKTOP
);
assert.equal(
  models.resolveWinePresentationMode(
    models.LaunchKind.WINE_EXE, models.DisplayMode.DESKTOP, true),
  models.WinePresentationMode.MANAGED_WINDOWS
);
assert.equal(
  models.resolveWinePresentationMode(
    models.LaunchKind.WINE_EXE, models.DisplayMode.DESKTOP, false),
  models.WinePresentationMode.DESKTOP
);
assert.equal(
  models.resolveWinePresentationMode(
    models.LaunchKind.WINE_EXE, models.DisplayMode.DESKTOP, true, true),
  models.WinePresentationMode.DESKTOP
);
assert.equal(
  models.canTransitionEngineState(models.EngineState.STOPPED, models.EngineState.PREPARING),
  true
);
assert.equal(
  models.canTransitionEngineState(models.EngineState.PREPARING, models.EngineState.READY),
  true
);
assert.equal(
  models.canTransitionEngineState(models.EngineState.READY, models.EngineState.PREPARING),
  false
);

console.log('catalog/model unit tests: passed (including legacy display-mode migration and infrastructure filtering)');
