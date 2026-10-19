# Generic Wine/FEX runtime probes

These tests contain no game-name compatibility policy. Build the native probes
with the repository Makefile inside the configured Docker container:

    make simd-zero-probes WINE_ARCH=aarch64 NATIVE_ARCH=arm64-v8a BUILD_DIR=<build>

The SIMD binaries test architectural lane values, a previous zero overwritten
by MOVSS, repeated execution and non-self XOR. AVX is explicitly skipped when
CPUID/XCR0 does not advertise support. The x64 JIT probe tests RX/RWX and changed
constants; its four rows of six floats are also written to C:\vp-dynamic-fp.bin.

The ICU probe tests aliases, initialization, collation, conversion, case mapping,
and every advertised locale against ICU canonicalization and Windows NLS SNAME.
An optional CRT DLL path is explicitly loaded before ICU; this alone is not
proof that every ICU import binds to that CRT. Results overwrite C:\vp-icu-probe.txt;
check the timestamp after every launch. The ICU probe currently reproduces 24
NLS mismatches on the reviewed Wine runtime and deliberately returns nonzero.

The globalization project is an independent .NET6 executable. Publish it with
the .NET SDK for win-x64, transfer the self-contained runtime without modifying
any game files, and run VpCultureProbe.exe with no globalization environment
overrides. C:\vp-culture-result.txt records independent checks even after a
failure. Full culture enumeration currently fails because Windows CultureData
cross-checks ICU names against GetLocaleInfoEx; the other checks pass on the
reviewed tablet. Do not remove this assertion or filter cultures to report success.

See docs/graphics/MAIN_PROTON_MIGRATION.md and the local 2026-10-16 integration
report for the exact source, package, device evidence and remaining limits.
