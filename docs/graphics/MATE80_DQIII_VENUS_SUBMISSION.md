# Mate80 DQIII Venus submission investigation

## Scope and status

2026-11-06, base `45cfc80efd0726b4c9034911ebae5781fd098fb5`, 1.5.6.
The reported failure is grass-scene loading or graphics-quality changes in
Dragon Quest III HD-2D, original OHOS Wine, DXVK 1.10, Maleoon 920 phone.
The candidate has now been installed and tested on the phone, but there is
**no device acceptance**: the original GPU reset still reproduces. The official
rc-1.5.6 packages and tag are not replaced by this local diagnostic candidate.

The phone and tablet both use the VPP2 spawn envelope and the complete 11-field
graphics-host configuration. No omitted tablet-only launch parameter was found.
Their Vulkan routes differ: the phone uses Venus; previous Edge acceptance used
native Direct Vulkan. Phone Direct admission has not been qualified, and its
capability guard remains in place.

## Common corrections

Managed overlay: `patches/virglrenderer/0013-shadow-upload-retirement-and-failure.patch`.
It applies to OHOS Venus queues, including consumers from GNU clients; there is
no game-name or phone-model condition.

1. Signal the private upload-slot timeline in the upload submit itself. The old
   extra retirement submit followed the entire guest batch, unnecessarily making
   command-pool reuse depend on the guest's potentially unresolved timeline waits.
   The command pool owns only the upload command. Its completion is sufficient
   for reuse; guest waits, signals, pNext data, and the guest fence are preserved.
   This removes a dependency on the current guest batch. It does not bypass
   earlier blocked work on the same queue, and is not a claim that all possible
   decoder/queue wait cycles have been removed.
2. Stop the affected context when private preparation, deferred-host waiting,
   or upload submission fails. Previously the non-inline path could consume
   dirty generations, fail the upload, try a CPU retry with that dirty state
   already cleared, then overwrite the error with a successful guest submit.
   A timeout also does not permit resetting or writing an in-flight slot.
   Preserve OOM errors; report other internal failures as device loss, since
   VK_TIMEOUT is not a valid vkQueueSubmit result. Both locks are released on
   preparation failure. This prevents silent continuation with stale data;
   it does not recover a GPU that has already reset.

No texture format, compression capability, sampler default, graphics quality,
audio configuration, Direct gate, or FEX precision setting changes here.

## Validation

`host_tests/venus_shadow_submission_test.py` compiles actual production C
functions and submit/error branches against a dependency-aware Vulkan double.
The original code failed both independent retirement and upload-failure checks.
The candidate passes these plus preparation/wait failure and lock-release
checks for QueueSubmit and QueueSubmit2. Upload slots are cycled 16 times, with
each guest batch explicitly released before the next submission. A host double
cannot establish real GPU progress or application performance.

The test uses the pinned Venus Vulkan headers and is registered in the build
workflow. The overlay has passed clean-file application, idempotence, reversal,
and replay; the graphics source lock matches. The existing incremental OHOS
ARM64 Release build successfully links libvirglrenderer and the vtest host.
This is not a full clean HAP or remote CI acceptance.

## Phone evidence after reconnection

The unchanged release guest/UI payload plus the candidate Venus libraries still
fails after loading the title and confirming the existing autosave. The source
corrections above are error/lifetime fixes, not the demonstrated gameplay repair.

- All 160 captured guest remapped SPIR-V modules pass `spirv-val --target-env
  vulkan1.1`. This does not validate pipeline state, descriptor contents, feature
  contracts or memory synchronization.
- A combined-sampler diagnostic crashes with a native null dereference inside
  `libbishenggpucompiler_v300.so.15`, called through native
  `vkCreateGraphicsPipelines`. This is a separate experimental failure, not proof
  of OOM or proof that the default mode has the same fault.
- Disabling custom-border emulation does not repair the reset.
- In a trace-frame-association control, submit 16307 completes; submit 16308
  (command IDs 12877 and 12876) returns `VK_ERROR_DEVICE_LOST` from QueueWaitIdle
  after about 4.48 seconds. A private upload-slot timeout is not needed to
  reproduce this failure. This control changes timing and coverage sorting.
- Pipeline creation immediately preceding that batch succeeded. Matching shader
  FNV32 values are `ee674a87`, `41c7b2bf`, `c0feafef`, `9717290f`; all match the
  validated archive. Their creation order does **not** establish that they are
  the failing draws. Command recording-generation and submit association must
  be retained before further shader-specific controls.

Historical Edge evidence includes both Direct and Venus runs. Venus rendered the
forest but had low performance and a quality-switch communication stall. Thus
"phone uses Venus" is an architectural difference, not a complete root cause.
The historical Direct log and current phone log expose different shaderDemote,
robustness2/nullDescriptor and dynamic-state capabilities. Aggregate BC is 1
on Edge and 0 on phone, yet **both** logs select BC1-BC7 upload-time decompression.
Do not attribute the reset to BC based only on the aggregate feature bit. Phone
scalability settings are currently 3, whereas accepted Edge gameplay used low
quality. Exact runtime/settings/feature comparisons remain necessary.

Evidence stays in the fixed private task directory
`workspace_temp/rc156-phone-smoke`: `dqiii-investigation.json`,
`shader-validation.json`, `phone-shaders.tar.gz`, `failure-submit.private.log`,
and the native crash record (large logs may be retained as verified gzip).
The diagnostic application was stopped and the temporary screen timeout restored.

## Device checks still required

1. Start through the ordinary UI with DXVK 1.10 and the production profile;
   remove the previous combined-sampler experiment from the trial invocation.
2. Load the same grass scene; collect bounded first-error evidence.
3. Change graphics quality, then load the scene again; verify input, audio,
   rendered content, and continued frame progression.
4. Exit and restart; confirm there is no GPU reset, device loss, or upload abort.
5. Regress a D3D11 cube and the previously working tablet path when available.

Earlier CPU-upload, final-presenter, and occlusion-pass controls did not repair
the phone failure. Combined samplers subsequently triggered a native compiler crash;
they remain an unqualified experiment, not the default fix. If the first
non-completing GPU submission persists, the next comparison must isolate its
shader/descriptor and queue dependencies rather than raise a timeout or claim
that a faster menu proves the loading issue fixed.
