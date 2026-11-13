# Legacy and Modern DXVK BC runtime candidate, 2026-10-17

Both DXVK source trees now implement an oversized sampled Texture2D subset without
changing game files, logical dimensions, feature level, or recompressing BC data.
This is a bounded runtime candidate packaged in a signed debug HAP. It is not
a formal release or whole-game qualification. The final checkpoint below
supersedes the historical isolation results and installed-state notes.
Modern 2.6.2 carries the same lossless storage policy, adapted to its image,
buffer, sampler and command-stream interfaces. Existing Steam, geometry and
noncoherent mapped-memory fixes are retained.

## Capability selection

Selection uses the active Vulkan adapter and the resource's actual format,
optimal tiling, sampled/transfer usage, mutable-format flags, extent and mip
count. A supported native image retains the existing path. If native BC is
unavailable, ordinary small supported images keep the existing RGBA decode.
Large decoded images can select lossless BC-word storage to bound expansion.
GPU names or a Maleoon generation number are never used to force a size limit.

A BC1/BC2/BC3 resource in the supported subset takes the new path when the
queried decoded extent/mip limit is exceeded, or its estimated RGBA8 mip-chain
payload reaches the residency threshold. The default threshold is 64 MiB;
WINEHUA_DXVK_BC_RGBA_LIMIT_MB=0 disables only the residency policy. Native BC
images retain their path regardless of this threshold. Logs identify forced,
extent or rgba-residency selection. This threshold is a per-image heuristic,
not an aggregate GPU memory limit or an eviction implementation.

A Maleoon 920 route that genuinely supports 16384-wide decoded images is not
clamped to 8192. It can still use lossless storage for a large unsupported BC
image because of memory cost; that is independent of the size capability.

 The physical RGBA16_UNORM backing is checked independently for supported
extent, usage and maximum resource size. The shader metadata buffer must fit
maxUniformBufferRange; devices without this capacity keep existing behavior.
The compact sampler-only buffer remains in use when virtual BC is disabled.

WINEHUA_DXVK_VIRTUAL_BC=0 disables the new path. The default queries capabilities.
Value 1 forces virtual storage on otherwise capable adapters for qualification;
the app must not set 1 as a product default. Test runs on the tablet did not
force this variable: its actual 8K route selected compatibility automatically.

## Storage and semantics

The atlas stores original BC words, two uint32 words per RGBA16_UNORM texel.
Conversion through normalized 16-bit channels is exact. All original mip data
is packed into one physical mip, with separate metadata for every SRV's logical
mip range and sRGB interpretation. Shaders fetch words, decode the original BC
block, apply sRGB conversion before filtering, and implement logical sampling.
There is no second lossy codec or texture resizing.

For a single-mip 14360x4724 BC3 image on the tested 8K route:

| Storage | Bytes |
| --- | ---: |
| Original BC3 blocks | 67,836,640 |
| Physical 8192x1036 atlas | 67,895,296 |
| Fully decoded RGBA8 pixels | 271,346,560 |

These are texture payload estimates, not total application residency. CPU source
data, initialization packing, upload staging, metadata, GPU allocation overhead
and retired submissions add to the working set. Initialization still packs a
full atlas; bounded streaming and residency policy are separate work.

Supported resource subset: one-layer, single-sample Texture2D; DEFAULT or
IMMUTABLE usage; shader-resource binding only; no CPU access, sharing, cube or
other MiscFlags. BC typeless/UNORM/sRGB SRVs in the same family are supported.
BC staging buffers keep the original compressed layout for transfers.

Implemented operations include dimensions/mip queries, Load, Sample,
SampleLevel, SampleBias, SampleGrad, Gather, integer offsets, all five D3D
address modes, point/bilinear/trilinear and bounded anisotropic integration.
UpdateSubresource, virtual-to-virtual copies and virtual-to/from BC staging
operate on the original BC bytes. ClearState, descriptor replacement and
deferred command lists carry or reset the metadata.

The first GPU regression exposed negative signed remainder during wrap/mirror
addressing. Vulkan does not guarantee the required result for negative signed
modulo operands. The shader now computes modulo using unsigned magnitudes and
restores the mathematical wrapped coordinate. Boundary cases remain in the
end-to-end suite.

