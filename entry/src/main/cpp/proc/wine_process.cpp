#include "proc/wine_process.h"
#include "wine/wine_constants.h"
#include "phone_adapter/phone_adapter.h"
#include <AbilityKit/native_child_process.h>

#include <unistd.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <dirent.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <chrono>
#include <fstream>
#include <cctype>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

// -- 全局状态 --
static std::mutex gProcMutex;
static std::vector<WineProcessEntry> gProcRegistry;
struct PendingToplevel {
    pid_t clientPid;
    uint32_t toplevelId;
    std::string sessionId;
};
static std::vector<PendingToplevel> gPendingToplevels;

static uint64_t TimestampMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// 前向声明
static void EnsureMonitorRunning();

// -- 注册表辅助函数 --
static std::string NormalizePath(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    while (value.size() > 1 && value.back() == '/') value.pop_back();
    return value;
}

static pid_t ReadParentPid(pid_t pid) {
    std::ifstream status("/proc/" + std::to_string(pid) + "/status");
    std::string key;
    while (status >> key) {
        if (key == "PPid:") {
            pid_t parent = 0;
            status >> parent;
            return parent;
        }
        std::string rest;
        std::getline(status, rest);
    }
    return 0;
}

static bool IsProcessOrDescendant(pid_t clientPid, pid_t expectedParent) {
    pid_t current = clientPid;
    for (int depth = 0; current > 1 && depth < 16; depth++) {
        if (current == expectedParent) return true;
        const pid_t parent = ReadParentPid(current);
        if (parent <= 1 || parent == current) break;
        current = parent;
    }
    return false;
}

WineProcessEntry* AddProcess(pid_t pid, const std::string& exeFullPath, int stdoutFd,
                             const std::string& requestedSessionId) {
    std::lock_guard<std::mutex> lock(gProcMutex);
    std::string basename = exeFullPath;
    // 兼容 Windows 反斜杠路径 (C:\game\game.exe), 否则完整路径会显示为进程名
    auto slash = basename.find_last_of("/\\");
    if (slash != std::string::npos) basename = basename.substr(slash + 1);
    // Broker registers the child before the launch worker labels it as an
    // engine process. Relabeling must not erase an exit received in between.
    if (exeFullPath.rfind("@engine/", 0) == 0) {
        for (auto& entry : gProcRegistry) {
            if (entry.pid != pid) continue;
            entry.exeBasename = basename;
            entry.exeFullPath = exeFullPath;
            if (!requestedSessionId.empty()) entry.sessionId = requestedSessionId;
            return &entry;
        }
    }
    gProcRegistry.erase(std::remove_if(gProcRegistry.begin(), gProcRegistry.end(),
        [pid](const WineProcessEntry& entry) { return entry.pid == pid; }), gProcRegistry.end());
    while (gProcRegistry.size() >= 128) {
        auto ended = std::find_if(gProcRegistry.begin(), gProcRegistry.end(),
            [](const WineProcessEntry& entry) { return !entry.running; });
        if (ended == gProcRegistry.end()) break;
        gProcRegistry.erase(ended);
    }
    gProcRegistry.push_back({
        .pid = pid,
        .exeBasename = basename,
        .exeFullPath = exeFullPath,
        .sessionId = requestedSessionId.empty()
            ? "wine-" + std::to_string(pid) : requestedSessionId,
        .toplevelId = 0,
        .running = true,
        .startTimestampMs = TimestampMs(),
        .endTimestampMs = 0,
        .exitCode = -1,
        .exitCodeSource = "unknown",
        .stdoutFd = stdoutFd,
        .readerActive = std::make_shared<std::atomic<bool>>(true)
    });
    WineProcessEntry& added = gProcRegistry.back();
    for (auto pending = gPendingToplevels.begin(); pending != gPendingToplevels.end();) {
        const bool sameSession = !pending->sessionId.empty() &&
            pending->sessionId == added.sessionId;
        if (sameSession || IsProcessOrDescendant(pending->clientPid, pid)) {
            if (added.toplevelId == 0) added.toplevelId = pending->toplevelId;
            pending = gPendingToplevels.erase(pending);
        } else {
            ++pending;
        }
    }
    OH_LOG_INFO(LOG_APP, "[ProcReg] add pid=%{public}d name=%{public}s total=%{public}zu",
                pid, basename.c_str(), gProcRegistry.size());
    EnsureMonitorRunning();
    return &gProcRegistry.back();
}

pid_t FindRunningProcessByPath(const std::string& exeFullPath) {
    const std::string expected = NormalizePath(exeFullPath);
    std::lock_guard<std::mutex> lock(gProcMutex);
    for (const auto& entry : gProcRegistry) {
        if (entry.running && NormalizePath(entry.exeFullPath) == expected) return entry.pid;
    }
    return -1;
}

