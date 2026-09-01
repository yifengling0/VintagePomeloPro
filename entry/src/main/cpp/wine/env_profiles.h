#ifndef WINE_ENV_PROFILES_H
#define WINE_ENV_PROFILES_H

/**
 * env_profiles.h — 环境策略集中地 (重构第 3 步)
 *
 * 管线顺序只在此文件定义一次:
 *   BuildWineEnv (L0-L5 基线, wine_env.cpp)
 *     L4 窗口模式: WINEHUA_DESKTOP_MODE / SIMULATE_RESOLUTION
 *       取值 WindowingModeFor (wine_env_baseline.h), 与 master 同层。
 *       spawn 点不要再写这两键。
 *   → AppendD3dBackendEnv (dxvk/vkd3d 受管 overlay, wine_env.cpp)
 *   → AppendCompatEnvLines (设置页兼容档位, 本文件)
 *   → AppendStableDxvkEnv (桌面会话稳定化 overlay, 本文件, 可选;
 *     含本仓 host-shadow / WINEHUA_PERF_PROFILE)
 *     桌面与程序直启均使用产品能力解析器，不在 ArkTS 维护平行默认值。
 *   → WINEHUA_DESKTOP=shell (可选)
 *   → extraEnv (per-run/per-app 覆盖, 优先级最高)
 *
 * spawn 点声明 SessionEnvPolicy 拿成品, 不再各自追加策略行。
 *
 * WineHua 的 dxvkBackend / wineLang 控制面字段保留在公共接口中；产品
 * graphics resolver 仍以 d3dBackend 为最终策略来源。Box64 档位 policy
 * 仍在 ArkTS AppModels.resolveBox64PresetEnv；此处只做前缀门。
 */

#include <string>
#include <vector>

namespace winehua {

// -- 兼容模式全局档位 (设置页 → launchClient compatEnvStr 分号串) --
#ifdef __aarch64__
std::vector<std::string> FilterCompatLines(const std::string& compatEnvStr);

void AppendCompatEnvLines(std::vector<std::string>& env,
                          const std::string& compatEnvStr, bool automationMode);
#endif // __aarch64__

// -- 会话 DXVK 产品 capability --
// probeBase: 产品路由或明确的 LAB `WINEHUA_GRAPHICS_PROFILE` 的探测基准。
// 实现保证全部 probeBase 读取先于对 env 的写入, 允许调用方传同一 vector。
void AppendStableDxvkEnv(std::vector<std::string>& env,
                                const std::vector<std::string>& probeBase,
                                const std::string& d3dBackend);

// -- 会话/程序 env 管线 --
struct SessionEnvPolicy {
    std::string sockDir, sockName, libPath, binDir, homeDir, prefixDir;
    std::string wineLang = "zh_CN";
    int audioBootstrapFd = -1;
    // 空 = 不注入 D3D overlay (由 extraEnv 自带)
    std::string d3dBackend;
    std::string dxvkBackend = "dxvk_legacy";
    std::string compatEnvStr;
    bool automationMode = false;
    bool applyStableOverlay = false;
    bool desktopShellFlag = false;
    std::vector<std::string> extraEnv;
};

std::vector<std::string> BuildSessionEnv(const SessionEnvPolicy& p);

} // namespace winehua

#endif // WINE_ENV_PROFILES_H
