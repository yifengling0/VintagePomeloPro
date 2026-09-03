#include "wine/wine_launch.h"
#include "wine/wineboot_wait.h"
#include "proc/wine_process.h"
#include "wine/wine_env.h"
#include "wine/env_profiles.h"
#include "proc/spawner.h"
#include "wine/wine_constants.h"
#include "compositor/wayland_server.h"
#include "protocols/audio_ipc_protocol.h"
#include "graphics/graphics_broker.h"
#include "graphics/graphics_profile.h"
#include "phone_adapter/phone_adapter.h"

#include <unistd.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>
#include <strings.h>
#include <thread>
#include <vector>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

#include "proc/broker.h"
#include "common/wait_utils.h"
#include "input/controller/controller_runtime.h"

// 进程启动统一走 winehua::Spawner (重构第 4-5 步): kind 推导 token 布局,
// 全部 kind 经 broker 单一通道 spawn, 本文件只声明意图。

// -- prefix 初始化检测辅助函数 --
static bool FileHasData(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static bool DirExists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool FileContainsAsciiCaseInsensitive(const std::string& path, const char* needle) {
    FILE* file = fopen(path.c_str(), "r");
    if (!file) return false;

    char line[4096];
    const size_t needleLength = strlen(needle);
    bool found = false;
    while (!found && fgets(line, sizeof(line), file)) {
        const size_t lineLength = strlen(line);
        if (lineLength < needleLength) continue;
        for (size_t offset = 0; offset + needleLength <= lineLength; ++offset) {
            size_t index = 0;
            while (index < needleLength &&
                   static_cast<unsigned char>(line[offset + index]) < 0x80 &&
                   static_cast<unsigned char>(needle[index]) < 0x80 &&
                   std::tolower(static_cast<unsigned char>(line[offset + index])) ==
                       std::tolower(static_cast<unsigned char>(needle[index]))) {
                ++index;
            }
            if (index == needleLength) {
                found = true;
                break;
            }
        }
    }
    fclose(file);
    return found;
}

static bool IsWinePrefixCorePresent(const std::string& prefixDir) {
    const std::string prefix = prefixDir.empty() ? WINE_PREFIX : prefixDir;
    return FileHasData((prefix + "/system.reg").c_str()) &&
           FileHasData((prefix + "/user.reg").c_str()) &&
           DirExists((prefix + "/drive_c/windows/system32").c_str()) &&
           DirExists((prefix + "/drive_c/windows/temp").c_str()) &&
           DirExists((prefix + "/drive_c/users").c_str());
}

bool IsWinePrefixInitialized(const std::string& prefixDir) {
    const std::string prefix = prefixDir.empty() ? WINE_PREFIX : prefixDir;
    // A partially written registry used to pass the directory-only checks and
    // permanently skip wine.inf DefaultInstall. MMDeviceEnumerator is a core
    // registration installed by that path and is also required before any
    // application can enumerate the Wine audio endpoint.
    return IsWinePrefixCorePresent(prefix) &&
           FileContainsAsciiCaseInsensitive(
               prefix + "/system.reg", "bcde0395-e52f-467c-8e3d-c4579291692e");
}

bool IsWinePrefixInitialized() {
    return IsWinePrefixInitialized(WINE_PREFIX);
}

// fork 模式下子进程退出先变僵尸、/proc/<pid> 不消失（NCP 模式由 appspawn 立即 reap）。
// 存活检测必须识别僵尸，否则 wineboot 等待会白等到 kWinebootHangMs 超时。
//
// 后端分流: fork 后端 (手机) /proc 可靠 → 保留 /proc/<pid>/stat 僵尸检测;
// NCP 后端 (平板/2in1/PC) appspawn 子进程可能不在主进程 /proc 可见范围
// (命名空间/hidepid/SELinux), fopen 必失败 → 改查进程注册表 (running 状态
// 由系统 NCP 退出回调维护), 避免把活着的 explorer/wineserver 误判为死亡
// 导致 "explorer died before registering desktop root" 启动失败。
static bool IsProcessAliveNotZombie(pid_t pid) {
    if (!PhoneAdapter_IsPhoneMode()) {
        return IsProcessRegisteredRunning(pid);
    }
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE* f = fopen(path, "r");
    if (!f) {
        // 诊断 (限频): fork 后端 /proc 读不到是异常 — ENOENT=进程真退出或
        // 命名空间不可见; EPERM/EACCES=进程在但权限/沙箱禁止读取。
        static uint32_t sOpenFailLogN = 0;
        if (++sOpenFailLogN <= 5) {
            OH_LOG_WARN(LOG_APP, "[Launch-Async] /proc/%d/stat open failed errno=%d (%s)",
                        (int)pid, errno, strerror(errno));
        }
        return false;                       // /proc 消失 = 已退出
    }
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    char* rp = strrchr(buf, ')');               // state 字段在最后一个 ')' 之后
    return !(rp && rp[2] == 'Z');               // 僵尸 = 已退出
}

enum class WinebootWaitResult {
    Completed,
    Failed,
    NoProgress,
    AbsoluteTimeout,
};

static bool IsWinebootWorker(const WineProcessEntry& entry) {
    return entry.running && !strcasecmp(entry.exeBasename.c_str(), "wineboot.exe");
}

static void StopWinebootAttempt(pid_t launcherPid) {
    // wine/box64 launcher 与 PROCESSBROKER 创建的 wineboot.exe 是 NCP 兄弟
    // 进程，不一定构成可遍历的 /proc 父子树；两类 pid 都必须显式停止。
    for (const auto& entry : GetProcessListSnapshot()) {
        if (IsWinebootWorker(entry)) KillProcessTree(entry.pid);
    }
    KillProcessTree(launcherPid);
}

static const char* WinebootFailureReason(WinebootWaitResult result) {
    if (result == WinebootWaitResult::Failed) return "wineboot-failed";
    return result == WinebootWaitResult::AbsoluteTimeout
        ? "wineboot-cap" : "wineboot-no-progress";
}

static WinebootWaitResult WaitForWinebootCompletion(pid_t launcherPid,
                                                     const winehua::WinebootAttempt& attempt,
                                                     const std::string& prefixDir,
                                                     int* waitedMsOut) {
    constexpr int kPollMs = 500;
    constexpr int kWorkerRegistrationGraceMs = 2000;
    constexpr int kNoProgressGraceMs = 90 * 1000;
    constexpr int kAbsoluteCapMs = 5 * 60 * 1000;
    const std::string progressPaths[] = {
        prefixDir + "/drive_c/windows",
        prefixDir + "/drive_c/windows/mono",
        prefixDir + "/drive_c/windows/system32",
        prefixDir + "/system.reg",
        prefixDir + "/user.reg",
    };
    auto progressStamp = [&progressPaths]() -> int64_t {
        int64_t latest = 0;
        for (const auto& path : progressPaths) {
            struct stat st;
            if (stat(path.c_str(), &st) == 0 && (int64_t)st.st_mtime > latest)
                latest = (int64_t)st.st_mtime;
        }
        return latest;
    };

    int waitedMs = 0;
    int lastProgressMs = 0;
    int64_t lastStamp = progressStamp();
    bool observedWorker = false;
    while (waitedMs < kAbsoluteCapMs) {
        const auto progress = attempt.Inspect(GetProcessListSnapshot(), launcherPid);
        if (progress.failedPid > 0) {
            OH_LOG_ERROR(LOG_APP,
                         "[Launch-Async] wineboot failed pid=%{public}d exit=%{public}d; refusing desktop startup",
                         progress.failedPid, progress.exitCode);
            if (waitedMsOut) *waitedMsOut = waitedMs;
            return WinebootWaitResult::Failed;
        }
        const bool launcherRunning = progress.launcherRunning || IsProcessAliveNotZombie(launcherPid);
        const bool workerRunning = progress.workerRunning;
        observedWorker = observedWorker || progress.workerObserved;
        if (!launcherRunning && !workerRunning &&
            (observedWorker || waitedMs >= kWorkerRegistrationGraceMs)) {
            if (waitedMsOut) *waitedMsOut = waitedMs;
            return WinebootWaitResult::Completed;
        }

        usleep(kPollMs * 1000);
        waitedMs += kPollMs;
        const int64_t nowStamp = progressStamp();
        if (nowStamp != lastStamp) {
            lastStamp = nowStamp;
            lastProgressMs = waitedMs;
        }
        if (waitedMs % 10000 == 0) {
            OH_LOG_INFO(LOG_APP,
                        "[Launch-Async] wineboot still running (%{public}d s launcher=%{public}d worker=%{public}d)",
                        waitedMs / 1000, launcherRunning ? 1 : 0, workerRunning ? 1 : 0);
        }
        if (waitedMs - lastProgressMs >= kNoProgressGraceMs) {
            OH_LOG_ERROR(LOG_APP,
                         "[Launch-Async] wineboot launcher/worker alive but no prefix progress for %{public}d s",
                         kNoProgressGraceMs / 1000);
            if (waitedMsOut) *waitedMsOut = waitedMs;
            return WinebootWaitResult::NoProgress;
        }
    }
    if (waitedMsOut) *waitedMsOut = waitedMs;
    OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineboot exceeded %{public}d s absolute cap",
                 kAbsoluteCapMs / 1000);
    return WinebootWaitResult::AbsoluteTimeout;
}

