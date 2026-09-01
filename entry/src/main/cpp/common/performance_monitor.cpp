#include "common/performance_monitor.h"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <limits>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace performance_monitor {
namespace {
using Clock = std::chrono::steady_clock;
bool Unsigned(const std::string& text, uint64_t& out)
{
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) return false;
    char* end = nullptr;
    errno = 0;
    const auto value = strtoull(text.c_str(), &end, 10);
    if (errno || !end || *end) return false;
    out = value;
    return true;
}
bool Add(uint64_t a, uint64_t b, uint64_t& out)
{
    if (a > std::numeric_limits<uint64_t>::max() - b) return false;
    out = a + b;
    return true;
}
bool ReadLine(const std::string& path, std::string& line)
{
    FILE* file = fopen(path.c_str(), "re");
    if (!file) return false;
    char buffer[4096];
    const bool ok = fgets(buffer, sizeof(buffer), file) != nullptr;
    fclose(file);
    if (ok) line = buffer;
    return ok;
}
}

bool ParseProcessStat(const std::string& line, ProcessTicks& out)
{
    // comm may contain spaces and ')'. Fields 14/15 exclude child CPU times.
    const auto open = line.find('('), close = line.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close <= open) return false;
    std::istringstream id(line.substr(0, open));
    ProcessTicks parsed;
    if (!(id >> parsed.pid) || parsed.pid <= 0) return false;
    std::istringstream fields(line.substr(close + 1));
    std::string token;
    uint64_t user = 0, system = 0;
    for (int field = 3; field <= 22; ++field) {
        if (!(fields >> token)) return false;
        if (field == 14 && !Unsigned(token, user)) return false;
        if (field == 15 && !Unsigned(token, system)) return false;
        if (field == 22 && !Unsigned(token, parsed.startTicks)) return false;
    }
    if (!Add(user, system, parsed.cpuTicks)) return false;
    out = parsed;
    return true;
}

bool ParseSystemStat(const std::string& line, SystemTicks& out)
{
    std::istringstream fields(line);
    std::string token;
    if (!(fields >> token) || token != "cpu") return false;
    SystemTicks parsed;
    // guest/guest_nice are already included in user/nice; never double-count.
    for (int field = 0; field < 8; ++field) {
        uint64_t value = 0;
        if (!(fields >> token) || !Unsigned(token, value)) return false;
        if (!Add(parsed.total, value, parsed.total)) return false;
        if ((field == 3 || field == 4) && !Add(parsed.idle, value, parsed.idle)) return false;
    }
    out = parsed;
    return true;
}

Snapshot ReadSnapshot(bool appCpu, bool systemCpu)
{
    Snapshot result;
    const auto start = Clock::now();
    result.timestampMs = std::chrono::duration<double, std::milli>(start.time_since_epoch()).count();
    result.ticksPerSecond = sysconf(_SC_CLK_TCK);
    std::string line;
    if (systemCpu) {
        result.systemReadable = ReadLine("/proc/stat", line) && ParseSystemStat(line, result.system);
    }
    if (!appCpu || result.ticksPerSecond <= 0) return result;
    DIR* directory = opendir("/proc");
    if (!directory) return result;
    const uid_t ownUid = getuid();
    size_t scanned = 0;
    bool ownFound = false;
    // UID includes appspawn/NCP children and Wine forked grandchildren, which
    // are not necessarily all in Wine's root-process registry. No cmdline/env reads.
    for (;;) {
        errno = 0;
        const auto* entry = readdir(directory);
        if (!entry) {
            if (errno) result.appPartial = true;
            break;
        }
        uint64_t pid = 0;
        if (!Unsigned(entry->d_name, pid) || pid == 0 || pid > INT32_MAX) continue;
        if (++scanned > 4096 || result.processes.size() >= 256 ||
            Clock::now() - start > std::chrono::milliseconds(50)) {
            result.appPartial = true;
            break;
        }
        const std::string path = std::string("/proc/") + entry->d_name;
        struct stat info {};
        if (stat(path.c_str(), &info) != 0) {
            if (errno != ENOENT && errno != ESRCH) result.appPartial = true;
            continue;
        }
        if (info.st_uid != ownUid) continue;
        ProcessTicks process;
        if (!ReadLine(path + "/stat", line) || !ParseProcessStat(line, process) || process.pid != int(pid)) {
            result.appPartial = true;
            continue;
        }
        if (process.pid == getpid()) ownFound = true;
        result.processes.push_back(process);
    }
    closedir(directory);
    // Never label another UID's counts or an incomplete inaccessible namespace as the app.
    result.appReadable = ownFound;
    return result;
}

std::string ToJson(const Snapshot& value)
{
    std::ostringstream out;
    out.precision(17);
    out << "{\"timestampMs\":" << value.timestampMs
        << ",\"ticksPerSecond\":" << value.ticksPerSecond
        << ",\"appReadable\":" << (value.appReadable ? "true" : "false")
        << ",\"appPartial\":" << (value.appPartial ? "true" : "false")
        << ",\"systemReadable\":" << (value.systemReadable ? "true" : "false")
        << ",\"systemPercent\":" << value.systemPercent
        << ",\"systemTotal\":" << value.system.total << ",\"systemIdle\":" << value.system.idle
        << ",\"processes\":[";
    bool first = true;
    for (const auto& process : value.processes) {
        if (!first) out << ',';
        first = false;
        out << "{\"pid\":" << process.pid << ",\"startTicks\":" << process.startTicks
            << ",\"cpuTicks\":" << process.cpuTicks << '}';
    }
    out << "]}";
    return out.str();
}
} // namespace performance_monitor
