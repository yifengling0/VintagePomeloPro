#include "wine_process.h"
#include "wine/wine_constants.h"
#include "phone_adapter/phone_adapter.h"
#include "phone_adapter/phone_process.h"
#include "cef_utility_probe.h"

#include <unistd.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <dirent.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <climits>
#include <algorithm>
#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <AbilityKit/native_child_process.h>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

// -- 全局状态 --
static std::mutex gProcMutex;
static std::vector<WineProcessEntry> gProcRegistry;

// -- 主 wineserver 会话锚点 --
// gShutdownRequested: 任何 KillAllProcesses (stopAll/stopClient/resetWinePrefix)
// 都意味着主 wineserver 的死亡是主动停止而非引擎故障, ProcMon 据此抑制
// state:failed:wineserver 上报; 只在 RegisterWineserver (新会话建立) 时清除,
// 停止编排完成后仍保持 — 此后注册表里已无存活会话, 迟到的死亡检测同样不该报失败。
static std::atomic<pid_t> gWineserverPid{-1};
static std::atomic<bool> gShutdownRequested{false};
// gDesktopSessionEnded: desktop 根 toplevel 已销毁 (explorer 主动结束桌面会话)。
// 桌面主动退出时 wineserver 会跟随退出 (explorer 先走、wineserver 后走) — 这是
// 正常会话终结而非崩溃, ProcMon 据此把锚点死亡按 state:stopped 收口而非误报
// state:failed:wineserver。只在 RegisterWineserver (新会话建立) 时清除。
// PC 窗口模式没有 desktop root, 此标记永不置位, 崩溃判定行为不变。
static std::atomic<bool> gDesktopSessionEnded{false};
// gDesktopShellMarked: 桌面 root 首次出现时标记过 "桌面 shell 基础进程" 集合后
// 置位。root 重建 (explorer 重连重新提交 root) 不重复标记, 避免把已在跑的用户
// 程序误标为不可结束。BeginDesktopSession (每次桌面会话 spawn explorer 前) 清除,
// 热重启复用旧 wineserver 也能在新 root 出现时重新标记。
static std::atomic<bool> gDesktopShellMarked{false};

static uint64_t TimestampMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// 前向声明
static void EnsureMonitorRunning();
static void NotifyChildReaper();
static void HandleProcessDeath(pid_t pid, int reason = -1,
                               const char* source = "unknown", uint64_t generation = 0);

// -- 进程后端: fork (手机) vs NCP (Pad/真机) --
// 两种设备模式的进程操作差异集中在同一处:
//   fork 后端 (手机): kill 用 POSIX，退出由注册子进程的 waitpid 确认;
//                     沙箱内 /proc 也可能不可读，不触发 NCP 退出回调
//   NCP 后端 (Pad/真机): appspawn 托管, 沙箱 /proc 对 NCP 不可见 — 判活/
//                     kill 走系统 NCP API, 退出检测用 NCP 退出回调
// 所有「判活/杀/等退出/注册回调」的分支都经 IsForkBackend() 选择,
// 调用方无需感知设备模式; 新增模式只改这里。
static bool IsForkBackend() {
    return PhoneAdapter_IsPhoneMode();
}

// 杀子进程 (按后端分流): fork 后端原生 kill 有效; NCP 后端优先官方终止
// API (沙箱内 kill(2) 可能受限/pid ns 不可见), 失败 fallback 到 SIGKILL —
// SELF_FORK 模式子进程官方 API 明确不可终止, 恰由 fallback 覆盖
void KillChildProcess(pid_t pid) {
    if (IsForkBackend()) {
        kill(pid, SIGKILL);
        return;
    }
    auto rc = OH_Ability_KillChildProcess((int32_t)pid);
    if (rc != NCP_NO_ERROR) {
        kill(pid, SIGKILL);
    }
}

// NCP 退出回调是否已注册 (注册后 ProcMon 轮询降级为纯 zombie 感知 —
// 真实退出由系统回调权威上报, 轮询不再承担判死职责)
static std::atomic<bool> gNcpExitCbRegistered{false};
static std::atomic<bool> gProcHiddenLogged{false};

// -- 权威退出标记 (fork reaper / NCP 回调 / fork server 共用) --
// 沙箱 /proc 不可见不代表退出。完成真实退出时记录，新的 PID 代次登记时清除。
static std::mutex gExitWaitMutex;
static std::set<pid_t> gExitedPids;

