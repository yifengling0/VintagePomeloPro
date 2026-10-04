# Steam GPU follow-up: owner, scene/input and build provenance

Baseline: `87b0c5c1ec44a9700faed44217b078ea67de8533` on `feature/main_proton`.
This patch changes host-side correctness, diagnostics and the Wine cache gate.
No gitlink, Wine overlay, FEX, queue/fence policy, resolution or fullscreen-priority
policy is changed. It does not claim the recorded game's Steam occlusion is fixed
on a device: that session's game parent had SHM, and no decisive scene/draw/input
trace was captured. Overlay exit=11 still needs its first fatal stack.

## Owner authority

- Explicit Wayland toplevel/subsurface roles retain their offsets and viewport
  geometry, including legitimate 1×1 children.
- Role-less `GetLayerInfo` uses only `ResolvePresentBinding`; rejection or a
  missing bound owner is terminal. The generation-zero geometry bypass is gone.
- A bootstrap requires a unique exact state/buffer-extent match. Same-PID matches
  take precedence. Claims are checked **after** uniqueness, so an occupied peer
  cannot turn an ambiguous candidate set into an apparently unique owner.
- Unique cross-PID geometry remains an explicitly named legacy weak identity
  path. It is not claimed to be a validated explicit owner token.
- Existing binding geometry queries (including zero fallback extent), resize,
  pending takeover, consumed-frame retirement and generation rejection remain.

## Shared scene/input eligibility

`BuildLayerListLocked` obtains SHM-or-consumed-GPU eligibility through
`IsContentVisibleLocked`. GPU evidence contains only dimensions and binding
identity, is protected by the existing toplevel lock, and is revalidated against
live producer/owner resources, protocol edges, generation and lifecycle flags.
It is never a retained resource pointer or copied pixel buffer.

The renderer's private no-SHM visibility amendment is removed. Actual input
rebuilds the same candidates and uses the existing fullscreen picker. Menu and
modal visibility/interception use that same parent content eligibility. Owner
lanes, top-anchored rules, modal ordering, fullscreen priority and black-border
swallow/release behavior remain in place. Source or owner teardown cannot make
old consumed metadata prove a new binding generation.

No GraphicsBroker lock or public self-locking resolver is called while holding
the window-tree lock. The lock-held passive resolver accepts the snapshot's
layers and fullscreen decision, suppresses its normal selection-log cache, and
returns no pointer outside the diagnostic's lock scope.

## Opt-in bounded diagnostics and upload accounting

Existing `frameDiagnostics` or frame trace enables new scene samples at most
once per two seconds. Each sample reports its monotonic time and ID, current
center hit and modal/black-border result, candidate z-order, priority, SHM/GPU
eligibility, lifecycle flags and filtering reason. Candidate/draw detail is
bounded to 16 entries with explicit truncation counts. Sample identity does not
participate in scene equality and cannot create idle redraws.

- `lastInputTarget`, `last_event_us` and `last_xy` describe the last actual pointer
  event. A passive center query never changes this history or injects input.
- `SCENE-DRAW-ISSUED` means the renderer issued that layer's draw call.
- `SCENE-SWAP ... ok` is the corresponding EGL swap result.
- `consumeAgeMs` describes NativeImage consumption, not drawing.
- `upload_calls_issued` and `upload_bytes` accumulate each actual SHM texture
  allocation/update call, including mixed scenes and failed-swap frames. Static
  reuse and native-only layers add zero. Bytes are payload dimensions × 4, not a
  stale whole-desktop vector size.
- `upload_us` is opt-in CPU API submission time; `upload_timed_frames` identifies
  measured samples. These are neither GL-success/GPU-DMA measurements nor
  dropped-frame or unique-content-FPS counters. No glFinish/readback was added.

## Wine cache/source identity

Both direct `build_wine.sh` and the always-executed top-level Wine stamp recipe
run the read-only identity gate before cache reuse. The check-only route does not
download a missing toolchain. A newer stamp or older source mtimes cannot bypass
source validation.

