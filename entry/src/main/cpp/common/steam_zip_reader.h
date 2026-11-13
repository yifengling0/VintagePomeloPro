#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace steamzip {
// Destination must not exist. All names, sizes and the client root are checked
// before creating it. The caller owns cleanup of this fresh staging directory.
void Extract(int fd, const std::string& destination,
             const std::function<void(uint64_t, uint64_t)>& progress);
}