## Validation

Source base: Legacy 7c5cc47f772b9564af177cea920da81a40ffd47c in the authoritative
vpp-account-cloud checkout. Host layout validation covers a complete 16384x16384
BC1/2/3 mip chain in an 8K physical atlas, odd dimensions, row pitch, mip tails,
SRV ranges and capacity rejection.

tests/runtime/d3d11_virtual_bc_probe.cpp uses Microsoft D3DCompiler-produced
DXBC, the actual DXVK compiler, resource bindings, real GPU execution and
readback. Expected decoded pixels come from Wine bcdec, with independent CPU
address/filter calculations. The --compile mode generates compute, vertex and
pixel DXBC. --dump translates all three without dispatching; all actual host
and tablet SPIR-V modules passed spirv-val --target-env vulkan1.1 before dispatch.

| Run | Result |
| --- | --- |
| x64 Windows, RTX 2080 Ti, forced virtual storage | 69,120 compute + 368,640 raster samples; zero failures |
| ARM64X Legacy, USB MatePad Mini, Maleoon 910 8K route, automatic selection | Same 437,760 samples; zero failures |
| Compressed staging read/write, pitched subregion update, full-resource copy, same-resource subregion copy | Six format/color-space runs passed byte comparison and subsequent sampling |
| Regular Texture2D to virtual BC SRV switch, ClearState and deferred draw/restore | Passed in raster runs |
| 16384-wide BC1/2/3 with 15 logical mips | Native host creation and automatic tablet compatibility creation passed |
| Capable host automatic mode versus explicitly disabled virtual BC | Native readback files byte-identical |

The sample fixture is 14360x4724 with 14 mips. Each BC1/2/3 UNORM/sRGB
combination tests two SRV mip origins, four filters and five address modes.
Each combination has 11,520 compute and 61,440 raster comparisons. Tolerance is
0.0003 per channel; the final tablet compute maximum was at most 0.00000060.
Raster results passed the tolerance; their maximum is not separately reported.
Gradient nearest-mip fixtures avoid exact half-level ties after floating-point
log2; explicit SampleLevel fixtures retain half-level coverage.

Native BC hardware has decoder rounding/filter behavior that differs from
Wine's CPU decoder. Native qualification therefore compares the two actual
native GPU captures, rather than relaxing the virtual-path CPU tolerance.
The host native capture SHA256 is
3d0a80db57cf136b4669d89428c76d2ac1cf31dfd6beabc5620f60bd21461ab0
for both modes and both DXVK versions. Phone qualification is separate below.

## Original-game and phone evidence added in this round

The original five modified bundles plus Addressables catalog were restored as a
complete set and read back. All six SHA256 values match the original manifest.
No game assembly or save file was edited. The previous 2K resource experiment
was backed up separately; it is not the resource set currently running.

With only extent compatibility, the original game still failed: at 08:22:13 the
system explicitly killed the bundle with ResourceLeak:Gpu Leak and recorded
4,611,128 KiB (about 4.4 GiB) GPU use. This establishes a system GPU-resource
kill, not an unhandled BC creation exception or proof of a resource-release bug.
See original-regression-20261017/confirmed-gpu-kill.txt.

Forcing all eligible BC images into lossless storage entered the existing indoor
save with approximately 2.1 GiB GPU use, but menu performance was only about
9-13 FPS. The automatic 64 MiB policy preserves the small-image path. It entered
the original indoor save, showed about 28-41 FPS in captured scene frames and
about 62 FPS at the menu. Five minutes of bounded samples on the same game PID
30537 measured 3,114,220,282-3,115,655,930 bytes (about 2.9 GiB) GPU use, without
another recorded GPU-resource kill in that interval. This is bounded scene
survival, not repeated scene-change or long-term gameplay qualification.

Phone: HUAWEI Mate 80 VYG-AL00, Maleoon 920 through Virtio-GPU Venus. Queried
16K capability is retained. The earlier Modern forced-path run passed all
437,760 samples, six compressed transfer cases and three actual SPIR-V modules
(vulkan1.3 validation). With the 64 MiB default candidate:

