// SPDX-License-Identifier: LGPL-2.1-or-later
#include "../native/gamescope/ohos/BrokerTransport.h"
#include "../native/gamescope/ohos/SessionProtocol.h"
#include <cassert>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <thread>

using namespace gamescope::ohos;
unsigned FdCount() {
    DIR* directory = opendir("/proc/self/fd"); assert(directory);
    unsigned count = 0;
    while (readdir(directory)) ++count;
    closedir(directory); return count;
}
SpawnRequest Request() {
    SpawnRequest r;
    r.sequence = 1; r.fdCount = 4; r.argc = 2; r.argvBytes = 13;
    strcpy(r.displayName, ":12"); memcpy(r.argv, "Xwayland\0:12", 13);
    return r;
}
int main() {
    const auto baseline = FdCount();
    int endpoints[2], pipeFds[2];
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, endpoints) == 0);
    assert(pipe2(pipeFds, O_CLOEXEC) == 0);
    int passed[]{pipeFds[0], pipeFds[1], pipeFds[0], pipeFds[1]};
    auto r = Request(); assert(ValidateSpawn(r));
    assert(SendSpawn(endpoints[0], r, passed));
    {
        SpawnMessage message;
        assert(ReceiveSpawn(endpoints[1], message, 100));
        for (int fd : message.fds) assert(fcntl(fd, F_GETFD) & FD_CLOEXEC);
        assert(write(message.fds[1], "k", 1) == 1);
        char value = 0; assert(read(pipeFds[0], &value, 1) == 1 && value == 'k');
        SpawnResult result; result.sequence = r.sequence; result.error = 0; result.pid = 123;
        assert(SendSpawnResult(endpoints[1], result));
        SpawnResult received;
        assert(AwaitSpawnResult(endpoints[0], r.sequence, received, 100));
        assert(received.pid == 123);
    }
    const auto withEndpoints = FdCount();
    // Malformed payloads and excess SCM_RIGHTS must never retain descriptors.
    for (unsigned i = 0; i < 32; ++i) {
        assert(SendSpawn(endpoints[0], r, passed));
        SpawnMessage message;
        assert(ReceiveSpawn(endpoints[1], message, 100));
        assert(message.fds.size() == 4);
    }
    assert(FdCount() == withEndpoints);
    r.argv[0] = '|'; assert(!SendSpawn(endpoints[0], r, passed));
    r = Request(); r.argc = 3; assert(!ValidateSpawn(r));
    r = Request(); r.argvBytes = MaxSpawnBytes+1; assert(!ValidateSpawn(r));
    r = Request(); r.fdCount = 5; assert(!ValidateSpawn(r));
    r = Request(); r.displayName[1] = '-'; assert(!ValidateSpawn(r));
    r = Request();
    SpawnResult result; result.sequence = 9; result.error = 0; result.pid = 123;
    assert(SendSpawnResult(endpoints[1], result));
    assert(!AwaitSpawnResult(endpoints[0], 1, result, 100) && errno == EPROTO);
    assert(!AwaitSpawnResult(endpoints[0], 1, result, 5) && errno == ETIMEDOUT);
    // A reply with unsolicited descriptors is rejected and all received FDs
    // are retired. Send a full request into the result receiver to exercise it.
    assert(SendSpawn(endpoints[1], r, passed));
    assert(!AwaitSpawnResult(endpoints[0], 1, result, 100) && errno == EPROTO);
    assert(FdCount() == withEndpoints);
    {
        int excess[8]; for (int& fd : excess) fd = pipeFds[0];
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(excess))]{};
        iovec data{&r, sizeof(r)}; msghdr raw{};
        raw.msg_iov = &data; raw.msg_iovlen = 1;
        raw.msg_control = control; raw.msg_controllen = sizeof(control);
        auto* h = CMSG_FIRSTHDR(&raw); h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS;
        h->cmsg_len = CMSG_LEN(sizeof(excess)); memcpy(CMSG_DATA(h), excess, sizeof(excess));
        assert(sendmsg(endpoints[0], &raw, MSG_NOSIGNAL) == sizeof(r));
        SpawnMessage message;
        assert(!ReceiveSpawn(endpoints[1], message, 100) && errno == EPROTO);
    }
    assert(FdCount() == withEndpoints);
    std::thread waiting([&] {
        SpawnMessage message;
        assert(!ReceiveSpawn(endpoints[1], message, 500));
    });
    shutdown(endpoints[0], SHUT_RDWR); waiting.join();
    for (int fd : {endpoints[0], endpoints[1], pipeFds[0], pipeFds[1]}) close(fd);
    assert(FdCount() == baseline);
    SessionConfig config;
    assert(ValidateSessionConfig(config));
    config.mode = SessionMode::Compositor;
    assert(!ValidateSessionConfig(config));
    config.surfaceId = 1; strcpy(config.runtimeDirectory, "/files/gs-rt"); strcpy(config.logPath, "/files/gs.log");
    assert(ValidateSessionConfig(config));
    config.fpsLimit = 31; assert(!ValidateSessionConfig(config));
    config.fpsLimit = 30; config.outputHeight = 0; assert(!ValidateSessionConfig(config));
    std::puts("Gamescope FD ownership, reply identity, deadlines, disconnect and config bounds: PASS");
}