bool IsPidExited(pid_t pid) {
    std::lock_guard<std::mutex> lock(gExitWaitMutex);
    return gExitedPids.count(pid) > 0;
}

bool IsProcessAliveNotZombie(pid_t pid) {
    if (pid <= 0 || IsPidExited(pid)) return false;
    WineProcessEntry entry{};
    if (QueryProcessSnapshot(pid, &entry)) {
        // Preserve the real wait status: ProcMon must not race the reaper by
        // completing a zombie with an unknown reason first.
        if (entry.waitPending) return true;
        if (!entry.running) return false;
    }
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE* f = fopen(path, "r");
    if (f) {
        char buf[512];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        char* rp = strrchr(buf, ')');
        return !(rp && strlen(rp) >= 3 && (rp[2] == 'Z' || rp[2] == 'X'));
    }
    const int error = errno;
    // ENOENT can also mean a hidden PID namespace, including on phones.
    // Unknown visibility is not evidence that a live process has exited.
    if (!gProcHiddenLogged.exchange(true)) {
        OH_LOG_WARN(LOG_APP, "[Alive] /proc/%{public}d/stat unreadable errno=%{public}d; "
                    "backend=%{public}s, await authoritative exit", pid, error,
                    IsForkBackend() ? "fork" : "ncp");
    }
    return true;
}

bool IsLaunchChildExited(pid_t pid) {
    return IsPidExited(pid);
}

void RegisterWineserver(pid_t pid) {
    /* 热重启/复用场景: 若已有存活的旧 wineserver (旧会话锚点), 保持旧锚点。
     * wine 单实例语义下, 新 spawn 的 wineserver 检测到旧实例会连接后正常退出
     * —— 这不是引擎故障 (master 同款: wineserver 不登记, 新实例退出无感)。
     * 仅当旧锚点已死 (冷启动 / stopAll 杀干净 / 引擎崩溃后重建) 才接管为新锚点,
     * 否则 ProcMon 会把"新实例正常退出"误判为 state:failed:wineserver。
     * 新 pid 仍 AddProcess 登记 (ProcMon 监视 + KillAllProcesses 可杀),
     * 只是不作为会话锚点判定死法。 */
    pid_t existing = gWineserverPid.load(std::memory_order_acquire);
    if (existing > 0 && IsProcessAliveNotZombie(existing)) {
        OH_LOG_WARN(LOG_APP, "[ProcReg] wineserver %{public}d alive, keep anchor; new spawn %{public}d will attach",
                    existing, pid);
        AddProcess(pid, "wineserver", -1);
        return;
    }
    AddProcess(pid, "wineserver", -1);
    gShutdownRequested.store(false);
    gDesktopSessionEnded.store(false);
    gWineserverPid.store(pid);
    OH_LOG_WARN(LOG_APP, "[ProcReg] wineserver %{public}d registered as session anchor", pid);
    WineProcessEntry entry{};
    if (QueryProcessSnapshot(pid, &entry) && !entry.running)
        HandleProcessDeath(pid, entry.exitCode, entry.exitCodeSource.c_str(), entry.generation);
}

pid_t GetWineserverPid() {
    return gWineserverPid.load();
}

void MarkDesktopSessionEnded() {
    gDesktopSessionEnded.store(true);
}

void BeginDesktopSession() {
    gDesktopShellMarked.store(false);
}

void MarkDesktopShellProcesses() {
    bool expected = false;
    if (!gDesktopShellMarked.compare_exchange_strong(expected, true)) {
        return;  // 已标记过 (root 重建), 不重复
    }
    std::lock_guard<std::mutex> lock(gProcMutex);
    size_t count = 0;
    for (auto& entry : gProcRegistry) {
        if (entry.running) {
            entry.desktopShell = true;
            count++;
        }
    }
    OH_LOG_WARN(LOG_APP, "[ProcReg] desktop shell processes marked: %{public}zu running", count);
}

static bool ReadProcessParent(pid_t pid, pid_t* parent)
{
    char path[64];
    char buffer[512];
    char* rightParen;
    FILE* file;
    size_t length;
    int parsedParent;

    if (!parent || pid <= 0) return false;
    snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(pid));
    file = fopen(path, "r");
    if (!file) return false;
    length = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    if (!length) return false;
    buffer[length] = '\0';
    rightParen = strrchr(buffer, ')');
    if (!rightParen || sscanf(rightParen + 2, "%*c %d", &parsedParent) != 1)
        return false;
    *parent = static_cast<pid_t>(parsedParent);
    return true;
}