std::string FindSessionIdForClientPid(pid_t clientPid) {
    std::lock_guard<std::mutex> lock(gProcMutex);
    pid_t current = clientPid;
    for (int depth = 0; current > 1 && depth < 16; depth++) {
        for (const auto& entry : gProcRegistry) {
            if (entry.running && entry.pid == current) return entry.sessionId;
        }
        const pid_t parent = ReadParentPid(current);
        if (parent <= 1 || parent == current) break;
        current = parent;
    }
    // 诊断 (限频): 查不到会话 — 客户端自身未登记且 /proc 祖先链不可读。
    // NCP 后端 appspawn 子进程可能不在主进程 /proc 可见范围 (命名空间/
    // hidepid/SELinux), 会导致 created 事件 sessionId 为空、ArkTS 关联不上。
    static uint32_t sEmptyLogN = 0;
    if (++sEmptyLogN <= 3) {
        OH_LOG_WARN(LOG_APP,
                    "[ProcReg] FindSessionIdForClientPid empty for pid=%{public}d (registry miss + /proc unreadable?)",
                    clientPid);
    }
    return "";
}

bool GetProcessBySessionId(const std::string& sessionId, WineProcessEntry* result) {
    if (sessionId.empty() || !result) return false;
    std::lock_guard<std::mutex> lock(gProcMutex);
    for (const auto& entry : gProcRegistry) {
        if (entry.running && entry.sessionId == sessionId) {
            *result = entry;
            return true;
        }
    }
    return false;
}

bool AssociateToplevelWithSession(const std::string& sessionId, pid_t clientPid,
                                  uint32_t toplevelId) {
    if (toplevelId == 0) return false;
    std::lock_guard<std::mutex> lock(gProcMutex);
    for (auto& entry : gProcRegistry) {
        const bool sameSession = !sessionId.empty() && entry.sessionId == sessionId;
        if (sameSession || IsProcessOrDescendant(clientPid, entry.pid)) {
            if (entry.toplevelId == 0) entry.toplevelId = toplevelId;
            return true;
        }
    }
    gPendingToplevels.push_back({clientPid, toplevelId, sessionId});
    if (gPendingToplevels.size() > 32) gPendingToplevels.erase(gPendingToplevels.begin());
    return false;
}

void RemoveToplevelAssociation(uint32_t toplevelId) {
    std::lock_guard<std::mutex> lock(gProcMutex);
    for (auto& entry : gProcRegistry) {
        if (entry.toplevelId == toplevelId) entry.toplevelId = 0;
    }
    gPendingToplevels.erase(std::remove_if(gPendingToplevels.begin(), gPendingToplevels.end(),
        [toplevelId](const PendingToplevel& pending) {
            return pending.toplevelId == toplevelId;
        }), gPendingToplevels.end());
}

bool IsProcessRegisteredRunning(pid_t pid) {
    std::lock_guard<std::mutex> lock(gProcMutex);
    for (const auto& entry : gProcRegistry) {
        if (entry.pid == pid && entry.running) return true;
    }
    return false;
}

void RemoveProcess(pid_t pid, int exitCode, const std::string& exitCodeSource) {
    std::lock_guard<std::mutex> lock(gProcMutex);
    for (auto& entry : gProcRegistry) {
        if (entry.pid == pid) {
            // /proc polling and NCP notifications can arrive after waitpid.
            // An observation without a status must not erase the real result.
            if (!entry.running && entry.exitCode >= 0 && exitCode < 0) return;
            OH_LOG_INFO(LOG_APP,
                        "[ProcReg] complete pid=%{public}d name=%{public}s exit=%{public}d source=%{public}s",
                        pid, entry.exeBasename.c_str(), exitCode, exitCodeSource.c_str());
            entry.running = false;
            if (entry.endTimestampMs == 0) entry.endTimestampMs = TimestampMs();
            entry.exitCode = exitCode;
            entry.exitCodeSource = exitCodeSource;
            if (entry.stdoutFd >= 0) { close(entry.stdoutFd); entry.stdoutFd = -1; }
            return;
        }
    }
}

void KillAllProcesses() {
    std::lock_guard<std::mutex> lock(gProcMutex);
    for (auto& entry : gProcRegistry) {
        if (entry.running) {
            OH_LOG_INFO(LOG_APP, "[ProcReg] killAll pid=%{public}d name=%{public}s",
                        entry.pid, entry.exeBasename.c_str());
            *(entry.readerActive) = false;
            kill(entry.pid, SIGKILL);
            if (entry.stdoutFd >= 0) { close(entry.stdoutFd); entry.stdoutFd = -1; }
        }
    }
    gPendingToplevels.clear();
}

