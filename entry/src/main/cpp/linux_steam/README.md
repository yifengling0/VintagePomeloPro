# Linux ARM64 Steam host diagnostics

This directory now also contains the native GNU execution backend, Xwayland
integration and Linux Steam API bridge. The sections below describe the original
phase-1/phase-2 probes; current integration and build entry points are documented
in [source integration](../../../../../docs/linux-steam/SOURCE_INTEGRATION.zh-CN.md).

The HOST diagnostics verify the existing OHOS Wayland host. That probe does
not load Linux ELFs, start Xwayland or run Steam. Build it explicitly with
`-DWINEHUA_ENABLE_STEAMOHOS_HOST=ON`. The CMake option defaults to OFF;
the Linux Steam branch's `entry/build-profile.json5` enables it and GNU exec.

`host_v1/` is an unchanged snapshot of the registry/ABI experiment from
`steam-ohos-lab`. Its provenance and SHA-256 hashes are in
`host_v1/provenance.json`. `surface_probe.cpp` adds a real Wayland client using
the product's existing client library and generated xdg protocol.

The async smoke NAPI runs registry/sync discovery, xdg configure/ack, alternating
SHM frames, buffer releases, frame callbacks and a synthetic keyboard sequence.
It uses an explicitly captured application socket, owns its descriptors and
never changes WAYLAND_DISPLAY or the Wine launch environment.

The keyboard sequence runs only while the probe's exact live toplevel has
keyboard focus. In desktop mode activate the probe by clicking its checkerboard.
An inactive window is not a valid target for `SendKeyEvent`, even when its
toplevel ID is known. No synthetic key is sent merely to acquire focus.

`framePresented`, `gnuExecutionVerified` and `steamClientStarted` are always
false in the protocol result. A frame callback is not proof of display scanout.
Visible output needs separate screenshot/device evidence.

Run Linux reference tests from the repository root:

```sh
python3 host_tests/linux_steam_host_test.py
```

They require a C/C++ compiler and the Linux Wayland client/server development
libraries. The reference server is a fixture, not the OHOS compositor.

See [the implementation and evaluation report](../../../../../docs/linux-steam/HOST_PHASE_1.zh-CN.md)
for build/launch instructions, device findings and the next execution gates.

## Native GNU execution (separate opt-in)

`-DWINEHUA_ENABLE_STEAMOHOS_GNU_EXEC=ON` builds the isolated NCP execution
probe (`linux-steam-gnu`). This is real native AArch64 execution: signed
file-backed GNU rtld, paired glibc and GNU ELF fixtures, without QEMU or an
instruction interpreter. The GNU process owns its TLS and never calls back
into the OHOS libc. The supervising NCP keeps its original TP.

The tablet passed main, local/initial/dynamic TLS, two pthreads, errno, dlvsym,
pthread key destructors, exit status and timeout/reaping. Steam has not been
started; the HOST protocol probe's historical false flags remain unchanged.
See [native phase 2](../../../../../docs/linux-steam/NATIVE_PHASE_2.zh-CN.md)
for actual limitations, glibc source/patch provenance, packaging, evidence and
the separate GNU Wayland, Xwayland, CEF and Vulkan gates.
