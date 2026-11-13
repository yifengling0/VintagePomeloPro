# UE4 HD-2D: Edge map GPU bottleneck investigation

## Current status (2026-11-05)

The user reports approximately 30 FPS with reduced in-game quality and confirms
that the latest occlusion-query candidate no longer flickers. These are later
results than the original measurements below; do not treat the old 4-11 FPS
captures as the current quality setting or a controlled comparison.

The disappearing characters/well reproduced in a small D3D11 occlusion probe.
Waiting for GPU completion alone and using static constants still left 128/384
incorrect results. Splitting each occlusion pass preserved all counts. A query
slot generation-completion guard plus pass isolation passed all 1536 queries.
No forced-visible result is enabled. The generation guard and pass isolation are now managed in legacy DXVK patch
0004. Maleoon devices enable pass isolation by default, with an explicit
DXVK_WINEHUA_SPLIT_OCCLUSION_PASS=0/1 diagnostic override. Normal OHOS and GNU
launch paths already enable command-query-reset generation protection. Patch
0003 retains the tested stage-aware sampler specialization and deferred-state
restore. Fresh x86/ARM64X production builds exclude the experiment shader dumps,
forced-visibility and StageSampler instrumentation. The user verified the prior
candidate DLLs; the new production bytes still need broader GPU regression.

Audio initially failed because the game requests a 12-channel spatial stream
and then requires correctly paced writable events. OHOS Wine now supports
shared multichannel downmix; render ReleaseBuffer no longer immediately signals
another write. The original probe failed after 27 updates in 38 ms with
0x88890006; the fix passed 300 updates in 3068 ms. The actual game produced
nonzero PCM and the user confirmed audible music. A subsequent low-volume
report is addressed by retaining front L/R unity gain and limiting only actual
over-range downmix sums. See `OHOS_SPATIAL_AUDIO.md` for scope and verification.

The original bounded GPU measurements and rejected experiments follow for
reference. See `ue4-hd2d-edge-performance.json` for their identities and hashes.

## Original measurements before in-game quality reduction

MatePad Edge, Maleoon 916B; original OHOS Wine / FEX / legacy DXVK ARM64X.
The existing opening-forest autosave was loaded without advancing the story.
The camera and character remained stationary; foliage animation changes draw
counts slightly. These are settled HUD observations and two-frame GPU timestamp
captures, not long benchmarks with controlled thermal state.

| Route and actual viewport | Observed FPS | Main HDR pass | Heavy full-screen compute |
| --- | ---: | ---: | ---: |
| X11 / Venus, 1560 x 878 | ~4.3 | 105-110 ms | ~39 ms |
| Wayland / native Vulkan, 1560 x 878 | ~5.8 | ~103 ms | ~39 ms |
| Wayland / native Vulkan, 1040 x 585 | ~11.4 | 50.2-50.8 ms | 16.0-16.4 ms |

The smaller viewport is verified in the draw profile, not inferred from launch
arguments. It was obtained by temporarily changing the existing output-scale
setting from 100% to 150%; swapchain size became 1040 x 693. The game rendered the
forest and character correctly. Earlier Engine.ini ScreenPercentage / ResX tests
did **not** change the measured viewport and cannot support a conclusion that
resolution is irrelevant.

The GPU total for two frames fell from 350.6 ms to 181.8 ms. CPU recording of the
main pass was approximately 2-11 ms, substantially less than its GPU execution.
The measured dominant cost is GPU rendering, including material sampling and a
full-screen compute shader, rather than an authentication wait or a stuck EVENT
query. This does not isolate every CPU cost or prove a hardware limit.

Changing X11/Venus to Wayland/native also changes the Vulkan transport. The
4.3-to-5.8 comparison is **not** a pure X11-versus-Wayland compositor benchmark.
The main HDR and compute costs remain expensive with native Vulkan, so transport
and shadow-upload overhead do not explain the whole remaining problem.

On this tiled GPU, timestamps inside a pass concentrate execution time near a
marker. Attribute the approximately 103 ms to the entire 75-77-draw pass, not to
its first draw. The heavy compute identity is
`CS_911c688d855af060cba5d2d63af5223528b30e9f`; no unsupported inference about the
game's named visual effect is made from its hash alone.

## Rejected or inconclusive trials

- Disabling virtual BC did not improve the full-resolution map (~4.3 FPS on
  X11/Venus). Aggregate hardware BC support is false; do not advertise unsupported
  BC features or bypass capability checks to gain performance.
- Disabling custom-border emulation alone retained a correct scene but remained
  around 5-6 FPS on the native route, with roughly the same main-pass and compute
  costs. It is not the dominant residual cost in this scene.
- Combined image samplers showed about 15 FPS but omitted the forest and
  character. That result is invalid as a performance improvement and must not be
  enabled as a release fix.
- Exact-module offline SPIR-V optimization reduced 216 captured modules from
  67.1 MB to 6.6 MB, with Vulkan 1.1 validation before and after. The full-resolution
  X11/Venus map stayed near 4.2-4.3 FPS. Smaller binaries alone do not demonstrate
  faster shader execution.
- Anisotropy configuration trials did not confirm DXVK config-file loading.
  Do not claim that anisotropy was disabled, or derive an FPS comparison from
  those launches. The newly created temporary game-side config was removed.

## Follow-up from the original measurements

At the lower, verified viewport the whole-frame GPU budget is still roughly
90 ms, versus 33.3 ms required for 30 FPS. Resolution alone has not solved this.
Next comparisons should hold scene, resolution, runtime and route constant:

1. Confirm actual sampler filtering and effective quality options before
   measuring them; a requested setting without runtime evidence is insufficient.
2. Isolate material/shader families with bounded diagnostics, retaining complete
   rendering in accepted optimizations. Inspect sample counts, overdraw,
   register pressure and generated machine-code efficiency where tooling permits.
3. Identify the expensive compute operation by its real resources/dispatch and
   compare its implementation or the game's quality options. Do not silently
   omit a dispatch or visual effect as a general compatibility fix.
4. Productionize compatibility-path specialization only after live 0/1/0
   transitions, deferred contexts, state restoration, border and BC regression
   tests. Sparse-ID bool freezing is covered by isolated host tests but has not
   yet been delivered through the managed runtime/lock pipeline.

GPU cost scaling with resolution identifies a useful optimization direction;
it does not establish whether hardware throughput, driver code generation or
compatibility-generated code is ultimately responsible. Those original settings were below the requested playable target; the later
low-quality result is recorded above.