- First automatic run completed all sampling but found 1,181 differing BC2 sRGB
  mip-0 staging rows. Sampling after restoring original staging data passed.
- Two subsequent complete automatic runs passed all sampling and transfers.
  Added diagnostics record the first differing byte and mapped-memory traces.
  A separate transfer-only run also passed all six formats.
- A six-round same-process test was interrupted during round 2. Device logs
  explicitly record LowMemoryKill at 08:42:56 with isForeground=0 after another
  test application took foreground. It has no FINAL result and is not a pass.
- The isolated earlier ordinary decoded-BC staging path also produced Venus
  SIGABRT failures. This path cannot reconstruct original BC bytes from its
  decoded image; do not describe it as a qualified transfer path.

At this historical checkpoint the intermittent first-run mismatch was open.
Subsequent ring/export ordering isolation and final regression are recorded below. Actual
phone Steam/game-card regression with this exact candidate remains pending.

## Later copy-coalescing and device investigation checkpoint

Both implementations now coalesce contiguous complete BC block rows into atlas
rectangles; padded and partial regions keep the stride-aware path. The new x64
Legacy and Modern DLLs each passed all 437,760 sampling comparisons and transfer
checks, with zero failures. Native automatic/off captures remain byte-identical.
The new Legacy ARM64X pair passed the complete tablet GPU fixture at 09:44:59.
Evidence: artifacts/dxvk-texture-compat-20261017/coalesced-final.

Before that replacement, the 64 MiB automatic Legacy pair also passed original
save loading, held-direction movement, indoor-to-outdoor transition, full
outdoor map display, outdoor movement and return indoors. The game process
64970 measured 2,835,196,810 GPU bytes after the round trip. Screenshots show
about 8-10 FPS outdoors and 25-40 FPS indoors; this is functional coverage, not
a performance or long-term stability qualification. The later coalesced build was subsequently retested with the original game:
save loading, held-direction movement, indoor-to-outdoor transition, map display,
outdoor movement, return indoors and continued movement all passed. Process
65134 measured 3,171,068,946 GPU used bytes after the round trip and
3,240,760,722 bytes at the later indoor sample (allocated 3,255,242,752).
Screenshots show approximately 5-8 FPS outdoors and 25-41 FPS indoors. This
qualifies the bounded functional sequence only; residency and outdoor performance
remain limitations. Evidence: unity-xxdxtrwzt-tablet-20261016/
coalesced-original-game-result.json and coalesced-*.jpeg.

Modern remains unqualified on the phone. Same-process transfer repetitions
again grew GPU allocation until system intervention, including a confirmed
09:37:08 GPU_THRESHOLD_KILLER event with FOREGROUND=1. A later 09:48:08
ResourceLeak:Gpu Leak event occurred with FOREGROUND=0. Neither event is
misreported as a native exception or a completed conformance run.

One actual allocator defect was corrected: a private cached VkBuffer could
overwrite an already-owned global VkBuffer handle. The old owned handle is now
destroyed before replacement, while shared chunk buffers remain pool-owned.
This correction alone did not stop the phone growth. Host diagnostic logs
show large buffer destroy and memory free requests arriving, and zero remaining
tracked buffer bindings at the recorded memory-free boundaries. These logs do
not prove immediate physical reclamation by the driver.

Guest and host command-pool RELEASE_RESOURCES experiments did not fix the
growth. A temporary device-idle-before-large-free experiment still observed
1,181 differing BC2 sRGB rows, entered repetition 2 with GPU used bytes near
2 GB and allocated bytes near 3 GB, and was deliberately stopped after the
failure was established. It has no FINAL result. The device-idle-before-free
code was removed and Modern x64 and ARM64X rebuilt successfully.

A separate diagnostic host fixture directly allocated and freed 96 MiB
Vulkan buffers 32 times, half with mapping, CPU writes and explicit flush.
It completed at 10:01:14, returning used bytes to 8,153,122 and allocated bytes
to 8,630,272 before guest workload creation. This rules out unconditional
growth in that isolated basic allocation sequence; actual transfer and image
work need separate isolation. Evidence: phone-modern/native-allocation-baseline-*
and wait-idle-free-*. A sandbox-loaded helper was rejected by the loader and
did not execute; it is not a test result.

