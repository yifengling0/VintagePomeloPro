#include "proc/spawner.h"
#include "wine/wine_constants.h"
#include "wine/wine_exe.h"

#include <string>
#include <sys/types.h>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_SPAWN"
#include <hilog/log.h>

namespace winehua {
namespace {

// 会话上下文 (见 spawner.h): binDir 默认与 prefix (smoke 遥测判定) 由此取;
// homeDir 前缀 / WINEPREFIX 权威由 broker 服务端追加 (broker.cpp)
std::string gBinDir, gPrefixDir;

#ifdef __aarch64__
constexpr bool kNeedsWineLoaderToken = false;
#else
constexpr bool kNeedsWineLoaderToken = true;
#endif

void LogSpawnEnv(const SpawnRequest& req)
{
    if (req.env.empty()) return;
    std::string envJoined;
    for (const std::string& line : req.env) {
        if (!envJoined.empty()) envJoined += ";";
        envJoined += line;
    }
    OH_LOG_INFO(LOG_APP, "[Spawner] env kind=%{public}d count=%{public}zu [%{public}s]",
                (int)req.kind, req.env.size(), envJoined.c_str());
}

pid_t SpawnLogged(const SpawnRequest& req, const std::string& params) {
    LogSpawnEnv(req);
    const pid_t pid = SpawnViaBroker(params, req.env);
    if (pid <= 0)
        OH_LOG_ERROR(LOG_APP, "[Spawner] broker spawn FAILED kind=%{public}d params=%{public}s",
                     (int)req.kind, params.c_str());
    else
        OH_LOG_INFO(LOG_APP, "[Spawner] broker spawn kind=%{public}d pid=%{public}d params=%{public}s",
                    (int)req.kind, (int)pid, params.c_str());
    return pid;
}

} // namespace

void Spawner::ConfigureSession(std::string homeDir, std::string binDir, std::string prefixDir) {
    (void)homeDir;  // broker 服务端权威 (gBrokerHomeDir), 此处仅记录备查
    gBinDir = std::move(binDir);
    gPrefixDir = std::move(prefixDir);
}

// 全部 kind 统一走 broker (重构第 5 步): broker 服务端补 homeDir 前缀、
// WINEPREFIX 会话权威 (尾部 __env 追加, 后写胜出)、audio bootstrap fd;
// broker 本身在主进程内以线程运行, 启动不依赖 wineserver, 无先后环。
pid_t Spawner::Spawn(const SpawnRequest& req) {
    const std::string& binDir = req.binDir.empty() ? gBinDir : req.binDir;

    // token 布局: binDir|[desktop]|[wine]|argv... (broker 收到后再补
    // homeDir 前缀; __env 段由 SpawnViaBroker 序列化追加)。
    // wine_child Main 按此解析; wineserver 由 Main 截获转入本体。
    std::string params = binDir;
    switch (req.kind) {
    case SpawnKind::Wineserver:
        params += "|wineserver|-f|-p";
        break;
    case SpawnKind::Wineboot:
        if (req.desktopSurface) params += "|__winehua_desktop__";
        if (kNeedsWineLoaderToken) params += "|wine";
        params += "|wineboot|--init";
        break;
    case SpawnKind::DesktopShell:
        params += "|__winehua_desktop__";
        if (kNeedsWineLoaderToken) params += "|wine";
        params += "|explorer";
        break;
    case SpawnKind::WineExe:
        if (kNeedsWineLoaderToken) params += "|wine";
        break;
    case SpawnKind::GuestElf:
        params += "|__winehua_guest_elf__";
        break;
    case SpawnKind::HostElf:
        params += "|__winehua_host_elf__";
        break;
    }
    for (const std::string& arg : req.argv) {
        params += "|";
        params += arg;
    }

    // smoke prefix 的 wineserver 带退出遥测 (会话级判定, 与调用方无关)
    if (req.kind == SpawnKind::Wineserver && gPrefixDir == WINE_SMOKE_PREFIX) {
        SpawnRequest withTelemetry = req;
        withTelemetry.env.push_back("WINEHUA_PROCESS_EXIT_TELEMETRY=1");
        return SpawnLogged(withTelemetry, params);
    }
    return SpawnLogged(req, params);
}

} // namespace winehua