static std::vector<pid_t> SnapshotProcessDescendants(pid_t root)
{
    std::vector<pid_t> roots;
    std::vector<pid_t> descendants;
    DIR* proc;

    if (root <= 0) return descendants;
    roots.push_back(root);
    for (size_t index = 0; index < roots.size(); ++index)
    {
        proc = opendir("/proc");
        if (!proc) break;
        while (dirent* entry = readdir(proc))
        {
            char* end;
            long value;
            pid_t parent;
            pid_t pid;

            if (entry->d_name[0] < '1' || entry->d_name[0] > '9') continue;
            value = strtol(entry->d_name, &end, 10);
            if (*end || value <= 0 || value > INT_MAX) continue;
            pid = static_cast<pid_t>(value);
            if (!ReadProcessParent(pid, &parent) || parent != roots[index]) continue;
            if (std::find(descendants.begin(), descendants.end(), pid) != descendants.end())
                continue;
            descendants.push_back(pid);
            roots.push_back(pid);
        }
        closedir(proc);
    }
    return descendants;
}

// -- 注册表辅助函数 --
WineProcessEntry* AddProcess(pid_t pid, const std::string& exeFullPath, int stdoutFd,
                             ProcessRegistration registration) {
    if (pid <= 0) return nullptr;
    WineProcessEntry* result = nullptr;
    {
        std::lock_guard<std::mutex> lock(gProcMutex);
        std::string basename = exeFullPath;
        auto slash = basename.find_last_of("/\\");
        if (slash != std::string::npos) basename = basename.substr(slash + 1);
        // Broker and app labels do not create a new process incarnation or
        // resurrect a child that exited during its startup handshake.
        if (registration == ProcessRegistration::Relabel) {
            for (auto& entry : gProcRegistry) {
                if (entry.pid != pid) continue;
                entry.exeBasename = basename;
                entry.exeFullPath = exeFullPath;
                if (stdoutFd >= 0 && entry.stdoutFd < 0) entry.stdoutFd = stdoutFd;
                result = &entry;
                break;
            }
        }
        if (!result) {
            gProcRegistry.erase(std::remove_if(gProcRegistry.begin(), gProcRegistry.end(),
                [pid](const WineProcessEntry& entry) { return entry.pid == pid; }), gProcRegistry.end());
            while (gProcRegistry.size() >= 128) {
                auto ended = std::find_if(gProcRegistry.begin(), gProcRegistry.end(),
                    [](const WineProcessEntry& entry) { return !entry.running && !entry.waitPending; });
                if (ended == gProcRegistry.end()) break;
                gProcRegistry.erase(ended);
            }
            {
                std::lock_guard<std::mutex> exitLock(gExitWaitMutex);
                gExitedPids.erase(pid);
            }
            static uint64_t nextGeneration = 0;
            gProcRegistry.push_back({
                .pid = pid,
                .exeBasename = basename,
                .exeFullPath = exeFullPath,
                .running = true,
                .startTimestampMs = TimestampMs(),
                .endTimestampMs = 0,
                .exitCode = -1,
                .exitCodeSource = "unknown",
                .stdoutFd = stdoutFd,
                .readerActive = std::make_shared<std::atomic<bool>>(true),
                .waitPending = registration == ProcessRegistration::ForkChild,
                .generation = ++nextGeneration
            });
            result = &gProcRegistry.back();
            OH_LOG_WARN(LOG_APP, "[ProcReg] add pid=%{public}d name=%{public}s total=%{public}zu",
                        pid, basename.c_str(), gProcRegistry.size());
        }
    }
    EnsureMonitorRunning();
    if (registration == ProcessRegistration::ForkChild) NotifyChildReaper();
    // NAPI callbacks can reenter the registry. Never notify with its lock held.
    if (gStateTsfn)
        napi_call_threadsafe_function(gStateTsfn, strdup("evt:proc-updated"), napi_tsfn_blocking);
    return result;
}

void RemoveProcess(pid_t pid, int exitCode, const std::string& exitCodeSource) {
    HandleProcessDeath(pid, exitCode, exitCodeSource.c_str());
}