The gate canonicalizes WINE_SRC and BUILD_DIR; checks native and every OHOS
Makefile/config.status srcdir; and fingerprints the build/environment/guard
scripts, ordered overlay hashes, architecture/host/configuration and compiler.
It records the actual Wine HEAD, active wine-valve gitlink, local delta, generated
inputs and a full effective-source content digest. For mounts without readable
Git metadata it explicitly records an unverified pin and full-source digest;
it never substitutes the legacy Wine pin or silently changes identity basis.
The generated configure is tied to the same manifest.

Missing provenance or mismatched identity fails clearly and asks for a new
isolated BUILD_DIR. No reset, make clean, deletion, automatic reconfiguration or
manifest overwrite repairs an existing user's tree/cache. The manifest records
source/configure identity; a future real Wine/HAP build must still establish
object → stripped library → package-member hashes before device conclusions.

## Regression commands

Use the repository top-level Makefile; these native host tests need no Docker:

```sh
make test-gpu-followup test-egl-multi-consumer test-zc-binding-lifecycle \
     test-steam-gpu-contracts test-frame-loop-diagnostics test-cef-render-switches
python3 host_tests/gpu_owner_contract_test.py --baseline
python3 host_tests/gpu_scene_input_test.py --baseline
python3 host_tests/steam_gpu_contract_test.py --baseline
make -k test
```

The new defaults pin the first two baseline checks to the exact `87b0c5c` above.
They require the intended assertion failures; compilation/setup failure is not
accepted as a red test. The old four-fix baseline remains pinned to `960bf939`.

Owner tests execute full resolver/GetLayerInfo/consume/invalidation functions.
Scene/input tests compile complete production translation units, including the
real manager, layer builder, fullscreen picker, snapshot and InputResolver; only
platform resource/logging/ready-marker I/O is substituted. Cases include no-SHM
and SHM parents, real priority raises, menus, GPU modal interception, hide,
minimize, deactivate, destroyed/forged parents, rebinding generation, passive
history preservation and black borders. EGL tests execute real draw branches
for first upload, changed pixels, resize, reuse, absent pixels and native-only
layers, including a zero extra timing-call assertion when disabled.

Build tests use disposable sources/configured caches and compare every file
before/after rejected checks, including symlinks, spaces, relocated builds,
arch/overlay/source changes, missing Git metadata, and the **actual top-level
stamp recipe** with deliberately misleading future mtimes.

Host success does not establish OHOS build success, real NativeImage correctness,
visual foreground switching, device performance or Overlay stability. Those
remain explicit device acceptance items.

## Independent host validation (2026-10-04)

The frozen implementation was independently reviewed with no remaining blocking
source finding. Nine owner cases, five full-production scene/input cases, actual
EGL upload branches, upload aggregation across failed swaps, both cache fixtures,
and the other runnable host prerequisites passed. The exact `87b0c5c` owner and
scene baselines and original `960bf939` baseline reproduced their designated
assertion failures. The remaining top-level test body was run with prerequisite
targets explicitly marked already executed; it and spawn-log metadata passed.

`make -k test` returned **2**, not a full pass:

- Five of nine Wine stability cases were blocked by LeakSanitizer's fatal ptrace
  limitation in this executor; the other four passed
- Broker startup could not create its AF_UNIX socket
- Gamepad's socket-backed start failed; independent AF_UNIX STREAM and DGRAM
  probes both returned EPERM

These are recorded environment blockers, not waived passing gates. No claim of
complete ASan/LSan coverage, OHOS/Wine/HAP build, package identity chain, new
device installation, device foreground/input acceptance or measured performance
improvement is made. Review/test logs are under
`.temp/codex-runs/gpu-followup-independent-20261004/`; implementation focused logs
are under `.temp/codex-runs/gpu-followup-fix-20261004/`.

No commit or push was performed for this patch during implementation/review.
