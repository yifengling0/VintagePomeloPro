# Texture and Vulkan shadow regressions

Build `d3d11_virtual_bc_probe.cpp` with `scripts/build_virtual_bc_probe.py`.
Pass `--compiler-dir` (llvm-mingw bin), `--bcdec-include` (Wine
`dlls/d3dx9_36`), `--arch` (`x86_64`, `i686` or `arm64ec`) and `--output-dir`.
No tools, game assets or dependencies are downloaded.

Run the x64 executable with `--compile` on Windows using Microsoft's
D3DCompiler. Copy the three generated `virtual_bc_*.dxbc` files alongside
each architecture's executable. The same DXBC is used across architectures.
Run `vp_virtual_bc.exe <log-path>` through the selected DXVK profile. Exit zero
and `FINAL failures=0` are both required. The full run compares 437,760
compute/raster samples against independent CPU addressing/filter calculations
and Wine's BC decoder. `--transfer-only --repeat=6` exercises six same-device
cycles; `--wide-only` checks logical 16K creation without claiming rendering.
`WINEHUA_DXVK_VIRTUAL_BC=1` is a qualification override, not a product default.
For native BC hardware compare `--native-capture` binary outputs in automatic
and disabled (`WINEHUA_DXVK_VIRTUAL_BC=0`) modes byte-for-byte.

Build the independent raw Vulkan regression with
`scripts/build_vulkan_shadow_probe.py --llvm-mingw <root> --arch arm64ec
--output <dir>` (also supports `x86_64`). Its arguments are
`<log-path> [MiB=96] [submit2=0] [rounds=1] [map-before-bind=0]`.
It checks CPU input and the complete source/destination buffers for buffer
copies, update commands and image round trips. Require process exit zero and
`NATIVE-ISOLATION FINAL success`. Map-before-bind is a legal Vulkan order and
must pass as well as bind-before-map.

Runtime identity is pinned in `docs/graphics/texture-compat-runtime.lock.json`.
`scripts/package_texture_compat_runtime.py` stages reviewed DLLs and the paired
top-level/nested Venus payload into a rawfile directory, updates both nested
manifests and the extraction content marker, and retains unrelated runtime
members. HAP assembly/signing and device validation are separate steps.
See `docs/graphics/VIRTUAL_BC_RUNTIME_VALIDATION_20261017.md` for exact tested
artifacts and remaining acceptance limits.