void KillProcessTree(pid_t root) {
    if (root <= 1) return;
    // BFS 收集 root 的全部后代 (wine/box64 的 fork 链), 后代先杀、根最后杀,
    // 避免子进程被 reparent 后脱离进程树继续存活。
    std::vector<pid_t> killOrder;
    std::unordered_set<pid_t> members;
    members.insert(root);
    bool added = true;
    while (added) {
        added = false;
        std::vector<pid_t> candidates;
        DIR* dir = opendir("/proc");
        if (!dir) break;
        struct dirent* e;
        while ((e = readdir(dir))) {
            const pid_t pid = atoi(e->d_name);
            if (pid > 1 && !members.count(pid)) candidates.push_back(pid);
        }
        closedir(dir);
        for (pid_t pid : candidates) {
            if (members.count(ReadParentPid(pid))) {
                members.insert(pid);
                killOrder.push_back(pid);
                added = true;
            }
        }
    }
    for (auto it = killOrder.rbegin(); it != killOrder.rend(); ++it) {
        if (kill(*it, SIGKILL) != 0 && errno != ESRCH) {
            OH_LOG_WARN(LOG_APP, "[ProcReg] tree kill child pid=%{public}d failed: %{public}s",
                        *it, strerror(errno));
        }
    }
    if (kill(root, SIGKILL) != 0 && errno != ESRCH) {
        OH_LOG_WARN(LOG_APP, "[ProcReg] tree kill root pid=%{public}d failed: %{public}s",
                    root, strerror(errno));
    }
    OH_LOG_INFO(LOG_APP, "[ProcReg] tree kill root=%{public}d descendants=%{public}zu",
                root, killOrder.size());
}

// -- 进程退出状态日志 --
void LogProcessExit(const char* tag, pid_t pid, int status) {
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        OH_LOG_ERROR(LOG_APP, "[%{public}s] CRASH pid=%{public}d signal=%{public}d(%{public}s) core=%{public}d",
                     tag, pid, sig, strsignal(sig), WCOREDUMP(status) ? 1 : 0);
    } else if (WIFEXITED(status)) {
        OH_LOG_INFO(LOG_APP, "[%{public}s] process %{public}d exited code=%{public}d",
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

// -- SIGCHLD handler: reap NCP child processes spawned by broker --
void sigchld_handler(int) {
    const int savedErrno = errno;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        LogProcessExit("broker-child", pid, status);
        const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) :
            (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
        RemoveProcess(pid, exitCode, WIFSIGNALED(status) ? "signal" : "waitpid");
        if (gStateTsfn) {
            char msg[64];
            snprintf(msg, sizeof(msg), "%d:exited", pid);
            napi_call_threadsafe_function(gStateTsfn, strdup(msg), napi_tsfn_blocking);
        }
    }
    errno = savedErrno;
}

// -- NCP 进程存活监控 --
// NCP 子进程由 appspawn 创建，不是主进程的 fork() 子进程，
// SIGCHLD 收不到它们的退出事件。
//
// 后端分流:
//   fork 后端 (手机/电视): 同 PID 命名空间, /proc/<pid> 可靠 → 保留轮询。
//   NCP 后端 (平板/2in1/PC): appspawn 子进程可能不在主进程 /proc 可见范围
//     (PID 命名空间 / hidepid 挂载 / SELinux), access() 会误判存活进程为
//     死亡 → 以系统 NCP 退出回调为权威退出信号, /proc 轮询停用。
//     症状: 每个进程注册后 1s 被误标 "no longer alive", 启动线程以为
//     explorer/wineserver 已死 → explorer-died 启动失败 (MatePad Pro 复现)。
static std::atomic<bool> gMonitorRunning{false};
static std::thread gMonitorThread;
static std::atomic<bool> gNcpCallbackActive{false};

static void OnNcpChildExit(int32_t pid, int32_t signal) {
    OH_LOG_INFO(LOG_APP, "[ProcMon] NCP exit callback pid=%{public}d signal=%{public}d",
                pid, signal);
    RemoveProcess(pid, -1, "ncp-exit");
    if (gStateTsfn) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%d:exited", pid);
        napi_call_threadsafe_function(gStateTsfn, strdup(msg), napi_tsfn_blocking);
    }
}

