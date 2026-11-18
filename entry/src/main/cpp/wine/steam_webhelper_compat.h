#pragma once

#include "proc/program_compat.h"
#include "wine/dll_overrides.h"

namespace winehua {

// The managed DXVK9 overlay contains x86 D3D9 only. Steam's x64 CEF must
// resolve its D3D9 import with Wine; leave the client and launched games alone.
inline bool SteamWebhelperD3d9Overrides(int argc, char* const argv[],
                                       const std::string& backend,
                                       const std::string& managedOverlay,
                                       const std::string& overrides,
                                       std::string& result)
{
    if (backend != "dxvk_legacy_d3d9" || managedOverlay != "1" ||
        ProcessImageName(argc, argv) != "steamwebhelper.exe" ||
        !NativeDllPreferred(overrides, "d3d9")) return false;
    return MergeDllOverrides(overrides, "d3d9=b", result);
}

} // namespace winehua
