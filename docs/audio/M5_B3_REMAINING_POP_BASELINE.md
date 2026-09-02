# M5 B3 Remaining Pop Baseline

> Date: 2026-09-02
> Purpose: lock the actual B3 Host path before remaining-pop and FPS experiments.

## Identifiers

- branch at lock: `fix/controller-canonical-axis` (investigation continues on `diag/m5-b3-remaining-pop`)
- HEAD / B3 commit: `cde21f8f208248834551c1dfb96c9d5d929b5409`
- message: `fix(audio): mix Wine endpoints in HarmonyOS instead of host S16 clamp`
- Wine gitlink: `11880f1fc6480890f914a512624e2b98cadd7b2f`
- product: `com.vintage.pomelopro` 1.3.4 / 1003004
- parent dirty at lock (not mixed into this work):
  - `docs/controller-hub-capability.md`
  - `entry/src/main/cpp/input/controller/gamepad_bridge.cpp/.h`
  - `thirdparty/wine` (WHGP reconnect + leftover freetype)
  - untracked `docs/audio-m5-b-class-mixer-vs-multi-renderer.md`

Rollback tag: `m5-b3-baseline-cde21f8f`

Device HAP must be confirmed against this commit before treating a combat log as this baseline. Version 1.3.4 was packaged; install SHA must be recorded in the results file after the next flash.

## Class names

B3 does not use `HostRenderStream`.

| Role | Actual type |
| --- | --- |
| Broker | `winehua::AudioBroker` |
| Per-endpoint renderer object | `AudioBroker::AudioRendererSlot` |
| Shared-memory stream | `winehua::AudioStream` |
| Mix format | 48000 / 2ch / S16LE |
| Callback target | `kAudioTargetCallbackFrames` (960, 20 ms) |

## Renderer create vs start

- OPEN (`OpenStream`): create Ring + `OH_AudioRenderer` via `CreateRendererSlotLocked`. Do **not** call `OH_AudioRenderer_Start`.
- START (`StartStream`): `wantStarted=true`, prime until `queued >= 2 * callbackFrames` (need 1920) or 80 ms, then `OH_AudioRenderer_Start` + 3 ms fade-in (`kAudioRampFrames=144`).
- Both endpoints: `USAGE_GAME` + `LATENCY_NORMAL`.

## Logical vs physical started

| Field | Meaning |
| --- | --- |
| `AudioStream::started()` / ring state | Wine logical start. `SetStarted(false)` also `ClearHold()`. |
| `AudioRendererSlot::wantStarted` | Host wants the endpoint audible / priming / fading out. |
| `AudioRendererSlot::rendererStarted` | Physical `OH_AudioRenderer` has been started. |

Empty Ring while logical running: callback returns `AUDIO_DATA_CALLBACK_RESULT_INVALID` after a couple of consecutive underruns without hold. Hold-pad is used on partial underrun. Telemetry is 1 Hz, not per-callback.

## Stop / fade / ClearHold order (actual B3, not the desired product order)

```text
Wine STOP
→ wantStarted = false
→ if rendererStarted: BeginFadeLocked(-1)   // 144 frames, 40 ms timeout
→ AudioStream::SetStarted(false)
    → ring state = STOPPED
    → ClearHold()                            // last PCM hold is discarded here
→ PumpRendererLifecycleLocked
→ lifecycle thread: wait fade remaining==0 or 40 ms
→ OH_AudioRenderer_Stop
→ rendererStarted = false, gain reset to 1
```

Callback during fade-out is allowed only because `fadingOut` is true. If the Ring is already empty, fade ramps **zeros or missing hold**, not the last valid PCM. This is a state-machine order bug, not evidence that 3 ms is too short.

Desired product order (not implemented yet):

```text
logical Stop
→ keep last emitted sample
→ fade last PCM to 0 in callback
→ stopReady
→ control thread OH_AudioRenderer_Stop
→ then ClearHold / Flush
```

## RESET / CLOSE

- RESET: cancel fade, physical Stop, Flush, Ring reset, ClearHold.
- CLOSE: Release renderer.

Interrupt PAUSE/STOP: `rendererStarted=false`, keep `wantStarted`. RESUME re-primes.

## Known telemetry defect at this baseline

`MaxAdjacentDelta` walked interleaved L/R samples, so combat `maxDelta≈50578` is not a time-domain click. Phase 1 replaces this with per-channel L→L / R→R and callback-boundary delta.

## FPS observation (not proven)

User report: pops coincide with FPS drop; mute/disable sound → FPS does not drop.

Do **not** treat that as “clipping causes FPS”. Classify first:

- edge-only gaps around Renderer Start/Stop → lifecycle / P4
- steady low FPS for the whole fight, mute restores FPS → P1 IPC / event / dual callback / shared combat load

`underrun=0` does not prove there is no FPS cost. Ring is ~4096 frames ≈ 85 ms.

## Next experiments (one variable each)

1. G00 (this code, unity gain, bit-exact) combat + FPS / 1% low / frame gaps.
2. Rebuild with `kAudioDiagGainProfile = G11` and repeat the same save/fight/volume.
3. Do not change SRC, Ring size, WASAPI event, Renderer count, or Wine until G00/G11 and GET_STATUS numbers exist.
4. B2 only if remaining issues are Start/Stop edge clicks or edge-only frame gaps.
