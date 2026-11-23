#pragma once
#include "spawn_codec.h"
#include <string>
namespace winehua::spawn {
struct SteamGameOpenGLSetting {
    std::string directory;
    bool enabled;
};
void SetSteamGameOpenGLSettings(const std::vector<SteamGameOpenGLSetting>& settings);
bool ApplySteamGameOpenGLSetting(Request& request);

enum class FreshWineGraphics { Unchanged, Venus, MissingRuntime };
// Resolve graphics against the selected child address space before spawning.
// This never retries a launch or changes the active desktop consumer.
FreshWineGraphics ConfigureFreshWineGraphics(Request& request, bool fresh,
                                             const std::string& binDir);
}
