#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace steamohos {
// Descriptors are explicitly retained in the GNU process; all others close.
// parentTask runs once in the supervising NCP after fork, receives its child
// PID for session lifecycle coordination, and must not throw.
struct NativeExecOptions {
    std::vector<int> passFds;
    std::string workingDirectory;
    uint32_t outputLimit = 32768; // Clamped to 1–256 KiB by the isolated runner.
    bool persistentSession = false; // Product sessions have no diagnostic lifetime budget.
    bool libcForkOnly = false; // Dedicated single-thread phone supervisor: skip inherited SDK atfork hooks.
    bool traceSyscalls = false; // Explicit diagnostic only; never enabled by default.
    bool traceSignals = false; // Follow this child's threads; preserve delivered signals.
    void (*parentTask)(void*, int childPid) = nullptr;
    // Optional bounded diagnostic in the supervisor, every five seconds and
    // once after exit. Must not throw or manipulate the child. Never runs in GNU TLS.
    void (*parentPoll)(void*, int childPid, bool finished) = nullptr;
    void* context = nullptr;
    int diagnosticFd = -1; // Optional supervisor-only regular-file mirror, rotated at 8 MiB.
};
struct NativeExecResult {
    std::string stage = "not_started";
    std::string output;
    std::string faultDiagnostics;
    int setupError = 0, execError = 0, waitError = 0;
    int exitCode = -1, signal = 0;
    bool timedOut = false, reaped = false, outputTruncated = false;
    bool hostTpUnchanged = false;
    uint64_t elapsedMs = 0;
};
// Kernel exec only. No dlopen of glibc, TP replacement or instruction emulation.
// Call in the isolated diagnostic NCP, never in a Wine or compositor process.
NativeExecResult RunNativeExec(const std::vector<std::string>& argv,
                               const std::vector<std::string>& environment,
                               uint32_t timeoutMs, const NativeExecOptions& options = {});
NativeExecResult RunNativeLoader(const std::vector<std::string>& argv,
                                 const std::vector<std::string>& environment,
                                 uint32_t timeoutMs, const NativeExecOptions& options = {});
std::string NativeExecJson(const NativeExecResult& result);
std::string JsonString(const std::string& text);
}
