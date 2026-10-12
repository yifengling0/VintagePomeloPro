// Exercise the production session pipeline; only platform runtime discovery
// is stubbed. Protect the transport invariant across overlays and app extras.
#include "wine/env_profiles.h"
#include "proc/wine_spawn_compatibility.h"
#include "proc/wine_image_policy.h"
#include <filesystem>
#include <fstream>
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
static int runtimeEnvironmentCalls = 0;
void AppendVulkanRuntimeEnv(std::vector<std::string>&, const std::string&) { ++runtimeEnvironmentCalls; }
static bool Has(const std::vector<std::string>& env, const std::string& line) {
    return std::count(env.begin(), env.end(), line) == 1;
}
static void TestCleanProcessGraphics() {
    using namespace winehua::spawn;
    char path[] = "/tmp/vp-clean-graphics-XXXXXX";
    assert(mkdtemp(path));
    const std::filesystem::path runtime(path);
    const auto icd = runtime / "guest_vulkan/share/vulkan/icd.d" / (std::string("venus_icd.") + WINE_WINE_ARCH + ".json");
    std::filesystem::create_directories(icd.parent_path());
    std::ofstream(icd) << "{}";
    Request original; original.home = "/home/session";original.bin = runtime;
    original.argv = {"wine", "Z:/folder with spaces/sample.exe", "中文参数"};
    original.names = {"wineserver_sock", "wine_audio_bootstrap"};
    original.env = {"WINEHUA_VULKAN_BACKEND=venus", "WINEHUA_VULKAN_BACKEND=direct",
        "WINEHUA_D3D_BACKEND=dxvk_legacy", "WINEHUA_VK_PRECISE_MAP=1",
        "FEX_SMCCHECKS=1", "FEX_X87REDUCEDPRECISION=0", "WINEPREFIX=/kept-prefix"};
    auto request = original;
    assert(ConfigureFreshWineGraphics(request, false, runtime) == FreshWineGraphics::Unchanged);
    assert(request.env == original.env && runtimeEnvironmentCalls == 0);
    winehua::SetProductDirectVulkanVerified(true);
    assert(ConfigureFreshWineGraphics(request, true, runtime) == FreshWineGraphics::Venus);
    assert(runtimeEnvironmentCalls == 1 && winehua::IsProductDirectVulkanVerified());
    assert(request.argv == original.argv && request.names == original.names && request.home == original.home);
    assert(Has(request.env, "WINEHUA_VULKAN_BACKEND=venus"));
    assert(!Has(request.env, "WINEHUA_VULKAN_BACKEND=direct"));
    assert(Has(request.env, "WINEHUA_VK_PRECISE_MAP=0"));
    assert(Has(request.env, "VN_WINEHUA_STRONG_RING_BARRIER=1"));
    assert(Has(request.env, "DXVK_WINEHUA_PRECISE_SHADOW=1"));
    assert(Has(request.env, "FEX_SMCCHECKS=1") && Has(request.env, "FEX_X87REDUCEDPRECISION=0"));
    assert(Has(request.env, "WINEPREFIX=/kept-prefix"));
    const auto resolved = request.env;
    assert(ConfigureFreshWineGraphics(request, true, runtime) == FreshWineGraphics::Unchanged);
    assert(request.env == resolved && runtimeEnvironmentCalls == 1);
    std::vector<unsigned char> packet;
    assert(Encode(request, packet));
    Request decoded; assert(Decode(packet, decoded));
    assert(decoded.env == request.env && decoded.argv == original.argv && decoded.names == original.names);
    std::filesystem::remove(icd);
    request = original;
    assert(ConfigureFreshWineGraphics(request, true, runtime) == FreshWineGraphics::MissingRuntime);
    assert(request.env == original.env && runtimeEnvironmentCalls == 1);
    std::filesystem::remove_all(runtime);
    winehua::SetProductDirectVulkanVerified(false);
}

int main() {
    TestCleanProcessGraphics();
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
    winehua::ProductGraphicsPolicy policy;
    assert(winehua::ResolveProductGraphicsPolicy(winehua::D3dBackendKind::DxvkLegacy, &policy));
    assert(policy.guest.directVulkan && policy.vulkanDesktop);
    auto productEnv = env;
    setenv("WINEHUA_GRAPHICS_PROFILE", "isolate-product-egl-desktop", 1);
    env = winehua::BuildSessionEnv(session);
    assert(Has(env, "WINEHUA_VULKAN_BACKEND=direct"));
    assert(winehua::ResolveSessionGraphicsPolicy("isolate-product-egl-desktop",
        winehua::D3dBackendKind::DxvkLegacy, &policy));
    assert(policy.guest.directVulkan && !policy.vulkanDesktop);
    // All guest settings stay identical; only the host consumer changes.
    const auto removeProfile = [](std::vector<std::string>& values) {
        values.erase(std::remove_if(values.begin(), values.end(), [](const auto& value) {
            return value.rfind("WINEHUA_GRAPHICS_PROFILE=", 0) == 0;
        }), values.end());
        std::sort(values.begin(), values.end());
    };
    auto eglEnv = env;
    removeProfile(productEnv);
    removeProfile(eglEnv);
    assert(productEnv == eglEnv);
    setenv("WINEHUA_GRAPHICS_PROFILE", "product-vulkan", 1);
    winehua::SetProductDirectVulkanVerified(false);
    env = winehua::BuildSessionEnv(session);
    assert(Has(env, "WINEHUA_VULKAN_BACKEND=venus"));
    assert(Has(env, "WINEHUA_VK_PRECISE_MAP=0"));
    assert(Has(env, "DXVK_WINEHUA_PRECISE_SHADOW=1"));
    assert(Has(env, "DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED=1"));
    std::cout << "PASS: automatic Direct, protected mapping contract and Venus fallback\n";
}
