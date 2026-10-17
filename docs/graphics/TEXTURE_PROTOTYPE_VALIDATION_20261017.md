# Texture compatibility prototype: tablet evidence

Collected locally on 2026-10-17 (Asia/Shanghai), using the authorized USB tablet,
Maleoon 910 and the app's Legacy Vulkan route. Device identifiers, unrelated
system logs, notifications and screenshots are omitted. No runtime DLL, HAP,
game resource or feature-level setting was replaced during these probe tests.

## Measured capabilities

`tests/runtime/vulkan_texture_caps_probe.c` loads the active Wine Vulkan loader
and queries physical-device format and image properties. The result ends with
`probe_complete=1 result=0`.

| Property | Result |
| --- | --- |
| textureCompressionBC | false |
| textureCompressionASTC_LDR / ETC2 | true / true |
| maxImageDimension2D | 8192 |
| maxImageArrayLayers | 2048 |
| BC1/3/5/6H/7 queried formats | Unsupported |
| ASTC 4x4, 6x6 UNORM/SRGB | Sampled + transfer queries succeed, including mutable images |
| ETC2 RGB/RGBA UNORM/SRGB | Sampled + transfer queries succeed, including mutable images |
| ASTC/ETC2 maximum queried 2D extent | 8192 x 8192 |

ETC2 has only query evidence. No ETC2 upload, encoder or sampling test was run.
These results belong to this route and driver, not every Maleoon configuration.

## Actual GPU sampling

`vulkan_texture_sample_probe.c` and `vulkan_texture_sample.comp` create a logical
device, explicitly enable supported ASTC, allocate real images and submit an
offscreen compute sampler. Four cases use an 8x8 texture with a 4x4 mip:

1. RGBA8 UNORM control.
2. Mutable RGBA8 UNORM image with sRGB view.
3. ASTC 4x4 UNORM image and view.
4. Mutable ASTC 4x4 UNORM image with sRGB view.

Each case checks 64 texel fetches, a bilinear sample spanning four blocks, and
the second mip. It repeats after updating the lower-right 4x4 region, checking
that other texels and the mip stay intact. A GPU fence and shader-to-host barrier
precede CPU verification; NaNs and untouched output fail the check. The GPU wait
has a 10-second limit. Result: all four cases and both passes have zero mismatched
channels, `probe_complete=1 failures=0`. Largest observed absolute errors:

| Case | Maximum channel error (normalized 0..1) |
| --- | --- |
| RGBA8 UNORM | 0.000000030 |
| RGBA8 sRGB view | 0.000119984 |
| ASTC UNORM | 0.000258505 |
| ASTC sRGB view | 0.000119984 |

The ASTC inputs here are legal constant-color void-extent fixtures. This proves
the tested sampler/upload/view/update path, not arbitrary ASTC content, arrays,
cubes, odd block boundaries, full-size allocation or DXVK integration. Small
images require the same 8192-byte driver allocation: the 80 versus 320 byte
fixture payload does not measure a fourfold real allocation saving at this size.

## BC-to-ASTC feasibility

`bc_astc_benchmark.cpp` runs as a single-thread Wine PE64 program on the tablet.
It uses the existing Wine MIT BC decoder and Arm astcenc 5.2.0, clean commit
`2319d9c4d4af53a7fc7c52985e264ce6e8a02a9b` (Apache-2.0), built for SSE2.
The fixture parser limits count, dimensions and payload sizes; a 180-second
watchdog bounds the benchmark. No codec dependency is added to the app package.

Original bundles were hash-checked against the reviewed game profile before
reading. Two unmodified BC3 512x512 crops cover the large map and a Spine animal
atlas; a procedural 256x256 BC1 fixture covers transparent and four-color blocks.
Each runs LDR and sRGB profiles at fastest, fast and medium presets: 18 completed
codec trials, zero codec failures. Errors compare ASTC CPU-decoded pixels with
the BC-decoded reference. They measure additional encoding loss, not loss from
the original artwork. PSNR below is in 8-bit code values, not linear-light space.

| Fixture, fast preset | Encode time LDR / sRGB | RGB PSNR LDR / sRGB | Max alpha error LDR / sRGB |
| --- | --- | --- | --- |
| Map BC3, 512x512 | 55.210 / 54.954 ms | 56.478 / 57.979 dB | 0 / 0 |
| Animal BC3, 512x512 | 49.503 / 48.771 ms | 60.194 / 61.190 dB | 16 / 16 |
| Procedural BC1, 256x256 | 73.339 / 73.358 ms | 52.789 / 52.949 dB | 0 / 0 |

