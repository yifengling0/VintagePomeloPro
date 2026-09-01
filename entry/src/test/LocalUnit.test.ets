import { describe, it, expect } from '@ohos/hypium';
import { chooseCoverFileName, chooseExecutablePath } from '../main/ets/model/AppCatalogRules';
import {
  AppSource,
  ControlBarDirection,
  DEFAULT_GAMEPAD_LAYOUT,
  DEFAULT_VIRTUAL_KEY_CONFIG,
  DEFAULT_VIRTUAL_MOUSE_CONFIG,
  DisplayMode,
  EngineState,
  LaunchKind,
  ViewMode,
  WineLanguage,
  WinePresentationMode,
  RightJoystickOutputMode,
  VirtualGamepadLayout,
  VirtualKey,
  VirtualKeyConfig,
  canTransitionEngineState,
  cloneGamepadLayout,
  cloneVirtualKeyConfig,
  cloneVirtualMouseConfig,
  createDefaultGlobalSettings,
  createDefaultInputProfile,
  normalizeLaunchPath,
  resolveDisplayMode,
  resolveLaunchDisplayMode,
  resolveWinePresentationMode,
  resolveInputProfileId,
  resolveVirtualJoystickDirection,
  resolveVirtualKeyMapping,
  stableAppId
} from '../main/ets/model/AppModels';
import { InputStateTracker } from '../main/ets/service/InputStateTracker';
import { InputEventGate } from '../main/ets/service/InputEventGate';

