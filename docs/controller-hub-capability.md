# Controller Hub capability notes

Base: VintagePomeloPro **1.3.3** / canonical coordinate architecture.

## Canonical Controller Space

Hub, WHGP v2, and source adapters share one platform-independent space:

| Control | Range | Positive direction |
| --- | --- | --- |
| Stick X | `[-1, 1]` | right |
| Stick Y | `[-1, 1]` | **up** |
| Hat X | `-1 / 0 / +1` | right |
| Hat Y | `-1 / 0 / +1` | **up** |
| Trigger | `[0, 1]` | pressed |

Source adapters normalize platform raw input. Hub merges only canonical state. WHGP v2 copies that state. `bus_ohos` maps canonical sticks to HID/XInput **without** analog Y inversion (XInput is already +Y=Up). SDL/`bus_sdl` raw-thumb inversion must not be copied onto this sink.

Hat stays a separate output conversion: HID hatswitch helper is +Y=Down, so winebus still uses `whgp_hat_y_to_hid`.

## Game Controller Kit (host)

| Item | Status |
|------|--------|
| Headers (`game_pad.h` / `game_device.h`) | Used via **dlopen** symbols in `game_controller_bridge.cpp` |
| `libohgame_controller.z.so` | Runtime `dlopen`; not statically linked |
| Online/offline | `OH_GameDevice_RegisterDeviceMonitor` → Hub `ResetSource(Physical)` + ArkTS UI |
| ABXY / shoulders / menu / L3R3 / DPad buttons | Mapped OH codes → `LogicalButton` in `physical_gamepad.cpp` |
| Sticks | `NormalizeOhosThumb`: `canonicalY = -rawY` (Kit/SDL: up is negative) |
| Hat | `NormalizeOhosHat`: Kit +Y=Down → Hub +Y=Up |
| Triggers | `NormalizeOhosTrigger` then `SetTrigger` |
| Deadzone | Hub radial inner **0.10** (settings deadzone still used by keyboard_legacy sink) |
| Kit vibration | **None** — Game Controller Kit is input-only |

## Touch overlay

`VirtualInputOverlay` keeps `ny = -dy / radius` (screen +Y is down). `InputDispatcher.setLogicalStick` submits that canonical pair through one `controllerSetStick` call. Do **not** apply a second WHGP Y negate.

## Rumble / haptics

Wine XInput/DInput force-feedback → `winebus` `hid_device_add_haptics` → WHGP `WHGP_MSG_RUMBLE` → host `GamepadBridge` recv loop → ArkTS `@kit.SensorServiceKit` vibrator.

| Item | Status |
|------|--------|
| Target | **External pad motors only** (`!isLocalVibrator`). Never the tablet motor. |
| Dual motor | Two exposed vibrators: low → first, high → second. One vibrator: `max(low,high)`. |
| Intensity | HD pattern (`VibratorPatternBuilder`) when `isHdHapticSupported`; else time vibration. |
| Permission | `ohos.permission.VIBRATE` |

## Wine / duplicate risk

| Item | Status |
|------|--------|
| `bus_sdl` on OHOS | Can enumerate physical pads → **duplicate** with Hub |
| Mitigation | `WINEHUA_CONTROLLER_HUB=1` gates `sdl_add_device` |
| `bus_ohos` | WHGP AF_UNIX v2 → `hid_device_add_gamepad()` + haptics; `is_gamepad=TRUE` (XInput + DInput). Host keeps up to 4 clients (32/64-bit winedevice); a new accept must **not** shut down the previous client. winebus backs off 50ms–1s after a drop. |
| Version mismatch | Log `WHGP protocol mismatch: peer=N expected=2` and disconnect; no v1 guess |
| Env | `WINEHUA_GAMEPAD_ENABLE`, `WINEHUA_GAMEPAD_MODE`, `WINEHUA_GAMEPAD_SOCKET` |

## Architecture

```
Touch Overlay ──NAPI SetStick──┐
  STICK analog + bound keys; D-Pad hat + bound keys; mouse at cursor
Physical Kit ─OHOS adapter──┼─► ControllerHub ─► WHGP v2 sock ─► winebus bus_ohos ─► DInput/XInput
keyboard_legacy ────────────┘ (legacy: GamepadManager → evdev only; Hub off for Wine)
Overlay keys always sendKey; hub buttons/axes are additive, not exclusive.

Wine haptics ──WHGP rumble──► GamepadBridge RecvLoop ──TSFN──► ArkTS vibrator (pad motors)
```

## File map

| Path | Role |
|------|--------|
| `entry/.../cpp/input/controller/*` | Hub, adapters, WHGP server (state + rumble recv), NAPI, Physical feed |
| `entry/.../cpp/input/game_controller_bridge.cpp` | Kit dlopen; dual-feed Hub + ArkTS TSFN including rumble |
| `entry/.../ets/service/GamepadManager.ets` | Overlay / legacy mapping; pad vibrator targeting |
| `thirdparty/wine/dlls/winebus.sys/bus_ohos.c` | Wine virtual gamepad + rumble write |
| `host_tests/controller_merge_test.cpp` | Canonical polarity, atomic SetStick, ownership, WHGP copy |
| `scripts/verify_whgp_protocol.py` | Host/Wine protocol header identity |

## Risks

- Do **not** pass Physical Kit Y into Hub without `NormalizeOhosThumb`. Raw Kit up is negative.
- Do **not** invert analog Y again in `bus_ohos`; that double-flips Physical after the adapter fix.
- Overlay D-pad/stick must not `return` after hub hat/analog — keys and highlight still run.
- Kit hat polarity may differ by pad firmware — verify on tablet.
- Harmony may not expose a gamepad vibrator (`getVibratorInfoSync` empty of `!isLocalVibrator`) — settings shows `震动: 无外接马达`; tablet will not buzz.
- Wine rebuild required after `bus_ohos` changes (`make wine` + `make hap` / `hap-unsigned`).
- Installing that HAP updates Host immediately (`libentry.so`) but Wine still uses the previous `winebus.so` until the **Wine session is restarted**. Host-ok / Wine-dead after a winebus/WHGP change is usually this, not a polarity bug.
- Mode switch needs session restart for Wine env to refresh.
- WHGP v1 peers are rejected; both sides must be rebuilt together.
