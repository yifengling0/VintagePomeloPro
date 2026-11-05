# OHOS Wine spatial audio: silent UE4 game

## Failure and evidence

The game uses `ISpatialAudioObjectRenderStream` with 12 static channels. A system
renderer marked running is insufficient evidence of audible sound.

1. The driver previously rejected more than two channels (0x88890008).
2. Accepting/downmixing the channels exposed a second defect: render
   `ReleaseBuffer` immediately signaled the event again, while the timer also
   signaled. A spatial client writes a fixed period on every event, so it filled
   the ring far faster than playback consumed it, failed with 0x88890006, and
   repeatedly recreated its stream. The game had only submitted silence.
3. With events paced by the timer and gated on at least one writable period,
   the stream stayed alive and produced real PCM. The user confirmed music.

The Edge spatial probe failed at 27 updates, 12960 frames and 38 ms before the
fix; it passed 300 updates, 144000 frames and 3068 ms afterward. The ordinary
12-channel WASAPI probe also passed. Diagnostic output showed nonzero game PCM
on the spatial stream; the ordinary stereo stream could legitimately be silent.

## Fix scope

- `0080`: shared render formats use up to 18 standard speaker channels, with
  validated masks and downmix to the existing stereo broker. Capture remains
  mono/stereo. GetMixFormat remains the established stereo Float32 contract.
- `0081`: remove render self-notification after ReleaseBuffer. Start and timer
  notifications remain, checking space for a full client period from the shared
  ring, without adding a synchronous broker status query in the timer.
- Multichannel front L/R keep unity gain. The initial worst-case fixed headroom
  lowered stereo content inside a 12-channel stream by about 14 dB, matching the
  user's low-volume report. Linked peak limiting now acts only if an actual
  mixed sample exceeds full scale; it preserves L/R balance and leaves in-range
  content unchanged. No global system-volume override or game-name rule is used.

Both patches are registered in `build_wine.sh` and the graphics source lock.
The real API probes are in `tests/runtime/spatial_audio`; the downmix contract
is exercised by `make test-wine-audio-downmix` (also in `make test`). Host tests
cover masks, all speaker routes, stereo unity gain and overload limiting.
The actual game speaker result and loudness are device checks, not host tests.
The user confirmed audible music and normal volume on Edge. Slight distortion
was not excluded; this round does not claim a perceptually transparent limiter.

The installed audio candidate uses bounded opt-in PCM diagnostics. They are not
part of the managed source patches. No saves, game files, or Steam accounts are
part of the fix. The simultaneous occlusion-query fix is managed separately in the legacy DXVK
overlay; it is not an audio dependency. The packaged audio library is rebuilt
from the managed source without the opt-in PCM diagnostic instrumentation.
