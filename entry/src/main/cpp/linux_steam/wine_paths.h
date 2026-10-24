#pragma once
#include <string>
#include <vector>
namespace steamohos {
inline bool WindowsPath(const std::string& input, const std::string& prefix,
                        const std::string& zroot, std::string& output) {
    if (input.empty() || input[0] != '/' || input.find_first_of("\\\0", 0, 2) != input.npos) return false;
    std::vector<std::string> parts; size_t at = 1;
    while (at <= input.size()) {
        size_t end = input.find('/', at); if (end == input.npos) end = input.size();
        const auto part = input.substr(at, end - at);
        if (part == "..") { if (parts.empty()) return false; parts.pop_back(); }
        else if (!part.empty() && part != ".") {
            if (part.find(':') != part.npos) return false;
            for (unsigned char c : part) if (c < 32) return false;
            parts.push_back(part);
        }
        at = end + 1;
    }
    std::string path; for (const auto& part : parts) path += '/' + part;
    for (const auto& drive : {std::pair<char, std::string>{'C', prefix.empty() ? "" : prefix + "/drive_c"}, {'Z', zroot}}) {
        const auto& base = drive.second;
        if (base.empty() || base[0] != '/' || base == "/") continue;
        if (path != base && (path.size() <= base.size() || path.compare(0, base.size() + 1, base + '/') != 0)) continue;
        output = std::string(1, drive.first) + ":\\";
        if (path.size() > base.size()) output += path.substr(base.size() + 1);
        for (char& c : output) if (c == '/') c = '\\';
        return true;
    }
    return false;
}
}
