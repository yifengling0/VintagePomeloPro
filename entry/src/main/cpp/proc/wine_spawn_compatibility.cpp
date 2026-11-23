#include "wine_spawn_compatibility.h"
#include "wine_image_policy.h"
#include "graphics/graphics_profile.h"
#include "wine/wine_env.h"
#include "wine/wine_constants.h"
#include <cstdlib>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

namespace winehua::spawn {
namespace {
std::mutex steamGameOpenGLMutex;
std::map<std::string, bool> steamGameOpenGLDirectories;

struct OpenGLOverride {
    const char* key;
    const char* value;
    const char* inheritedKey;
};

constexpr OpenGLOverride steamOpenGLOverrides[] = {
    { "MESA_GL_VERSION_OVERRIDE", "3.3FC", "WINEHUA_STEAM_PREVIOUS_GL_VERSION" },
    { "MESA_GLSL_VERSION_OVERRIDE", "330", "WINEHUA_STEAM_PREVIOUS_GLSL_VERSION" },
};

std::string CanonicalPath(const std::string& path) {
    if (path.empty() || path.front() != '/') return {};
    char* resolved = realpath(path.c_str(), nullptr);
    if (!resolved) return {};
    std::string result(resolved);
    free(resolved);
    return result;
}

void RestoreInheritedOpenGL(Request& request, const OpenGLOverride& override) {
    const std::string saved = ImageEnvironment(request, override.inheritedKey);
    if (saved.empty()) return;
    const std::string originalPrefix = std::string(override.key) + '=';
    const std::string inheritedPrefix = std::string(override.inheritedKey) + '=';
    auto& env = request.env;
    env.erase(std::remove_if(env.begin(), env.end(), [&](const std::string& line) {
        return line.rfind(originalPrefix, 0) == 0 || line.rfind(inheritedPrefix, 0) == 0;
    }), env.end());
    if (saved.front() == '1') env.push_back(originalPrefix + saved.substr(1));
}

bool SteamGameOpenGL33Enabled(const Request& request) {
    std::lock_guard<std::mutex> lock(steamGameOpenGLMutex);
    if (steamGameOpenGLDirectories.empty()) return false;
    const std::string image = CanonicalPath(WineImageNativePath(request));
    const auto slash = image.find_last_of('/');
    if (slash == std::string::npos) return false;
    std::string name = image.substr(slash + 1);
    std::transform(name.begin(), name.end(), name.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
    });
    if (name.size() < 4 || name.compare(name.size() - 4, 4, ".exe") ||
        name == "steam.exe" || name == "steamwebhelper.exe") return false;
    // Prefer the closest owner, including games that have opted out.
    for (auto parent = slash; parent > 0; parent = image.rfind('/', parent - 1)) {
        const auto entry = steamGameOpenGLDirectories.find(image.substr(0, parent));
        if (entry != steamGameOpenGLDirectories.end()) return entry->second;
    }
    return false;
}
} // namespace

void SetSteamGameOpenGLSettings(const std::vector<SteamGameOpenGLSetting>& settings) {
    std::map<std::string, bool> directories;
    if (std::any_of(settings.begin(), settings.end(), [](const auto& setting) { return setting.enabled; })) {
        for (const auto& setting : settings) {
            const std::string path = CanonicalPath(setting.directory);
            struct stat st{};
            if (path.size() <= 1 || stat(path.c_str(), &st) || !S_ISDIR(st.st_mode)) continue;
            const auto [entry, inserted] = directories.emplace(path, setting.enabled);
            if (!inserted) entry->second = entry->second && setting.enabled;
        }
    }
    std::lock_guard<std::mutex> lock(steamGameOpenGLMutex);
    steamGameOpenGLDirectories.swap(directories);
}

bool ApplySteamGameOpenGLSetting(Request& request) {
    // Wine forwards the creator's Unix environment. Restore its previous
    // values before resolving the new image, even if the setting was removed.
    for (const auto& override : steamOpenGLOverrides) RestoreInheritedOpenGL(request, override);
    if (!SteamGameOpenGL33Enabled(request)) return false;
    for (const auto& override : steamOpenGLOverrides) {
        const std::string prefix = std::string(override.key) + '=';
        const auto original = std::find_if(request.env.rbegin(), request.env.rend(),
            [&](const std::string& line) { return line.rfind(prefix, 0) == 0; });
        const std::string saved = original == request.env.rend() ? "0" : "1" + original->substr(prefix.size());
        UpsertEnvLine(request.env, std::string(override.inheritedKey) + '=' + saved);
        UpsertEnvLine(request.env, prefix + override.value);
    }
    return true;
}

FreshWineGraphics ConfigureFreshWineGraphics(Request& request, bool fresh,
                                             const std::string& binDir) {
    if (!fresh || ImageEnvironment(request, "WINEHUA_VULKAN_BACKEND") != "direct")
        return FreshWineGraphics::Unchanged;
    const std::string icd = binDir + "/guest_vulkan/share/vulkan/icd.d/venus_icd."
        WINE_WINE_ARCH ".json";
    if (access(icd.c_str(), R_OK) != 0) return FreshWineGraphics::MissingRuntime;
    // Opt-in A/B for the validated legacy DXVK range-publication contract.
    // HOST_COHERENT clients without these explicit upload hooks still need
    // the whole-map fallback. Capture the request before Venus policy rebuilds it.
    const bool preciseMaps =
        ImageEnvironment(request, "WINEHUA_FRESH_VENUS_PRECISE_MAP") == "1" &&
        ImageEnvironment(request, "WINEHUA_D3D_BACKEND") == "dxvk_legacy" &&
        ImageEnvironment(request, "DXVK_WINEHUA_PRECISE_SHADOW") == "1" &&
        ImageEnvironment(request, "DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED") == "1";
    // Preserve the route while observing the existing rate-limited ring
    // counters. LAB observation profiles may otherwise rebuild that route.
    const bool ringPerf =
        ImageEnvironment(request, "WINEHUA_FRESH_VENUS_PERF_SUMMARY") == "1";
    auto environment = request.env;
    // Start NCP has a clean low address space but no verified native Vulkan
    // driver/producer IPC. Keep GPU work in the existing renderer process.
    AppendVulkanRuntimeEnv(environment, binDir);
    std::vector<std::string> venus;
    if (winehua::BuildVenusGuestGraphicsEnvironment(
            winehua::ParseD3dBackend(ImageEnvironment(request, "WINEHUA_D3D_BACKEND")), &venus))
        for (const auto& value : venus) UpsertEnvLine(environment, value);
    UpsertEnvLine(environment, "WINEHUA_VULKAN_BACKEND=venus");
    UpsertEnvLine(environment, preciseMaps ? "WINEHUA_VK_PRECISE_MAP=1"
                                         : "WINEHUA_VK_PRECISE_MAP=0");
    UpsertEnvLine(environment, ringPerf ? "VN_WINEHUA_PERF_SUMMARY=1"
                                      : "VN_WINEHUA_PERF_SUMMARY=0");
    UpsertEnvLine(environment, "WINEHUA_PRESENT_BACKEND=venus_broker_present");
    UpsertEnvLine(environment, "WINEHUA_VULKAN_PRESENT=1");
    request.env = std::move(environment);
    return FreshWineGraphics::Venus;
}
}
