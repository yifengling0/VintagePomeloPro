// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <atomic>
#include <string>
#include <thread>

namespace winehua::gamescope {
// Owns one session endpoint. Stop wakes receive and joins before FD reuse.
class SpawnBroker {
public:
    SpawnBroker(int endpoint, std::string runtimeDirectory, std::string logPath);
    ~SpawnBroker();
    SpawnBroker(const SpawnBroker&) = delete;
    void Stop();
private:
    void Run();
    int endpoint_;
    std::string runtimeDirectory_, logPath_;
    std::atomic<bool> stopped_{false};
    std::thread worker_;
};
} // namespace winehua::gamescope
