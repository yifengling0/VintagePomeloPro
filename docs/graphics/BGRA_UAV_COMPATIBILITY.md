# Legacy DXVK BGRA UAV compatibility

DRAGON QUEST III HD-2D Remake (UE4 4.27.2) failed in CreateTexture2D with
E_INVALIDARG for an 815x815 B8G8R8A8_TYPELESS texture using RTV, SRV and UAV.
The Maleoon 910 path supports RGBA8 storage images but not BGRA8 storage images.

The managed Legacy DXVK overlay keeps the BGRA image backing and permits an
RGBA storage-only view through mutable format plus extended usage. Real base
and alternate image capabilities and limits are checked. Typed float image UAV
loads/stores and clears map R/B channels explicitly; uploads, copies, ordinary
render targets and sRGB sampling retain their BGRA semantics. There is no
per-frame CPU image conversion and no game-name rule or capability-bit override.

Two mask words cover the 64 pixel/compute UAV slots. Their values are part of
the existing pipeline state, including rebind, unbind and deferred restore.
Only shaders that use this mapping bake these words into the final SPIR-V.
A valid specialization-based prototype still returned RGBA bytes from BGRA
stores on the tested path; baking the per-pipeline masks passed both BGRA and
RGBA variants. This observation alone does not identify which Vulkan/translation
layer lost the specialization. D3D9, HUD and presenter specialization behavior
is retained. The patch also corrects ClearState repeatedly clearing one UAV slot
instead of every slot in its loop.

Fallback activates only when native BGRA storage is absent and RGBA storage is
available. Typeless, UNORM and sRGB BGRA backing is supported, with only linear
UNORM UAVs exposed. Shared, imported, GDI and multisample resources are excluded.
`DXVK_WINEHUA_BGRA_UAV=0` disables the fallback for diagnosis.

The overlay is pinned in both graphics locks. Legacy x86/x64/ARM64X builders
apply it idempotently and reject mismatched patches. Make dependencies include
the patch, helper and lock, so incremental builds cannot silently keep an old
DXVK stamp. Assembly records effective source identity and managed patches.
The local wine-data payload, nested DLL inventory, outer payload hash and
extraction content marker are paired. Unrelated runtime members are preserved.

Reproducible probe sources are in `tests/runtime/bgra_uav`. Sanitized validation
and binary identities are in `bgra-uav-validation.json`. Device validation covers
the 31 GPU checks and the game title menu; it is not a full-game compatibility
or performance qualification. The x86 pair was compiled but not device-tested.
DXVK 2.6 and the separate GNU DXVK distribution were not changed in this task.

## Historical map performance follow-up

These measurements predate the later Edge fixes. For current status and the
managed sampler/occlusion fixes, see `UE4_HD2D_EDGE_PERFORMANCE.md`.

### Earlier measurements

Early captures suggested that the opening forest rendered, but later fresh
captures contain a black main scene. Observed gameplay remains around 2-4 FPS
with the packaged configuration; scene visibility has not been qualified. The BGRA allocation/colour fix does not qualify
map performance. CPU samples mostly show EVENT-query/QPC polling while waiting
for GPU completion. Sampled GPU intervals contain large rendering/dependency
costs; they do not yet identify a hardware limit or one shader as the root cause.

Diagnostic-only removal of BC/border emulation still yielded only about 6 FPS.
With those diagnostic flags unchanged, disabling depth of field reduced compute
calls from roughly 48 to 33 and yielded roughly 7-8 FPS. These are observations,
not a validated performance fix. Screen-percentage experiments lacked viewport
verification, and a 600x400 launch was reset by the game to 1200x800, so neither
proves resolution independence. All temporary instrumentation, runtime overrides
and Engine.ini edits were removed/restored; the device DLLs again match the
packaged BGRA runtime. See `ue4-map-performance.json` for evidence limitations.

## Edge partial BC support

The same game was copied directly to Edge (190 files, 16,532,425,687 bytes; all
SHA-256 checks matched). The X11/Venus path advertised textureCompressionBC but
rejected BC4--BC7 image queries, causing BC5 2D creation to return E_INVALIDARG.
After fixing family selection, the next failure was a BC1 typeless 3D resource.
The managed second overlay checks each BC family's views and concrete/mutable
2D/3D image capabilities once at adapter initialization. A partial family uses
the existing upload-time decoder for the entire family, preserving representation
across views and copies. This can expand otherwise supported native 2D images
and increase memory use; it is not a performance optimization.

The restored normal runtime passes the BC fixture's 2D/3D creation and BC1/BC5
GPU sampling tests, and the game reaches a visible Chinese title menu. Two
reverted sampler-specialization prototypes improved only menu FPS; the tested
map-related scenes stayed around 5 FPS with a black main scene. A Wayland
comparison through native Maleoon Vulkan also rendered the title but dropped
to 2-3 FPS with a black post-title scene. It did not reach a qualified identical
save/map comparison, and also changed the Vulkan implementation, so it does not
isolate compositor overhead. Neither prototype is packaged or left on Edge.

A separate X11 startup failure selected display :1 while the installed NCP shim
only staged server-0.xkm. The source shim now stages the actual display number
and compiles incrementally. The earlier test used a temporary server-1.xkm staging helper. The RC 1.5.6
host is rebuilt with this source fix, so manual launches stage the selected
display keymap themselves.
The original X11 preference was restored after the Wayland comparison.

See `bc-family-validation.json` and `tests/runtime/bc_fallback` for binary
identities, probe sources, validation boundaries and the fallback's limitations.
