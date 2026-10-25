#pragma once
#include <string>

namespace steamohos {
// Keep game-relative Windows paths short without changing the Unix library,
// Steam's install path, or paths outside the authenticated install directory.
inline std::string ProtonGameWindowsPath(const std::string& path, const std::string& install) {
    const bool local = !install.empty() && (path == install || (path.size() > install.size() &&
        path.compare(0, install.size(), install) == 0 && path[install.size()] == '/'));
    std::string result = local ? "J:" + path.substr(install.size()) : "Z:" + path;
    if (local && result.size() == 2) result += '/';
    for (char& ch : result) if (ch == '/') ch = '\\';
    return result;
}
}
