// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "BrokerProtocol.h"
#include <cerrno>
#include <chrono>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace gamescope::ohos {
struct SpawnMessage {
    SpawnRequest request;
    std::vector<int> fds;
    SpawnMessage() = default;
    SpawnMessage(const SpawnMessage&) = delete;
    ~SpawnMessage() { for (int fd : fds) close(fd); }
};
inline bool WaitReadable(int fd, int timeoutMs) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - std::chrono::steady_clock::now()).count();
        pollfd input{fd, POLLIN, 0};
        int result = poll(&input, 1, int(remaining > 0 ? remaining : 0));
        if (result > 0 && (input.revents & POLLIN)) return true;
        if (result < 0 && errno == EINTR && std::chrono::steady_clock::now() < end) continue;
        errno = result == 0 ? ETIMEDOUT : ECONNRESET;
        return false;
    }
}
inline bool SendSpawn(int fd, const SpawnRequest& request, const int* fds) {
    if (!ValidateSpawn(request)) { errno = EINVAL; return false; }
    for (uint32_t i = 0; i < request.fdCount; ++i)
        if (fds[i] < 0) { errno = EBADF; return false; }
    alignas(cmsghdr) char control[CMSG_SPACE(MaxSpawnFds*sizeof(int))]{};
    iovec data{const_cast<SpawnRequest*>(&request), sizeof(request)};
    msghdr message{};
    message.msg_iov = &data; message.msg_iovlen = 1;
    message.msg_control = control; message.msg_controllen = CMSG_SPACE(request.fdCount*sizeof(int));
    auto* rights = CMSG_FIRSTHDR(&message);
    rights->cmsg_level = SOL_SOCKET; rights->cmsg_type = SCM_RIGHTS;
    rights->cmsg_len = CMSG_LEN(request.fdCount*sizeof(int));
    memcpy(CMSG_DATA(rights), fds, request.fdCount*sizeof(int));
    ssize_t count;
    do { count = sendmsg(fd, &message, MSG_NOSIGNAL | MSG_DONTWAIT); } while (count < 0 && errno == EINTR);
    return count == sizeof(request);
}
inline bool ReceiveSpawn(int fd, SpawnMessage& output, int timeoutMs) {
    if (!output.fds.empty()) { errno = EINVAL; return false; }
    if (!WaitReadable(fd, timeoutMs)) return false;
    alignas(cmsghdr) char control[CMSG_SPACE(MaxSpawnFds*sizeof(int))]{};
    iovec data{&output.request, sizeof(output.request)};
    msghdr message{};
    message.msg_iov = &data; message.msg_iovlen = 1;
    message.msg_control = control; message.msg_controllen = sizeof(control);
    ssize_t count;
    do { count = recvmsg(fd, &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT); } while (count < 0 && errno == EINTR);
    for (auto* rights = CMSG_FIRSTHDR(&message); rights; rights = CMSG_NXTHDR(&message, rights)) {
        if (rights->cmsg_level != SOL_SOCKET || rights->cmsg_type != SCM_RIGHTS ||
            rights->cmsg_len < CMSG_LEN(0)) continue;
        const size_t n = (rights->cmsg_len-CMSG_LEN(0))/sizeof(int);
        const auto* values = reinterpret_cast<const int*>(CMSG_DATA(rights));
        output.fds.insert(output.fds.end(), values, values+n);
    }
    if (count != sizeof(output.request) || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        !ValidateSpawn(output.request) || output.fds.size() != output.request.fdCount) {
        errno = EPROTO; return false;
    }
    return true;
}
inline bool SendSpawnResult(int fd, const SpawnResult& result) {
    ssize_t count;
    do { count = send(fd, &result, sizeof(result), MSG_NOSIGNAL | MSG_DONTWAIT); } while (count < 0 && errno == EINTR);
    return count == sizeof(result);
}
inline bool AwaitSpawnResult(int fd, uint32_t sequence, SpawnResult& result, int timeoutMs = SpawnTimeoutMs) {
    if (!WaitReadable(fd, timeoutMs)) return false;
    // recvmsg also closes unsolicited ancillary descriptors, including a
    // malformed reply. A plain recv would leak any supplied SCM_RIGHTS FDs.
    alignas(cmsghdr) char control[CMSG_SPACE(MaxSpawnFds*sizeof(int))]{};
    iovec data{&result, sizeof(result)};
    msghdr message{};
    message.msg_iov = &data; message.msg_iovlen = 1;
    message.msg_control = control; message.msg_controllen = sizeof(control);
    ssize_t count = recvmsg(fd, &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    bool ancillary = false;
    for (auto* h = CMSG_FIRSTHDR(&message); h; h = CMSG_NXTHDR(&message, h)) {
        ancillary = true;
        if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS && h->cmsg_len >= CMSG_LEN(0)) {
            auto* values = reinterpret_cast<int*>(CMSG_DATA(h));
            for (size_t i = 0; i < (h->cmsg_len-CMSG_LEN(0))/sizeof(int); ++i) close(values[i]);
        }
    }
    if (count != sizeof(result) || ancillary || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        result.magic != BrokerMagic || result.version != BrokerVersion || result.sequence != sequence) {
        errno = EPROTO; return false;
    }
    if (result.error || result.pid <= 0) { errno = ECHILD; return false; }
    return true;
}
} // namespace gamescope::ohos
