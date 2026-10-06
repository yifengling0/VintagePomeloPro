# Mesa 26.2.4 OHOS experimental port

## Fixed source identities

- Superproject base: `284026ce64c631961394f2aee6fadb3a4380e579`
- Previous Mesa gitlink: `330124bf18f8e135346c3f592c8f8d99121fd343`
- Official Mesa target: annotated tag `mesa-26.2.4`, peeled commit
  `96cb43121031992b85767f9c1be8f3f48e22b1d2`
- Experimental branches: `codex/mesa-26.2.4-port` and
  `codex/mesa-26.2.4-ohos-port`

The WineHua tree and official Mesa 25.0.1 share the older
`373b232675b72dfa9dac331c15123ca790c1f990` (22.3-devel) merge base. The port
therefore uses a final-content semantic inventory rather than replaying the
898 historical pre-`e5d8c3f` commits or only the final 17 commits.

The complete 116-path inventory has these dispositions:

- 27 ported runtime/build paths
- 6 upstream-equivalent paths
- 83 paths dropped because they are outside this project's explicit Wayland
  VirGL and Venus runtime closure
- 0 unclassified paths

Notably excluded are the old native-OHOS EGL platform, Zink/kopper and blend
patch train, bundled Vulkan registry/header changes, GN/OAT integration, old
OHOS helper scripts and deleted Rust wrap files. Mesa 26 already supplies the
required registry definitions, vtest-only build organization and stronger
ring publication ordering.

## Ported behavior

- WineHua GL Present protocol v4 and host-feedback pacing
- WineHua Vulkan Present protocol v1 with ring drain before private present
- Remote vtest shadow-memory flush/invalidate transport
- Persistent-mapping tracking and opt-in pre-submit publication
- Required ring notification behavior and default-off correlation diagnostics
- Mesa 26 `MapMemory2`/`UnmapMemory2`, queue, renderer-submit and sync ownership
  adaptations
- Explicit Wayland platform compilation with runtime display detection
- Isolated top-level GL and Vulkan build entry points

The old `VN_WINEHUA_STRONG_RING_BARRIER` switch is deliberately absent. Mesa
26.2.4 publishes ring tail with unconditional `memory_order_seq_cst`, which is
stronger and makes the old opt-in fence redundant.

## Prerequisites

- HarmonyOS/OpenHarmony native SDK with Clang and sysroot for the requested
  architecture
- Meson 1.4 or newer, Ninja, Python 3, Mako and pkg-config
- `wayland-scanner` and wayland-protocols 1.41 or newer
- Fresh, absolute and mutually distinct Mesa source, build and install roots
- Initialized pinned dependency submodules required by the existing helpers

The Mesa-only entry points intentionally skip unrelated LLVM-mingw acquisition
and ARM64EC dual-ABI validation.

## Native build commands

Use new absolute directories for every candidate:

```sh
make mesa-guest-gfx \
  MESA_SOURCE_ROOT=/absolute/path/mesa-26.2.4-ohos \
  MESA_BUILD_ROOT=/absolute/path/build/mesa26-gl \
  MESA_INSTALL_ROOT=/absolute/path/install/mesa26-gl

make mesa-guest-vulkan \
  MESA_SOURCE_ROOT=/absolute/path/mesa-26.2.4-ohos \
  MESA_BUILD_ROOT=/absolute/path/build/mesa26-vulkan \
  MESA_INSTALL_ROOT=/absolute/path/install/mesa26-vulkan
```

The GL target builds `-Dplatforms=wayland` in virpipe mode. It does not force
`egl-native-platform=wayland`: Mesa 26 detects Wayland displays at runtime and
the graphics broker sets `EGL_PLATFORM=wayland`.

## Verification status

Passed host-only structural checks:

- Fresh full Wayland/EGL/GLES2 GL build and link, including
  `platform_wayland.c`, `dri_screen.c`, VirGL, vtest and state tracker changes
- Fresh Venus build/link and every modified Venus object
- Private protocol field/version/layout guard
- Source/build/install isolation and Mesa-only environment guard
- Existing GPU ownership, zero-copy binding lifecycle, EGL multi-consumer and
  Steam GPU contract tests
- Diff hygiene and local recovery-patch apply checks

These checks prove source and link structure on the host. They do not prove an
OHOS binary or runtime behavior.

Not run in this environment:

- Native ARM64 OHOS GL and Venus compilation
- Packaging staging and target ELF dependency/loader closure
- Device EGL/GL capability and no-software-fallback checks
- RichMan8, War3, Kingdom Rush, multiwindow and lifecycle scenarios
- Vulkan coherent/non-coherent shadow mapping, fence/timeout, ring reuse,
  private-present resize, BC-format, DXVK and Heaven tests
- Repeated FD/RSS and runtime loaded-object verification

## Known limitations and adoption gate

Persistent coherent pre-submit publication preserves the fork's final
classification policy: an allocation is treated as CPU-written only after an
explicit `vkFlushMappedMemoryRanges` observation. Legal coherent CPU writes do
not have to call that function, so VKD3D/device evidence is required before
claiming the causal chain is complete.

Private Vulkan present drains renderer decode and orders the host QueueSubmit
call. It does not claim GPU completion or actual display completion.

This branch is an experimental source candidate. Native compilation and all
device/runtime items above must be recorded before adoption, merge or default
gitlink replacement. The previous Mesa gitlink and its complete matching
artifacts remain the rollback baseline.

## Experimental publication order

Native OHOS compilation is explicitly deferred and is **NOT RUN** for this
publication. Publication creates isolated experimental branches only; it does
not authorize a pull request, merge, default-branch mutation or release.

1. Commit and push `codex/mesa-26.2.4-ohos-port` first without force.
2. Verify the remote branch resolves to the exact local Mesa commit and that
   the target tag remains the ancestor:

   ```sh
   git fetch origin refs/heads/codex/mesa-26.2.4-ohos-port
   test "$(git rev-parse HEAD)" = \
        "$(git rev-parse origin/codex/mesa-26.2.4-ohos-port)"
   git merge-base --is-ancestor \
     96cb43121031992b85767f9c1be8f3f48e22b1d2 HEAD
   ```

3. Update the superproject `thirdparty/mesa` gitlink to that exact, remotely
   reachable Mesa commit. Commit the gitlink together with these build scripts,
   tests and documentation on `codex/mesa-26.2.4-port`.
4. From a fresh detached checkout, verify the pinned submodule resolves:

   ```sh
   git submodule sync -- thirdparty/mesa
   git submodule update --init -- thirdparty/mesa
   test "$(git -C thirdparty/mesa rev-parse HEAD)" = \
        "$(git ls-tree HEAD thirdparty/mesa | awk '{print $3}')"
   ```

5. Push only `codex/mesa-26.2.4-port` without force and verify its remote SHA
   with the same `fetch`, `rev-parse` and `test` pattern. Do not change
   `.gitmodules`, `feature/main_proton`, `main`, or either repository's default
   branch.