Diagnostic host HAPs replace only the two packaged virglrenderer library
members of the verified 20:25:51 baseline, with all other payload bytes checked
and debug profile/package signatures verified. They are investigation artifacts,
not release integration. Native diagnostic edits are confined to a preparation
tree and must be removed before final packaging. The installed-state table
below describes the earlier checkpoint; later diagnostic identities and final
restoration must be recorded separately.

## Earlier artifacts and installed state

Local artifact root:
F:/VintagePomelo-Workspace/artifacts/dxvk-texture-compat-20261017/residency-64m

| ARM64X DLL | SHA256 |
| --- | --- |
| Legacy d3d11.dll | daa6ac175d13f9ed71388a86174e99be6bd308357de281de954703e31326d1f7 |
| Legacy dxgi.dll | 47684e5920b70aa6f1dfb21a16d5232e98d92375edb449b47ac30a5510631b1f |
| Modern d3d11.dll | f499015d94c50beeeab4850554fa8ac963194ccc9984959442e6f7cf1fcc267c |
| Modern dxgi.dll | 014908fe541682c4f3677bb8e578f7927559888f058c670fa8fb4c15435c1f6e |

host-results.json records both versions' full forced-path conformance and
native auto/off binary comparisons. dll-hashes.json records all host and
ARM64X artifacts. Both versions built successfully for x64 and ARM64X. The
probe executable and three precompiled DXBC modules accompany the artifacts.

At this checkpoint the tablet has the Legacy candidate installed in
wine/dxvk/legacy/arm64x, and the phone has the Modern candidate installed in
wine/dxvk/modern-2.6/arm64x. Both d3d11 readbacks match the hashes above. No HAP
or release runtime manifest was changed. Original DLL pairs remain in the
private device-evidence folders for rollback. Device installation is an
experimental test state, not release-package integration.

Tablet records are under artifacts/unity-xxdxtrwzt-tablet-20261016, including
original-regression-20261017/readback, the original/all-BC/automatic monitoring
runs, screenshots and command ledger. Phone records are under
artifacts/dxvk-texture-compat-20261017/phone-modern. Raw screenshots/logs remain
private local evidence; do not attach unrelated application screens publicly.

## Remaining acceptance boundaries

This does not implement oversized ordinary RGBA textures, BC4/5/6/7, arrays,
cubes, UAV/render-target resources, CPU-mapped sampled resources, or shared
images. Copies between virtual storage and an ordinary decoded BC texture are
explicitly rejected; they require a separate conversion path. BC staging buffers
do not implement decoded-image readback/re-encoding.

The phone transfer mismatch was repaired by the ring/export ordering change
below. General release still requires Steam/game regression and
foreground/background budgets to be qualified. It does not provide
complete FL11 resource support or a general memory-eviction system. Optional
ASTC/ETC2 recompression remains separate and default-off.


## 10:48 phone transfer isolation checkpoint

The 96 MiB native Vulkan fixture completed all 24 buffer/image transfer rounds
on the same logical VkDevice used immediately afterwards by the guest fixture.
Every native round passed and the fixture ended with NATIVE-ISOLATION FINAL
success. The 4 MiB ARM64EC guest fixture on that device failed all three phases,
reading the previous native pattern 0x12345627. This separates a reproducible
Venus-path failure from a blanket lack of native Vulkan transfer support.

Independent guest runs with timeline semaphores enabled, ARM64EC and x64,
QueueSubmit and QueueSubmit2, explicit device masks, dedicated allocations and
no command batching all failed. CPU input and update payload checks were correct.
A boundary diagnostic confirmed the expected source word in the host shared
shadow, successful native command begin/end, matching host command-buffer
identities at submission, and wrong host-mapped output after real queue idle.
The CPU-upload profile preserved source data but still returned a zero target.
None of these negative experiments is a production workaround. The earlier
96 MiB three-phase run interrupted in background had no FINAL and is excluded
from completed results.