void KillAllProcesses() {
    // 主动停止标记: 之后主 wineserver 的死亡检测 (ProcMon) 不再报 state:failed
    gShutdownRequested.store(true);
    std::vector<pid_t> trackedPids;
    {
        std::lock_guard<std::mutex> lock(gProcMutex);
        for (auto& entry : gProcRegistry) {
            if (!entry.running) continue;
            OH_LOG_WARN(LOG_APP, "[ProcReg] killAll pid=%{public}d name=%{public}s",
                        entry.pid, entry.exeBasename.c_str());
            *(entry.readerActive) = false;
            trackedPids.push_back(entry.pid);
            KillChildProcess(entry.pid);
        }
    }

    const pid_t self = getpid();
    // The early phone server belongs to the App lifetime. Runtime refresh and
    // session restart must drain its Wine children without discarding the
    // clean pre-ArkUI process from which Direct children are launched.
    const pid_t directForkServer = Phone_GetDirectForkServerPid();
    std::vector<pid_t> descendants = SnapshotProcessDescendants(self);
    OH_LOG_INFO(LOG_APP,
                "[ProcReg] killAll session descendants=%{public}zu tracked=%{public}zu",
                descendants.size(), trackedPids.size());
    for (pid_t pid : trackedPids)
        if (pid != self) kill(pid, SIGKILL);
    for (pid_t pid : descendants)
        if (pid != self && pid != directForkServer) kill(pid, SIGKILL);

    /* AppSpawn children are not waitable by the main process.  Give the
     * kernel a short, bounded opportunity to reap them and repeat the scan in
     * case a broker child was forked while the first signal pass was running.
     */
    for (unsigned int pass = 0; pass < 20; ++pass)
    {
        bool anyAlive = false;
        descendants = SnapshotProcessDescendants(self);
        for (pid_t pid : descendants) {
            if (pid == directForkServer) continue;
            if (!IsProcessAliveNotZombie(pid)) continue;
            anyAlive = true;
            kill(pid, SIGKILL);
        }
        if (!anyAlive) break;
        usleep(100000);
    }
}

// stopAll 末尾调用: 后台 zombie 感知等待注册表进程 (含主 wineserver) 全部死亡,
// 完成发一次 state:stopped (蓝图「完全退出」判据的进程侧; Wayland server 由
// 调用方在此之前同步停掉)。30s 封顶兜底: SIGKILL 下进程卡 D-state 不死时
// 不至于永不发声 (ArkTS 侧另有超时硬放行, 双保险)。
void NotifyWhenSessionDrained() {
    std::thread([]() {
        constexpr int kDrainTimeoutMs = 30000;
        int waitedMs = 0;
        while (waitedMs < kDrainTimeoutMs) {
            bool anyAlive = false;
            for (const auto& entry : GetProcessListSnapshot()) {
                if (entry.running && IsProcessAliveNotZombie(entry.pid)) {
                    anyAlive = true;
                    break;
                }
            }
            if (!anyAlive) break;
            usleep(100000);
            waitedMs += 100;
        }
        if (waitedMs >= kDrainTimeoutMs) {
            OH_LOG_ERROR(LOG_APP, "[ProcReg] drain wait timeout: process survived SIGKILL");
        }
        gWineserverPid.store(-1);
        OH_LOG_WARN(LOG_APP, "[ProcReg] session drained in %{public}d ms, emit state:stopped",
                    waitedMs);
        if (gStateTsfn) {
            napi_call_threadsafe_function(gStateTsfn, strdup("state:stopped"),
                                          napi_tsfn_blocking);
        }
    }).detach();
}

// -- 进程退出状态日志 --
void LogProcessExit(const char* tag, pid_t pid, int status) {
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        OH_LOG_ERROR(LOG_APP, "[%{public}s] CRASH pid=%{public}d signal=%{public}d(%{public}s) core=%{public}d",
                     tag, pid, sig, strsignal(sig), WCOREDUMP(status) ? 1 : 0);
    } else if (WIFEXITED(status)) {
        OH_LOG_WARN(LOG_APP, "[%{public}s] process %{public}d exited code=%{public}d",
                    tag, pid, WEXITSTATUS(status));
    } else {
        OH_LOG_WARN(LOG_APP, "[%{public}s] process %{public}d terminated status=0x%{public}x",
                    tag, pid, status);
    }
}

