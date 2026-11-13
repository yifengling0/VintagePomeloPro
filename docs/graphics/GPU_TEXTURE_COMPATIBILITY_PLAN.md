# GPU texture compatibility across games

> 2026-10-17 final candidate status: Legacy and Modern now implement the
> guarded lossless BC1/2/3 sampled Texture2D path described in
> [the runtime validation record](VIRTUAL_BC_RUNTIME_VALIDATION_20261017.md).
> Native format/usage/extent queries remain authoritative, including 16K on
> capable adapters. The 64 MiB decoded-cost policy bounds eligible large-image
> expansion without recompression. Optional lossy recompression remains separate
> and disabled. The design alternatives and rollout gates below are historical
> planning context; the runtime record is authoritative for completed work.


Status: Legacy and Modern have an experimental lossless virtual BC1/2/3
Texture2D implementation. It preserves logical dimensions and original BC
blocks, selects storage using actual Vulkan format/usage/extent queries and a
64 MiB decoded-image budget, and does not lower feature level or resize assets.
Host conformance passed for both versions; Legacy also passed the real 8K GPU
fixture. Modern phone transfer correctness now passes after the first-shadow-export
ordering repair, including six transfer rounds with memory returning to baseline.
Steam, broader games and long-term residency remain release gates; this is not
a release-qualified universal path. See
[runtime evidence](VIRTUAL_BC_RUNTIME_VALIDATION_20261017.md) and
[earlier prototype results](TEXTURE_PROTOTYPE_VALIDATION_20261017.md).

Product decision, 2026-10-17: default to the original native BC image when it is
supported, otherwise preserve the BC-decoded 8-bit/HDR channel values using the
appropriate uncompressed backing. Recompression is an explicit per-game option,
off by default. Prioritize original-resolution oversized-image compatibility;
this must work independently of the recompression setting. See the
[PomeloTechLabs source review](POMELO_DXVK_TEXTURE_REFERENCE_20261017.md).

## Evidence and current limits

The tested tablet runs Maleoon 910 through the Legacy DXVK 1.10.3 route. Its
sampled RGBA8 image limit is 8192. BC3 8192x4 creation succeeds; 14360x4724
fails with E_INVALIDARG. Keeping FL11 avoids disabling unrelated shader features,
but does not supply its complete 16384 resource-size contract.

The reviewed Legacy source commit is 7c5cc47f772b9564af177cea920da81a40ffd47c,
matching the parent graphics lock. The account-cloud checkout now contains the
implementation in its Legacy and Modern submodules; the earlier reference
review used the matching Legacy checkout under /home/liufeng/src/vpp-proton.
Relevant implementation points:

- thirdparty/dxvk/src/dxgi/dxgi_format.cpp: BC1/2/3/7 -> RGBA8,
  BC4 -> R8, BC5 -> RG8, BC6H -> RGBA16F when native BC is unavailable.
- thirdparty/dxvk/src/d3d11/d3d11_texture.cpp: supports only device-local sampled
  BC emulation; mapped, shared, render-target and UAV combinations are rejected.
- thirdparty/dxvk/src/d3d11/d3d11_initializer.cpp: initial data conversion.
- thirdparty/dxvk/src/d3d11/d3d11_context.cpp: UpdateSubresource conversion,
  resource-copy and readback restrictions.

BC emulation is currently detected by comparing source/destination compressed
flags. Replacing RGBA8 with ASTC would make both sides compressed and bypass
these conversion checks. A format-table-only patch would upload invalid data.

The observed original crash was an actual system resource kill, not evidence of
an unhandled native exception. BC3 costs 1 byte/pixel; expanded RGBA8 costs 4.
BC1 expands by 8x. A 8192x8192 BC3 mip costs 64 MiB versus 256 MiB in RGBA8,
with additional staging and allocator overhead. Full mip chains add about 1/3.

The 2026-10-17 query through this actual route reports BC=false, ASTC LDR=true,
ETC2=true. ASTC 4x4 UNORM/SRGB sampled + transfer images, including mutable
images, query successfully; their maximum extent is still 8192x8192. A separate
offscreen device test passes allocation, upload, mip fetch, bilinear filtering,
UNORM/SRGB view reinterpretation, and an aligned partial update. These fixtures
are small constant-color ASTC blocks, not full game rendering qualification.