// -- WoW64 syswow64 预填充辅助 --
static bool EnsureDir(const std::string& path, mode_t mode)
{
    if (DirExists(path.c_str())) return true;
    if (mkdir(path.c_str(), mode) == 0 || errno == EEXIST) return DirExists(path.c_str());
    OH_LOG_ERROR(LOG_APP, "[Launch-Async] mkdir %{public}s failed: %{public}s",
                 path.c_str(), strerror(errno));
    return false;
}

static bool EnsureDirRecursive(const std::string& path, mode_t mode)
{
    if (path.empty() || path == "/") return true;
    if (DirExists(path.c_str())) return true;

    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0)
    {
        if (!EnsureDirRecursive(path.substr(0, slash), mode)) return false;
    }
    return EnsureDir(path, mode);
}

static bool EnsureExternalPePrefixSkeleton(const std::string& prefixDir)
{
    // The external-PE runtime resolves 64-bit Windows binaries from
    // x86_64-windows instead of copying them into drive_c.  wineboot therefore
    // does not necessarily materialize directories which Windows services and
    // diagnostics still use as working/output directories.
    static const char* const suffixes[] = {
        "/drive_c/windows/system32",
        "/drive_c/windows/system32/drivers",
        "/drive_c/windows/system32/spool",
        "/drive_c/windows/system32/tasks",
        "/drive_c/windows/temp",
    };

    bool ok = true;
    for (const char* suffix : suffixes)
        ok = EnsureDirRecursive(prefixDir + suffix, 0777) && ok;

    OH_LOG_INFO(LOG_APP, "[Launch-Async] external-PE prefix skeleton %{public}s",
                ok ? "ready" : "failed");
    return ok;
}

