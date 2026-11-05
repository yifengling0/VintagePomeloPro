# PE32 WoW64 SEH / SMC smoke

Build `seh_smoke.c` with `i686-w64-mingw32-clang -O2 -msse2` and run under the selected original Wine CPU engine. It writes `C:\windows\temp\box-seh-smoke.txt`; remove the prior result before a run and require a fresh PASS file. It never opens game files or saves.

It verifies that generated-code null read/write faults reach x86 VEH with the correct EIP, GPR, flags, XMM0 and x87 values, then resume. It also allocates guest RWX code, runs it, checks Windows permissions are unchanged by internal SMC protection, edits the return value and runs it again. This exercises real PE32 instructions and the Wine exception route, rather than source-string assertions.

A PASS does not establish game, audio or video compatibility. See `docs/architecture/box64-game-engine-compatibility-20261105.md` for current boundaries.
