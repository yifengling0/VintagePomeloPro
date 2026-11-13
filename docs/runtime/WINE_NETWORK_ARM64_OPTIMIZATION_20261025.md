# Wine network safety and native ARM64 crypto, 2026-10-25

## Scope and source identity

This branch is an implementation candidate, not a performance result or a
release approval. The active Wine source is `thirdparty/wine-valve` at
`cd547f7a0ee9d3d59b3ec47317a7852dedf0443b`. Overlay 0066 and 0067 are registered in
`scripts/build_wine.sh`. No submodule pointer changes are required. The legacy
`thirdparty/wine` source is not the patch target.

The native route uses `NATIVE_ARCH=arm64-v8a WINE_ARCH=aarch64`. An x86_64 Wine
or Box64 override is a separate route and must not be presented as native
ARM64 crypto. Final-package ELF inspection and actual loaded-library evidence
are still required.

## P0: non-identifying measurements

`smoke/winehua_platform_network.c` uses `GetTickCount64` for standalone DNS,
TCP connect, WinHTTP send/response/read/total, and total WinINet measurements.
It records status/error, address family, bytes, an FNV-1a response checksum,
and connection reuse observation. It no longer emits DNS numeric addresses.
New Wine TRACE records contain only candidate number, family, elapsed time,
error and cache/reuse state. TRACE is not enabled by default.

Timing definitions matter:

- `tcpConnectMs`: the standalone TCP probe, excluding its address lookup.
- `dnsMs`: WinHTTP resolver callback interval.
- `connectTlsMs`: CONNECTING to CONNECTED callback interval. This Wine version
  emits CONNECTED after TLS, so it is explicitly **not TCP-only**.
- `sendRequestTotalMs`: complete synchronous `WinHttpSendRequest`, potentially including
  DNS, TCP, TLS and proxy setup. It overlaps the callback intervals.
- `ttfbMs`: response-header wait after sending, not a packet-level timestamp.
- `readTotalMs`: body read loop; `totalMs`: complete request probe.
- `reused`: 0 means a new-connect callback was observed, 1 means send succeeded
  without one, 2 means unknown. Family 0 means unavailable, not IPv4.

Intervals are not additive. The checksum is for functional comparison, not
security. No new output includes URLs, hostnames, paths, request/response
headers, credentials, certificate contents, payloads or numeric endpoints.
The WinINet helper's ABI is unchanged; only its overall duration is available
in this file.

## P1: bounded TCP candidates, not HTTP retry

0066 resolves an owned AF_UNSPEC list, stably partitions IPv4 before IPv6 and
deduplicates endpoints (including IPv6 scope). Same-family order is retained.
Asynchronous resolution owns its list until successful transfer; timeout and
failure release it without exposing a worker-owned pointer. Executed ownership
fixtures cover success, timeout followed by worker completion and submit failure.

A cached endpoint is tried once. Only a TCP failure permits one fresh lookup;
that list skips the failed cached endpoint. One monotonic deadline bounds all
finite-timeout attempts, including the fresh-lookup wait. Zero and negative
infinite-timeout conventions remain unchanged. A failed nonblocking-mode switch aborts before connect; a failed restoration
closes the socket rather than returning it. Readiness is followed by
`getsockopt(SO_ERROR)`, not assumed to mean connection success. Only a successful
socket updates the cached endpoint.

The retry helper is called only in `open_connection`, before TLS, proxy HTTP
CONNECT or application request data. TLS/certificate and proxy CONNECT failures
exit without re-entering the candidate loop. The existing send/read layer is
unchanged; structural call-boundary checks show address retries remain pre-send.
This is not an end-to-end injected HTTP send/read failure test.
No connection-pool key, 30-second idle policy, TLS validation, cipher suite,
proxy policy or public API changes are made. This is sequential bounded
fallback, not Happy Eyeballs. A black-holed first endpoint can consume the
remaining budget; this change does not promise a fast family race.

WinINet changes only its timeout conversion to normalized seconds and
microseconds. Its existing multi-address budget and winner promotion remain.

## P2: capability-gated upstream Nettle fat dispatch

`VP_NETTLE_ASM` accepts only `auto` (default) or `off`.

