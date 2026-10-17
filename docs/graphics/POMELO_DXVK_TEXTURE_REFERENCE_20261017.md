# PomeloTechLabs DXVK texture reference review

Reviewed public `main` snapshots on 2026-10-17 through the GitHub API and clean,
isolated source clones. At the start of this reference-only review the locked Legacy source was `7c5cc47f772b9564af177cea920da81a40ffd47c`; this reference-only stage did not replace a submodule, runtime DLL, app setting
or device file. Subsequent implementation and final candidate identities are in
[VIRTUAL_BC_RUNTIME_VALIDATION_20261017.md](VIRTUAL_BC_RUNTIME_VALIDATION_20261017.md).

| Repository | Reviewed revision | Relevant behavior |
| --- | --- | --- |
| [dxvk-ohos-legacy](https://github.com/PomeloTechLabs/dxvk-ohos-legacy) | fb2cfafba97790a1cbf2ae902c9d09971ea989b3 | BC decode, ETC2 color recompression, EAC encoder/mapping code, and packed 16-bit output selection |
| [dxvk-ohos-modern](https://github.com/PomeloTechLabs/dxvk-ohos-modern) | 7b924f77be58a5bb701e200bfad908991faec08a | Mobile BC fallback still maps to RGBA8/R8/RG8/RGBA16F in the reviewed snapshot |

## What Legacy already implements

[`dxgi_format.cpp`](https://github.com/PomeloTechLabs/dxvk-ohos-legacy/blob/fb2cfafba97790a1cbf2ae902c9d09971ea989b3/src/dxgi/dxgi_format.cpp#L977)
selects fallback backing through `WINEHUA_DXVK_BC_BACKING` when BC fallback is
needed. Empty or `default` selects the smallest supported candidate: ETC2/EAC,
then packed 16-bit color where applicable, then RGBA8. Explicit `rgba8` disables
those smaller color backings in this selector. This is the reference fork's
behavior, not a supported switch promised for the current app build.

[`d3d11_bc.cpp`](https://github.com/PomeloTechLabs/dxvk-ohos-legacy/blob/fb2cfafba97790a1cbf2ae902c9d09971ea989b3/src/d3d11/d3d11_bc.cpp#L200)
decodes BC into pixels, optionally packs channels into 16-bit color, or feeds
BGRA intermediate data into its bundled etcpak ETC2/EAC encoders. It adjusts
output block row/slice pitches for the encoded backing. This is additional
lossy encoding/quantization relative to BC-decoded content. The codec's comment
also records gradient banding with heuristic ETC2 mode selection; full search
is preferred there, with `GTAV_OHOS_ETC2_FAST=1` enabling the faster mode search.
These comments are reference findings, not new measurements on our tablet.

Its texture, initializer and update paths detect changed BC backing by actual
format difference, so compressed-to-compressed conversion is recognized. This
is useful reference code for the existing app's compressed-flag-based detection,
which would miss BC-to-ASTC/ETC2. Copy, readback, typeless view families, partial
blocks, sRGB, device feature enablement and resource restrictions still require
independent review and tests before porting the path into Wine.

One concrete integration issue is visible in the reviewed decoder: selection
of `GetEtc2Target(targetFormat)` is guarded by `info.pixelBytes == 4`. BC4 and
BC5 decode to one and two bytes per pixel, so the EAC targets are not selected
for those sources, even when the format table selects an EAC backing. The
encoder functions' presence therefore does not establish a working BC4/BC5-to-EAC
path. Keep the existing R8/RG8 quality path and fix/test that contract before
enabling EAC. This is a static source finding, not a reproduced device failure.

The private ASTC/ETC2/EAC DXGI IDs in
[`CreateTexture2D1`](https://github.com/PomeloTechLabs/dxvk-ohos-legacy/blob/fb2cfafba97790a1cbf2ae902c9d09971ea989b3/src/d3d11/d3d11_device.cpp#L223)
are a narrow, immutable, preprocessed-payload contract guarded by
`DXVK_NATIVE_OHOS`. The Wine branch rejects those private IDs. Their presence
does not mean ordinary Windows game's BC textures transparently become ASTC.

## Decisions for this app

The user's quality policy takes priority over the reference fork's memory-first
default. The proposed per-game `纹理重压缩（节省显存）` switch defaults to off,
including old game records with no saved value. Native BC remains preferred
when supported; otherwise normal BC color textures decode to full RGBA8,
retaining alpha and correct sRGB interpretation. Signed/HDR and channel formats
retain their corresponding exact decoded representations. No automatic 16-bit
quantization, resizing or recompression is introduced by low-memory detection.

Turning the option on may select a qualified supported ETC2/ASTC path for
eligible sampled resources. It changes runtime backing and bounded caches;
original game assets/catalogs remain intact. Mode changes apply after restarting
the game, and caches must include encoding, source/layout, codec version and
color interpretation in their keys. Do not enable a UI switch until the selected
runtime actually implements and qualifies the capability. Legacy and Modern
remain separate implementations.

## Oversized textures are the next priority

The reviewed snapshots retain the logical extent in image creation and validate
it against Vulkan image limits. No complete transparent oversized-texture
tiling/sampling implementation was found in the reviewed D3D11/DXBC paths.
ETC2/ASTC reduces storage cost but does not raise this tablet's measured 8192
sampled-image limit. The game's 14360x4724 texture remains oversized after
recompression at the same dimensions.

The quality-preserving candidate is a logical Texture2D backed by multiple
decoded RGBA8 images/layers. The game continues to observe 14360x4724, and each
physical image fits the queried format/usage limits. Shader operations must
translate logical coordinates, return logical GetDimensions, compute LOD from
logical derivatives, and handle filtering/addressing across tile boundaries.
Creation-only splitting or normalized UV scaling cannot provide this contract.

Raw original BC3 blocks stored in a 3590x1181 integer-image grid are a second
candidate for this map. Shader decoding avoids another lossy encoding step and
keeps compressed payload size, but requires correct decoding and sampling;
it is a prototype direction, not existing native BC support. Compare it with
RGBA tiling under the same pixel, sampler and performance tests.

Both candidates must handle mip chains, updates/copies, view offsets, seams,
Load/Gather, explicit LOD/gradients, border/wrap/clamp and anisotropy for every
operation they claim to support. Start with a guarded sampled Texture2D subset.
Do not replace a missing implementation with global FL10 or silent downsizing.
Tiling alone also does not solve the separate RGBA memory expansion problem:
padding and staging need budgets, and the default quality mode must remain
honest about unsupported allocation cases.

See [the revised implementation plan](GPU_TEXTURE_COMPATIBILITY_PLAN.md) for
delivery order and acceptance gates. This review changes that plan; it does not
claim the optional compression setting or oversized-texture runtime is shipped.
