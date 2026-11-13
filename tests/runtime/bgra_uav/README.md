# BGRA storage-image compatibility regression

The probe never opens game resources or save files. It writes only
`C:\\bgra-compat-result.txt` and requires `PASS total=31` with process exit zero.
Compile with an existing llvm-mingw toolchain, for example:

```sh
x86_64-w64-mingw32-clang++ -O2 -std=c++17 -static -Wl,--no-insert-timestamp \
  tests/runtime/bgra_uav/bgra-compat-probe.cpp -ld3d11 -ldxgi -o bgra-compat-probe.exe
```

The checked-in header contains the six DXBC shaders compiled from the adjacent
HLSL sources with D3DCompile, shader model 5.0, entry point `main`. The high-slot
compute shader exercises slot 33. No shader compiler,
network download or game installation is required to run the probe.

On a device without native BGRA storage support, select Legacy DXVK and run the
probe through the normal Wine launcher. It checks five formats and three bind
combinations at the original failing 815x815 size, float/uint clear, upload,
native/emulated copy, compute store/load, pixel store, RGBA/BGRA rebinding,
linear/sRGB SRVs, RTV clear, mip/array clear, deferred commands and UAV slot 33.
The typed-load case requires the actual advertised RGBA typed-load support.

With `DXVK_SHADER_DUMP_PATH`, `DXVK_WINEHUA_DUMP_REMAPPED_SPIRV=1` and state-cache
disabled, validate each final `*.remapped.spv` with
`spirv-val --target-env vulkan1.1`. Pipeline-specific mapping masks must be baked
before driver compilation; the BGRA and RGBA versions of the same shader must
produce their respective byte orders. Disable trace after collecting evidence.

See `docs/graphics/BGRA_UAV_COMPATIBILITY.md` for scope and recorded validation.