- `aarch64 + auto`: compile **and link** a `sys/auxv.h`, `getauxval(AT_HWCAP)`
  probe with the configured OHOS compiler/sysroot. If successful, configure
  Nettle 3.10.2 with `--enable-assembler --enable-fat`.
- Probe failure, `off`, or x86_64: `--disable-assembler --disable-fat`.
- Invalid input fails before environment setup or downloads.

The patch adapts upstream `fat-arm64.c` to OHOS HWCAP, guarded by `__OHOS__`
(the SDK defines it independently of the build's `__MUSL__` define). AES, PMULL, SHA1 and
SHA2 retain upstream feature checks, dispatch and C implementations. The
OHOS route ignores upstream environment feature forcing. No new assembly
or global `+crypto` flag is introduced. GMP remains `--disable-assembly`.

The shared release extraction is never patched. A version/architecture/patch-digest
private source copy receives a forward/reverse-checked overlay. A completed
build identity records architecture, version, requested/effective mode,
patch digest and target. The top-level dependency skip predicate checks that
identity, so changing `auto`/`off` or the patch does not silently reuse old
libraries. After configure, the build requires `fat-arm64.c`, all AES/GHASH/SHA
fat objects and the matching upstream `arm64/fat` assembly inputs; silent
configure downgrade fails before make. The identity is written only after all
libraries are staged.

A successful probe proves linkability, not actual hardware acceleration.
Build evidence must also inspect `arm64/fat` configuration, crypto/fat objects
and AArch64 ELF headers. Device dispatch and absence of SIGILL remain separate
gates.

## P3: DNS transport and bounds

0067 retains server order, retry count, rcode handling and A/AAAA fallback.
TCP connect validates SO_ERROR. Partial I/O, EINTR and EAGAIN retry against one
CLOCK_MONOTONIC deadline for the entire TCP transaction. UDP TC switches to TCP
on the same server. Socket lengths follow the actual family.

The getaddrinfo synthesizer counts valid records before writing. Required size
is `qlen + count * (12 + RDATA length)`, with overflow/count checks; insufficient
space returns exact required length through `ERROR_MORE_DATA`. ANCOUNT uses
both bytes, and every successful getaddrinfo allocation is freed. DNS wire
names are bounded to 255 bytes before writes; ARCOUNT is explicitly zeroed.

## Reproducible host gates

Run through the repository top-level Makefile:

```sh
ASAN_OPTIONS=detect_leaks=0 \
NETTLE_SRC=/path/to/official/nettle-3.10.2 \
make WINE_SRC=/path/to/pristine/wine-valve test-wine-network-contract
```

Use pristine pinned source, never a shared source already modified by other
builds. `NETTLE_SRC` can be omitted when the official release extraction or
initialized pinned Nettle submodule is present. The existing network-route
test uses the repository's initialized `thirdparty/wine-valve` directly.

The three new tests apply real overlays to disposable copies and compile actual
production helpers with `-Wall -Wextra -Werror -fsanitize=address,undefined`.
They cover closed-port SO_ERROR, candidate ordering/deduplication, finite and
infinite budgets, cache fallback, TLS/proxy failures without candidate restart and pre-send call boundaries, WinINet promotion,
TCP short I/O/EINTR/EAGAIN, UDP TC, address lengths, exact synthesis sizes,
16-bit counts, DNS name limits and header initialization. Nettle tests execute
upstream dispatch with stub backend pointers across 128 HWCAP masks, not ARM
instructions on the host. Shell tests execute actual mode/identity helpers
and the real top-level deps skip predicate.

`detect_leaks=0` is needed in the restricted host environment where LeakSanitizer
cannot inspect processes. AddressSanitizer and UndefinedBehaviorSanitizer remain
enabled. These tests are not an upstream crypto KAT suite or a product build.

The narrow `gnutls` target runs only the existing TLS dependency-chain script,
without building unrelated dependencies or updating the aggregate deps stamp:

```sh
make NATIVE_ARCH=arm64-v8a WINE_ARCH=aarch64 VP_NETTLE_ASM=off gnutls
make NATIVE_ARCH=arm64-v8a WINE_ARCH=aarch64 VP_NETTLE_ASM=auto gnutls
```

Run in an appropriately configured build environment (or via `scripts/vpbuild.sh`).
An SDK API26 build is only a build smoke: it does not establish compatibility
with the product's API23 target or actual HAP loading. Do not change those target
settings to make the smoke pass.

## Validation ledger: 2026-10-25

Status: **implementation and host/API26 build checks passed; not merge-ready,
not production validated. API26 is not the product API23 compatibility gate.**

- PASS: top-level `test-wine-network-contract`, including the existing Linux/OHOS
  route tests and all new production-helper fixtures. ASan/UBSan were enabled;
  LeakSanitizer was disabled for the restricted host environment.
- PASS: full targeted `make gnutls` with `VP_NETTLE_ASM=off`, exit 0, using the
  restored API26 SDK. Recorded identity: `aarch64`, Nettle `3.10.2`, requested
  `off`, effective `off`, target `aarch64-linux-ohos`.
- PASS: full targeted `make gnutls` with `VP_NETTLE_ASM=auto`, exit 0, using the
  same API26 SDK. Recorded identity: requested `auto`, effective `arm64-fat`.
  GNU m4 was supplied as a workspace-only build dependency; ASM generation
  was not bypassed.
- PASS: repeating unchanged `auto` returned exit 0 and took the identity-based
  already-ready skip path.
- PASS: generated Nettle Makefile selected `fat-arm64.c`; the configured fat
  AES/GHASH/SHA assembly inputs and compiled fat/crypto objects were verified.
  `libgnutls.so.30.37.1`, `libnettle.so.8.11` and `libhogweed.so.6.11` are ELF64
  AArch64. Nettle off/auto binaries are distinct (SHA256 prefixes
  `3955d85a446c` and `4f0d2e638755`, respectively).
- PASS: platform-network smoke executables compiled for x86 and x64. Its
  packaging step stopped at a missing guest shader; this is not a successful
  packaged smoke or HAP result.
- PASS: source review, diff/untracked whitespace checks and unchanged gitlink
  ledger. No shared source tree or submodule pointer was changed for this patch.

Local verification logs are under
`.temp/codex-runs/wine-network-arm64-20261025/`: the final aggregate log,
`make-gnutls-api26-off.log`, `make-gnutls-api26-auto-final.log`,
`make-gnutls-api26-auto-skip.log`, `gnutls-off-artifacts.txt`,
`gnutls-auto-artifacts.txt`, and `platform-network-smoke-build.log`.

NOT VERIFIED / BLOCKED: product API23 compilation/loading compatibility, full
Wine/product build, final HAP assembly/inspection, actual loaded-library
identity, device runtime dispatch, crypto known-answer tests, device
DNS/TCP/WinHTTP/WinINet and certificate-positive/negative behavior, SIGILL
absence, and cold/warm performance comparisons. No acceleration percentage or
runtime performance improvement is claimed from these build results.

## Required device/build acceptance before release

1. Build distinct `VP_NETTLE_ASM=off` and `auto` candidates through the top-level
   Makefile / `scripts/vpbuild.sh`, with compatible configured SDK/toolchains.
2. Check packaged ELF architectures and actual loaded GnuTLS/Nettle libraries.
3. Run upstream crypto vectors or equivalent known-answer tests on the target;
   establish runtime dispatch and no SIGILL, including C fallback capability.
4. Run DNS/TCP/WinHTTP/WinINet smoke. Self-signed and wrong-host certificates must
   still fail. Cover IPv4/IPv6, first-candidate failure and cold/warm multi-run
   comparisons, recording errors, status, bytes/checksum and stage timings.
5. Compare functional equivalence before interpreting timing changes. Do not
   quote performance gains without these measurements.

Host gates alone do not make this branch merge-ready. Product build, HAP
inspection, target crypto vectors, certificate behavior and device performance
remain unverified until their actual evidence is recorded.

## Rollback

- Crypto: set `VP_NETTLE_ASM=off` and rebuild dependencies/package through the
  normal top-level entry point. Identity mismatch forces replacement of ASM
  outputs; no blanket `make clean` is required.
- Network overlays: remove the corresponding 0066/0067 registrations, then
  rebuild from a **new clean isolated Wine source and build directory**. Removing
  a registration does not undo an overlay already applied to persistent source.
- Do not reverse/reset a shared or user-edited submodule. Preserve gitlinks,
  existing builds and unrelated worktrees.
