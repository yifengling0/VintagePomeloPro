#include "wine_spawn_compatibility.h"
#include "wine_image_policy.h"
#include "graphics/graphics_profile.h"
#include "wine/wine_env.h"
#include "wine/wine_constants.h"
#include <unistd.h>

namespace winehua::spawn {
FreshWineGraphics ConfigureFreshWineGraphics(Request& request, bool fresh,
                                             const std::string& binDir) {
    if (!fresh || ImageEnvironment(request, "WINEHUA_VULKAN_BACKEND") != "direct")
        return FreshWineGraphics::Unchanged;
    const std::string icd = binDir + "/guest_vulkan/share/vulkan/icd.d/venus_icd."
        WINE_WINE_ARCH ".json";
    if (access(icd.c_str(), R_OK) != 0) return FreshWineGraphics::MissingRuntime;
    auto environment = request.env;
    // Start NCP has a clean low address space but no verified native Vulkan
    // driver/producer IPC. Keep GPU work in the existing renderer process.
    AppendVulkanRuntimeEnv(environment, binDir);
    std::vector<std::string> venus;
    if (winehua::BuildVenusGuestGraphicsEnvironment(
            winehua::ParseD3dBackend(ImageEnvironment(request, "WINEHUA_D3D_BACKEND")), &venus))
        for (const auto& value : venus) UpsertEnvLine(environment, value);
    UpsertEnvLine(environment, "WINEHUA_VULKAN_BACKEND=venus");
    UpsertEnvLine(environment, "WINEHUA_VK_PRECISE_MAP=0");
    UpsertEnvLine(environment, "WINEHUA_PRESENT_BACKEND=venus_broker_present");
    UpsertEnvLine(environment, "WINEHUA_VULKAN_PRESENT=1");
    request.env = std::move(environment);
    return FreshWineGraphics::Venus;
}
}
