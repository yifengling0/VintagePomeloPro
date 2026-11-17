// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <cstdint>
#include <memory>
struct OHIPCRemoteProxy;
namespace winehua::gamescope {
// A NativeWindow contains a producer IPC object. Its surface ID is local to
// the creating process; it is never a substitute for transferring that object.
class OutputConnection {
public:
    explicit OutputConnection(OHIPCRemoteProxy* proxy) : proxy_(proxy) {}
    ~OutputConnection();
    OutputConnection(const OutputConnection&) = delete;
private:
    OHIPCRemoteProxy* proxy_;
};
int StartOutputChild(uint64_t surfaceId, int sessionFd, int spawnFd,
                     int32_t* pid, std::unique_ptr<OutputConnection>& connection);
}
