# Software WOW64 thread-context regression

Patch 0065 prevents NtGetContextThread from suspending its own caller. It compares
thread objects (including duplicate/opened handles), then asks the CPU backend to
flush the current guest state. Other-thread suspension and access checks remain
in force. This is a runtime fix and does not modify a game.

Build the probe as PE32 and the watchdog as native ARM64 using llvm-mingw:

```sh
i686-w64-mingw32-clang -O2 -Wall wow64_context_probe.c -o wow64_context_probe.exe
aarch64-w64-mingw32-clang -O2 -Wall wow64_context_runner.c -o wow64_context_runner.exe
```

Put both executables in the same Windows directory and run the native watchdog
with FEX enabled. It launches each case in a separate process, waits at most six
seconds, and terminates timed-out children. The suite returns nonzero on failure.
Results overwrite `C:\windows\temp\vp-wow64-context-runner.txt` and
`vp-wow64-context-{0..7}.txt`; verify freshness after each run.

| Mode | Target | Required result |
| --- | --- | --- |
| 0 | Current-thread pseudo handle | Success; suspend count 0 to 0 |
| 1 | Duplicate self handle, GET_CONTEXT only | Success; count 0 to 0 |
| 2 | OpenThread self handle, ALL_ACCESS | Success; count 0 to 0 |
| 3 | Another thread created suspended | Success; count 1 to 1; worker subsequently finishes |
| 4 | Invalid handle | ERROR_INVALID_HANDLE |
| 5 | Duplicate self handle, QUERY_INFORMATION only | ERROR_ACCESS_DENIED; count 0 to 0 |
| 6 | Another live thread waiting on an event | Success; count 0 to 0; worker finishes |
| 7 | Duplicate self handle, ALL_ACCESS | Success; count 0 to 0 |

All eight rows must complete with exit code zero; the watchdog must report
`PASS failures=0`. Count queries use a separate query handle, so GET_CONTEXT-only
access remains covered. No assertions depend on a particular current-thread EIP:
Windows does not promise a useful execution context for that case.

On the unpatched OHOS software-WOW64 runtime, the current-thread pseudo-handle
and OpenThread self-handle cases timed out before GetThreadContext returned. Do not use manual ResumeThread as a product fix.