The animal's fast preset changes alpha by more than 8/255 in 32 LDR and 30 sRGB
pixels out of 262144; 190443 reference pixels have partial alpha. High average
PSNR cannot qualify all edge/alpha-test behavior. The fastest preset is worse:
the procedural BC1 fixture has maximum alpha error 115/255 in both profiles.
Fast preserves alpha for this BC1 fixture, which is not a guarantee for all BC1.

ASTC 4x4 payload is one byte per pixel, versus four for RGBA8. It equals BC3's
payload but doubles BC1's original payload. Compared with the current RGBA8
fallback, both use 75% fewer payload bytes, before allocator/mip/staging costs.
The first encoder context took 228.691 ms versus about 3 ms after warmup. The
first encode also includes warmup. These are one ordered pass with small inputs;
they do not establish production latency, sustained speed or full-game quality.
Future runs need repeated full-size inputs, encoder context reuse, memory limits,
GPU sampling of the transcoded content, frame-time and disk-cache measurements.

## Reproduction and evidence

Build the sampler using `scripts/build_texture_sample_probe.py`, providing a
MinGW compiler, glslangValidator, spirv-val and Vulkan headers. It compiles and
validates SPIR-V, embeds it in a generated header, builds the PE, and writes input
and artifact hashes. The default device output is
`C:\windows\temp\vp-texture-sample.tsv`; argv[1] can override it.

Build the codec trial using `scripts/build_bc_astc_benchmark.py`, providing an
llvm-mingw bin directory, the clean pinned astc-encoder checkout, the Wine bcdec
include directory, and an output directory. It validates the encoder revision,
builds the static library and PE, and writes a hash manifest. Defaults on device:
`C:\windows\temp\vp-bc-fixtures.bin` and
`C:\windows\temp\vp-bc-astc-benchmark.tsv`. argv[1]/argv[2] override them.

Fixture format is little-endian: magic 0x43425056, version 1, count (1..4), then
for each fixture an ASCII name[32], uint32 kind (1 or 3), width, height, byteCount,
followed by tightly packed BC blocks. Dimensions must be multiples of four and
at most 512. Cropped game content and raw device evidence remain local only.

Local evidence directory: `artifacts/unity-xxdxtrwzt-tablet-20261016`.

| Local evidence | SHA256 |
| --- | --- |
| texture-capabilities.tsv | 7bf99d365a32bdaa5c24cc23c4414964cff903e4aec66c53256aa8d36140ee1c |
| texture-sample.tsv | 984b5ec4dbde4e818fbf5cc7b70be5a7bbe0033ce5d9c9b6af0de378277932db |
| bc-astc-benchmark.tsv | e0105ae4310feb18b3792413609f2abdb4012eaa657474b8404eb4eb762e760e |
| Device-tested vp_bc_astc_benchmark.exe | 9e2f31e1330f8231e92a1a2c968e1ad0fc265410e8f99d4a10bbe26b14ae0c19 |

The build manifest and local evidence hashes should be read from the local
artifacts for exact executable identity. The device-tested benchmark executable
hash is recorded separately from later reproducible rebuilds, whose PE metadata
can change their hash. Both build wrappers compiled successfully; the sampler
was also validated with spirv-val.

## Remaining product work

Use these results to implement a resource-level encoding model, enable chosen
device features, and preserve original BC layout/data across initialization,
updates, views, copies and readback. Add a quality-qualified ASTC path with
asynchronous bounded caching. For exact sampling or logical textures larger
than 8192, prototype raw BC-block shader sampling or tiled logical textures with
complete sampler semantics. ASTC alone cannot fix the 14360x4724 map limit.
Keep original dimensions and shader features; no global FL10 downgrade is added.

At this early checkpoint the game's 2K resource experiment was installed with
original backups. It was subsequently fully restored and SHA256-verified before
the original-game runtime tests; see VIRTUAL_BC_RUNTIME_VALIDATION_20261017.md.
This experiment survived the previous foreground kill window and six minutes of
background process monitoring, but map display, save/load, scene transitions and
long-term stability remain unqualified. It is not evidence of a universal fix.
