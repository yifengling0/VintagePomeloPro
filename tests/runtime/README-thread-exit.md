# OHOS ARM64 thread-exit regression

`thread_exit_probe.c` exercises Windows thread termination through native Wine,
FEX, and WOW64. Build it with the corresponding llvm-mingw compiler:

```sh
aarch64-w64-mingw32-clang -O2 -Wall -Wextra -static thread_exit_probe.c -o thread-exit-aarch64.exe
x86_64-w64-mingw32-clang -O2 -Wall -Wextra -static thread_exit_probe.c -o thread-exit-x86_64.exe
i686-w64-mingw32-clang -O2 -Wall -Wextra -static thread_exit_probe.c -o thread-exit-i686.exe
```

Run each executable with a mode and a distinct absolute Windows output path,
for example `thread-exit-x86_64.exe forced C:\vp-thread-exit-x64-forced.txt`.

| Mode | Iterations | Thread behavior |
| --- | ---: | --- |
| `normal` | 256 | Return from the thread entry point |
| `exit` | 256 | Call `ExitThread(42)` |
| `forced` | 32 | Terminate a thread waiting in `WaitForSingleObject(INFINITE)` |
| `forced-user` | 32 | Terminate a thread executing a user-mode loop |
| `forced-main` | 1 | A surviving worker terminates the initial thread while it waits |

All modes must log `PASS`, the expected iteration count, and `failures=0`.
The caller checks the Windows wait result and exit code 42. Windows completion
can precede native pthread cleanup, so also check system faults and native Wine
stderr after each run, including a short observation after the PE exits. A PASS
file alone does not prove the native signal-stack cleanup completed safely.

Repeat `forced-main` with a fresh process to cover the initial thread's native
pthread stack. The surviving worker checks exit code 42 and writes PASS; an early
whole-process exit is a failure. This specifically exercises revision 2's main
thread stack range, which differs from the kernel stack supplied to workers.

Patch 0063 switches OHOS ARM64 exits out of the Wine signal stack, disables that
stack before pthread cleanup, and prevents TEB recycling after a failed join.
Revision 2 caches the main pthread stack bounds during process initialization and
checks 16-byte CFA alignment. It adds no libc stack query inside the exit handler.
The invalid-stack and disable-failure guards terminate the affected process;
these guards firing are test failures even without a new SIGSEGV report.

Keep FEX enabled for the x86_64/WOW64 cases. After the probes, verify actual
Steam card cold start, warm activation, library/store operation, and exit/restart.
Classify the reporter's cleanup failures separately from the CEF first fault.

For a paired unsigned HAP, use `scripts/package_thread_exit_runtime.py` to replace
the native library and update the manifest and extraction identity. Both
`runtime_library_guard.py` and `check_runtime_components.py --hap` validate the
recorded library hash, version marker, and any matching nested native libraries.
Library diagnostics identify the intended build; they are not execution proof.
