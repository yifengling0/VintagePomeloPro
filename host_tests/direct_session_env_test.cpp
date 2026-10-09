// Exercise the production session pipeline; only platform runtime discovery
// is stubbed. Protect the transport invariant across overlays and app extras.
#include "wine/env_profiles.h"
#include "wine/wine_env.h"
#include "graphics/graphics_profile.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iostream>

std::vector<std::string> BuildWineEnv(const std::string&, const std::string&,
    const std::string&, const std::string&, int, const std::string&,
    const std::string&, const std::string&) { return {}; }
void UpsertEnvLine(std::vector<std::string>& env, const std::string& line) {
    auto prefix = line.substr(0, line.find('=') + 1);
    env.erase(std::remove_if(env.begin(), env.end(), [&](const auto& old) {
        return old.rfind(prefix, 0) == 0;
    }), env.end());
    env.push_back(line);
}
void AppendD3dBackendEnv(std::vector<std::string>& env, const std::string& backend,
    const std::string&, const std::string&) {
    UpsertEnvLine(env, "WINEHUA_D3D_BACKEND=" + backend);
    UpsertEnvLine(env, "DXVK_WINEHUA_PRECISE_SHADOW=1");
}
void AppendVulkanRuntimeEnv(std::vector<std::string>&, const std::string&) {}
static bool Has(const std::vector<std::string>& env, const std::string& line) {
    return std::count(env.begin(), env.end(), line) == 1;
}
int main() {
    winehua::SessionEnvPolicy session;
    session.d3dBackend = "dxvk_legacy";
    session.applyStableOverlay = true;
    setenv("WINEHUA_GRAPHICS_PROFILE", "isolate-vulkan-direct", 1);
    auto env = winehua::BuildSessionEnv(session);
    assert(Has(env, "WINEHUA_D3D_BACKEND=dxvk_legacy"));
    assert(Has(env, "WINEHUA_VULKAN_BACKEND=direct"));
    assert(Has(env, "DXVK_WINEHUA_PRECISE_SHADOW=0"));
    assert(Has(env, "WINEHUA_VK_PRECISE_MAP=0"));
    setenv("WINEHUA_GRAPHICS_PROFILE", "isolate-vulkan-direct-precise-map", 1);
    env = winehua::BuildSessionEnv(session);
    assert(Has(env, "WINEHUA_VULKAN_BACKEND=direct"));
    assert(Has(env, "WINEHUA_VK_PRECISE_MAP=1"));
    assert(Has(env, "DXVK_WINEHUA_PRECISE_SHADOW=1"));
    assert(Has(env, "DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1"));
    setenv("WINEHUA_GRAPHICS_PROFILE", "isolate-vulkan-direct-whole-map", 1);
    env = winehua::BuildSessionEnv(session);
    assert(Has(env, "WINEHUA_VULKAN_BACKEND=direct"));
    assert(Has(env, "WINEHUA_VK_PRECISE_MAP=0"));
    assert(Has(env, "DXVK_WINEHUA_PRECISE_SHADOW=1"));
    assert(Has(env, "DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1"));
    setenv("WINEHUA_GRAPHICS_PROFILE", "isolate-vulkan-direct", 1);
    session.extraEnv = {"WINEHUA_VULKAN_BACKEND=venus", "DXVK_LOG_LEVEL=info"};
    env = winehua::BuildSessionEnv(session);
    assert(Has(env, "WINEHUA_VULKAN_BACKEND=direct"));
    assert(Has(env, "DXVK_LOG_LEVEL=info"));
    setenv("WINEHUA_GRAPHICS_PROFILE", "observe-product-summary", 1);
    env = winehua::BuildSessionEnv(session);
    assert(Has(env, "WINEHUA_VULKAN_BACKEND=venus"));
    assert(Has(env, "DXVK_WINEHUA_PRECISE_SHADOW=1"));
    setenv("WINEHUA_GRAPHICS_PROFILE", "product-vulkan", 1);
    session.extraEnv = {"WINEHUA_GRAPHICS_PROFILE=isolate-vulkan-direct"};
    env = winehua::BuildSessionEnv(session);
    assert(!Has(env, "WINEHUA_VULKAN_BACKEND=direct"));
    assert(Has(env, "DXVK_WINEHUA_PRECISE_SHADOW=1"));
    winehua::SetProductDirectVulkanVerified(true);
    session.extraEnv = {"WINEHUA_VULKAN_BACKEND=venus", "WINEHUA_VK_PRECISE_MAP=0",
        "DXVK_WINEHUA_PRECISE_SHADOW=0", "DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=0",
        "WINEHUA_GRAPHICS_PROFILE=isolate-vulkan-direct", "DXVK_LOG_LEVEL=info"};
    env = winehua::BuildSessionEnv(session);
    assert(Has(env, "WINEHUA_GRAPHICS_PROFILE=product-vulkan"));
    assert(Has(env, "WINEHUA_VULKAN_BACKEND=direct"));
    assert(Has(env, "WINEHUA_VK_PRECISE_MAP=1"));
    assert(Has(env, "DXVK_WINEHUA_PRECISE_SHADOW=1"));
    assert(Has(env, "DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1"));
    assert(Has(env, "DXVK_LOG_LEVEL=info"));
    winehua::SetProductDirectVulkanVerified(false);
    env = winehua::BuildSessionEnv(session);
    assert(Has(env, "WINEHUA_VULKAN_BACKEND=venus"));
    assert(Has(env, "WINEHUA_VK_PRECISE_MAP=0"));
    assert(Has(env, "DXVK_WINEHUA_PRECISE_SHADOW=1"));
    assert(Has(env, "DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1"));
    std::cout << "PASS: automatic Direct, protected mapping contract and Venus fallback\n";
}
