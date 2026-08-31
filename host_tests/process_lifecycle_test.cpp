#include "proc/wine_process.h"
#include "wine/wineboot_wait.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

napi_threadsafe_function gStateTsfn = nullptr;
std::string gSockPath;
static int checks;

static void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::_Exit(1);
    }
}

static pid_t ExitedChild(int code, int signal = 0) {
    pid_t child = fork();
    Check(child >= 0, "fork succeeds");
    if (child == 0) {
        if (signal) raise(signal);
        _exit(code);
    }
    AddProcess(child, "C:/windows/system32/wineboot.exe", -1);
    siginfo_t info{};
    int result;
    do { result = waitid(P_PID, child, &info, WEXITED | WNOWAIT); }
    while (result < 0 && errno == EINTR);
    Check(result == 0, "child exited but wait status remains available");
    return child;
}

static WineProcessEntry Snapshot(pid_t pid) {
    WineProcessEntry entry{};
    Check(QueryProcessSnapshot(pid, &entry), "registered child exists");
    return entry;
}

int main() {
    const winehua::WinebootAttempt failedAttempt(GetProcessListSnapshot());
    const pid_t failed = ExitedChild(1);
    errno = EBUSY;
    sigchld_handler(SIGCHLD);
    const int handlerErrno = errno;
    auto entry = Snapshot(failed);
    Check(!entry.running && entry.exitCode == 1 && entry.exitCodeSource == "waitpid",
          "real failed child wait status survives registry publication");
    Check(handlerErrno == EBUSY, "SIGCHLD processing preserves errno");
    auto progress = failedAttempt.Inspect(GetProcessListSnapshot(), -1);
    Check(progress.failedPid == failed && progress.exitCode == 1,
          "failed boot worker prevents completion even after it disappears");
    Check(progress.workerObserved && !progress.workerRunning,
          "worker that exited between polls is still observed");
    const uint64_t endedAt = entry.endTimestampMs;
    RemoveProcess(failed);
    entry = Snapshot(failed);
    Check(entry.exitCode == 1 && entry.exitCodeSource == "waitpid" &&
          entry.endTimestampMs == endedAt, "late unknown exit cannot erase waitpid result");

    int pipeFds[2];
    Check(pipe(pipeFds) == 0, "reader pipe created");
    close(pipeFds[1]);
    ReaderThread(pipeFds[0], failed, std::make_shared<std::atomic<bool>>(true));
    Check(Snapshot(failed).exitCode == 1, "second waitpid consumer preserves reaped status");
    AddProcess(failed, "@engine/wineboot", -1, "@engine/wineboot");
    entry = Snapshot(failed);
    Check(!entry.running && entry.exitCode == 1 && entry.endTimestampMs == endedAt &&
          entry.sessionId == "@engine/wineboot", "engine relabel does not resurrect exited launcher");
    Check(failedAttempt.Inspect(GetProcessListSnapshot(), failed).failedPid == failed,
          "failed launcher remains visible after engine relabel");

    const winehua::WinebootAttempt nextAttempt(GetProcessListSnapshot());
    Check(nextAttempt.Inspect(GetProcessListSnapshot(), -1).failedPid == 0,
          "old failed boot does not poison next attempt");
    const pid_t signaled = ExitedChild(0, SIGTERM);
    sigchld_handler(SIGCHLD);
    entry = Snapshot(signaled);
    Check(entry.exitCode == 128 + SIGTERM && entry.exitCodeSource == "signal",
          "signal termination is a known failure");
    Check(nextAttempt.Inspect(GetProcessListSnapshot(), signaled).failedPid == signaled,
          "signaled boot also prevents completion");

    const winehua::WinebootAttempt successAttempt(GetProcessListSnapshot());
    const pid_t success = ExitedChild(0);
    Check(pipe(pipeFds) == 0, "successful reader pipe created");
    close(pipeFds[1]);
    ReaderThread(pipeFds[0], success, std::make_shared<std::atomic<bool>>(true));
    entry = Snapshot(success);
    Check(!entry.running && entry.exitCode == 0, "reader owns a successful wait status");
    progress = successAttempt.Inspect(GetProcessListSnapshot(), success);
    Check(progress.failedPid == 0 && progress.workerObserved &&
          !progress.workerRunning && !progress.launcherRunning,
          "successful boot can complete despite earlier failures");

    auto before = GetProcessListSnapshot();
    winehua::WinebootAttempt reuseAttempt(before);
    entry = before.front();
    entry.startTimestampMs -= 1; // distinct incarnation, even if wall clock moved backwards
    entry.exeBasename = "WINEBOOT.EXE";
    entry.running = false;
    entry.exitCode = 2;
    Check(reuseAttempt.Inspect({entry}, -1).failedPid == entry.pid,
          "reused PID and case-insensitive worker name are recognized");
    entry.exeBasename = "notepad.exe";
    Check(reuseAttempt.Inspect({entry}, -1).failedPid == 0,
          "unrelated failed application is not a boot failure");
    entry.exeBasename = "wineboot.exe";
    entry.exitCode = -1;
    Check(reuseAttempt.Inspect({entry}, -1).failedPid == 0,
          "unknown NCP exit is not invented into a known failure");
    entry.running = true;
    progress = reuseAttempt.Inspect({entry}, -1);
    Check(progress.workerRunning && progress.workerObserved && !progress.failedPid,
          "live worker continues waiting");
    std::printf("process lifecycle: %d checks PASS (real fork/waitpid + production registry)\n", checks);
    std::fflush(stdout);
    // The production monitor is intentionally detached for the app lifetime.
    std::_Exit(0);
}
