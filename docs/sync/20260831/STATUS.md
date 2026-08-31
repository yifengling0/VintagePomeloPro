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
- T5 in progress; T6-T8 pending. Intermediate commits are not release-qualified. No full integration or phone-regression completion claim.

## Resume

Read PLAN.md and commits.json, inspect current Git status before acting, then execute the next unfinished task. Preserve the full requested scope.
