# RC 1.5.6 / 1005060

This release keeps version 1.5.6 and is traced by the annotated `rc-1.5.6` tag.

- Aokana: the earlier FEX full-SMC IR-block state fix is retained (upstream
  PR 5618). Strict x87 remains a per-game option rather than a global override.
- Dragon Quest III HD-2D: managed legacy DXVK BGRA UAV and partial BC-family
  fallback, sampler specialization/state restore, and real occlusion-query
  generation/pass isolation. Maleoon enables the pass quirk automatically.
  The user confirmed no flicker and roughly 30 FPS with reduced game quality.
- OHOS Wine audio: shared multichannel/spatial downmix, writable-period event
  pacing, front-channel unity gain and linked overload limiting. The user
  confirmed music and normal loudness; slight distortion remains unqualified.
- Runtime upgrades: targeted FEX replacement now refreshes Wineboot's builtin
  identity, so preserved prefixes receive the repaired DLLs.
- X11: stage the XKM file for the display actually reserved, including :1.

All new runtime changes are managed patches with build dependencies, hashes,
and host/API probe coverage. No game-name switch, forced-visible query result,
user-save replacement or game-library reset is required. Direct/manual game
launches use the same runtime and default device quirk as the tested path.

The RC reuses the previously paired full X11 runtime, retaining its exact-output
receipt and explicit local-candidate acceptance policy. New legacy DXVK and
OHOS audio binaries are rebuilt from the managed sources; host/ArkTS are rebuilt
in Release mode. Media, fonts, networking, GNU components and the FEX SMC fix
are preserved by inventory checks. The older mainline binary lock differs from
the full X11 rebuild: this RC is not a clean-CI/mainline-binary-lock acceptance
claim. The final archive checks and source/build identities are recorded in the
artifact manifest. The newly packaged bytes have not undergone another full
on-device game regression. AppGallery signing does not upload to the store.