The experimental Unity profiles edit exact reviewed resource versions, maintain
Addressables CRC and preserve unrelated objects. They are a diagnostic workaround,
not a cross-game product feature. See UNITY_TEXTURE_COMPAT.md.

## Recommended architecture

### 1. Actual capability and resource-use policy

Probe the active route, not just the GPU marketing name or Vulkan version:
individual BC/ASTC/ETC2 UNORM/SRGB formats, sampled/transfer/filter feature bits,
image extents, mutable-format family support, array/cube/3D usage and available
memory-budget extensions. Use actual usage/flags in image-format queries.
Cache capabilities by GPU/driver UUID and route/build version; invalidate on
changes. Unknown capabilities must not enable an unverified conversion path.

Enable the selected compression feature at logical-device creation as well.
The reviewed Legacy D3D11 device currently only enables textureCompressionBC;
merely observing ASTC support on the physical device does not enable ASTC on the
DXVK VkDevice. The standalone sampling probe explicitly enables supported ASTC.

Select storage per resource at creation time. Native BC remains the preferred
path where that exact format and usage are supported. Keep original D3D format,
dimensions, mip count, row layout and GetDesc/GetDimensions semantics separate
from backing Vulkan storage. Use an explicit encoding record, not a compressed
versus uncompressed boolean. The record must also govern creation, upload,
update, copies, views, mapping and readback. Native BC requires no game edits.

### 2. Preserve decoded quality by default; make recompression optional

With the option off, do not re-encode decoded data or quantize RGBA8 into 16-bit
packed color to reduce memory. BC1/2/3/7 use RGBA8 with the proper UNORM/sRGB
views when native BC is unavailable. BC4/5 retain the corresponding R8/RG8 and
signed interpretations; BC6H retains HDR precision. Preserve alpha as well as
RGB. This preserves the existing BC-decoded content, not the original artwork
before its original BC encoding.

The proposed setting is `纹理重压缩（节省显存）`, default off, stored per game.
Description: `将不支持的压缩纹理重新编码以减少显存占用，可能降低画质；重启游戏后生效。`
It changes runtime backing/cache only, not installed game bundles or catalogs.
Do not enable it automatically after detecting memory pressure, a game crash,
an unsupported extent or a preset change. Legacy and Modern must each advertise
the actually implemented capability before exposing an enabled control; do not
add a cosmetic switch to a runtime with no encoder path. Native BC stays native
even when the option is on. Unsupported conversions retain the quality path.

Optional candidate: transcode BC1/2/3/7 LDR sampled textures to ASTC 4x4 with
native hardware sampling. The reviewed PomeloTechLabs Legacy code already uses
etcpak for ETC2/EAC; its codec and upload/update plumbing are references for
reuse and measurement. Select the optional codec only after actual route and
quality qualification. ASTC 4x4 uses 16 bytes per block, the same storage
as BC3 and about 1/4 of RGBA8. This introduces a second lossy compression step;
measure alpha edges, sRGB gradients, text, normal maps and premultiplied alpha.
Do not promise bit-exact output or zero quality loss.

Use a content-sensitive quality gate, not only average RGB PSNR. In the initial
device benchmark, astcenc's fastest preset introduces alpha errors up to 115/255
in a synthetic BC1 transparency fixture. The fast preset preserves alpha in that
fixture, but two real BC3 crops still show that a few alpha values may change by
16/255. No preset is qualified for all content. Alpha-test thresholds, transparent
edges, signed normals and data textures need independent checks and an exact
fallback. Normal maps cannot be identified reliably from DXGI format alone.

Do not reinterpret BC bytes as ASTC bytes. Decode and encode the actual block
contents. Preserve sRGB sampling and typeless view families. BC4/5 signed data and
BC6H HDR require separate policies; LDR ASTC is not an HDR/SNORM substitute.
The existing exact R8/RG8/RGBA16F fallbacks remain available.

Start with DEFAULT/IMMUTABLE, device-local, 2D sampled resources. Then cover all
mips, arrays, cube faces, partial updates and edge blocks. Unaligned updates must
reconstruct affected blocks from authoritative source data. Store original BC
blocks where compressed D3D readback/copy semantics need them; readback must not
re-encode lossy ASTC into a different BC payload. Invalidate cached data on writes.
Mixed backing encodings require conversion, not a raw Vulkan image copy.