static bool HasRuntimeFileExtension(const char* name)
{
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    return !strcasecmp(dot, ".dll") ||
           !strcasecmp(dot, ".drv") ||
           !strcasecmp(dot, ".sys") ||
           !strcasecmp(dot, ".exe");
}

static bool CopyFileIfNeeded(const std::string& src, const std::string& dst)
{
    struct stat srcSt;
    struct stat dstSt;
    if (stat(src.c_str(), &srcSt) != 0 || !S_ISREG(srcSt.st_mode)) return false;
    if (stat(dst.c_str(), &dstSt) == 0 && S_ISREG(dstSt.st_mode) &&
        dstSt.st_size == srcSt.st_size && dstSt.st_mtime >= srcSt.st_mtime)
        return true;

    int inFd = open(src.c_str(), O_RDONLY);
    if (inFd < 0)
    {
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] open src %{public}s failed: %{public}s",
                     src.c_str(), strerror(errno));
        return false;
    }

    std::string temporaryTemplate = dst + ".winehua.tmp.XXXXXX";
    std::vector<char> temporary(temporaryTemplate.begin(), temporaryTemplate.end());
    temporary.push_back('\0');
    int outFd = mkstemp(temporary.data());
    if (outFd < 0)
    {
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] create temporary for %{public}s failed: %{public}s",
                     dst.c_str(), strerror(errno));
        close(inFd);
        return false;
    }
    fchmod(outFd, 0666);

    char buffer[64 * 1024];
    bool ok = true;
    ssize_t n;
    while ((n = read(inFd, buffer, sizeof(buffer))) > 0)
    {
        char* p = buffer;
        ssize_t remaining = n;
        while (remaining > 0)
        {
            ssize_t w = write(outFd, p, remaining);
            if (w < 0)
            {
                ok = false;
                OH_LOG_ERROR(LOG_APP, "[Launch-Async] write dst %{public}s failed: %{public}s",
                             dst.c_str(), strerror(errno));
                break;
            }
            p += w;
            remaining -= w;
        }
        if (!ok) break;
    }
    if (n < 0)
    {
        ok = false;
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] read src %{public}s failed: %{public}s",
                     src.c_str(), strerror(errno));
    }

    if (ok && fsync(outFd) != 0)
    {
        ok = false;
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] fsync dst %{public}s failed: %{public}s",
                     dst.c_str(), strerror(errno));
    }
    close(outFd);
    close(inFd);
    if (ok && rename(temporary.data(), dst.c_str()) != 0)
    {
        ok = false;
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] replace dst %{public}s failed: %{public}s",
                     dst.c_str(), strerror(errno));
    }
    if (!ok) unlink(temporary.data());
    return ok;
}

static bool EnsureWow64Files(const std::string& binDir, const std::string& prefixDir)
{
    const std::string srcDir = binDir + "/i386-windows";
    const std::string dstDir = prefixDir + "/drive_c/windows/syswow64";

    DIR* src = opendir(srcDir.c_str());
    if (!src)
    {
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] wow64 source missing %{public}s: %{public}s",
                     srcDir.c_str(), strerror(errno));
        return false;
    }
    if (!EnsureDirRecursive(dstDir, 0777))
    {
        closedir(src);
        return false;
    }

    int total = 0;
    int copied = 0;
    int failed = 0;
    while (dirent* entry = readdir(src))
    {
        if (entry->d_name[0] == '.' || !HasRuntimeFileExtension(entry->d_name)) continue;
        total++;
        std::string srcPath = srcDir + "/" + entry->d_name;
        std::string dstPath = dstDir + "/" + entry->d_name;
        if (CopyFileIfNeeded(srcPath, dstPath)) copied++;
        else failed++;
    }
    closedir(src);

    OH_LOG_INFO(LOG_APP, "[Launch-Async] wow64 syswow64 total=%{public}d ok=%{public}d failed=%{public}d",
                total, copied, failed);
    return total > 0 && failed == 0;
}
static bool IsWineserverSocketReady(const std::string& prefix) {
    char sockDir[512];
    snprintf(sockDir, sizeof(sockDir), "%s/.wineserver", prefix.c_str());
    DIR* d = opendir(sockDir);
    if (!d) return false;
    bool found = false;
    struct dirent* de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char sockPath[1024];
        snprintf(sockPath, sizeof(sockPath), "%s/%s/socket", sockDir, de->d_name);
        struct stat st;
        if (stat(sockPath, &st) == 0 && S_ISSOCK(st.st_mode)) { found = true; break; }
    }
    closedir(d);
    return found;
}