Evidence under phone-modern includes native-and-guest-same-device-4m-*,
guest-boundary-trace-4m-*, boundary-cpu-trace-4m-*, submit-actual-4m-*,
explicit-device-mask-4m-*, dedicated-bind-4m-* and no-command-batching-4m-*.
The temporary diagnostic library is not a release artifact. Resource binding
is being isolated next; exact restoration and final candidate identity must be
recorded before handoff. No new game resource or assembly modifications were made.


The plain BindMemory2-to-BindMemory control failed as well and was removed.
Deferring the destination's first map until after queue idle passed all three
4 MiB destination comparisons. However, the already-mapped source still read
zero after invalidate; the fixture was strengthened to count that as a failure.
This is evidence for a visibility difference associated with an existing map,
not full conformance. Remapping at the flush/invalidate boundary did not repair
the premapped fixture and was removed. The next isolated test moves the first
host map to the resource-binding ring thread; it is not a production change.
Evidence: bind1-route-4m-*, lazy-readback-map-4m-* and remap-premapped-4m-*.


First-map-on-ring was invalidated by a race with blob export: the second native
map returned VK_ERROR_MEMORY_MAP_FAILED and the client disconnected. It had no
FINAL and is not a completed conformance result. An explicit AArch64 cache
maintenance diagnostic completed but still failed the premapped 4 MiB fixture.
Both experiments were removed. The subsequent cached-memory-type control also failed and was removed;
native host allocation flags remain unchanged. No memory
policy change has been accepted for production from these controls.

## Final ring/export repair and packaged candidate

The Vulkan allocation runs on the primary ring, while WineHua exports/maps the
native shadow through the vtest thread. Waiting only for allocation completion
did not order the ring's resource operations before this first export.
`patches/mesa/0009-venus-order-shadow-export-after-ring.patch` drains primary-ring
CPU commands before BO export when `VN_WINEHUA_REMOTE_MEMORY_SYNC=1`.
`vn_ring_roundtrip` synchronizes the opposite direction and cannot substitute.
This does not insert GPU DeviceWaitIdle, change native memory types, or forbid
legal map-before-bind. Cached types, explicit cache maintenance, remapping,
dedicated allocation, Bind1, Submit2, disabling batching and idle-before-free
diagnostics did not solve the fault and are absent from the final runtime.
Temporary host diagnostics were removed; the host renderer is the baseline.

| Final source component | Revision / SHA256 |
| --- | --- |
| Legacy DXVK | 5dff1d7b3dc3c2cd7f51356b99213bcc427b1af9 |
| Modern DXVK | 01febe2d48fdce76b581369a1d526d2d33b75c7a |
| Mesa base plus nine managed overlays | 557fa3e781ea6e78fcff3ee759c25f60c3055064 |
| Venus ICD | 0519fc902f06b4f96029c2070d5914b3788d48f3e7bca6b6ca9e2df746755437 |

The two DXVK revisions also merge complete BC rows into copy runs. Modern
releases the prior owned metadata buffer before replacement. The complete 15
DLL pins and overlay hashes are in `texture-compat-runtime.lock.json`.
Both DXVK source branches were pushed to winehua/dxvk.

Completed final verification:

- x64 host: full conformance for both DXVK versions; native automatic/disabled
  captures byte-identical. Both i686 runtimes build and pass the full suite.
- Phone ARM64EC and x64: raw 96 MiB, three-phase transfers with QueueSubmit and
  QueueSubmit2 pass. Full source/destination bytes are checked, not a first word.
- Legal Map-before-Bind passes, including 96 MiB, QueueSubmit2, two rounds in
  the final installed HAP (`phone-modern/final-rc1-map-before-bind-96m.log`).
- Six same-device Modern transfer rounds pass. GPU use peaks at 682,700,962
  bytes and returns to 3,933,553 bytes (`map-order-modern-transfer.log`).
- The final HAP phone run passes all 437,760 compute/raster comparisons plus
  all BC1/2/3 UNORM/sRGB transfer cases. Compute max error is at most 0.00000060.
  GPU use returns to 3,933,553 bytes (`final/phone/final-rc1-modern-full.log`).