Prototype native/CPU/GPU conversion throughput before selecting an encoder.
A persistent cache may help immutable content, keyed by source bytes, all layout
and color-space parameters, codec version and target format. Bound it, use atomic
writes, and handle full storage without invalidating a successful live texture.
Texture names alone are insufficient cache keys.

The initial single-thread PE64 benchmark converts two real 512x512 BC3 crops
with the fast preset in about 49-55 ms after warmup. This is a feasibility result,
not a whole-game loading budget: the test has one ordered pass and no full-size
textures or frame-time measurement. Reuse contexts and measure cold/warm cache
behavior with repeated larger inputs before deciding how much conversion can
run synchronously. Perform CPU cache work asynchronously while preserving D3D
initialization/update ordering and keeping authoritative BC blocks available.

### 3. Handle images larger than the physical 2D limit separately

Implementation update (2026-10-17): Legacy and Modern have a BC1/2/3 sampled Texture2D
runtime candidate using original BC words in an RGBA16_UNORM atlas and shader
sampling. Native BC retains its existing path. Decoded images that fit queried
Vulkan limits also retain theirs unless their expanded size exceeds the 64 MiB
policy budget. Both the 8K tablet automatic path and forced host compatibility
path passed 437,760 sample comparisons, including raster derivatives, plus
compressed transfer/readback checks. See
[VIRTUAL_BC_RUNTIME_VALIDATION_20261017.md](VIRTUAL_BC_RUNTIME_VALIDATION_20261017.md)
for the exact subset, artifacts, rollback and uncompleted game/release gates.
The following alternatives remain useful design context; RGBA tiling and
optional recompression are not implemented by this candidate.

ASTC does not turn an 8192 device into a native 16384 device. A transparent
solution needs logical textures backed by tiles/array layers or encoded BC block
storage plus shader sampling emulation. An oversized BC3 image can store raw
4x4 blocks in an integer image of ceil(width/4) by ceil(height/4), but every
sample operation then needs an actual decoder and sampler implementation.

An alternative candidate would decode to RGBA8 tiles rather than resizing the
image; it is not the implemented virtual-BC backing. For the measured 14360x4724 map, the
game must still see 14360x4724; multiple backing images or array layers stay
within each measured format's extent limit. Estimate padding, all mips and
staging before choosing tile dimensions. A fixed square tile array can consume
more memory than the untiled decoded image; tiling solves extent, not memory
pressure. Bound uploads and measure the separate residency problem.

Raw BC-block shader sampling is an alternative without an additional lossy
encoding step: the same map's BC3 blocks occupy a 3590x1181 integer-image grid.
It keeps the original compressed data and decodes samples on the GPU. It does
not create native BC support, and decoder precision, sampler semantics and
performance must pass independent conformance tests before selection. This can
be explored with recompression off; do not equate it with optional ASTC/ETC2.

Preserve logical dimensions and mip layout. Sampling must handle wrap/clamp/
border, bilinear/trilinear seams, explicit LOD/gradients, derivatives, Load,
Gather and GetDimensions, plus view offsets and copy subregions. Native filtering
of adjacent array layers cannot provide filtering across tile boundaries.
This requires DXBC/SPIR-V lowering, descriptors and resource metadata changes.
It is substantially larger than a CreateTexture2D exception or format remap.

For an initial prototype, restrict support to a clearly defined sampled Texture2D
subset with conformance tests. Do not claim full FL11 resource compatibility until
remaining operations and image types are handled. Keep FL10 as a user-selected
per-game option where appropriate; do not apply it globally or silently resize
render targets, UAVs, fonts, sprites, normals, or CPU-visible resources.

Lossless shader BC sampling is also a candidate for reducing memory without a
second compression pass. Compare its decoder/filter cost with ASTC hardware
sampling on real devices before choosing a product default.

### 4. Budget and lifecycle