static void PrepareDesktopSessionGraphicsEnv(const LaunchParams& params)
{
    OH_LOG_INFO(LOG_APP, "[Launch-Async] preparing graphics env for child processes");
    auto& gb = winehua::GraphicsBroker::GetInstance();
    gb.SetWineRuntimeBinaryDir(params.winehuaBin);
    gb.SetRequestedBackend(winehua::GraphicsBackend::Virgl);
    gb.SetVulkanPresentMode(winehua::UsesVenusPresent(
        winehua::ParseD3dBackend(params.d3dBackend)));
    gb.EnsureStarted(params.sockDir);

    winehua::GraphicsBackendState state = gb.GetState();
    if (state.active != winehua::GraphicsBackend::Virgl) {
        OH_LOG_ERROR(LOG_APP,
                     "[Launch-Async] GL env unavailable: requested=%{public}s active=%{public}s error=%{public}s",
                     winehua::GraphicsBroker::BackendName(state.requested),
                     winehua::GraphicsBroker::BackendName(state.active),
                     state.lastError.c_str());
        return;
    }

    /* env 组装统一走 BuildSessionEnv (env_profiles.cpp); 此处只确保
     * graphics broker 就绪并记录状态。 */
    LogGraphicsBackendStateForLaunch("DesktopSession");
}

static winehua::SessionEnvPolicy SessionPolicyFromLaunch(const LaunchParams& p, int audioFd)
{
    winehua::SessionEnvPolicy s;
    s.sockDir = p.sockDir;
    s.sockName = p.sockName;
    s.libPath = p.libPath;
    s.binDir = p.winehuaBin;
    s.homeDir = p.homeDir;
    s.prefixDir = p.prefixDir;
    s.wineLang = p.wineLang;
    s.audioBootstrapFd = audioFd;
    s.d3dBackend = p.d3dBackend;
    s.dxvkBackend = p.dxvkBackend;
    s.compatEnvStr = p.compatEnvStr;
    s.automationMode = p.automationMode;
    return s;
}

// -- 引擎阶段/失败事件 (单一协调者 -> ArkTS 观察者) --
// 事件通道复用 gStateTsfn 单字符串; 语法:
//   phase:<name>    进入某个初始化阶段 (graphics/wineserver/wineboot/explorer/ready)
//   fail:<reason>   致命失败并带结构化原因
//   wine-ready      终态成功 (保留)
//   <pid>:wine-*    进程级事件 (游戏启动结果, 保留)
static void EmitEngineEvent(const char* event)
{
    if (gStateTsfn)
        napi_call_threadsafe_function(gStateTsfn, strdup(event), napi_tsfn_blocking);
}

static void EmitEnginePhase(const char* phase)
{
    std::string event = "phase:";
    event += phase;
    EmitEngineEvent(event.c_str());
}

static void EmitEngineFail(const char* reason)
{
    std::string event = "fail:";
    event += reason;
    EmitEngineEvent(event.c_str());
}

