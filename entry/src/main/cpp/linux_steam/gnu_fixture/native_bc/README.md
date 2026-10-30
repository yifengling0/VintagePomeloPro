# Native Vulkan BC adapter

This layer requires **both** `STEAMOHOS_NATIVE_FEX_WSI` and
`STEAMOHOS_NATIVE_BC`. The HAP-owned native graphics companion now enables it,
with runtime capability checks. `scripts/build_linux_steam_native_graphics.py`
builds the normal payload; `scripts/diagnostics/build_native_bc_runtime.py`
retains the isolated diagnostic entry. Generated SPIR-V and structure-size
headers remain in the reused incremental build directory. Delivery checks and
complete-game acceptance limits are recorded in
[the delivery note](../../../../../../../docs/linux-steam/NATIVE_GAME_DELIVERY.zh-CN.md).

The adaptation happens before a native Linux game queries BC support. It is
independent of DXVK and does not replace the original Wine/virgl/DXVK paths.

| Guest storage | GPU sampled storage |
| --- | --- |
| BC1 RGB | ETC2 RGB |
| BC1 RGBA | ETC2 RGB + single-bit punchthrough alpha |
| BC2, BC3, BC7 | ETC2 RGBA + EAC alpha |
| BC4, BC5 UNORM/SNORM | R8/RG8 UNORM/SNORM |
| BC6H UFLOAT/SFLOAT | RGBA16F |

The original compressed blocks remain in a compact device buffer for exact
copies and readback. GPU dispatches produce the sampled representation without
a CPU pixel copy or a full RGBA image intermediate for the ETC2 cases. ETC2/EAC
re-encoding is **lossy**. BC4/5/6 backing may use more storage than their source;
this is not a promise of a uniform memory saving for every BC format. Presentation
continues to use the existing NativeBuffer path.

The device creation adapter explicitly enables KHR buffer device address even
for a Vulkan 1.1 application and accepts the KHR function alias. A missing address
entry point produces a feature error instead of an indirect null call.

Implemented coverage includes format queries, image/view creation, combined
image/block-buffer requirements and binding, buffer uploads, exact block
copies/readback, mixed equal-byte-size compressed/uncompressed copies, mips,
array layers, nonzero partial updates, and command/pool lifecycle. Private meta
work preserves the application's compute pipeline and stage push constants.

Current constraints include optimal 2D images, supported usage/create flags and
limited pNext structures. External compressed images, sparse/alias images and
3D images are rejected. Devices with transfer-only queues are excluded because
the adapter requires a compute-capable recording queue. Larger images, cube/view
reinterpretation, repeated instance recreation, memory-pressure behavior and
quality acceptance need further coverage. Synthetic tests are not complete Vulkan
conformance. The combined diagnostic game run passed presentation and input;
normal delivery currently still encounters a pre-existing SQLite missing-table
error and is not recorded as full-game acceptance.

`build_native_vk_coherence_probe.py --bc --api11 --steam-launch` builds the
independent decoder/resource probe. The decoder compares all eleven decoder
variants against the separate bcdec CPU reference. Resource tests exercise all
sixteen Vulkan BC formats with odd sizes, multiple mips/layers, suballocation,
raw mixed copies, partial updates, guards and hardware sampling. Sampling coverage
is narrower than the exact-copy checks; it does not yet test all realistic game
textures and every mip/layer sampling combination.

Decoder provenance: GameNative Mesa commit
`6bcfbaa701a8d1efa7f6c54145495eda5329f767`,
`src/vulkan/wrapper/wrapper_bcn_decode.comp`. The bcdec MIT notice is preserved in
`LICENSE.bcdec`. The independent host reference is in `host_tests/vendor/bcdec`.
