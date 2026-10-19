#include "proc/wine_process.h"
#include "wine/wineboot_wait.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>

napi_threadsafe_function gStateTsfn = reinterpret_cast<void*>(1);
std::string gSockPath;
static int checks;
std::atomic<bool> ProcessTestPhoneMode{true};
static unsigned ncpReentries = 0;
int ProcessTestNcpKill(int32_t pid) {
    // System callbacks may synchronously query process state. A registry lock
    // held across NCP termination would deadlock this real implementation.
    WineProcessEntry entry;
    if (QueryProcessSnapshot(pid, &entry)) ++ncpReentries;
    return -1; // exercise the POSIX fallback too
}

static std::atomic<pid_t> hiddenPid{-1};
static constexpr pid_t syntheticClient = 99999001;
static pid_t syntheticParent = 0;
static std::atomic<bool> blockAncestry{false}, ancestryEntered{false}, ancestryReleased{false};
extern "C" FILE* __real_fopen(const char*, const char*);
extern "C" FILE* __wrap_fopen(const char* path, const char* mode) {
    const std::string hiddenPath = "/proc/" + std::to_string(hiddenPid.load()) + "/stat";
    if (syntheticParent && std::string(path) == "/proc/" + std::to_string(syntheticClient) + "/stat") {
        if (blockAncestry.load()) {
            ancestryEntered = true;
            while (!ancestryReleased.load()) usleep(1000);
        }
        FILE* file = tmpfile();
        std::fprintf(file, "%d (window-client) S %d 0\n", syntheticClient, syntheticParent);
        rewind(file);
        return file;
    }
    if (hiddenPath == path) { errno = ENOENT; return nullptr; }
    return __real_fopen(path, mode);
}
void WineHuaCefUtilityProbeNoteExit(int32_t, int32_t) {}

static pthread_t mainThread;
static std::atomic<int> callbacks{0};
static std::atomic<int> failures{0};
static std::atomic<int> stopped{0};
static std::atomic<bool> callbackOnMain{false};
static std::atomic<bool> interruptRegistration{false};
static std::atomic<int> registrationInterrupts{0};

int ProcessTestStateCallback(void* data) {
    if (!std::strcmp(static_cast<char*>(data), "state:failed:wineserver")) ++failures;
    if (!std::strcmp(static_cast<char*>(data), "state:stopped")) ++stopped;
    const bool exitEvent = std::strstr(static_cast<char*>(data), "evt:proc-exited:") != nullptr;
    if (exitEvent && pthread_equal(pthread_self(), mainThread)) callbackOnMain = true;
    // Reenter the public registry API: notification must hold no registry lock.
    GetProcessListSnapshot();
    std::free(data);
    if (exitEvent) ++callbacks;
    return 0;
}

void ProcessTestLog(const char* format) {
    if (std::strstr(format, "[ProcReg] add") && interruptRegistration.exchange(false)) {
        // The production registration log runs with gProcMutex held. Deliver
        // a real signal here to prove the handler does not take that lock.
        raise(SIGCHLD);
        ++registrationInterrupts;
    }
}

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
    siginfo_t info{};
    int result;
    do { result = waitid(P_PID, child, &info, WEXITED | WNOWAIT); }
    while (result < 0 && errno == EINTR);
    Check(result == 0, "child exited but wait status remains available");
    interruptRegistration = true;
    AddProcess(child, "C:/windows/system32/wineboot.exe", -1, ProcessRegistration::ForkChild);
    return child;
}

static void AwaitExit(pid_t pid) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    WineProcessEntry entry{};
    do {
        if (QueryProcessSnapshot(pid, &entry) && !entry.running && !entry.waitPending) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    Check(false, "registered child exit published before deadline");
}

static WineProcessEntry Snapshot(pid_t pid) {
    WineProcessEntry entry{};
    Check(QueryProcessSnapshot(pid, &entry), "registered child exists");
    return entry;
}