static bool LaunchPadMode(LaunchParams* p, int audioBootstrapFd) {
    winehua::Spawner::ConfigureSession(p->homeDir, p->winehuaBin, p->prefixDir);

    // Prefix registry and user data survive runtime upgrades, while the
    // syswow64 PE files are managed copies. Validate them before wineserver
    // starts so an interrupted prior refresh cannot leave a zero-length DLL.
    if (!EnsureExternalPePrefixSkeleton(p->prefixDir) ||
        !EnsureWow64Files(p->winehuaBin, p->prefixDir)) {
        OH_LOG_ERROR(LOG_APP, "[Launch-Async] external-PE prefix preparation failed");
        EmitEngineFail("prefix-prepare");
        return false;
    }

    // 图形栈必须是真实前置条件：VirGL receiver 未激活时继续启动只会得到一个
    // 无法渲染的桌面/窗口。直接报告具体原因，而不是在 SHM 回退里带病运行。
    // EnsureStarted 已同步等待 vtest socket（上限 4s），因此 GetState 的结果
    // 就是当前真实条件：Virgl=就绪，Shm+lastError=具体缺失项。
    {
        auto& gb = winehua::GraphicsBroker::GetInstance();
        const winehua::GraphicsBackendState gfxState = gb.GetState();
        if (gfxState.active != winehua::GraphicsBackend::Virgl) {
            OH_LOG_ERROR(LOG_APP,
                         "[Launch-Async] graphics backend unavailable: active=%{public}s error=%{public}s",
                         winehua::GraphicsBroker::BackendName(gfxState.active),
                         gfxState.lastError.c_str());
            EmitEngineFail("graphics-unavailable");
            return false;
        }
    }

    // -- broker 先于 wineserver 启动 (重构第 5 步) --
    // broker 是主进程内的线程, 启动不依赖 wineserver; 自此所有进程
    // (wineserver/wineboot/explorer/exe) 统一经 broker 单一通道 spawn,
    // homeDir 前缀 / WINEPREFIX 权威 / audio fd 由 broker 服务端补齐。
    gBrokerHomeDir = p->homeDir;
    gBrokerPrefixDir = p->prefixDir;
    StartBrokerServer();
    setenv("PROCESSBROKER", WINE_BROKER_SOCKET, 1);

    /* Hub socket must exist before wineboot/winedevice load winebus. */
    winehua::controller::EnsureBridgeForWineLaunch(p->prefixDir);

    // -- wineserver via broker --
    // broker → wine_child Main → 截获 argv[0]=="wineserver" 转入本体
    // (wineserver 是纯 Unix ELF, 不能走 wine loader 的 PE 解析)。
    // smoke prefix 的退出遥测由 Spawner 自动附加。
    pid_t wsChildPid = -1;
    {
        winehua::SpawnRequest wsReq{winehua::SpawnKind::Wineserver};
        // gamepad env 必须在此注入: winedevice (winebus 加载方) 是 wineserver
        // 的服务进程, env 继承自 wineserver。只注入 wineboot/explorer 链
        // (BuildSessionEnv) 时 winebus 读不到这些键, 门禁/模式全吃缺省 —
        // keyboard_legacy 兜底完全失效 (bus_ohos/bus_sdl 的 env 唯一来源)。
        // 上游: winehua/master 12aba3d4
        winehua::controller::AppendWineGamepadEnv(wsReq.env);
#ifdef __aarch64__
        winehua::AppendCompatEnvLines(wsReq.env, p->compatEnvStr, p->automationMode);
#endif
        wsChildPid = winehua::Spawner::Spawn(wsReq);
        if (wsChildPid <= 0) {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineserver spawn FAILED");
            EmitEngineFail("wineserver-spawn");
            return false;
        }
        OH_LOG_INFO(LOG_APP, "[Launch-Async] wineserver pid=%{public}d (via broker)", (int)wsChildPid);
        // 登记引擎核心进程: 用户应用全部退出/被杀后注册表仍非空,
        // 避免 handleNativeState('exited') 误判引擎 STOPPED 而拆掉桌面连接。
        AddProcess(wsChildPid, "@engine/wineserver", -1, "@engine/wineserver");
        if (!WaitFor("wineserver socket", [p]() { return IsWineserverSocketReady(p->prefixDir); }, 5000, 100)) {
            OH_LOG_WARN(LOG_APP, "[Launch-Async] wineserver socket not detected, "
                        "wineboot will recover via server_connect retry+start_server");
        }
    }

    EmitEnginePhase("wineboot");

    // -- wineboot --init --
    const std::string initMarker = p->prefixDir + "/.winehua-init-in-progress";
    const bool prefixCorePresent = IsWinePrefixCorePresent(p->prefixDir);
    bool prefixReady = prefixCorePresent && IsWinePrefixInitialized(p->prefixDir)
        && access(initMarker.c_str(), F_OK) != 0;

    if (!prefixReady) {
        // --init intentionally honors .update-timestamp. That is correct for a
        // fresh prefix, but cannot repair an old prefix which wrote the timestamp
        // before wine.inf finished. Preserve the prefix and force DefaultInstall
        // with --update only when the core registry/files already exist.
        const bool repairIncompletePrefix = prefixCorePresent;
        const char* winebootOption = repairIncompletePrefix ? "--update" : "--init";
        OH_LOG_INFO(LOG_APP,
                    "[Launch-Async] %{public}s; preparing WoW64 and running wineboot %{public}s...",
                    repairIncompletePrefix ? "prefix core present but critical registrations missing"
                                           : "prefix not initialized",
                    winebootOption);
        auto* ws = WaylandServer::GetInstance();
        ws->SetDesktopRootRecognitionEnabled(false);
        // wineboot creates shell-owned helper windows while initializing a fresh
        // prefix.  Keep those helpers on the desktop path even when the smoke
        // suite itself uses managed windows; otherwise the first clean-prefix
        // session can leave Wayland/audio/graphics services half initialized.
        const bool desktopSurface = ws->IsDesktopMode() || p->automationMode;
        // 注意: wineboot --init 只需要初始化 prefix, 不传完整环境变量以节省 entryParams 长度
        // (argv/兼容档位由 Spawner 按 kind 注入; aarch64 归一为不带 wine 加载器
        // token — Main 的 box64 路径自注 binDir/wine ELF)
        // 首启 wineboot 失败允许从标记重跑一次 (慢设备/瞬时崩溃), 避免一次失败
        // 就把整条启动链打回; 只有重试耗尽才发 fail:。
        constexpr int kMaxWinebootAttempts = 2;
        constexpr int kWinebootRetryBackoffMs = 2000;
        bool winebootOk = false;
        int winebootWaitMs = 0;
        for (int attempt = 1; attempt <= kMaxWinebootAttempts && !winebootOk; attempt++) {
            if (attempt > 1) {
                OH_LOG_WARN(LOG_APP,
                            "[Launch-Async] retrying wineboot %{public}s (attempt %{public}d/%{public}d)",
                            winebootOption, attempt, kMaxWinebootAttempts);
                usleep(kWinebootRetryBackoffMs * 1000);
            }
            if (FILE* marker = fopen(initMarker.c_str(), "w")) {
                fputs("wineboot\n", marker);
                fclose(marker);
            } else {
                OH_LOG_ERROR(LOG_APP, "[Launch-Async] cannot create prefix init marker: %{public}s",
                             initMarker.c_str());
                EmitEngineFail("wineboot-failed");
                return false;
            }
            winehua::SpawnRequest wbReq{winehua::SpawnKind::Wineboot};
            wbReq.desktopSurface = desktopSurface;
            wbReq.env = {"LANG=" + p->wineLang + ".UTF-8",
                         "LC_ALL=" + p->wineLang + ".UTF-8"};
#ifdef __aarch64__
            winehua::AppendCompatEnvLines(wbReq.env, p->compatEnvStr, p->automationMode);
#endif
            winehua::controller::AppendWineGamepadEnv(wbReq.env);
            const winehua::WinebootAttempt bootAttempt(GetProcessListSnapshot());
            const pid_t childPid = winehua::Spawner::Spawn(wbReq);
            if (childPid <= 0) {
                OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineboot spawn FAILED (attempt %{public}d)",
                             attempt);
                if (attempt >= kMaxWinebootAttempts) {
                    EmitEngineFail("wineboot-spawn");
                    return false;
                }
                continue;
            }
            OH_LOG_INFO(LOG_APP, "[Launch-Async] wineboot started, pid=%{public}d (attempt %{public}d)",
                        childPid, attempt);
            // 登记 wineboot 为引擎核心: broker 会先以 basename 入任务列表
            // (已知短暂可见), 随后同 pid 覆盖为 @engine/wineboot, 避免用户
            // 杀光应用时误判引擎 STOPPED。等待循环走 /proc 存活, 不依赖登记。
            AddProcess(childPid, "@engine/wineboot", -1, "@engine/wineboot");
            /* 等待外层 wine/box64 launcher 以及 PROCESSBROKER 实际创建的
             * wineboot.exe。只等 launcher 会在 Windows worker 仍运行时提前
             * 启动 Explorer，导致所有客户端永久卡在 boot event。 */
            const WinebootWaitResult waitResult =
                WaitForWinebootCompletion(childPid, bootAttempt, p->prefixDir, &winebootWaitMs);
            if (waitResult != WinebootWaitResult::Completed) {
                StopWinebootAttempt(childPid);
                OH_LOG_ERROR(LOG_APP,
                             "[Launch-Async] wineboot attempt %{public}d/%{public}d did not complete",
                             attempt, kMaxWinebootAttempts);
                if (attempt >= kMaxWinebootAttempts) {
                    EmitEngineFail(WinebootFailureReason(waitResult));
                    return false;
                }
                continue;
            }
        /* wineboot 已退出: registry 仍在 wineserver flush 途中 (实测落盘延迟
         * 稳定 ~13s), 宽限窗口等文件就绪 — 文件到位即通过, 不会满等 */
            if (!WaitFor("wine prefix",
                         [&p]() { return IsWinePrefixInitialized(p->prefixDir); },
                         60000, 200)) {
                OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineboot exited but prefix incomplete, abort (attempt %{public}d)",
                             attempt);
                if (attempt >= kMaxWinebootAttempts) {
                    EmitEngineFail("wineboot-failed");
                    return false;
                }
                continue;
            }
            winebootOk = true;
        }
        OH_LOG_INFO(LOG_APP, "[Launch-Async] wineboot completed (%{public}d s)", winebootWaitMs / 1000);
        unlink(initMarker.c_str());
        // wineboot 退出仅代表 prefix 初始化结束; wineserver socket 才是 Wine
        // 服务栈对外的就绪信号。未就绪时若直接放行, 首个 GUI 进程可能抢跑
        // 建立渲染 surface 失败 (PC 首启白屏的竞态来源之一)。非致命: socket
        // 就绪后由 explorer/应用自身的 server_connect 重试收敛。
        if (!WaitFor("wineserver socket after wineboot",
                     [p]() { return IsWineserverSocketReady(p->prefixDir); }, 5000, 100)) {
            OH_LOG_WARN(LOG_APP, "[Launch-Async] wineserver socket not ready after wineboot");
        }
        ws->SetDesktopRootRecognitionEnabled(true);
        ws->PromotePendingDesktopRoot();
    } else {
        /* 二启 (prefix 已初始化): 显式播种 wineboot boot 事件。
         * 每个新 wineserver 会话的内核对象全空, 第一个客户端 (explorer) 会
         * 在 ntdll run_wineboot 里触发 wineboot --init——该路径继承 appspawn
         * 环境 (LD_PRELOAD=libappspawn_helper.z.so 等), 实测 wineboot 卡死
         * (注册表已写但 .update-timestamp 不更新), SetEvent 永不执行, 之后
         * 所有 Wine 进程都卡在 boot 事件等待, 窗口全部出不来。这里用与首启
         * 相同的干净环境显式跑一次 wineboot: 正常完成后事件 signaled,
         * explorer 的 run_wineboot 检查事件已存在, 立即放行。
         * 参数必须用 --init: wineboot.c 的 wWinMain 传 update_wineprefix(update),
         * 而 update_wineprefix 的参数名就是 force——--update 会让 force=true,
         * 无条件重装 wine.inf 并弹出 "Setting up Wine" 等待窗; --init 传
         * force=false, 仅当 wine.inf 时间戳变化 (升级) 才重装。 */
        OH_LOG_INFO(LOG_APP, "[Launch-Async] prefix ready; seeding wineboot boot event (--init)...");
        winehua::SpawnRequest wbReq{winehua::SpawnKind::Wineboot};
        wbReq.env = {"LANG=" + p->wineLang + ".UTF-8",
                     "LC_ALL=" + p->wineLang + ".UTF-8"};
#ifdef __aarch64__
        winehua::AppendCompatEnvLines(wbReq.env, p->compatEnvStr, p->automationMode);
#endif
        winehua::controller::AppendWineGamepadEnv(wbReq.env);
        const winehua::WinebootAttempt bootAttempt(GetProcessListSnapshot());
        const pid_t childPid = winehua::Spawner::Spawn(wbReq);
        if (childPid <= 0) {
            OH_LOG_ERROR(LOG_APP, "[Launch-Async] wineboot --init spawn FAILED");
        } else {
            OH_LOG_INFO(LOG_APP, "[Launch-Async] wineboot --init pid=%{public}d", childPid);
            AddProcess(childPid, "@engine/wineboot", -1, "@engine/wineboot");
            int aliveMs = 0;
            const WinebootWaitResult waitResult =
                WaitForWinebootCompletion(childPid, bootAttempt, p->prefixDir, &aliveMs);
            if (waitResult != WinebootWaitResult::Completed) {
                StopWinebootAttempt(childPid);
                EmitEngineFail(WinebootFailureReason(waitResult));
                return false;
            }
            OH_LOG_INFO(LOG_APP, "[Launch-Async] wineboot launcher + worker completed (%{public}d ms)",
                        aliveMs);
            if (!WaitFor("wineserver socket after wineboot seed",
                         [p]() { return IsWineserverSocketReady(p->prefixDir); }, 5000, 100)) {
                OH_LOG_WARN(LOG_APP, "[Launch-Async] wineserver socket not ready after wineboot seed");
            }
        }
    }

    // -- explorer desktop shell (仅 desktop 模式) --
    PrepareDesktopSessionGraphicsEnv(*p);

    if (p->automationMode)
    {
        OH_LOG_INFO(LOG_APP, "[Launch-Async] automation session ready; Explorer intentionally skipped");
    }
    else if (WaylandServer::GetInstance()->IsDesktopMode())
    // -- explorer (Desktop 或 Pad 模式均启动) --
    {
        auto* ws = WaylandServer::GetInstance();
        ws->SetDesktopRootRecognitionEnabled(true);
        int dw = ws->OutputWidth() > 0 ? ws->OutputWidth() : 1280;
        int dh = ws->OutputHeight() > 0 ? ws->OutputHeight() : 720;
        OH_LOG_INFO(LOG_APP, "[Launch-Async] explorer desktop size: outputW=%{public}d outputH=%{public}d → %{public}dx%{public}d",
                    ws->OutputWidth(), ws->OutputHeight(), dw, dh);
        char desktopSize[64];
        EmitEnginePhase("explorer");
        constexpr int kExplorerMaxAttempts = 3;
        constexpr int kExplorerRetryBackoffMs = 2000;
        int explorerAttempt = 0;
        /* 附带 winehua_keep.exe: 加入 shell desktop 并持久运行,
         * 避免最后一个用户应用退出后 wineserver 自动关闭桌面.
         * 仅 Pad 桌面模式需要, Phone 模式走单窗口, 无需此逻辑. */
        snprintf(desktopSize, sizeof(desktopSize), "/desktop=shell,%dx%d", dw, dh);
        winehua::SessionEnvPolicy explorerPolicy = SessionPolicyFromLaunch(*p, audioBootstrapFd);
        explorerPolicy.applyStableOverlay = true;
        std::vector<std::string> explorerEnv = winehua::BuildSessionEnv(explorerPolicy);
        winehua::SpawnRequest exReq{winehua::SpawnKind::DesktopShell};
        exReq.argv = {desktopSize, "winehua_keep.exe"};
        exReq.env = std::move(explorerEnv);
        bool explorerRootReady = false;
        while (!explorerRootReady && explorerAttempt < kExplorerMaxAttempts) {
            explorerAttempt++;
            const pid_t exPid = winehua::Spawner::Spawn(exReq);
            OH_LOG_INFO(LOG_APP, "[Launch-Async] explorer desktop attempt=%{public}d/%{public}d pid=%{public}d (via broker)",
                        explorerAttempt, kExplorerMaxAttempts, (int)exPid);
            if (exPid > 0) {
                // 桌面壳进程登记为引擎核心进程: 保证用户程序停止后桌面保持存活,
                // 且"关闭运行中的程序"不会把桌面一起带走。
                AddProcess(exPid, "@engine/explorer", -1, "@engine/explorer");
            }
            ws->PromotePendingDesktopRoot();
            if (exPid <= 0) {
                OH_LOG_ERROR(LOG_APP, "[Launch-Async] explorer desktop spawn failed (attempt %{public}d)",
                             explorerAttempt);
                if (explorerAttempt >= kExplorerMaxAttempts) {
                    EmitEngineFail("explorer-spawn");
                    return false;
                }
                usleep(kExplorerRetryBackoffMs * 1000);
                continue;
            }
            /* 桌面根是 wine-ready 的真实前置条件:
             * - root toplevel 注册 → 条件满足, 立即放行;
             * - explorer 死亡 → 自动重试 (慢设备/内存压力下 explorer 可能瞬崩);
             * - wineserver 死亡 → 明确失败;
             * 仅"进程活着但 root 永不注册"的挂死由看门狗兜底。 */
            constexpr int kRootCheckIntervalMs = 100;
            constexpr int kRootWatchdogMs = 10 * 60 * 1000;
            int waitedMs = 0;
            bool attemptFailed = false;
            while (ws->GetDesktopRootToplevelId() == 0) {
                if (!IsProcessAliveNotZombie(exPid)) {
                    OH_LOG_ERROR(LOG_APP,
                                 "[Launch-Async] explorer desktop died before registering desktop root (attempt %{public}d/%{public}d)",
                                 explorerAttempt, kExplorerMaxAttempts);
                    attemptFailed = true;
                    break;
                }
                if (wsChildPid > 0 && !IsProcessAliveNotZombie(wsChildPid)) {
                    OH_LOG_ERROR(LOG_APP,
                                 "[Launch-Async] wineserver died before explorer registered desktop root");
                    EmitEngineFail("wineserver-died");
                    return false;
                }
                if (waitedMs >= kRootWatchdogMs) {
                    OH_LOG_ERROR(LOG_APP,
                                 "[Launch-Async] explorer alive but desktop root never registered (%d s), abort",
                                 waitedMs / 1000);
                    EmitEngineFail("explorer-root-timeout");
                    return false;
                }
                usleep(kRootCheckIntervalMs * 1000);
                waitedMs += kRootCheckIntervalMs;
            }
            if (attemptFailed) {
                if (explorerAttempt >= kExplorerMaxAttempts) {
                    EmitEngineFail("explorer-died");
                    return false;
                }
                usleep(kExplorerRetryBackoffMs * 1000);
                continue;
            }
            explorerRootReady = ws->GetDesktopRootToplevelId() != 0;
        }
        OH_LOG_INFO(LOG_APP, "[Launch-Async] explorer desktop root ready tl=%{public}d",
                    ws->GetDesktopRootToplevelId());
    }
    else
    {
        // 非桌面模式 (PC/受管窗口/单窗口): 不自动启动 explorer 文件管理器窗口。
        // master phase-2 合并曾在此自动弹出 explorer, 导致 PC 上每次引擎启动
        // 都多弹一个 explorer 窗口; 用户需要文件管理时用"文件资源管理器"卡片
        // 手动打开 (Index.ets 手动启动走相同的 broker 路径)。
        OH_LOG_INFO(LOG_APP,
                    "[Launch-Async] non-desktop engine ready; explorer window intentionally not auto-started");
    }
    return true;
}