// -- fork/exec 后关闭继承的 fd --
void CloseInheritedFds(std::initializer_list<int> keepFds) {
    DIR* d = opendir("/proc/self/fd");
    if (!d) return;
    int dfd = dirfd(d);
    dirent* e;
    while ((e = readdir(d))) {
        int fd = atoi(e->d_name);
        if (fd <= 2 || fd == dfd) continue;
        if (std::find(keepFds.begin(), keepFds.end(), fd) != keepFds.end()) continue;
        close(fd);
    }
    closedir(d);
}

// Process-lifetime self-pipe. The signal path only writes a wake byte;
// registered child ownership and status publication belong to the worker.
static volatile sig_atomic_t gReaperWriteFd = -1;

static void NotifyChildReaper() {
    const int savedErrno = errno;
    const int fd = gReaperWriteFd;
    if (fd >= 0) {
        const char byte = 1;
        ssize_t written;
        do { written = write(fd, &byte, 1); } while (written < 0 && errno == EINTR);
        // EAGAIN means a wake is already queued. No locks, NAPI or logging.
    }
    errno = savedErrno;
}

void sigchld_handler(int) { NotifyChildReaper(); }

struct ProcessDeathNotification {
    pid_t pid = -1;
    int reason = -1;
    bool updated = false;
    bool anchor = false;
};

// Caller holds gProcMutex. Reaping and publishing share this critical section
// so a reused PID cannot receive a previous incarnation's status.
static ProcessDeathNotification CompleteProcessLocked(pid_t pid, int reason,
                                                       const char* source, uint64_t generation) {
    ProcessDeathNotification result;
    auto it = std::find_if(gProcRegistry.begin(), gProcRegistry.end(),
        [pid](const WineProcessEntry& entry) { return entry.pid == pid; });
    if (generation && (it == gProcRegistry.end() || it->generation != generation)) return result;
    const bool waitResult = !strcmp(source, "waitpid") || !strcmp(source, "signal");
    if (it != gProcRegistry.end()) {
        const bool haveWaitResult = it->exitCodeSource == "waitpid" || it->exitCodeSource == "signal";
        result.updated = it->running || (waitResult && !haveWaitResult) ||
                         (!haveWaitResult && it->exitCode < 0 && reason >= 0);
        if (result.updated) {
            it->running = false;
            if (!it->endTimestampMs) it->endTimestampMs = TimestampMs();
            it->exitCode = reason;
            it->exitCodeSource = source;
            OH_LOG_WARN(LOG_APP, "[ProcReg] complete pid=%{public}d name=%{public}s exit=%{public}d source=%{public}s",
                        pid, it->exeBasename.c_str(), reason, source);
        }
        if (waitResult) it->waitPending = false;
    }
    {
        std::lock_guard<std::mutex> exitLock(gExitWaitMutex);
        gExitedPids.insert(pid);
    }
    result.pid = pid;
    result.reason = reason;
    pid_t expected = pid;
    result.anchor = gWineserverPid.compare_exchange_strong(expected, -1);
    return result;
}

static void DispatchProcessDeath(const ProcessDeathNotification& result) {
    if (result.pid <= 0) return;
    if (result.updated) {
        WineHuaCefUtilityProbeNoteExit(result.pid, result.reason);
        if (gStateTsfn) {
            char msg[64];
            snprintf(msg, sizeof(msg), "evt:proc-exited:%d", result.pid);
            napi_call_threadsafe_function(gStateTsfn, strdup(msg), napi_tsfn_blocking);
        }
    }
    if (!result.anchor || gShutdownRequested.load(std::memory_order_acquire)) return;
    if (gDesktopSessionEnded.load(std::memory_order_acquire)) {
        OH_LOG_WARN(LOG_APP, "[ProcMon] wineserver exited after desktop session end, clean stop");
        KillAllProcesses();
        NotifyWhenSessionDrained();
    } else if (gStateTsfn) {
        OH_LOG_ERROR(LOG_APP, "[ProcMon] wineserver died unexpectedly, emit state:failed:wineserver");
        napi_call_threadsafe_function(gStateTsfn, strdup("state:failed:wineserver"), napi_tsfn_blocking);
    }
}

static void HandleProcessDeath(pid_t pid, int reason, const char* source, uint64_t generation) {
    ProcessDeathNotification result;
    {
        std::lock_guard<std::mutex> lock(gProcMutex);
        result = CompleteProcessLocked(pid, reason, source, generation);
    }
    DispatchProcessDeath(result);
}

