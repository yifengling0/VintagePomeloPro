/**
 * broker.cpp — Process Broker: Unix socket server
 *
 * 在主进程中运行，接收来自 spawn_process (ntdll.so) 的子进程创建请求。
 * 每个请求包含 entryParams 字符串 + N 个命名 fd (SCM_RIGHTS, 可选 FDS 命名行)。
 * Broker 在主进程上下文调用 OH_Ability_StartNativeChildProcess，
 * 从而绕过 appspawn 子进程中无法嵌套调用 NCP API 的限制。
 *
 * 协议 (简单二进制):
 *   请求: "SPAWN\n{entryParams}\n[FDS:name0,name1,...\n]" + SCM_RIGHTS{N fd, N<=16}
 *   响应: [childPid: int32_le] [status: int32_le]   (8 字节)
 */
#include "proc/broker.h"
#include "common/wait_utils.h"
#include "wine/wine_constants.h"
#include "audio/audio_broker.h"
#include "proc/wine_process.h"
// 由 LaunchPadMode 在启动 Broker 前设置
std::string gBrokerHomeDir;
std::string gBrokerPrefixDir;

// -- 从 entryParams 解析进程名 (登记到任务列表用) --
// entryParams 形如 "homeDir|binDir|[wine]|argv0|argv1|...|__env=K=V|..."
// 或 guest/host ELF / desktop 标记路径。跳过 homeDir/binDir (前两个 '/' 段) 与
// wine/__winehua_* 标记段, 取第一个可执行段 basename (兼容 '/' 与 '\\')。
// 取不到时回退 "wine"。
static std::string ParseProcessName(const char* entryParams) {
    std::string name = "wine";
    const std::string params = entryParams ? entryParams : "";
    size_t pos = 0;
    int slashSegsLeft = 2;  // homeDir + binDir, 均以 '/' 开头
    bool guestElfNext = false;
    while (pos < params.size()) {
        size_t end = params.find('|', pos);
        std::string seg = params.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!seg.empty()) {
            if (seg.rfind("__env=", 0) == 0) break;  // env 段结束 argv
            if (guestElfNext) {  // __winehua_guest_elf__ 后的绝对路径即 exe
                auto slash = seg.find_last_of("/\\");
                if (slash != std::string::npos) seg = seg.substr(slash + 1);
                if (!seg.empty()) name = seg;
                break;
            }
            if (seg[0] == '/' && slashSegsLeft > 0) { --slashSegsLeft; }
            else if (seg == "wine" || seg == "__winehua_desktop__") { /* 标记段 */ }
            else if (seg == "__winehua_guest_elf__" || seg == "__winehua_host_elf__") { guestElfNext = true; }
            else {
                auto slash = seg.find_last_of("/\\");
                if (slash != std::string::npos) seg = seg.substr(slash + 1);
                if (!seg.empty()) name = seg;
                break;
            }
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return name;
}

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <atomic>
#include <utility>
#include <vector>
#include <AbilityKit/native_child_process.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x2330
#define LOG_TAG "WL_Broker"
#include <hilog/log.h>

static const char* kBrokerSocketPath = WINE_BROKER_SOCKET;

static std::atomic<bool> gBrokerRunning{false};
static std::atomic<bool> gBrokerListening{false};

static bool BrokerSocketConnectable()
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, kBrokerSocketPath);
    const bool ok = connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0;
    close(fd);
    return ok;
}

/**
 * Wine 的 win32u (winstation.c get_desktop_window) 在每次会话中，当默认
 * 桌面还没有窗口时，会自动以 "explorer.exe /desktop" 启动 shell。这个
 * 无路径的自启在 PC/受管窗口模式下每次引擎启动都会多弹一个 explorer
 * 窗口。这里抑制它：用户需要文件管理时通过"文件资源管理器"卡片手动
 * 打开（带 Z:\ 路径，不匹配本条件）。
 */
static bool IsAutoShellExplorerRequest(const char* entryParamsRaw)
{
    if (!entryParamsRaw || !entryParamsRaw[0]) return false;
    if (ParseProcessName(entryParamsRaw) != "explorer.exe") return false;

    const std::string params = entryParamsRaw;
    size_t exePos = params.find("explorer.exe");
    if (exePos == std::string::npos) return false;
    size_t p = exePos + strlen("explorer.exe");
    while (p < params.size() && params[p] == '|') p++;
    size_t end = params.find('|', p);
    std::string arg = params.substr(p, end == std::string::npos ? std::string::npos : end - p);
    // 自动 shell 参数是裸 /desktop（无 = 无路径）；带路径的是用户打开文件夹。
    if (arg == "/desktop") return true;
    return arg.rfind("/desktop", 0) == 0 && arg.find('=') == std::string::npos &&
        arg.size() <= 16;
}