void LaunchThreadFunc(LaunchParams* p) {
    OH_LOG_INFO(LOG_APP, "[Launch-Async] wineserver + wineboot + wine starting in background");
    OH_LOG_INFO(LOG_APP, "[Launch-Async] XKB_CONFIG_ROOT=%{public}s",
                (p->winehuaBin + "/../share/X11/xkb").c_str());

    auto& graphicsBroker = winehua::GraphicsBroker::GetInstance();
    graphicsBroker.SetWineRuntimeBinaryDir(p->winehuaBin);
    graphicsBroker.SetVulkanPresentMode(winehua::UsesVenusPresent(
        winehua::ParseD3dBackend(p->d3dBackend)));
    EmitEnginePhase("graphics");
    graphicsBroker.EnsureStarted(p->sockDir);

    int audioBootstrapFd = CreateAudioBootstrapFd(p->sockDir);
    // Resolve VirGL/backend state before the explorer session env is built so
    // NCP children inherit the active receiver rather than an early SHM snapshot.
    PrepareDesktopSessionGraphicsEnv(*p);
    // 会话 env 不再预先构建: 唯一消费者是 explorer 桌面链, 它在 LaunchPadMode
    // 内用 BuildSessionEnv 现取现建, 图形状态更新鲜。

    mkdir(p->prefixDir.c_str(), 0755);

    EmitEnginePhase("wineserver");

    bool ok = false;
    ok = LaunchPadMode(p, audioBootstrapFd);

    if (ok) {
        EmitEnginePhase("ready");
        EmitEngineEvent("wine-ready");
    }

    delete p;
}