static void ReapRegisteredChildren() {
    std::vector<ProcessDeathNotification> notifications;
    {
        std::lock_guard<std::mutex> lock(gProcMutex);
        for (auto& entry : gProcRegistry) {
            if (!entry.waitPending) continue;
            int status = 0;
            pid_t waited;
            do { waited = waitpid(entry.pid, &status, WNOHANG); }
            while (waited < 0 && errno == EINTR);
            if (waited != entry.pid) continue;
            LogProcessExit("broker-child", entry.pid, status);
            notifications.push_back(CompleteProcessLocked(entry.pid,
                WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                WIFSIGNALED(status) ? "signal" : "waitpid", entry.generation));
        }
    }
    for (const auto& notification : notifications) DispatchProcessDeath(notification);
}

bool EnsureChildReaper() {
    static const bool started = [] {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) return false;
        struct sigaction action{};
        action.sa_handler = sigchld_handler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
        if (sigaction(SIGCHLD, &action, nullptr) != 0) {
            close(fds[0]); close(fds[1]); return false;
        }
        gReaperWriteFd = fds[1];
        std::thread([readFd = fds[0]] {
            for (;;) {
                pollfd pfd{readFd, POLLIN, 0};
                int result = poll(&pfd, 1, 1000);
                if (result < 0 && errno == EINTR) continue;
                char bytes[256];
                while (read(readFd, bytes, sizeof(bytes)) > 0) {}
                ReapRegisteredChildren();
            }
        }).detach();
        return true;
    }();
    return started;
}

// -- NCP 子进程退出回调 (系统级, 绕开沙箱 /proc 不可见问题) --
// NCP 子进程由 appspawn 创建, SIGCHLD 收不到它们的退出事件; 系统在子进程
// 退出时回调本函数 (pid + signal)。这是退出检测的权威信号 — ProcMon 轮询
// 的 /proc 判活在沙箱里对 NCP 不可靠 (可能全部误判), 只保留 zombie 感知。
static void OnNcpChildExit(int32_t pid, int32_t signal) {
    OH_LOG_ERROR(LOG_APP, "[ProcReg] NCP child pid=%{public}d terminated signal/reason=%{public}d(0x%{public}x) name=%{public}s",
                 pid, signal, (unsigned int)signal, strsignal(signal));
    HandleProcessDeath((pid_t)pid, signal, "ncp-exit");
}

void NoteCreateNcpDeath(int32_t pid) {
    OH_LOG_WARN(LOG_APP, "[ProcReg] Create NCP proxy died pid=%{public}d", pid);
    HandleProcessDeath((pid_t)pid, -1, "ipc-death");
}

void NotePhoneForkServerChildExit(int32_t pid, int waitStatus) {
    LogProcessExit("phone-fork-server", pid, waitStatus);
    HandleProcessDeath(pid, WIFEXITED(waitStatus) ? WEXITSTATUS(waitStatus) : -1,
                       WIFEXITED(waitStatus) ? "phone-fork-server" : "phone-fork-server-signal");
}

void RegisterNcpExitCallback() {
    if (gNcpExitCbRegistered.load(std::memory_order_acquire)) return;
    // 无条件注册 (napi Init 最早时机): 沙箱 /proc 对 NCP 进程不可见,
    // 退出检测以系统回调为权威信号。手机 fork 模式注册后不触发
    // (fork 子进程不走 NCP), 空转无害 — 模式分支只留在 spawn 之后的
    // 判活/杀/等待操作里 (IsProcessAliveNotZombie / KillChildProcess /
    // IsLaunchChildExited, 此时 IsForkBackend 已由 setPhoneMode 置位)。
    auto rc = OH_Ability_RegisterNativeChildProcessExitCallback(OnNcpChildExit);
    if (rc == NCP_NO_ERROR) {
        gNcpExitCbRegistered.store(true, std::memory_order_release);
        OH_LOG_WARN(LOG_APP, "[ProcReg] NCP exit callback registered (rc=0)");
    } else {
        // 注册失败: 保持 ProcMon 轮询全职责 (zombie 感知 + /proc 判死),
        // 但 IsProcessAliveNotZombie 已保守化 — 宁可僵尸误活不可活进程误死
        OH_LOG_ERROR(LOG_APP, "[ProcReg] NCP exit callback register FAILED rc=%{public}d", (int)rc);
    }
}