// 处理单个请求: recvmsg(entryParams + fd) → StartNativeChildProcess → sendmsg(childPid, status)
static void HandleRequest(int conn_fd)
{
    OH_LOG_INFO(LOG_APP, "[Broker] handling request on fd=%{public}d", conn_fd);

    // 1) 接收请求头 + entryParams
    char buf[16384];
    memset(buf, 0, sizeof(buf));

    struct msghdr msg = {};
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len = sizeof(buf) - 1;

    // SCM_RIGHTS 控制消息缓冲区 (最多接收 kMaxFds 个 fd)
    static const int kMaxFds = 16;  // OHOS NativeChildProcess_FdList 上限
    union {
        char buf[CMSG_SPACE(sizeof(int) * 16)];
        struct cmsghdr align;
    } ctrl;

    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl.buf;
    msg.msg_controllen = sizeof(ctrl.buf);

    ssize_t n = recvmsg(conn_fd, &msg, 0);
    if (n <= 0) {
        // n==0: 对端 connect 后立即 close (StartBrokerServer 就绪探测), 非错误
        if (n == 0)
            OH_LOG_INFO(LOG_APP, "[Broker] probe connection (readiness check), ignoring");
        else
            OH_LOG_ERROR(LOG_APP, "[Broker] recvmsg failed: %{public}s", strerror(errno));
        close(conn_fd);
        return;
    }
    buf[n] = '\0';

    // 2) 解析 "SPAWN\n{entryParams}\n[FDS:name0,name1,...\n]"
    //    entryParams 到第一个 '\n' 为止; 其后是可选段: FDS: (逗号分隔 fd 名)。
    //    环境变量已序列化为 |__env=K=V| 段嵌入 entryParams, 无需额外解析。
    if (strncmp(buf, "SPAWN\n", 6) != 0) {
        OH_LOG_ERROR(LOG_APP, "[Broker] bad protocol: %{public}s", buf);
        close(conn_fd);
        return;
    }
    char* entryParamsRaw = buf + 6;
    char* fdsLine = nullptr;
    {
        char* nl = strchr(entryParamsRaw, '\n');
        if (nl) {
            *nl = '\0';  // 截断 entryParams
            char* rest = nl + 1;
            if (strncmp(rest, "FDS:", 4) == 0) {
                fdsLine = rest + 4;
                char* nl2 = strchr(fdsLine, '\n');
                if (nl2) {
                    *nl2 = '\0';
                }
            }
        }
    }

    OH_LOG_INFO(LOG_APP, "[Broker] request entryParams=%{public}s fds=%{public}s",
                entryParamsRaw, fdsLine ? fdsLine : "(none)");

    // 抑制 Wine 自动启动的 explorer shell（无路径 /desktop），避免 PC/受管
    // 窗口模式每次引擎启动都多弹一个 explorer 窗口。
    if (IsAutoShellExplorerRequest(entryParamsRaw)) {
        OH_LOG_INFO(LOG_APP, "[Broker] suppress auto explorer shell request (desktop-less host)");
        int32_t suppressed[2] = { -1, -1 };
        send(conn_fd, suppressed, sizeof(suppressed), MSG_NOSIGNAL);
        close(conn_fd);
        return;
    }

    // 3) 提取 fd (SCM_RIGHTS, 可能多个)
    int recvFds[kMaxFds];
    int nFds = 0;
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        int cnt = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        if (cnt < 0) cnt = 0;
        if (cnt > kMaxFds) {
            OH_LOG_WARN(LOG_APP, "[Broker] received %{public}d fds > max %{public}d, truncating", cnt, kMaxFds);
            cnt = kMaxFds;
        }
        memcpy(recvFds, CMSG_DATA(cmsg), cnt * sizeof(int));
        nFds = cnt;
        OH_LOG_INFO(LOG_APP, "[Broker] received %{public}d fd(s) via SCM_RIGHTS", nFds);
    }

    // 4) 解析 fd 名字列表 (逗号分隔)
    char* fdNames[kMaxFds] = {};
    int nNames = 0;
    if (fdsLine) {
        char* saveptr = nullptr;
        for (char* tok = strtok_r(fdsLine, ",", &saveptr); tok && nNames < kMaxFds;
             tok = strtok_r(nullptr, ",", &saveptr)) {
            fdNames[nNames++] = tok;
        }
        if (nNames != nFds) {
            OH_LOG_WARN(LOG_APP, "[Broker] FDS name count %{public}d != fd count %{public}d", nNames, nFds);
        }
    }

    // 5) 构造 NativeChildProcess 参数。
    // Wine 服务进程会把创建者的环境重新序列化给 broker，但 NCP 不会继承
    // LaunchPad 的环境。把会话 prefix 放在最后，使 clean smoke 的
    // .wine-smoke 覆盖可能残留的默认 .wine 值。
    std::string fullParams = gBrokerHomeDir.empty() ? entryParamsRaw
                            : (gBrokerHomeDir + "|" + entryParamsRaw);
    if (!gBrokerPrefixDir.empty())
        fullParams += "|__env=WINEPREFIX=" + gBrokerPrefixDir;
    OH_LOG_INFO(LOG_APP, "[Broker] dispatch prefix=%{public}s",
                gBrokerPrefixDir.empty() ? "(inherited)" : gBrokerPrefixDir.c_str());
    char* entryParamsCopy = strdup(fullParams.c_str());

    // 建 fd 链表: 名字取自 FDS 行; 无 FDS 行且恰好 1 个 fd 时回退旧命名 wineserver_sock
    NativeChildProcess_Fd nodes[kMaxFds];
    memset(nodes, 0, sizeof(nodes));
    int nNodes = 0;
    for (int i = 0; i < nFds; i++) {
        const char* name = nullptr;
        if (fdsLine && i < nNames) {
            name = fdNames[i];
        } else if (!fdsLine && nFds == 1) {
            name = "wineserver_sock";  // 向后兼容旧协议
        } else {
            OH_LOG_WARN(LOG_APP, "[Broker] fd[%{public}d]=%{public}d has no name, skipping", i, recvFds[i]);
            continue;
        }
        if (strlen(name) > 20) {
            OH_LOG_WARN(LOG_APP, "[Broker] fdName '%{public}s' exceeds 20 chars (OHOS limit)", name);
        }
        nodes[nNodes].fdName = const_cast<char*>(name);
        nodes[nNodes].fd = recvFds[i];
        nodes[nNodes].next = nullptr;
        if (nNodes > 0) nodes[nNodes - 1].next = &nodes[nNodes];
        OH_LOG_INFO(LOG_APP, "[Broker] fd[%{public}d] name=%{public}s fd=%{public}d", nNodes, name, recvFds[i]);
        nNodes++;
    }

    int audioBootstrapFd = winehua::AudioBroker::GetInstance().CreateBootstrapHandle();
    if (audioBootstrapFd >= 0 && nNodes < kMaxFds) {
        nodes[nNodes].fdName = const_cast<char*>("wine_audio_bootstrap");
        nodes[nNodes].fd = audioBootstrapFd;
        if (nNodes > 0) nodes[nNodes - 1].next = &nodes[nNodes];
        nNodes++;
    }

    NativeChildProcess_FdList fdList = {};
    fdList.head = (nNodes > 0) ? &nodes[0] : nullptr;
    NativeChildProcess_Args args = {};
    args.entryParams = entryParamsCopy;
    args.fdList = fdList;

    NativeChildProcess_Options options = {};
    options.isolationMode = NCP_ISOLATION_MODE_NORMAL;

    // 5) 调用 StartNativeChildProcess (在主进程上下文，可以调用多次)。
    // 全部走 Main: wineserver 由 wine_child Main 截获 argv[0] 转入本体
    // (纯 Unix ELF 不能走 wine loader 的 PE 解析)。
    int32_t childPid = -1;
    int32_t ret = OH_Ability_StartNativeChildProcess(
        "libwine_child.so:Main", args, options, &childPid);

    OH_LOG_INFO(LOG_APP, "[Broker] StartNativeChildProcess ret=%{public}d childPid=%{public}d",
                ret, childPid);

    if (ret == 0 && childPid > 0) {
        // 子进程继承请求方会话：Wine 的 CreateProcess 由游戏进程连上 broker
        // 发起，SO_PEERCRED 拿到请求方 pid 并解析其 sessionId。多进程游戏
        // （launcher 拉起真正进程）建窗的往往是子进程，若不继承会话，窗口
        // 的 created 事件会以 wine-<子pid> 自注册，ArkTS 的 associateToplevel
        // 匹配不到卡片启动会话 → 自动拉起缺失（程序能跑但跳不到桌面）。
        std::string inheritedSession;
        {
            struct ucred cred;
            socklen_t credLen = sizeof(cred);
            if (getsockopt(conn_fd, SOL_SOCKET, SO_PEERCRED, &cred, &credLen) == 0 &&
                cred.pid > 1) {
                inheritedSession = FindSessionIdForClientPid(cred.pid);
            }
        }
        // 全量登记: App 侧主动启动 (SpawnViaBroker) 与 wine 内部自启
        // (ohos_broker_spawn_child / loader.c 自启 wineserver) 都汇到 broker。
        // 统一登记使 explorer 里双击的 exe 出现在任务列表; App 侧调用者随后
        // 会用更准确的路径 AddProcess 覆盖 (AddProcess 同 pid 幂等)。
        AddProcess(childPid, ParseProcessName(entryParamsRaw), -1, inheritedSession);
    }

    free(entryParamsCopy);
    // 注意: 所有 fd 的所有权已转移给 StartNativeChildProcess，不要在这里 close

    // 6) 发送响应: childPid + status (8 字节，小端序)
    int32_t response[2];
    response[0] = childPid;  // pid (低 32 位)
    response[1] = ret;       // NCP_ReturnCode

    ssize_t sent = send(conn_fd, response, sizeof(response), MSG_NOSIGNAL);
    if (sent != sizeof(response)) {
        OH_LOG_ERROR(LOG_APP, "[Broker] send response failed: %{public}s", strerror(errno));
    }

    close(conn_fd);
}