Track original BC bytes, destination image bytes, staging, pending submissions,
and retired allocations separately. Bound simultaneous decode/upload work and
release staging after the GPU fence. Use VK_EXT_memory_budget when supported,
while recognizing that HarmonyOS background resource policies may be lower than
the advertised heap budget. Trim reclaimable caches on background transitions.
Do not free live game resources or assume a diagnostic GpuLeak label proves a
missing release. Correct resource eviction needs backing data and restoration.

Expose coherent DXGI memory information; the tested Unity log reports 128 MiB,
which should be investigated separately from the several-GiB measured residency.
Do not arbitrarily advertise more VRAM to suppress the symptom.

## Delivery order and acceptance gates

Completed groundwork: actual format queries and the small offscreen sampling
suite run on the tablet; 18 BC-to-ASTC codec trials complete. These establish
prototype feasibility, not a universal runtime fix. The new priority is:

1. Add explicit per-resource logical/backing metadata without changing the
   existing quality default. Preserve original dimensions, BC layout/data and
   initialization/update/copy/view semantics; reject unsupported combinations.
2. Implement an experimental oversized sampled-Texture2D subset with
   recompression off, first using the measured 14360x4724 case and a 16384-wide
   fixture on an 8192 route. Compare pixels against CPU/unsplit references at
   tile seams, all mip transitions, alpha edges and UNORM/sRGB boundaries.
   Cover Sample/SampleLevel/SampleGrad/Load/Gather/GetDimensions, address modes
   and anisotropic filtering before claiming support for each operation.
   Guard unimplemented operations explicitly instead of silently corrupting
   their results. Shader features remain available; feature-level reporting
   must match implemented resource contracts.
3. Validate memory independently: bound staging and retired allocations; measure
   repeated scene transitions, foreground and background residency. Compare
   RGBA tiling with raw-BC shader sampling for quality, time and working set.
4. Integrate the separate optional recompression path and cache only after
   encoder, alpha/color, full-size upload and frame-time qualification. Use the
   reference ETC2 implementation where suitable; ASTC is another measured
   candidate. Verify default-off, restart, account-independent per-game
   persistence, capability gating, fallback and cache separation across modes.
5. Regression-test unmodified games: this Unity title, a smaller-texture Unity
   title, another engine with BC textures, dynamic texture updates, and a GPU
   with native BC. Include repeated scene transitions, save/load and at least
   five minutes in background, checking actual process survival and fresh logs.

Unqualified resource types and optional recompression remain outside the enabled subset.
The bounded candidate enables automatic selection only for the tested BC1/2/3 subset;
general release remains gated by the runtime validation record.
Qualify Legacy and Modern independently, including the final DLLs and package.
Retain reversible game-resource experiments explicitly as such. Recompression
stays opt-in after qualification; do not promote it to the product default.

## Runtime selection and rollback contract

| Resource and measured capability | Intended path | Current qualification |
| --- | --- | --- |
| Exact native BC format, extent and usage supported | Original BC image | Existing path; retain it |
| No exact native BC support; recompression off; extent supported | Quality-preserving RGBA8/R8/RG8/HDR decode | Existing supported subset; product default |
| Recompression explicitly on; qualified codec/format/usage and extent | ETC2 or ASTC backing, original BC/layout retained | Reference ETC2 code; standalone ASTC prototype only |
| Alpha/data-sensitive, signed/HDR, CPU-visible, shared, or unqualified conversion | Exact existing fallback where supported and within budget | Retain explicit unsupported cases |
| Logical extent exceeds physical limit | Raw BC-block shader sampling or tiled logical texture | Legacy BC1/2/3 sampled Texture2D candidate tested; full game and release gates remain; ASTC alone cannot fix extent |
| Low memory with live resources | Bound uploads, reclaim completed staging and caches, restore-backed eviction if implemented | No arbitrary live-resource deletion |

Start rollout with the qualified DEFAULT/IMMUTABLE sampled Texture2D subset,
under an explicit experimental per-game option, with telemetry identifying the
chosen storage and reason for fallback. GPU capabilities select valid backings
within the user's chosen quality policy; they must not turn recompression on.
Legacy and Modern need independent implementations and artifact checks.
Keep the original game files, logical dimensions and supported shader features;
FL10 and reduced-resolution resource profiles remain explicit temporary options.
Keeping FL11 today does not assert complete 16K texture or other missing FL11
contracts. Do not turn the experiments into a global feature-level workaround.