// -- NCP 进程存活监控 --
// NCP 子进程由 appspawn 创建，不是主进程的 fork() 子进程，
// SIGCHLD 收不到它们的退出事件。通过 /proc/<pid> 轮询检测退出。
// 判活必须 zombie 感知 (fork 模式子进程退出后 /proc 不立即消失),
// 否则已退出的进程会被长期误判为存活。
// 注: 沙箱 /proc 对 NCP 可能不可见 — 真实退出以 NCP 退出回调为准,
// 轮询仅作僵尸感知兜底 (IsProcessAliveNotZombie 对不可见一律判活)。
static std::atomic<bool> gMonitorRunning{false};
static std::thread gMonitorThread;

static void ProcessMonitorLoop() {
    OH_LOG_WARN(LOG_APP, "[ProcMon] started");
    while (gMonitorRunning.load(std::memory_order_relaxed)) {
        sleep(1);

        for (const auto& entry : GetProcessListSnapshot()) {
            if (!entry.running || entry.waitPending) continue;
            if (!IsProcessAliveNotZombie(entry.pid))
                HandleProcessDeath(entry.pid, -1, "proc-zombie", entry.generation);
        }
    }
    OH_LOG_WARN(LOG_APP, "[ProcMon] stopped");
}

static void EnsureMonitorRunning() {
    bool expected = false;
    if (gMonitorRunning.compare_exchange_strong(expected, true)) {
        gMonitorThread = std::thread(ProcessMonitorLoop);
        gMonitorThread.detach();
    }
}

// -- 客户端 stdout/stderr 读取线程 (每个进程独立) --
void ReaderThread(int fd, pid_t pid, std::shared_ptr<std::atomic<bool>> active) {
    char buf[2048];
    std::string pending;
    while (*active) {
        pollfd pfd{fd, POLLIN, 0};
        int ready = poll(&pfd, 1, 100);
        if (ready == 0 || (ready < 0 && errno == EINTR)) continue;
        if (ready < 0) break;
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            pending.append(buf, n);
            size_t pos;
            while ((pos = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, pos);
                OH_LOG_INFO(LOG_APP, "[wine:%{public}d] %{public}s", pid, line.c_str());
                pending.erase(0, pos + 1);
            }
        } else if (n == 0) {
            break;
        } else {
            if (errno == EINTR) continue;
            break;
        }
    }
    if (!pending.empty()) {
        OH_LOG_INFO(LOG_APP, "[wine:%{public}d] %{public}s", pid, pending.c_str());
    }
    close(fd);

    // EOF is not process exit. The registered reaper/callback owns status;
    // only this reader closes its fd, including when the number is reused.
    std::lock_guard<std::mutex> lock(gProcMutex);
    for (auto& entry : gProcRegistry)
        if (entry.pid == pid && entry.readerActive == active && entry.stdoutFd == fd)
            entry.stdoutFd = -1;
}

// -- stderr pipe reader (后台线程, 逐行日志) --
void StartStderrLogger(int fd, const char* tag,
                       std::shared_ptr<std::atomic<bool>> done) {
    std::thread([fd, tag, done]() {
        char buf[4096];
        std::string pending;
        while (true) {
            if (done && *done) break;
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = 0;
                pending.append(buf, n);
                size_t pos;
                while ((pos = pending.find('\n')) != std::string::npos) {
                    std::string line = pending.substr(0, pos);
                    if (!line.empty())
                        OH_LOG_INFO(LOG_APP, "[%{public}s] %{public}s", tag, line.c_str());
                    pending.erase(0, pos + 1);
                }
            } else {
                if (n == 0 || (n < 0 && errno != EINTR)) break;
            }
        }
        if (!pending.empty())
            OH_LOG_INFO(LOG_APP, "[%{public}s] %{public}s", tag, pending.c_str());
        close(fd);
    }).detach();
}

std::vector<WineProcessEntry> GetProcessListSnapshot() {
    std::lock_guard<std::mutex> lock(gProcMutex);
    return gProcRegistry;
}

bool QueryProcessSnapshot(pid_t pid, WineProcessEntry* outEntry) {
    if (!outEntry) return false;
    std::lock_guard<std::mutex> lock(gProcMutex);
    auto it = std::find_if(gProcRegistry.begin(), gProcRegistry.end(),
        [pid](const WineProcessEntry& entry) { return entry.pid == pid; });
    if (it == gProcRegistry.end()) return false;
    *outEntry = *it;
    return true;
}