- Both devices automatically unpack the updated runtime. Twelve selected DLL/ICD
  readbacks match the lock (`final/device-runtime-readback.json`). These tests
  do not depend on a later manual runtime replacement.

Artifact: `VintagePomeloPro-1.4.8-texture-compat-rc1-debug-signed.hap` under
`F:/VintagePomelo-Workspace/artifacts/dxvk-texture-compat-20261017/final`.
SHA256: `834de6d013e49a23cfd06c235852798e0561c1dcfbd2a20caf6badadb60d095a`.
Nested runtime SHA256:
`58c3d89445d3526a8b23ade5ece7ad5ca118f2713a2539bedc89e7c0a187ae30`.
Texture lock SHA256:
`1456a9a8c7587338f23b421118c573737c1afab5a71bcadce38fabd64c1fb21d`.

This HAP updates only the paired top/nested ICD, two DXVK profiles and runtime
identity of the already compiled 1.4.8 Steam/Rich8 baseline (base SHA256
`3ec060102e34b5d8c0c0c5f61186645d111dfad5291360e750f1a20e984d8bdf`).
All other payload members are byte-verified unchanged. Baseline Float32 audio,
thread-exit repair and graphics migration identity are retained. The whole dirty
Native/ArkTS tree was not rebuilt. Reconstructing that baseline from this parent
commit alone is not asserted. Debug profile identity, verify-app, runtime
components and runtime-library guard pass. An altered top-level ICD is rejected
by the pairing validator. This is a debug-signed test candidate, not app-gallery
release signing or membership/payment acceptance.

## Final original-game and remaining application boundaries

The original game's six resource hashes were restored before runtime regression;
no assembly or save was edited. The same final Legacy DLLs passed save-load,
held movement, indoor/outdoor transition, outdoor map, outdoor movement, return
indoors and movement after return. See the private local evidence
`artifacts/unity-xxdxtrwzt-tablet-20261016/coalesced-original-game-result.json`.
After the final HAP upgrade, the original launcher, existing save, indoor
movement, transition outdoors, full map display, return indoors and subsequent
movement were rechecked in the same running game. Final captures are named
`final-rc1-tablet-*` in that private local evidence directory. A home-screen interruption resumed the same game via
its running desktop and is not counted as a texture crash.

GPU residency after the earlier complete round trip was 3,171,068,946 to
3,240,760,722 bytes. Indoor captures showed 25-41 FPS; outdoors 5-8 FPS.
This is bounded playability, not long-term stability or acceptable outdoor
performance. Ordinary oversized RGBA, BC4/5/6/7, arrays/cubes and the other
unsupported resources above remain outside this implementation.

The final HAP also starts RichMan8 with normal visible title-animation colors.
Its current observed presentation uses the GL route; this is a color baseline
regression, not independent D3D11/Modern game qualification.

Ordinary Steam-card qualification still fails: CEF reports
`GL_INVALID_OPERATION: Vertex buffer is not big enough for the draw call`, and
Venus present logs report `target or object not ready` / `-EAGAIN` for a private
surface. Standalone ANGLE D3D11 pbuffer draw/readback fixtures pass, including a final
unique-log run of indexed counts 6 through 98,304 using BufferData/SubData
(`phone-modern/angle-large-final-1202b.log`),
so this is not evidence that all ANGLE rendering fails. A GPU-rasterization
argument trial did not establish that CEF received the requested switch and is
not an accepted product fix. Wine initialization also intermittently fails with
special-handler exhaustion; a cold retry passed later fixtures. Neither this
startup fault nor Steam black content is declared repaired by the texture fix.
Raw application screenshots and logs remain private local evidence.

Phone testing stopped at the system's 1% low-battery/emergency-mode warning.
The Steam `-cef-disable-d3d11` control was started but its rendering result was
not qualified before that interruption; it is not a product fallback. Offline
inspection confirms that Steam recognizes `-cef-disable-d3d11`, while no
`-cef-disable-gpu-rasterization` switch was found among its CEF launch strings.
