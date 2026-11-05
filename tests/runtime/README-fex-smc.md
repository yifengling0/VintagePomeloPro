# FEX dynamic-code block state

`fex_smc_blocks.c` is an independent x86/x64 probe with synthetic instruction
bytes. It needs no game, Mono, Unity assemblies, graphics driver, or save data.

Build with each llvm-mingw compiler:

```sh
i686-w64-mingw32-clang -O2 tests/runtime/fex_smc_blocks.c -o fex-smc-i686.exe
x86_64-w64-mingw32-clang -O2 tests/runtime/fex_smc_blocks.c -o fex-smc-x86_64.exe
```

Run natively as a reference, then in fresh WOW64 / ARM64EC FEX processes with
`FEX_X87REDUCEDPRECISION=0` and `1`. Argument 1 is an optional output filename;
otherwise the report goes to stdout. Delete any old output before each run.

The 1,248 checks per executable cover RX and RWX code, actual code mutation
plus `FlushInstructionCache`, x87 constants at eight TOP positions and three
precision controls, repeated PMOVMSKB and carry/conditional flags. A passing
test must print `errors=0 result=PASS` and exit 0. Do not disable SMC checking
to make this pass; dynamically generated code must remain observable.

Upstream fix: [FEX PR #5618](https://github.com/FEX-Emu/FEX/pull/5618),
commit `b21c49352ea56f23ececbe3c87f0fc681be8024b`.
The production overlay is `scripts/patches/fex-smc-block-state.patch`.
Both Wine FEX builds apply it unconditionally. `fex-simd-zero.json` retains
its established filename but uses repair version 2 to bind the SMC patch and
both DLL hashes. New packaging rejects pre-fix build records; validation can
still read historical repair-version-1 archives.

This fix is separate from the per-game strict x87 compatibility option.
It is not a GPU synchronization fix or a reason to reduce precision globally.
