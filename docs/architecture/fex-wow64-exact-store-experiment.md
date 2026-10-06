# WOW64 FEX exact-store experiment

This is a reversible, default-off x87 experiment for `libwow64fex.dll`. It
does not apply to `libarm64ecfex.dll`, PE64, or Linux builds.

## Build identities

`scripts/build_fex.sh` always applies the source overlay, but only its
`fex-pe` CMake invocation defines `FEX_WOW64_EXACT_STORE=1`. The guarded code
is therefore compiled out of the ARM64EC DLL. Existing CMake caches are
invalidated if that identity changes.

For clean timing builds, run `scripts/build_fex.sh` normally. For one-off hit
ratio collection, build with `FEX_EXACTSTORE_DIAGNOSTICS=1`; this applies the
separate diagnostics overlay and adds
`FEX_WOW64_EXACT_STORE_DIAGNOSTICS=1`. Diagnostic DLLs are not timing DLLs.

Record the SHA256 and PE architecture of both DLLs after every build. Do not
mix diagnostic and clean artifacts in one A/B result.

## Runtime controls

- `FEX_EXACTSTORE=1` enables the exact-normal fast path
- unset, empty, `0`, and every other value keep the original helper path
- in a diagnostic build, `FEX_EXACTSTORE_STATS=1` adds relaxed atomic
  attempt/hit/fallback counters and emits one bounded F32/F64 summary at
  process teardown

Use the verified pre-Context automation route. For ON, launch with
`automation/Start-WineHuaGameTest.ps1 -GamePath <path> -D3DEnvironment @{
FEX_EXACTSTORE='1' }`. With a diagnostic DLL, add
`FEX_EXACTSTORE_STATS='1'` to that map. The script serializes URL-encoded
`winehua.d3d_env_json`; `GameHook` validates these two exact keys,
`buildEnvironment()` passes them to `runWineProgram`, and native
`policy.extraEnv` installs them in the child environment before FEX constructs
its Context. For OFF, omit `FEX_EXACTSTORE` or pass `0`.

Stop the full VPP/Wine process tree and launch a fresh process for every
OFF/ON cell (the automation script already performs that stop). Changing an
environment variable after launch does not change existing Contexts or
already-generated handlers.

Use clean processes in interleaved OFF/ON order. Use a diagnostic DLL only to
measure hit ratio. Native Windows ARM64 build, disassembly, sticky-IE sequence
probe, HAP packaging/signing, device execution, and game A/B remain local
validation gates.

Build the reusable sticky-IE guest probe with an i686 MinGW compiler, for
example `i686-w64-mingw32-gcc -O2 -Wall -Wextra -o
fex_exact_store_sequence_probe.exe host_tests/fex_exact_store_sequence_probe.c`.
Run it natively on x86 and under a fresh WOW64 process for unset, `0`, and `1`.
Compare every printed result bit pattern, IE-before/after value, and EFLAGS
value; IE must be 1 before and after each accepted exact store.

## Host verification record (2026-10-06)

The pinned FEX source was staged out of tree, the product's seven existing
compatibility overlays were applied, then the exact-store overlays were
replayed. Using the official `llvm-mingw-20260826` toolchain (Clang 23.1.0),
CMake 3.31.10 and Ninja successfully built target `wow64fex` in
`RelWithDebInfo` with
`MINGW_TRIPLE=aarch64-w64-mingw32`, `BUILD_TESTING=False`, and
`ENABLE_LTO=False`, matching `scripts/build_fex.sh`:

- Clean experiment, `CMAKE_CXX_FLAGS=-DFEX_WOW64_EXACT_STORE=1`:
  COFF-ARM64 `libwow64fex.dll`, SHA256
  `d9b068d75c815f7434f69a3b5bef15a7c4c42ac8c90b804b1b2d883536284bd3`;
  the `FEX_EXACTSTORE` marker is present
- Diagnostic experiment, adding
  `-DFEX_WOW64_EXACT_STORE_DIAGNOSTICS=1`: COFF-ARM64
  `libwow64fex.dll`, SHA256
  `fde5c5380687489685181c87eb1cb59ae46c50a6050513a4210f2f3543a58328`;
  the stats option, summary format string, and counter symbol are present
- No-definition guard build, empty `CMAKE_CXX_FLAGS`: COFF-ARM64
  `libwow64fex.dll`, SHA256
  `cfa8fca0d19a767a0476a3a4139e5f38620227f76310013e1592e0a4da914924`;
  exact-store and counter markers are absent
- The same toolchain compiled the probe as PE32/i386:
  `fex_exact_store_sequence_probe.exe`, SHA256
  `aa234ac3e3bb8660d7bbce25ecbb91adf51c3b1d8e129683472b0d29ebfc78bb`

The portable verification command `make test-fex-exact-store` passed, as did
`bash -n scripts/build_fex.sh` and `git diff --check`. These are compilation,
overlay, oracle, and structural results. The compiled `GenerateABICall`
disassembly contains the `0x3f81` and `0x3c01` gate constants in the clean
experiment and excludes the experiment in the no-definition control. This is
static emitter evidence, not a device JIT capture. The PE32 probe was not
executed. HAP packaging/signing, device execution/JIT capture, native-x86 and
WOW64 probe comparison, and clean interleaved game A/B remain **NOT RUN**.

For a normal local rebuild, run `scripts/build_fex.sh`; for a separate
diagnostic artifact, run `FEX_EXACTSTORE_DIAGNOSTICS=1
scripts/build_fex.sh`. The focused host verification used `cmake --build
<build> --target wow64fex -j 8`. The probe compile command was
`i686-w64-mingw32-clang -O2 -Wall -Wextra -Werror -o
fex_exact_store_sequence_probe.exe host_tests/fex_exact_store_sequence_probe.c`.