// Broker 线程主循环
static void BrokerThreadFunc()
{
    OH_LOG_INFO(LOG_APP, "[Broker] thread starting");

    // 1) 创建 Unix socket
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        OH_LOG_ERROR(LOG_APP, "[Broker] socket() failed: %{public}s", strerror(errno));
        gBrokerRunning.store(false, std::memory_order_release);
        return;
    }

    // 2) 绑定到已知路径
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, kBrokerSocketPath);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        OH_LOG_ERROR(LOG_APP, "[Broker] bind(%{public}s) failed: %{public}s",
                     kBrokerSocketPath, strerror(errno));
        close(server_fd);
        gBrokerRunning.store(false, std::memory_order_release);
        return;
    }

    // 3) Listen
    if (listen(server_fd, 8) < 0) {
        OH_LOG_ERROR(LOG_APP, "[Broker] listen() failed: %{public}s", strerror(errno));
        close(server_fd);
        gBrokerRunning.store(false, std::memory_order_release);
        return;
    }

    gBrokerListening.store(true, std::memory_order_release);
    OH_LOG_INFO(LOG_APP, "[Broker] listening on %{public}s", kBrokerSocketPath);

    // 4) Accept 循环
    while (gBrokerRunning.load(std::memory_order_relaxed)) {
        int conn_fd = accept(server_fd, nullptr, nullptr);
        if (conn_fd < 0) {
            if (errno == EINTR) continue;
            OH_LOG_ERROR(LOG_APP, "[Broker] accept() failed: %{public}s", strerror(errno));
            break;
        }
        // 每个连接独立线程处理：一个 appspawn 慢请求不再阻塞其它程序启动，
        // 客户端 recv 总能等到自己这条请求的回应（成功或明确失败）。
        std::thread([](int fd) { HandleRequest(fd); }, conn_fd).detach();
    }

    close(server_fd);
    gBrokerListening.store(false, std::memory_order_release);
    gBrokerRunning.store(false, std::memory_order_release);
    unlink(kBrokerSocketPath);
    OH_LOG_INFO(LOG_APP, "[Broker] thread exiting");
}