static void ProcessMonitorLoop() {
    OH_LOG_INFO(LOG_APP, "[ProcMon] started (backend=%{public}s)",
                PhoneAdapter_IsPhoneMode() ? "fork" : "ncp");
    bool procProbeLogged = false;
    while (gMonitorRunning.load(std::memory_order_relaxed)) {
        sleep(1);

        if (!PhoneAdapter_IsPhoneMode()) {
            // NCP 后端: 一律不用 /proc 轮询 (appspawn 子进程可能不可见, 会误判
            // 存活进程为死亡并清空注册表 → 启动失败)。退出事件由系统 NCP 退出
            // 回调权威上报; 回调注册失败时仅退出检测降级 (注册表条目保留, 由
            // 显式 stop/重启路径清理), 启动流程不受影响。
            // 一次性诊断: 探测 NCP 子进程在 /proc 的可见性, 供后续分析设备差异
            // (ENOENT=命名空间/hidepid 隐藏, EPERM/EACCES=存在但权限/沙箱禁止)。
            if (!procProbeLogged) {
                procProbeLogged = true;
                std::lock_guard<std::mutex> lock(gProcMutex);
                for (const auto& entry : gProcRegistry) {
                    if (!entry.running) continue;
                    char procPath[64];
                    snprintf(procPath, sizeof(procPath), "/proc/%d", entry.pid);
                    errno = 0;
                    int r = access(procPath, F_OK);
                    OH_LOG_WARN(LOG_APP,
                                "[ProcMon] NCP /proc probe pid=%{public}d access=%{public}d errno=%{public}d (%{public}s) callback=%{public}s → 存活以 NCP 退出回调为准",
                                entry.pid, r, errno, errno ? strerror(errno) : "ok",
                                gNcpCallbackActive.load(std::memory_order_acquire) ? "active" : "failed");
                    break;
                }
            }
            continue;
        }

        std::vector<pid_t> exitedPids;
        {
            std::lock_guard<std::mutex> lock(gProcMutex);
            for (const auto& entry : gProcRegistry) {
                if (!entry.running) continue;
                char procPath[64];
                snprintf(procPath, sizeof(procPath), "/proc/%d", entry.pid);
                if (access(procPath, F_OK) != 0) {
                    exitedPids.push_back(entry.pid);
                    // 诊断: fork 后端 /proc 读不到是异常 — ENOENT=进程真退出或
                    // 命名空间不可见; EPERM/EACCES=进程在但权限/沙箱禁止读取。
                    OH_LOG_WARN(LOG_APP,
                                "[ProcMon] /proc/%d access failed errno=%d (%s)",
                                entry.pid, errno, strerror(errno));
                }
            }
        }

        for (pid_t pid : exitedPids) {
            OH_LOG_INFO(LOG_APP, "[ProcMon] pid=%{public}d no longer alive", pid);
            RemoveProcess(pid);
            if (gStateTsfn) {
                char msg[64];
                snprintf(msg, sizeof(msg), "%d:exited", pid);
                napi_call_threadsafe_function(gStateTsfn, strdup(msg), napi_tsfn_blocking);
            }
        }
    }
    OH_LOG_INFO(LOG_APP, "[ProcMon] stopped");
}

static void EnsureMonitorRunning() {
    // 注册系统 NCP 子进程退出回调 (幂等; fork 后端注册后不触发, 无副作用)。
    static std::once_flag sNcpCallbackOnce;
    std::call_once(sNcpCallbackOnce, [] {
        auto ret = OH_Ability_RegisterNativeChildProcessExitCallback(OnNcpChildExit);
        gNcpCallbackActive = (ret == NCP_NO_ERROR);
        OH_LOG_INFO(LOG_APP, "[ProcMon] NCP exit callback register ret=%{public}d", (int)ret);
    });
    if (!gMonitorRunning.load(std::memory_order_acquire)) {
        gMonitorRunning.store(true, std::memory_order_release);
        gMonitorThread = std::thread(ProcessMonitorLoop);
        gMonitorThread.detach();
    }
}

// -- 客户端 stdout/stderr 读取线程 (每个进程独立) --
void ReaderThread(int fd, pid_t pid, std::shared_ptr<std::atomic<bool>> active) {
    char buf[2048];
    std::string pending;
    while (*active) {
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

    int status = 0;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    // The SIGCHLD path may already own this child's status. Do not interpret
    // an uninitialized/absent status as a successful exit or overwrite it.
    if (waited != pid) return;
    LogProcessExit("wine", pid, status);
    int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) :
        (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
    RemoveProcess(pid, exitCode, WIFSIGNALED(status) ? "signal" : "waitpid");

    if (gStateTsfn) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%d:exited", pid);
        napi_call_threadsafe_function(gStateTsfn, strdup(msg), napi_tsfn_blocking);
    }
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
