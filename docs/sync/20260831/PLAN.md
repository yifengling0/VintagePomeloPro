# 2026-08-31 upstream integration

## Fixed inputs and completion

- Product base: `2c0436360ad3f821afe3fd6ea29d76e89f3781e7` (verified remote main).
- Upstream audited interval: `d256317e02c83ed81172938c31152ded16393a32..74f2bfe1aba89cbfbc729d1cf658b46f3aea6f80` (69 commits).
- Branch: `codex/sync-master-20260831`. Do not move existing main/master or touch another worktree.
- Complete the Native architecture migration, preserve product UI and recent graphics/controller behavior, then pass real phone actions. A build alone is not completion.
- Retain the complete original requested scope across sessions. `commits.json` is the source/disposition ledger, not proof of validation.

## Product contracts

Keep bundle/name/version/signing, games directory, catalog, floating desktop, PC immersion, phone layouts, HUD, virtual overlay, preferences and user data. Only necessary controls may be added in the existing UI style.
Retain WineEngineService/AppSessionService and public NAPI launch/session contracts. Internal events become typed; outward event strings and JSON stay compatible.
Keep product graphics policy as the sole default resolver, both DXVK batching defaults on, GL failure deadlines/backoff, present geometry, relative-pointer reset, SHM freshness, target generation/fence retirement. GLES Direct remains unqualified/off; busy-query experiments remain isolated.
Keep every gitlink and .gitmodules unchanged during inbound integration. No guest/host protocol changes. Keep controller Y convention, overlay/keyboard coexistence, disconnect/focus release and external-pad-only rumble.
Do not copy upstream Pad cursor or perfProfile fixes literally: product floating state is live; product graphics policy replaces the old profile selection.

## Ordered tasks

- T0: freeze inputs, ledger, isolated ext4 clone/container, baseline tests/device evidence.
- T1: adopt Native default-policy naming; retain equivalent resolver behavior; adapt process filtering; extract managed smoke payload sync from unused SmokeRunner. Keep live LAB APIs isolated.
- T2: compositor dead-code audit, helpers and frame planner/blitter separation. Preserve product calls and lock boundaries.
- T3: migrate PresentedFrame producers/renderers/input and DisplayPolicy together. df5fd270 is incomplete alone: pair with 37b83ad5 and 567697d3. Keep product fullscreen mapping and pointer geometry.
- T4: ZcBridge, layer/z-order, PresentTarget/common shader tools and DirectPassPolicy. Preserve GL/Vulkan distinction, failure deadlines, native-window ownership and device release.
- T5: InputSpaceMapper/StateTracker/Queue/Injector; independently verify scroll fix cee5b1a8 and product/controller paths.
- T6: ShmFrameSource/CommittedSurface, popup/window state, typed event bus, dependency injection, shared session state and atomic frame serial.
- T7: align compositor/frame,toplevel,input; protocols; wine/proc/graphics/input/audio/bridge/common. Private controller -> input/controller, renderer targets -> graphics, telemetry core -> common, telemetry NAPI -> bridge. Update every build/test/script include path and preserve blit -O2.
- T8: dual-ABI HAP, archive/signature/contracts, phone actions, performance and source/disposition completion audit.

Use one coherent, buildable checkpoint per batch. Intermediate checkpoints are not release-qualified. Do not blanket merge or resolve all conflicts with ours/theirs. Source docs have rebased SHAs: use the actual commit ledger.

## Validation and failure handling

Run affected host tests and Native compile per batch; NAPI/ArkTS changes require HAP compilation. Preserve existing tests and import upstream behavior tests, changing adapters rather than weakening assertions.
Final gates: make test, GLES target, HUD/navigation, CI release, catalog/input compiled-model tests; ARM64 and x86_64 HAP integrity/signatures; phone cold/warm launch, first game, restart, fullscreen input, scroll, IME, popup, menus, minimize/background, controller directions/release/rumble.
Run GL x86/x64 ten minutes and five resize/background cycles, both DXVK generations with frame progression, relevant War3/RA2/PAL2 scenarios. Three alternating baseline/candidate performance pairs; investigate >5% FPS/P95 regression. Existing 0x505 is a baseline issue, not a license for regression. Never claim unsupported GLES Direct or missing hardware was tested.
Build only via the existing winehua-dev image and root Makefile on ext4; API23, Guest x86_64, BUILD_GUEST_GFX=1; unique container vp-sync-master-20260831, explicit discovered SDK mount. Never replace vp-build. No overlapping heavy builds. Replace-install to preserve data; no default uninstall/reset.
Phone first: original screen timeout 600000ms; temporary PowerMgrKeepOnLock acquired using hidumper PowerManagerService -t, no power-mode change. Restore with -f only after the task's device testing is finished. Device IDs/signing material never enter tracked evidence.

## Handoff and later upstream contributions

Luna: deterministic inventory/path moves/test evidence. Terra: semantic adaptations; high effort for frame/input/ZC/lifetime/lock boundaries. Load this contract plus one task and relevant symbols, not all patches/history. Stop scope expansion; provide minimal failing evidence after two failed focused attempts.
Each task handoff: input/target SHA, source list, changed scope, checks and limitations, next task. Keep detailed logs/images in ignored output. Record actual usage rather than promising a savings percentage.
After product integration, create clean upstream-based branches for graphics correctness, graphics performance, and controller (87b23e56/2ad6ac24/d2629a9c, excluding release/UI). Submodule contributions precede gitlinks. Do not publish product history, identity, signing or machine paths. Stable fixes and default-off candidates go in separate PRs. Prepare the contribution packets here; no automatic master push.