int StartBrokerServer()
{
    if (gBrokerRunning.load(std::memory_order_acquire)) {
        const bool ready = WaitFor("broker listening", []() {
            return gBrokerListening.load(std::memory_order_acquire) &&
                   BrokerSocketConnectable();
        }, 2000, 20);
        OH_LOG_WARN(LOG_APP, "[Broker] already running, listening=%{public}s",
                    ready ? "yes" : "no");
        return ready ? 0 : -1;
    }

    // Remove a socket left by a previously killed application before the
    // worker is published as running. Waiting on file existence alone races
    // this unlink and can make Wine launch into a stale, refused socket.
    unlink(kBrokerSocketPath);
    gBrokerListening.store(false, std::memory_order_release);
    gBrokerRunning.store(true, std::memory_order_release);
    std::thread(BrokerThreadFunc).detach();

    // listen() 完成仍不够: bind 成功后 socket 文件已存在, listen 未完成时
    // connect 会 ECONNREFUSED。再加真实 connect, 与 winehua f9aaaaed 对齐。
    if (!WaitFor("broker listening", []() {
        return gBrokerListening.load(std::memory_order_acquire) &&
               BrokerSocketConnectable();
    }, 2000, 20)) {
        OH_LOG_ERROR(LOG_APP, "[Broker] failed to become ready");
        return -1;
    }
    return 0;
}
