#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace performance_monitor {
struct ProcessTicks {
    int pid = 0;
    uint64_t startTicks = 0;
    uint64_t cpuTicks = 0;
};
struct SystemTicks {
    uint64_t total = 0;
    uint64_t idle = 0;
};
struct Snapshot {
    double timestampMs = 0;
    long ticksPerSecond = 0;
    bool appReadable = false;
    bool appPartial = false;
    bool systemReadable = false;
    double systemPercent = -1; // HiDebug fallback when app sandbox denies /proc/stat.
    SystemTicks system;
    std::vector<ProcessTicks> processes;
};
bool ParseProcessStat(const std::string& line, ProcessTicks& out);
bool ParseSystemStat(const std::string& line, SystemTicks& out);
// Bounded, read-only procfs sampling. Call on a worker, never a render/UI thread.
Snapshot ReadSnapshot(bool appCpu, bool systemCpu);
std::string ToJson(const Snapshot& value);
} // namespace performance_monitor
