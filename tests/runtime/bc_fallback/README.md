# Partial BC format support

Build the PE64 fixture with the existing llvm-mingw toolchain:

```sh
x86_64-w64-mingw32-clang++ -O2 -std=c++17 -static \
  -Wl,--no-insert-timestamp -Itests/runtime/bgra_uav \
  tests/runtime/bc_fallback/bc-probe.cpp -ld3d11 -ldxgi -o bc-probe.exe
```

Run through the normal OHOS Wine launcher with Legacy DXVK. The fixture writes
only `C:\bc-probe-result.txt`, never opens a game or save, and must finish with
`complete=1 failures=0`. `bc_volume_shader.h` was generated from the adjacent
shader with Windows D3DCompile, entry `main`, profile `cs_5_0`, optimization 3.
The 2D reader reuses the checked-in BGRA fixture's `reader_dxbc`.

Checks include ten concrete BC formats at 64x64 with seven mips, five typeless
32x32x32 BC volumes, an initialized BC1 volume whose four differently colored
depth slices are sampled on the GPU, and BC5 UNORM/SNORM GPU sampling. Signed
channels, missing channels, alpha, non-finite values, and byte/depth pitches are
verified. Texture creation alone does not qualify decoding correctness.

On Edge's tested DXVK route, the aggregate BC bit was true while BC4--BC7
2D image queries failed. BC1 2D succeeded, but the game's BC1 volume failed.
The managed format-table overlay checks each family, both UNORM/SNORM or sRGB
views, concrete/mutable images, and 2D/3D support once at adapter initialization.
A partial family uses the existing upload-time decoder and coherent backing
format family for all its resources. This deliberately favors consistent view
and copy representation over retaining only a subset of native dimensions.
Full-capability families remain native. `WINEHUA_DXVK_BC_EMULATION=0` retains
the diagnostic opt-out; no support or feature level is fabricated.

Mapped/RT/UAV/shared BC emulation remains rejected by the existing explicit
checks. No per-game rule, texture resizing, or lossy recompression was added.
Fallback can increase memory use. This fixes capability selection, not the
separate UE4 map performance and rendering issues.
