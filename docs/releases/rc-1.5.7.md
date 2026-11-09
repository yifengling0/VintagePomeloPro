# rc-1.5.7

Version: 1.5.7 (1005070). Development continues on `main_proton_steamarm64`.
This branch includes the complete Proton/X11 integration history and the
subsequent fixes through this release; no history is replaced by a patch subset.

## Changes

- Unified Huawei checkout for ordinary and test accounts. The server identifies
  NORMAL/SANDBOX from verified receipts, isolates their entitlements, and preserves
  historical grants for official restoration. The client no longer requires a
  debug-signed installation before purchase. Failed historical receipt recovery
  no longer prevents checkout capability and reminder state from refreshing.
- Lifetime VIP uses the nonconsumable product type, official prices, and explicit
  purchase consent. Active or uncertain memberships suppress duplicate checkout.
- Retained X11 window lifecycle, focus/IME, presentation pacing, NativeBuffer,
  Steam/GNU Proton module delivery, offline import, and desktop recovery fixes.
- Retained per-game Box/FEX selection, strict x87 compatibility, FEX self-modifying
  code handling, Box64 exception/memory repairs, nested-executable working
  directories, launch preparation feedback, and reduced UI-thread DLL scanning.
- Retained HD-2D texture, shader float-controls, query lifecycle and spatial audio
  fixes. Legacy DXGI factories now share a Vulkan instance while their lifetimes
  overlap; device references remain valid after factories close.
- Venus private upload pools retire with the upload command, and internal upload
  failures stop the affected context instead of submitting stale contents.
- Fixed clean-build Wayland header staging to follow the pinned versioned build
  directory and require the headers Wayland actually generates.

## Validation and limits

Membership backend tests: 101 passed; packaged purchase tests: 60 passed.
Client checkout/recovery mocks and `proRelease` ArkTS compilation passed locally.
The updated backend and installed client's checkout entry were confirmed by the
user; a real NORMAL charge, delivery, renewal and refund were not exercised.
Auto-renewing checkout still requires a working renewal-reminder service.

DXGI concurrency/lifetime regression runs with ASan/UBSan. Venus upload failure
and retirement checks use a host Vulkan double. These tests do not qualify GPU
behavior: the reported Mate80 DQIII GPU reset remains unresolved.
Local flight-checkpoint diagnostic instrumentation is preserved in the working
tree and is outside the managed release overlays.

GitHub Actions builds an unsigned HAP and matching offline imports from the tag.
Successful compilation does not imply device acceptance; the existing reviewed
runtime gate still applies before automatic RC publication.