export default function localUnitTest() {
  describe('WineEngineApplicationArchitecture', () => {
    it('selectsDirectoryNamedExecutableFirst', 0, () => {
      const chosen = chooseExecutablePath('/games/Foo Game', [
        '/games/Foo Game/setup.exe',
        '/games/Foo Game/FooGame.exe',
        '/games/Foo Game/tools.exe'
      ]);
      expect(chosen).assertEqual('/games/Foo Game/FooGame.exe');
    });

    it('rejectsAmbiguousExecutableSelection', 0, () => {
      const chosen = chooseExecutablePath('/games/Unknown', [
        '/games/Unknown/client.exe', '/games/Unknown/launcher.exe'
      ]);
      expect(chosen).assertEqual('');
    });

    it('excludesInstallerWhenOnlyOneRunnableCandidateRemains', 0, () => {
      const chosen = chooseExecutablePath('/games/Unknown', [
        '/games/Unknown/setup.exe', '/games/Unknown/play.exe'
      ]);
      expect(chosen).assertEqual('/games/Unknown/play.exe');
    });

    it('doesNotSelectInstallerAsTheMainExecutable', 0, () => {
      expect(chooseExecutablePath('/games/SetupOnly', ['/games/SetupOnly/setup.exe'])).assertEqual('');
    });

    it('matchesVersionedDirectoryToStableExecutableName', 0, () => {
      const chosen = chooseExecutablePath('/games/palmod2.0', [
        '/games/palmod2.0/palmod.exe', '/games/palmod2.0/launcher.exe'
      ]);
      expect(chosen).assertEqual('/games/palmod2.0/palmod.exe');
    });

    it('prefersTheOnlyRootExecutableOverNestedTools', 0, () => {
      const chosen = chooseExecutablePath('/games/palmod2.0', [
        '/games/palmod2.0/sdlpal.exe',
        '/games/palmod2.0/pal/Pal.exe',
        '/games/palmod2.0/pal/PALS.EXE'
      ]);
      expect(chosen).assertEqual('/games/palmod2.0/sdlpal.exe');
    });

    it('usesCoverBeforeFolderArt', 0, () => {
      expect(chooseCoverFileName(['folder.png', 'Cover.JPG'])).assertEqual('Cover.JPG');
    });

    it('normalizesPathForSingletonAndStableId', 0, () => {
      expect(normalizeLaunchPath(' C:\\Games\\Foo\\FOO.EXE ')).assertEqual('c:/games/foo/foo.exe');
      expect(stableAppId(AppSource.DOWNLOAD, 'C:\\Games\\Foo.exe'))
        .assertEqual(stableAppId(AppSource.DOWNLOAD, 'c:/games/foo.exe'));
    });

    it('resolvesPerAppDisplayModeOverride', 0, () => {
      expect(resolveDisplayMode(null, DisplayMode.DESKTOP)).assertEqual(DisplayMode.DESKTOP);
      expect(resolveDisplayMode(DisplayMode.SINGLE_APP, DisplayMode.DESKTOP)).assertEqual(DisplayMode.SINGLE_APP);
    });

    it('keepsDesktopAsTheUserDefaultOnEveryDevice', 0, () => {
      expect(resolveLaunchDisplayMode(LaunchKind.WINE_DESKTOP, null,
        DisplayMode.SINGLE_APP)).assertEqual(DisplayMode.DESKTOP);
      expect(resolveLaunchDisplayMode(LaunchKind.WINE_EXE, DisplayMode.DESKTOP,
        DisplayMode.DESKTOP)).assertEqual(DisplayMode.DESKTOP);
      expect(resolveLaunchDisplayMode(LaunchKind.WINE_SYSTEM, null,
        DisplayMode.DESKTOP)).assertEqual(DisplayMode.DESKTOP);
    });

    it('mapsPcDesktopModeToManagedWineWindows', 0, () => {
      expect(resolveWinePresentationMode(LaunchKind.WINE_DESKTOP,
        DisplayMode.DESKTOP, true)).assertEqual(WinePresentationMode.DESKTOP);
      expect(resolveWinePresentationMode(LaunchKind.WINE_EXE,
        DisplayMode.DESKTOP, true)).assertEqual(WinePresentationMode.MANAGED_WINDOWS);
      expect(resolveWinePresentationMode(LaunchKind.WINE_EXE,
        DisplayMode.DESKTOP, false)).assertEqual(WinePresentationMode.DESKTOP);
      expect(resolveWinePresentationMode(LaunchKind.WINE_EXE,
        DisplayMode.SINGLE_APP, true)).assertEqual(WinePresentationMode.SINGLE_APP);
    });

    it('validatesEngineStateMachine', 0, () => {
      expect(canTransitionEngineState(EngineState.STOPPED, EngineState.PREPARING)).assertTrue();
      expect(canTransitionEngineState(EngineState.PREPARING, EngineState.READY)).assertTrue();
      expect(canTransitionEngineState(EngineState.PREPARING, EngineState.SWITCHING)).assertTrue();
      expect(canTransitionEngineState(EngineState.STOPPED, EngineState.READY)).assertFalse();
    });

    it('placesShoulderControlsOnTheirPhysicalControllerSides', 0, () => {
      const layout = createDefaultInputProfile().layoutLandscape;
      expect(layout.leftTrigger.centerX < 0.5).assertTrue();
      expect(layout.leftBumper.centerX < 0.5).assertTrue();
      expect(layout.rightBumper.centerX > 0.5).assertTrue();
      expect(layout.rightTrigger.centerX > 0.5).assertTrue();
      expect(layout.leftTrigger.centerY < layout.direction.centerY).assertTrue();
      expect(layout.rightTrigger.centerY < layout.action.centerY).assertTrue();
    });

    it('placesIndependentRightJoystickBesideTheActionButtons', 0, () => {
      const profile = createDefaultInputProfile();
      const layout = profile.layoutLandscape;
      expect(profile.virtualKeyConfig.showRightJoystick).assertTrue();
      expect(profile.virtualKeyConfig.rightJoystickSize).assertEqual(100);
      expect(profile.virtualKeyConfig.rightJoystickDeadzone).assertEqual(12);
      expect(profile.virtualKeyConfig.rightJoystickSensitivity).assertEqual(100);
      expect(profile.virtualKeyConfig.rightJoystickOutputMode).assertEqual(RightJoystickOutputMode.MOUSE);
      expect(resolveVirtualKeyMapping(profile.virtualKeyMappings, VirtualKey.RIGHT_STICK_UP, 0)).assertEqual(23);
      expect(resolveVirtualKeyMapping(profile.virtualKeyMappings, VirtualKey.RIGHT_STICK_DOWN, 0)).assertEqual(37);
      expect(resolveVirtualKeyMapping(profile.virtualKeyMappings, VirtualKey.RIGHT_STICK_LEFT, 0)).assertEqual(36);
      expect(resolveVirtualKeyMapping(profile.virtualKeyMappings, VirtualKey.RIGHT_STICK_RIGHT, 0)).assertEqual(38);
      expect(layout.camera.centerX > 0.5).assertTrue();
      expect(layout.camera.centerX < layout.action.centerX).assertTrue();
      expect(layout.camera.centerY > layout.action.centerY).assertTrue();
    });

    it('migratesSavedProfilesWithRightJoystickDefaults', 0, () => {
      const legacyConfig = cloneVirtualKeyConfig(JSON.parse('{"transparency":40,"joystickTransparency":50,' +
        '"joystickSize":175,"buttonSize":100,"showButtonA":true,"showButtonB":true,"showButtonX":true,' +
        '"showButtonY":true,"showButtonL":true,"showButtonR":true,"showButtonMenu":true,' +
        '"showButtonShift":true,"showButtonCtrl":true,"joystickDeadzone":15,"eightDirectionJoystick":true,' +
        '"dpadMode":"dpad","colorScheme":"default"}') as VirtualKeyConfig);
      const legacyLayout = cloneGamepadLayout(JSON.parse('{"direction":{"centerX":0.13,"centerY":0.7},' +
        '"leftTrigger":{"centerX":0.075,"centerY":0.45},"leftBumper":{"centerX":0.16,"centerY":0.45},' +
        '"action":{"centerX":0.88,"centerY":0.65},"rightBumper":{"centerX":0.84,"centerY":0.45},' +
        '"rightTrigger":{"centerX":0.925,"centerY":0.45},"aux":{"centerX":0.5,"centerY":0.1}}') as VirtualGamepadLayout);
      expect(legacyConfig.showRightJoystick).assertTrue();
      expect(legacyConfig.rightJoystickSize).assertEqual(175);
      expect(legacyConfig.rightJoystickSensitivity).assertEqual(100);
      expect(legacyConfig.rightJoystickOutputMode).assertEqual(RightJoystickOutputMode.MOUSE);
      expect(legacyLayout.camera.centerX).assertEqual(DEFAULT_GAMEPAD_LAYOUT.camera.centerX);
      legacyLayout.camera.centerX = 0.62;
      expect(DEFAULT_GAMEPAD_LAYOUT.camera.centerX).assertEqual(0.70);
    });

    it('usesVintageControlBarAndFullScreenMouseDefaults', 0, () => {
      const settings = createDefaultGlobalSettings();
      expect(settings.virtualInputEnabled).assertTrue();
      expect(settings.pageBackgroundUri).assertEqual('');
      expect(settings.controlBarDirection).assertEqual(ControlBarDirection.AUTO);
      expect(settings.virtualMouseConfig.mouseSensitivity).assertEqual(100);
      expect(settings.virtualMouseConfig.doubleClickInterval).assertEqual(100);
      expect(settings.virtualMouseConfig.showCursor).assertTrue();
      expect(settings.viewMode).assertEqual(ViewMode.GRID);
      expect(ViewMode.FILES).assertEqual('files');
      expect(settings.preserveUserDataOnReset).assertTrue();
      expect(settings.showFpsHud).assertFalse();
      expect(settings.wineLanguage).assertEqual(WineLanguage.CHINESE);
    });

    it('clonesVirtualMouseSettingsWithoutSharingMutableState', 0, () => {
      const cloned = cloneVirtualMouseConfig(DEFAULT_VIRTUAL_MOUSE_CONFIG);
      cloned.mouseSensitivity = 180;
      expect(DEFAULT_VIRTUAL_MOUSE_CONFIG.mouseSensitivity).assertEqual(100);
      expect(cloned.mouseSensitivity).assertEqual(180);
    });

    it('resolvesDesktopAndPerGameInputProfilesExplicitly', 0, () => {
      expect(resolveInputProfileId('default', 'desktop-custom')).assertEqual('desktop-custom');
      expect(resolveInputProfileId('app-game', 'desktop-custom')).assertEqual('app-game');
      expect(resolveInputProfileId('', '')).assertEqual('default');
    });

    it('appliesEditedVirtualKeyMappingAtRuntime', 0, () => {
      const profile = createDefaultInputProfile();
      const changed = profile.virtualKeyMappings.map((mapping) => mapping.virtualKey === VirtualKey.BUTTON_A ?
        { virtualKey: mapping.virtualKey, evdevCode: 28, keyLabel: 'Enter' } : mapping);
      expect(resolveVirtualKeyMapping(changed, VirtualKey.BUTTON_A, 57)).assertEqual(28);
    });

    it('quantizesVirtualJoystickDeadzoneAndEightDirections', 0, () => {
      const centered = resolveVirtualJoystickDirection(8, 0, 100, 15, true);
      expect(centered.up || centered.down || centered.left || centered.right).assertFalse();
      const diagonal = resolveVirtualJoystickDirection(70, -70, 100, 15, true);
      expect(diagonal.up).assertTrue();
      expect(diagonal.right).assertTrue();
      expect(diagonal.down || diagonal.left).assertFalse();
      const upLeft = resolveVirtualJoystickDirection(-70, -70, 100, 15, true);
      expect(upLeft.up).assertTrue();
      expect(upLeft.left).assertTrue();
      expect(upLeft.down || upLeft.right).assertFalse();
      const up = resolveVirtualJoystickDirection(0, -70, 100, 15, true);
      expect(up.up).assertTrue();
      expect(up.down || up.left || up.right).assertFalse();
      const left = resolveVirtualJoystickDirection(-70, 0, 100, 15, true);
      expect(left.left).assertTrue();
      expect(left.up || left.down || left.right).assertFalse();
      const fourWay = resolveVirtualJoystickDirection(70, -25, 100, 15, false);
      expect(fourWay.right).assertTrue();
      expect(fourWay.up).assertFalse();
    });

    it('keepsSharedVirtualKeyPressedUntilEverySourceReleases', 0, () => {
      const tracker = new InputStateTracker();
      expect(tracker.pressKey('touch-a', 28)).assertTrue();
      expect(tracker.pressKey('gamepad-a', 28)).assertFalse();
      expect(tracker.releaseKey('touch-a', 28)).assertFalse();
      expect(tracker.releaseKey('gamepad-a', 28)).assertTrue();
      expect(tracker.snapshot().heldKeys).assertEqual(0);
    });

    it('releasesPointersKeysAndButtonsOnSessionCancellation', 0, () => {
      const tracker = new InputStateTracker();
      tracker.markPointer('pointer-1');
      tracker.pressKey('pointer-1', 57);
      tracker.pressButton('pointer-1', 272);
      const released = tracker.releaseAll();
      expect(released.keys[0]).assertEqual(57);
      expect(released.buttons[0]).assertEqual(272);
      expect(tracker.snapshot().activePointers).assertEqual(0);
      expect(tracker.snapshot().heldKeys).assertEqual(0);
      expect(tracker.snapshot().heldButtons).assertEqual(0);
    });

    it('routesOnePhysicalActionThroughOnlyOnePointerChannel', 0, () => {
      const gate = new InputEventGate(320);
      expect(gate.shouldHandleTouch(2, 1, 1000)).assertTrue();
      expect(gate.shouldHandleMouse(2, 1, 1001)).assertFalse();
      expect(gate.shouldHandleTouch(1, 3, 2000)).assertFalse();
      expect(gate.shouldHandleMouse(1, 3, 2000)).assertTrue();
    });

    it('suppressesUnknownSyntheticMouseAfterTouch', 0, () => {
      const gate = new InputEventGate(320);
      expect(gate.shouldHandleTouch(0, 0, 1000)).assertTrue();
      expect(gate.shouldHandleMouse(0, 0, 1200)).assertFalse();
      expect(gate.shouldHandleMouse(0, 0, 1400)).assertTrue();
    });
  });
}
