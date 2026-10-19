# Reviewed Unity texture compatibility experiments

Current device state (2026-10-17): all five affected bundles and the Addressables
catalog have been restored together and read back against the original SHA256
manifest. The game assembly and saves were not edited. The original resource
set passed save loading, indoor movement, an outdoor transition, complete
outdoor map display, outdoor movement and return indoors using the experimental
Legacy runtime compatibility path. A later copy-coalescing build passed the
GPU conformance fixture but still needs an uninterrupted game regression.
The profiles below describe historical resource experiments, not the current
installed game or a default compatibility policy. See
[runtime evidence](VIRTUAL_BC_RUNTIME_VALIDATION_20261017.md).

`scripts/prepare_unity_texture_compat.py` prepares separate resource overrides for
an exact, reviewed game version. It does not install them or change Direct3D
feature levels. These profiles are experimental and are not enabled in the app.

## Profiles and validation status

The Immortal Farmer 1.150126 profile addresses two different limits on the tested
Maleoon 910 Legacy DXVK route:

- `immortal-farmer-1.150126-textures.json`: two 14360×4724 RawImage maps exceed
  the route's 8192 sampled RGBA8 image limit. Reduce only these maps by a power
  of two, keeping full normalized UVs.
- `immortal-farmer-1.150126-bc-budget.json`: additionally reduce 13 reviewed
  animal Spine textures to at most 4096 when `textureCompressionBC` is false.
  The device's BC3 fallback expands these images to RGBA8. The estimated reduction
  for these single-mip images is about 1.36 GiB; this is not a measured residency
  guarantee. Three textures using custom outline materials remain unchanged.

- `immortal-farmer-1.150126-low-memory.json`: retain the two map repairs and
  limit 34 strictly reviewed Spine textures (animal 14, attack 11, roles 2,
  plant 7) to 2048 only on a route reporting `textureCompressionBC=false`.
  This reduces the theoretical expanded Spine images by 2.8993 GiB versus
  originals, about 1.5357 GiB more than the first budget experiment. Atlas,
  material, skeleton, and non-target objects remain byte-identical. Detail
  loss and actual residency still require device validation.

The first budget experiment passed menu interaction, save creation, and opening
story for more than 14 minutes on the tablet, but failed background survival:
at 23:08:38 on 2026-10-16 the system killed the app with
`ResourceLeak(GpuLeak)`, `isForeground=0`, and signal 9. The GPU resource recorded
immediately before this kill was 3,880,427,520 bytes. It is not qualified as a
fix. The new low-memory profile is a separate, more conservative experiment.

The 2K experiment subsequently entered an indoor gameplay scene, responded to
menu/settings input and ran beyond the original kill window. The same Unity PID
25172 was present from its 23:20:23 launch through the 23:41:25 background monitor
end, and a separate 23:41:52 process snapshot. Home was pressed at 23:35:05, giving
over six minutes in background. GPU context totals peaked around 3.225 GiB in
the bounded samples. This establishes bounded process survival, not long-term
stability or successful foreground scene restoration. The map menu appeared
blank, save/load and repeated scene changes remain unqualified, and the app was
absent at the next morning's snapshot; no cause or time is assigned to that later
exit. Keep the resource experiment and its original backups explicitly separate
from the cross-game work in GPU_TEXTURE_COMPATIBILITY_PLAN.md.

The memory experiments follow a real `ResourceLeak(GpuLeak)` system kill of the
Unity process. The event label alone does not prove a missing resource release:
the game preloads animal assets and BC fallback greatly increases their size.
Texture-size compatibility and memory residency must be tested separately.

Spine material UV scale and offset must be identity. The linked atlas must declare
its original nonzero page size. The game's MaterialsTextureLoader retains that
logical size, so the tool preserves all atlas text, region coordinates, materials,
and skeleton data byte for byte while reducing actual sampled image resolution.
Custom materials, Sprite references, and unreviewed references are rejected.
This can reduce texture detail and still needs visual qualification, including
boss pages. It is not a general replacement for native compressed format support.

## Preparation

Install the pinned dependencies in `scripts/requirements-unity-texture-compat.txt`.
Supply the active route's actual Vulkan device and sampled-format limits, for
example (only for the measured device):

```json
{
  "maxImageDimension2D": 8192,
  "rgba8SampledMaxExtent2D": [8192, 8192],
  "textureCompressionBC": false
}
```

```sh
python3 scripts/prepare_unity_texture_compat.py \
  --source-dir /path/to/original/StandaloneWindows64 \
  --catalog /path/to/original/catalog.json \
  --profile docs/graphics/profiles/immortal-farmer-1.150126-bc-budget.json \
  --capabilities /path/to/current-route-capabilities.json \
  --output-dir /path/to/new-empty-output
```

The original catalog and all original bundles must match pinned SHA256 values.
Output contains changed bundles, a matching catalog, and an integrity manifest.
Every unrelated serialized object is compared with the original. UnityFS CRC is
computed over decompressed data blocks and written into the precise Addressables
request record together with the new size and cache identity. CRC checking stays
enabled; dependency tables and other request records remain intact.

For device experiments, stop the game, back up the complete affected resource and
catalog set, apply the set together, and verify readback. Always restore the set
together when abandoning an experiment. Never overwrite original backups. Game
code, user settings, and save files are outside this tool's scope.

Run `python3 host_tests/unity_texture_compat_test.py` for integrity and reference
validation. Passing host checks is not proof of successful gameplay or a safe GPU
working set. Device acceptance must cover startup beyond the prior kill window,
menu interactions, entering gameplay, repeated transitions, and memory behavior.