int main() {
    mainThread = pthread_self();
    Check(EnsureChildReaper(), "process reaper starts");
    // Emulate phone sandbox visibility for a genuinely live non-waitable
    // child. Neither its registry nor wineboot wait may infer exit from ENOENT.
    const pid_t hidden = fork();
    Check(hidden > 0 || hidden == 0, "hidden child fork succeeds");
    if (hidden == 0) { for (;;) pause(); }
    hiddenPid = hidden;
    AddProcess(hidden, "C:/hidden.exe", -1, ProcessRegistration::NativeChild);
    Check(IsProcessAliveNotZombie(hidden), "phone /proc ENOENT is unknown, not dead");
    Check(!IsLaunchChildExited(hidden), "unreadable phone child does not finish wineboot wait");
    usleep(1100000);
    Check(Snapshot(hidden).running, "ProcMon retains a live sandbox-hidden child");
    kill(hidden, SIGKILL);
    int hiddenStatus = 0;
    Check(waitpid(hidden, &hiddenStatus, 0) == hidden, "non-waitable owner retains wait status");
    std::thread([&] { NotePhoneForkServerChildExit(hidden, hiddenStatus); }).join();
    Check(IsLaunchChildExited(hidden) && !Snapshot(hidden).running,
          "authoritative server exit completes hidden child wait");
    hiddenPid = -1;
    const winehua::WinebootAttempt failedAttempt(GetProcessListSnapshot());
    const pid_t failed = ExitedChild(1);
    errno = EBUSY;
    raise(SIGCHLD);
    const int handlerErrno = errno;
    AwaitExit(failed);
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
    AddProcess(failed, "@engine/wineboot", -1);
    entry = Snapshot(failed);
    Check(!entry.running && entry.exitCode == 1 && entry.endTimestampMs == endedAt &&
          entry.exeFullPath == "@engine/wineboot", "engine relabel does not resurrect exited launcher");
    Check(failedAttempt.Inspect(GetProcessListSnapshot(), failed).failedPid == failed,
          "failed launcher remains visible after engine relabel");

    const winehua::WinebootAttempt nextAttempt(GetProcessListSnapshot());
    Check(nextAttempt.Inspect(GetProcessListSnapshot(), -1).failedPid == 0,
          "old failed boot does not poison next attempt");
    const pid_t signaled = ExitedChild(0, SIGTERM);
    AwaitExit(signaled);
    entry = Snapshot(signaled);
    Check(entry.exitCode == -1 && entry.exitCodeSource == "signal",
          "host signal is not fabricated into a Windows exit code");
    Check(nextAttempt.Inspect(GetProcessListSnapshot(), signaled).failedPid == 0,
          "Wine cleanup signals alone do not establish boot failure");

    const winehua::WinebootAttempt successAttempt(GetProcessListSnapshot());
    const pid_t success = ExitedChild(0);
    Check(pipe(pipeFds) == 0, "successful reader pipe created");
    close(pipeFds[1]);
    ReaderThread(pipeFds[0], success, std::make_shared<std::atomic<bool>>(true));
    AwaitExit(success);
    entry = Snapshot(success);
    Check(!entry.running && entry.exitCode == 0, "reaper owns exit status independently of reader EOF");
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

    // Other modules' synchronous helper children are not ours to reap.
    const pid_t unowned = fork();
    Check(unowned >= 0, "unowned helper fork succeeds");
    if (unowned == 0) _exit(42);
    siginfo_t info{};
    Check(waitid(P_PID, unowned, &info, WEXITED | WNOWAIT) == 0, "unowned helper becomes waitable");
    for (int i = 0; i < 5; ++i) {
        Check(EnsureChildReaper(), "recreation shares the existing reaper");
        raise(SIGCHLD);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int status = 0;
    Check(waitpid(unowned, &status, 0) == unowned && WIFEXITED(status) && WEXITSTATUS(status) == 42,
          "SIGCHLD worker leaves unrelated wait status to its owner");

    // A child can close stdout before exit. Its reader must not block waiting
    // for the child, and only the reader may close/release the descriptor.
    int gate[2];
    Check(pipe(pipeFds) == 0 && pipe(gate) == 0, "stdout and child gates created");
    const pid_t live = fork();
    Check(live >= 0, "live child fork succeeds");
    if (live == 0) {
        close(pipeFds[0]);
        close(pipeFds[1]);
        close(gate[1]);
        char byte;
        while (read(gate[0], &byte, 1) < 0 && errno == EINTR) {}
        _exit(7);
    }
    close(gate[0]);
    close(pipeFds[1]);
    AddProcess(live, "C:/test.exe", pipeFds[0], ProcessRegistration::ForkChild);
    auto active = Snapshot(live).readerActive;
    std::thread([&] { RemoveProcess(live); }).join(); // observer does not close stdout
    Check(fcntl(pipeFds[0], F_GETFD) >= 0, "status observer does not close reader-owned fd");
    std::atomic<bool> readerDone{false};
    std::thread reader([&] { ReaderThread(pipeFds[0], live, active); readerDone = true; });
    for (int i = 0; i < 100 && !readerDone; ++i) usleep(2000);
    Check(readerDone, "stdout EOF returns without waiting for live child");
    reader.join();
    const int replacement = open("/dev/null", O_RDONLY);
    Check(replacement >= 0, "descriptor replacement opened after reader closes");
    Check(replacement == pipeFds[0], "replacement actually reuses the reader descriptor number");
    Check(write(gate[1], "x", 1) == 1, "release live child");
    close(gate[1]);
    AwaitExit(live);
    Check(Snapshot(live).exitCode == 7, "known exit replaces early unknown observation");
    Check(fcntl(replacement, F_GETFD) >= 0, "reaper does not close replacement descriptor");
    close(replacement);
    AddProcess(live, "Z:/renamed/test.exe", -1);
    Check(Snapshot(live).exitCode == 7 && !Snapshot(live).running,
          "late app label cannot resurrect a fast-exiting fork child");

    Check(pipe(pipeFds) == 0, "idle reader pipe created");
    auto idleActive = std::make_shared<std::atomic<bool>>(true);
    readerDone = false;
    std::thread idleReader([&] { ReaderThread(pipeFds[0], -1, idleActive); readerDone = true; });
    usleep(10000);
    *idleActive = false;
    for (int i = 0; i < 100 && !readerDone; ++i) usleep(2000);
    Check(readerDone, "stop interrupts idle reader without closing its fd from another thread");
    idleReader.join();
    close(pipeFds[1]);

    // Concurrent short children exercise coalescing, registration and rapid
    // recreation without introducing a second process registry in production.
    std::vector<pid_t> children;
    for (int i = 0; i < 24; ++i) {
        pid_t child = fork();
        Check(child >= 0, "burst child fork succeeds");
        if (child == 0) _exit(i % 4);
        AddProcess(child, "C:/burst.exe", -1, ProcessRegistration::ForkChild);
        children.push_back(child);
    }
    for (size_t i = 0; i < children.size(); ++i) {
        AwaitExit(children[i]);
        Check(Snapshot(children[i]).exitCode == static_cast<int>(i % 4),
              "each burst child retains its actual exit result");
    }
    // Exercise the production session API with real children, including
    // early window events, multiwindow lifetime, unknown ancestry and PID reuse.
    pid_t game = fork();
    Check(game >= 0, "session child fork");
    if (!game) { for (;;) pause(); }
    AddProcess(game, "C:/game.exe", -1, ProcessRegistration::ForkChild);
    const auto early = AssociateWineSessionWindow(game, 701);
    const auto session = BeginRegisteredWineSession(game);
    Check(!session.empty() && session == early, "early event retains launch incarnation");
    Check(AssociateWineSessionWindow(game, 702) == session, "second window has same session");
    WineProcessEntry sessionEntry;
    Check(QueryWineSession(session, &sessionEntry) && sessionEntry.toplevelId == 701 &&
          sessionEntry.toplevelIds.size() == 2, "first window stays primary");
    ForgetWineSessionWindow(702);
    Check(QueryWineSession(session, &sessionEntry) && sessionEntry.toplevelId == 701,
          "closing auxiliary window retains primary");
    AssociateWineSessionWindow(game, 703);
    ForgetWineSessionWindow(701);
    Check(QueryWineSession(session, &sessionEntry) && sessionEntry.toplevelId == 703,
          "closing primary selects surviving window");
    Check(AssociateWineSessionWindow(99999999, 704).empty(), "hidden unknown PID is never guessed");
    ForgetWineSessionWindow(704);
    Check(!StopRegisteredWineSession("wine-" + std::to_string(game) + "-0"), "stale generation cannot stop live game");
    Check(kill(game, 0) == 0, "rejected stale stop leaves game alive");
    ProcessTestPhoneMode = false;
    Check(StopRegisteredWineSession(session), "exact session requests process stop");
    ProcessTestPhoneMode = true;
    Check(ncpReentries == 1, "NCP callback reentered registry without a lock cycle");
    AwaitExit(game);
    Check(QueryWineSession(session, &sessionEntry) && !sessionEntry.running,
          "session stop awaits real reaper state");
    ForgetWineSessionWindow(703);
    // Begin's ancestry lookup races Wayland destroy/recreate with the same id.
    // The delayed lookup must not resurrect or steal a newer window lifetime.
    constexpr pid_t raceOwner = 99999002, replacementOwner = 99999003;
    AddProcess(raceOwner, "C:/race.exe", -1, ProcessRegistration::NativeChild);
    AddProcess(replacementOwner, "C:/replacement.exe", -1, ProcessRegistration::NativeChild);
    const auto raced = BeginRegisteredWineSession(raceOwner);
    syntheticParent = raceOwner;
    AssociateWineSessionWindow(syntheticClient, 704);
    blockAncestry = true;
    std::thread bind([&] { BeginRegisteredWineSession(raceOwner); });
    for (int i = 0; i < 2000 && !ancestryEntered; ++i) usleep(1000);
    Check(ancestryEntered, "ancestry lookup is in progress outside registry lock");
    ForgetWineSessionWindow(704);
    const auto replacementSession = AssociateWineSessionWindow(replacementOwner, 704);
    ancestryReleased = true;
    bind.join();
    Check(QueryWineSession(raced, &sessionEntry) && sessionEntry.toplevelIds.empty(),
          "stale ancestry result cannot restore a destroyed/recreated window");
    Check(QueryWineSession(replacementSession, &sessionEntry) && sessionEntry.toplevelId == 704,
          "recreated window retains its new owner");
    ForgetWineSessionWindow(704);
    std::thread([=] { RemoveProcess(raceOwner); RemoveProcess(replacementOwner); }).join();
    blockAncestry = false;
    syntheticParent = 0;
    Check(StopRegisteredWineSession(session), "already stopped and windowless session is idempotent");
    AddProcess(game, "C:/new.exe", -1, ProcessRegistration::NativeChild);
    Check(!QueryWineSession(session, &sessionEntry) && !StopRegisteredWineSession(session),
          "PID reuse invalidates old session handle");
    std::thread([=] { RemoveProcess(game); }).join();
    for (const auto* name : {"steam.exe", "EXPLORER.EXE", "desktop", "wineserver", "@engine/test"}) {
        AddProcess(game, name, -1, ProcessRegistration::NativeChild);
        Check(!StopRegisteredWineSession(BeginRegisteredWineSession(game)), "shared infrastructure stop refused");
        std::thread([=] { RemoveProcess(game); }).join();
    }
    AddProcess(game, "C:/missing.exe", -1, ProcessRegistration::NativeChild);
    Check(!StopRegisteredWineSession(BeginRegisteredWineSession(game)),
          "backend rejected stop must not report success");
    std::thread([=] { RemoveProcess(game); }).join();
    // Fast wineserver exit must be reported even when it precedes anchor labeling.
    RegisterWineserver(failed);
    Check(GetWineserverPid() == -1 && failures == 1,
          "already-exited wineserver anchor reports failure without resurrection");
    const pid_t server = fork();
    Check(server >= 0, "live wineserver test child created");
    if (server == 0) { for (;;) pause(); }
    AddProcess(server, "wineserver", -1, ProcessRegistration::ForkChild);
    RegisterWineserver(server);
    usleep(1100000);
    Check(GetWineserverPid() == server && Snapshot(server).running && failures == 1,
          "live wineserver survives monitor and repeated label registration");
    ProcessTestPhoneMode = false;
    const unsigned beforeNcpStopAll = ncpReentries;
    KillAllProcesses();
    ProcessTestPhoneMode = true;
    Check(ncpReentries > beforeNcpStopAll, "stopAll NCP callback can reenter registry");
    AwaitExit(server);
    NotifyWhenSessionDrained();
    for (int i = 0; i < 500 && stopped == 0; ++i) usleep(2000);
    Check(stopped == 1 && failures == 1 && GetWineserverPid() == -1,
          "requested stop drains real children and emits stopped instead of failure");
    // PID reuse is explicitly a new incarnation, not an application relabel.
    AddProcess(live, "@engine/native-child", -1, ProcessRegistration::NativeChild);
    entry = Snapshot(live);
    Check(entry.exitCode == -1 && entry.readerActive != active &&
          !IsPidExited(live) && entry.running, "new fork registration resets prior PID incarnation");
    for (int i = 0; i < 100 && callbacks < 28; ++i) usleep(2000);
    Check(callbacks >= 28 && !callbackOnMain, "exit callbacks run outside interrupted main thread");
    Check(registrationInterrupts == 3, "real SIGCHLD delivered while registration holds its mutex");
    std::printf("process lifecycle: %d checks PASS (real SIGCHLD/fork/waitpid + production registry)\n", checks);
    std::fflush(stdout);
    // The production monitor is intentionally detached for the app lifetime.
    std::_Exit(0);
}
