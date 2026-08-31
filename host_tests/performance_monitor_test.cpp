#include "common/performance_monitor.h"
#include <cassert>
#include <iostream>
#include <sstream>
#include <unistd.h>
using namespace performance_monitor;

static std::string Stat(const char* user, const char* system, const char* start)
{
    std::ostringstream out;
    out << "42 (wine child ) thread) S";
    for (int field = 4; field <= 22; ++field) {
        out << ' ';
        if (field == 14) out << user;
        else if (field == 15) out << system;
        else if (field == 22) out << start;
        else out << (field == 19 ? "-10" : "999");
    }
    return out.str();
}
int main()
{
    ProcessTicks process;
    assert(ParseProcessStat(Stat("100", "25", "6789"), process));
    assert(process.pid == 42 && process.cpuTicks == 125 && process.startTicks == 6789);
    assert(!ParseProcessStat("42 (broken", process));
    assert(!ParseProcessStat("42 (truncated) S 1", process));
    assert(!ParseProcessStat(Stat("-1", "25", "6789"), process));
    assert(!ParseProcessStat(Stat("18446744073709551615", "1", "6789"), process));
    assert(!ParseProcessStat(Stat("12oops", "1", "6789"), process));
    SystemTicks system;
    assert(ParseSystemStat("cpu 100 20 30 400 50 6 7 8 90 10", system));
    assert(system.total == 621 && system.idle == 450);
    assert(!ParseSystemStat("cpu0 100 20 30 400 50 6 7 8", system));
    assert(!ParseSystemStat("cpu 100 20", system));
    assert(!ParseSystemStat("cpu 18446744073709551615 1 0 0 0 0 0 0", system));
    const auto none = ReadSnapshot(false, false);
    assert(!none.appReadable && !none.systemReadable && none.processes.empty());
    const auto live = ReadSnapshot(true, true);
    assert(live.timestampMs > 0 && live.ticksPerSecond > 0);
    assert(live.appReadable && live.systemReadable);
    bool self = false;
    for (const auto& p : live.processes) if (p.pid == getpid()) self = true;
    assert(self);
    const auto json = ToJson(live);
    assert(json.find("\"appReadable\":true") != std::string::npos);
    std::cout << "performance_monitor: parser, bounds, CPU units and live procfs checks passed\n";
}
