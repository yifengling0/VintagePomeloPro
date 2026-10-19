#include "linux_steam/native_exec.h"
#include <cassert>
#include <cstring>
#include <dirent.h>
#include <iostream>
#include <fcntl.h>
#include <pthread.h>
#include <atomic>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

static int inheritedSdkFd = -1;
static void InheritedSdkChildHook() { if(inheritedSdkFd>=0)close(inheritedSdkFd); }
static int Fds() {
    DIR* dir = opendir("/proc/self/fd"); assert(dir);
    int n = 0; while (readdir(dir)) ++n; closedir(dir); return n;
}
int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "hang")) { for (;;) pause(); }
    if (argc == 2 && !strcmp(argv[1], "exit37")) return 37;
    if (argc == 2 && !strcmp(argv[1], "output")) {
        for (int i = 0; i < 100; ++i) { std::string s(1024, 'a'); if (write(1, s.data(), s.size()) < 0) return 1; }
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "fd-check")) {
        int fd = atoi(argv[2]);
        if (!strcmp(argv[3], "closed")) return fcntl(fd, F_GETFD) == -1 && errno == EBADF ? 0 : 10;
        char value = 0;
        return read(fd, &value, 1) == 1 && value == 'X' ? 0 : 11;
    }
    if (argc == 3 && !strcmp(argv[1], "cwd-check")) {
        char actual[4096];
        return getcwd(actual, sizeof(actual)) && !strcmp(actual, argv[2]) ? 0 : 12;
    }
    assert(argc == 3);
    char self[4096]; ssize_t n = readlink("/proc/self/exe", self, sizeof(self)-1); assert(n > 0); self[n] = 0;
    for (uint32_t invalidTimeout : {0U, 9U, 14420001U}) {
        auto invalid = steamohos::RunNativeExec({self, "exit37"}, {}, invalidTimeout);
        assert(invalid.stage == "invalid_arguments" && invalid.setupError == EINVAL && !invalid.reaped);
    }
    const int before = Fds();
    auto manual = steamohos::RunNativeExec({self, "exit37"}, {}, 14420000);
    assert(manual.exitCode == 37 && manual.reaped && !manual.timedOut);
    char parentCwd[4096]; assert(getcwd(parentCwd, sizeof(parentCwd)));
    steamohos::NativeExecOptions cwdOptions;
    cwdOptions.workingDirectory = "/tmp";
    auto changed = steamohos::RunNativeExec({self, "cwd-check", "/tmp"}, {}, 1000, cwdOptions);
    assert(changed.exitCode == 0 && changed.reaped && !changed.setupError);
    char afterCwd[4096]; assert(getcwd(afterCwd, sizeof(afterCwd)) && !strcmp(parentCwd, afterCwd));
    cwdOptions.workingDirectory = "/not-present-steamohos-working-directory";
    auto badCwd = steamohos::RunNativeExec({self, "exit37"}, {}, 1000, cwdOptions);
    assert(badCwd.setupError == ENOENT && badCwd.reaped && badCwd.exitCode != 37);
    int inherited[2]; assert(!pipe2(inherited, O_CLOEXEC));
    steamohos::NativeExecOptions options;
    options.passFds = {inherited[0]};
    options.context = &inherited[1];
    options.parentTask = [](void* context, int childPid) {
        assert(childPid > 0 && childPid != getpid());
        char value = 'X'; assert(write(*static_cast<int*>(context), &value, 1) == 1);
    };
    const std::vector<std::string> fdArgs{self, "fd-check", std::to_string(inherited[0]), "open"};
    auto passing = steamohos::RunNativeExec(fdArgs, {}, 1000, options);
    assert(passing.exitCode == 0 && passing.reaped && passing.hostTpUnchanged);
    assert(fcntl(inherited[0], F_GETFD) & FD_CLOEXEC); // Parent ownership/flags preserved.
    // Reproduce a stale SDK atfork callback closing a newly reused display FD.
    assert(!pthread_atfork(nullptr,nullptr,InheritedSdkChildHook));
    inheritedSdkFd=inherited[0];
    auto hookClosed=steamohos::RunNativeExec(fdArgs,{},1000,options);
    assert(hookClosed.setupError==EBADF&&hookClosed.reaped);
    options.libcForkOnly=true;
    auto noSdkHooks=steamohos::RunNativeExec(fdArgs,{},1000,options);
    assert(noSdkHooks.exitCode==0&&noSdkHooks.reaped&&noSdkHooks.hostTpUnchanged);
    std::atomic<bool> keepThread{true};
    std::thread other([&] { while(keepThread.load())usleep(1000); });
    auto multiThread=steamohos::RunNativeExec(fdArgs,{},1000,options);
    assert(multiThread.stage=="fork_policy"&&multiThread.setupError==ENOTSUP&&!multiThread.reaped);
    keepThread.store(false);other.join();
    options.libcForkOnly=false;inheritedSdkFd=-1;
    auto notPassing = steamohos::RunNativeExec({self, "fd-check", std::to_string(inherited[0]), "closed"}, {}, 1000);
    assert(notPassing.exitCode == 0 && notPassing.reaped);
    for (const std::vector<int>& invalid : std::vector<std::vector<int>>{{-1}, {1},
            {inherited[0], inherited[0]}, std::vector<int>(17, inherited[0])}) {
        options.passFds = invalid;
        auto rejected = steamohos::RunNativeExec(fdArgs, {}, 1000, options);
        assert(rejected.setupError == EINVAL && rejected.stage == "invalid_pass_fds" && !rejected.reaped);
    }
    close(inherited[0]); close(inherited[1]);
    steamohos::NativeExecOptions lifecycle;
    lifecycle.parentTask = [](void*, int childPid) { assert(!kill(childPid, SIGTERM)); };
    auto stopped = steamohos::RunNativeExec({self, "hang"}, {}, 920000, lifecycle);
    assert(stopped.reaped && stopped.signal == SIGTERM && !stopped.timedOut && stopped.elapsedMs < 3000);
    options.passFds = {999999};
    auto closed = steamohos::RunNativeExec(fdArgs, {}, 1000, options);
    assert(closed.setupError == EINVAL && !closed.reaped);
    auto missing = steamohos::RunNativeExec({"/not-present-steamohos-executable"}, {}, 1000);
    assert(missing.execError == ENOENT && missing.exitCode == 127 && missing.reaped);
    auto failure = steamohos::RunNativeExec({self, "exit37"}, {}, 1000);
    assert(failure.exitCode == 37 && failure.reaped && !failure.execError);
    auto timeout = steamohos::RunNativeExec({self, "hang"}, {}, 100);
    assert(timeout.timedOut && timeout.signal == SIGKILL && timeout.reaped);
    auto output = steamohos::RunNativeExec({self, "output"}, {}, 1000);
    assert(output.exitCode == 0 && output.output.size() == 32768 && output.outputTruncated && output.reaped);
    char diagnosticPath[] = "/tmp/steamohos-native-diagnostic-XXXXXX";
    int diagnosticFd = mkstemp(diagnosticPath); assert(diagnosticFd >= 0); unlink(diagnosticPath);
    steamohos::NativeExecOptions mirrored; mirrored.outputLimit=1024; mirrored.diagnosticFd=diagnosticFd;
    auto mirrorResult = steamohos::RunNativeExec({self, "output"}, {}, 1000, mirrored);
    assert(mirrorResult.exitCode==0 && mirrorResult.output.size()==1024 && mirrorResult.outputTruncated);
    assert(lseek(diagnosticFd,0,SEEK_END)==100*1024); // latest errors are not discarded at the prefix cap
    assert(ftruncate(diagnosticFd,8*1024*1024-10)==0 && lseek(diagnosticFd,0,SEEK_END)>0);
    auto rotated = steamohos::RunNativeExec({self, "output"}, {}, 1000, mirrored);
    assert(rotated.exitCode==0 && lseek(diagnosticFd,0,SEEK_END)==100*1024);
    close(diagnosticFd);
    for (int i = 0; i < 3; ++i) {
        auto normal = steamohos::RunNativeExec({argv[1], "--inhibit-cache", "--library-path", argv[2],
            std::string(argv[2]) + "/steamohos_gnu_fixture", "normal"}, {"LANG=C"}, 3000);
        if (normal.exitCode || normal.execError) std::cerr << steamohos::NativeExecJson(normal) << '\n';
        assert(normal.exitCode == 0 && normal.reaped && normal.output.find(
            "STEAMOHOS_GNU_NATIVE_V1 main tls pthread errno dlvsym destructors PASS\n") != std::string::npos);
    }
    auto mapped = steamohos::RunNativeLoader({argv[1], "--inhibit-cache", "--library-path", argv[2],
        std::string(argv[2]) + "/steamohos_gnu_fixture", "normal"}, {"LANG=C"}, 3000);
    if (mapped.exitCode || mapped.setupError) std::cerr << steamohos::NativeExecJson(mapped) << '\n';
    assert(mapped.exitCode == 0 && mapped.reaped && mapped.hostTpUnchanged && mapped.output.find(
        "STEAMOHOS_GNU_NATIVE_V1 main tls pthread errno dlvsym destructors PASS\n") != std::string::npos);
    auto mappedExit = steamohos::RunNativeLoader({argv[1], "--library-path", argv[2],
        std::string(argv[2]) + "/steamohos_gnu_fixture", "exit37"}, {}, 1000);
    assert(mappedExit.exitCode == 37 && mappedExit.reaped);
    auto mappedTimeout = steamohos::RunNativeLoader({argv[1], "--library-path", argv[2],
        std::string(argv[2]) + "/steamohos_gnu_fixture", "hang"}, {}, 100);
    assert(mappedTimeout.timedOut && mappedTimeout.signal == SIGKILL && mappedTimeout.reaped);
    int status; errno = 0; assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    assert(Fds() == before);
    std::cout << "PASS: native GNU main/TLS/pthread/versioned DSO; exec errno, exit37, timeout, bounded output, reap, FD allowlist/parent task/cleanup\n";
}
