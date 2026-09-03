#pragma once

#include "input/controller/controller_types.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace winehua {
namespace controller {

// AF_UNIX WHGP server. Hub state listener pushes snapshots to connected winebus
// clients; rumble packets from winebus are forwarded to a JS/native listener.
//
// 32-bit and 64-bit winedevice each load winebus. Keep every live client instead
// of replacing the previous fd — kicking the first connection makes every
// winedevice reconnect in a tight loop (the WHGP reconnect storm).
class GamepadBridge {
public:
    using RumbleListener = std::function<void(uint16_t low, uint16_t high, uint32_t durationMs)>;

    static GamepadBridge& Instance();

    bool Start(const std::string& socketPath);
    void Stop();
    bool IsRunning() const;
    std::string SocketPath() const;
    void PublishState(uint32_t slot, const LogicalGamepadState& state);
    void AttachToHub();
    void SetRumbleListener(RumbleListener cb);

private:
    static constexpr size_t kMaxClients = 4;

    GamepadBridge() = default;
    void AcceptLoop();
    void RecvLoop(int fd);
    void WriteState(int fd, uint32_t slot, const LogicalGamepadState& state);
    void RemoveClientLocked(int fd);

    mutable std::mutex mutex_;
    std::string path_;
    int listenFd_ = -1;
    std::vector<int> clientFds_;
    bool running_ = false;
    std::thread acceptThread_;
    RumbleListener rumbleListener_;
};

}  // namespace controller
}  // namespace winehua
