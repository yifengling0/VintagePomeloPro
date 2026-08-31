# Integration status

## Current

- T0: isolated branch created, source SHAs verified, 69 commits classified. Baseline host tests passed.
- Phone connected, API26; temporary screen running lock verified, original timeout saved outside Git.
- Phone originally had 1.3.2. Verified and replacement-installed runtime-equivalent 1.3.3 baseline (RC source differs from main only in CI/docs); SDK signature verifier passed; package hash and screenshots stored outside Git.
- T1: default-policy naming adapted without changing resolver behavior; infrastructure processes filtered only from running cards; managed payload sync extracted and unused SmokeRunner removed. Existing model tests updated for current desktop-only and v6 keyboard contracts.
- T1 gates: `make test`, `make test-model`, `make NATIVE_ARCH=arm64-v8a check-native`, `git diff --check` passed. HAP/device validation remains pending.
- Build checks now have `check-native` and `test-model` Makefile targets; both run in the existing SDK Docker container. Signing material stays ignored (`signs/` added alongside `sign/`).
- T2 complete: all 12 source commits adapted. FramePlanner holds the toplevel lock only for planning/snapshots; FrameBlitter retains lock-free pixel work. Helpers, frame-trace gating and dead-code removal included.
- T2 product adaptations: current surface viewport over stale SHM geometry; parent-based fullscreen input and GPU fit; partial GPU child coverage; direct-output cache invalidation; fullscreen geometry in composition signature; displayed-space damage intersection; retained pixels replayed within output damage. Existing compositor state tests retained (30 checks); clipping tests added (38 checks).
- T2 gates: `make test` and ARM64 `check-native` passed. All recursive gitlinks match pinned commits. No ArkTS/UI or submodule changes in T2.
- T3 complete: PresentedFrame producers/renderers/input and display composers migrated together. Direct buffers keep root input coordinates. Added the lock missing from upstream WindowFrameComposer and publish input fit as a mutex-protected snapshot instead of cross-thread reads of new frame metadata.
- T3 gates: `make test` (34 compositor state checks, including buffer/input space separation and independent window-composer locking), ARM64 `check-native` passed. No UI or gitlink changes.
- T4 complete: ZcBridge owns geometry/key/publication state, occluders use the shared ordered layer list, z-order predicates and explicit sorting adopted; PresentTarget unifies target dispatch and named errors; renderer declares direct-pass capabilities.
- T4 product adaptations: fullscreen GPU placement and occlusion remain parent-based; Vulkan protocol-only surfaces remain supported; GL failure deadlines, target generations, fence quarantine, device release and default-off GLES Direct preserved. Presenter clocks/shaders shared but product pacing remains authoritative. Shared ZC publication map now has a mutex because PC renderers can run concurrently.
- T4 gates: `make test` (41 compositor/state checks incl. fresh-SHM fallback and capability rejection, 40 z-order checks) and ARM64 `check-native` passed. Host doubles replace only OHOS renderer discovery and ready-marker IPC.
- T5 complete: InputSpaceMapper, InputStateTracker, InputQueue and InputInjector own mapping, state, transport and Wayland injection respectively; upstream popup/scroll focus fixes adopted.
- T5 product adaptations: queued relative motion retains its surface and checks liveness; geometry/display-fit/epoch changes reset the relative baseline; IME keyboard focus still reaches TextInputManager. Pointer protocol callbacks invalidate relative state through an injected sink instead of restoring a circular dependency. InputTarget retains dimensions used by the product baseline guard.
- T5 gates: `make test` (including relative geometry/epoch/focus reset cases) and ARM64 `check-native` passed. Runtime dependencies are rebuilding in the isolated container; shader tools copied from the existing build container without modifying it.
- T6 complete: SHM helpers and named CommittedSurface geometry, ToplevelManager commit policies, PopupManager, typed event bus, compositor/input dependency injection, shared DesktopSessionState and atomic frame serial adopted (13 source commits including code in cb88f42b).
- T6 product adaptations: toplevel pixel copy remains unscaled; current root resize/output and engine retry/reset behavior remain unchanged. Created events retain sessionId/clientPid; product raise and IME surface teardown survive. No upstream engine-state side effects or alternate process registry introduced. Per-surface pointer liveness and relative-baseline callbacks receive explicit dependencies; GL geometry changes still invalidate the CPU base.
- T6 gates: `make test` (48 compositor state checks, 39 SHM checks, 90 input-state checks, 66 event contract checks; popup window/pixel size separation, repeated commit, geometry move, unmap cleanup and dragged-window restore covered) and ARM64 `check-native` passed. No UI/gitlink changes.
- T7 in progress; T8 pending. Intermediate commits are not release-qualified. No full integration or phone-regression completion claim.

## Resume

Read PLAN.md and commits.json, inspect current Git status before acting, then execute the next unfinished task. Preserve the full requested scope.
