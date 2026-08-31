#include "wine/env_profiles.h"
#include "graphics/graphics_profile.h"
#include "wine/wine_env.h"

#include <cstdlib>
#include <cstring>

namespace winehua {

#ifdef __aarch64__
std::vector<std::string> FilterCompatLines(const std::string& compatEnvStr)
{
    std::vector<std::string> raw;
    std::string cur;
    for (const char c : compatEnvStr) {
        if (c == ';') {
            if (!cur.empty()) raw.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) raw.push_back(cur);
    std::vector<std::string> filtered;
    for (const std::string& line : raw) {
        if (line.rfind("BOX64_DYNAREC_", 0) != 0)
            continue;
        if (line.find('|') != std::string::npos || line.find('\n') != std::string::npos)
            continue;
        if (line.find('=') == std::string::npos)
            continue;
        filtered.push_back(line);
    }
    return filtered;
}

void AppendCompatEnvLines(std::vector<std::string>& env,
                          const std::string& compatEnvStr, bool automationMode)
{
    if (automationMode)
        return;
    for (const std::string& line : FilterCompatLines(compatEnvStr))
        UpsertEnvLine(env, line);
}
#endif // __aarch64__

static std::string FindEnvValue(const std::vector<std::string>& probeBase, const char* key)
{
    const std::string prefix = std::string(key) + "=";
    for (auto it = probeBase.rbegin(); it != probeBase.rend(); ++it) {
        if (it->rfind(prefix, 0) == 0)
            return it->substr(prefix.size());
    }
    return {};
}

void AppendStableDxvkEnv(std::vector<std::string>& env,
                                const std::vector<std::string>& probeBase,
                                const std::string& d3dBackend)
{
    const D3dBackendKind backend = ParseD3dBackend(d3dBackend);
    if (!IsDxvkBackend(backend)) return;

    ProductGraphicsPolicy product;
    if (!ResolveProductGraphicsPolicy(backend, &product)) return;

    std::string graphicsExperiment =
        FindEnvValue(probeBase, "WINEHUA_GRAPHICS_PROFILE");
    GuestGraphicsPolicy guest = product.guest;
    if (graphicsExperiment == kProductVirglRoute ||
        graphicsExperiment == kProductVulkanRoute) {
        graphicsExperiment.clear();
    } else if (!graphicsExperiment.empty()) {
        ProductGraphicsPolicy experiment;
        if (ResolveLabGraphicsExperiment(graphicsExperiment, backend, &experiment)) {
            guest = experiment.guest;
        } else {
            // A stale or misspelled LAB identifier must never suppress the
            // product DXVK capability. It simply falls back to product.
            graphicsExperiment.clear();
        }
    }

    AppendProductDxvkEnv(env, d3dBackend, graphicsExperiment);
    if (guest.perfSummary) {
        UpsertEnvLine(env, "VN_WINEHUA_PERF_SUMMARY=1");
        UpsertEnvLine(env,
            "VN_WINEHUA_PERF_LOG=/storage/Users/currentUser/Download/"
            "com.vintage.pomelopro/winehua_guest_ring_perf.log");
        UpsertEnvLine(env, "MESA_LOG_LEVEL=debug");
    }
}

std::vector<std::string> BuildSessionEnv(const SessionEnvPolicy& p)
{
    std::vector<std::string> env = BuildWineEnv(p.sockDir, p.sockName, p.libPath,
                                                p.binDir, p.audioBootstrapFd, p.homeDir,
                                                p.prefixDir, p.wineLang);
    if (!p.d3dBackend.empty())
        AppendD3dBackendEnv(env, p.d3dBackend, p.dxvkBackend, p.binDir);
#ifdef __aarch64__
    AppendCompatEnvLines(env, p.compatEnvStr, p.automationMode);
#endif
    if (p.applyStableOverlay) {
        // `extraEnv` wins in the final result, but a permitted LAB id must be
        // visible while the product graphics capability is resolved.
        std::vector<std::string> graphicsProbe = env;
        for (const std::string& line : p.extraEnv)
            UpsertEnvLine(graphicsProbe, line);
        AppendStableDxvkEnv(env, graphicsProbe, p.d3dBackend);
    }
    if (p.desktopShellFlag)
        UpsertEnvLine(env, "WINEHUA_DESKTOP=shell");
    for (const std::string& line : p.extraEnv)
        UpsertEnvLine(env, line);
    return env;
}

} // namespace winehua
