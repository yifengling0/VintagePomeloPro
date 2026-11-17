// SPDX-License-Identifier: LGPL-2.1-or-later
// Exercise the production session owner over real sockets. Only OHOS process
// creation and the separately tested spawn broker are replaced by fixtures.
#include "../entry/src/main/cpp/gamescope/runtime_session.h"
#include "../entry/src/main/cpp/gamescope/output_connection.h"
#include "../entry/src/main/cpp/gamescope/spawn_broker.h"
#include "BrokerTransport.h"
#include <cassert>
#include <chrono>
#include <thread>
#include <functional>
#include <vector>
#include <fcntl.h>
#include <cstdio>
using namespace gamescope::ohos;
namespace {
enum class Scenario { Normal, Disconnect, Failure, ForeignPid, BadPacket };
Scenario scenario = Scenario::Normal;
std::vector<std::thread> children;
int nextPid = 500;
bool standaloneOutput = true;
void Wait(const std::function<bool()>& condition) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!condition() && std::chrono::steady_clock::now() < until) std::this_thread::yield();
    assert(condition());
}
}
namespace winehua::gamescope {
bool ProcessBackendAvailable() { return true; }
bool StandaloneOutputSupported() { return standaloneOutput; }
OutputConnection::~OutputConnection() = default;
SpawnBroker::SpawnBroker(int endpoint, std::string, std::string) : endpoint_(endpoint) {}
SpawnBroker::~SpawnBroker() { Stop(); }
void SpawnBroker::Stop() { if (!stopped_.exchange(true)) { close(endpoint_); endpoint_ = -1; } }
int StartOutputChild(uint64_t, int session, int spawn, int32_t* pid, std::unique_ptr<OutputConnection>&) {
    // Match caller FD ownership: the real IPC/fork handoff duplicates FDs.
    const int fd = fcntl(session, F_DUPFD_CLOEXEC, 0);
    const int broker = fcntl(spawn, F_DUPFD_CLOEXEC, 0);
    assert(fd >= 0 && broker >= 0);
    *pid = ++nextPid;
    children.emplace_back([fd, broker, child = *pid, which = scenario] {
        SessionConfig config;
        assert(recv(fd, &config, sizeof(config), 0) == sizeof(config));
        assert(ValidateSessionConfig(config));
        SessionEvent event; event.pid = child;
        auto emit = [&](SessionStage stage, const char* detail, int error = 0) {
            event.stage = stage; event.error = error;
            snprintf(event.detail, sizeof(event.detail), "%s", detail);
            assert(send(fd, &event, sizeof(event), MSG_NOSIGNAL) == sizeof(event));
        };
        if (which == Scenario::ForeignPid) { event.pid += 1; emit(SessionStage::Loaded, "foreign"); }
        else if (which == Scenario::BadPacket) {
            event.version += 1; emit(SessionStage::Loaded, "bad");
        } else if (which == Scenario::Failure) emit(SessionStage::Failed, "output-transfer", ENODEV);
        else {
            emit(SessionStage::Loaded, "real-socket-fixture");
            emit(SessionStage::OutputReady, "native-window-fixture");
            emit(SessionStage::DisplayReady, "x11=:77;wayland=gamescope-0");
            emit(SessionStage::Presented, "present-fixture");
            if (which == Scenario::Normal) {
                SessionCommand command;
                assert(WaitReadable(fd, 2000));
                assert(recv(fd, &command, sizeof(command), 0) == sizeof(command));
                assert(command.kind == CommandKind::OutputState && command.width == 1280 && command.height == 720 && command.visible == 0);
                assert(WaitReadable(fd, 2000));
                assert(recv(fd, &command, sizeof(command), 0) == sizeof(command));
                assert(command.kind == CommandKind::Stop);
                emit(SessionStage::Returned, "normal-return", 0);
            }
        }
        close(fd); close(broker);
    });
    return 0;
}
}
int main() {
    using namespace winehua::gamescope;
    SessionConfig config; config.mode = SessionMode::Compositor; config.surfaceId = 1;
    strcpy(config.runtimeDirectory, "/tmp/gamescope-session-test");
    strcpy(config.logPath, "/tmp/gamescope-session-test.log");
    assert(!RequestSessionStop());
    standaloneOutput = false;
    assert(StartSession(config) == ENOTSUP && children.empty());
    standaloneOutput = true;
    for (int round = 0; round < 5; ++round) {
        scenario = Scenario::Normal;
        assert(StartSession(config) == 0);
        Wait([] { return SessionStatus().presented; });
        const auto status = SessionStatus();
        assert(status.active && status.outputReady && status.displayReady && status.error == 0);
        assert(status.display == "x11=:77;wayland=gamescope-0");
        assert(StartSession(config) == EBUSY);
        assert(!UpdateSessionOutput(0, 720, true));
        assert(!UpdateSessionOutput(1280, 8193, true));
        assert(UpdateSessionOutput(1280, 720, false));
        assert(RequestSessionStop());
        Wait([] { return !SessionStatus().active; });
        assert(SessionStatus().stage == "returned" && SessionStatus().error == 0);
        assert(!RequestSessionStop());
    }
    for (auto which : {Scenario::Disconnect, Scenario::Failure, Scenario::ForeignPid, Scenario::BadPacket}) {
        scenario = which; assert(StartSession(config) == 0);
        Wait([] { return !SessionStatus().active; });
        const auto status = SessionStatus();
        const bool invalid = which == Scenario::ForeignPid || which == Scenario::BadPacket;
        assert(status.stage == (which == Scenario::Failure ? "failed" : invalid ? "protocol-error" : "disconnected"));
        if (which == Scenario::Failure) assert(status.error == ENODEV && status.detail == "output-transfer");
        else if (invalid) assert(status.error == EPROTO && status.detail == "invalid-session-event");
        else assert(status.error == ECONNRESET && status.detail == "session-closed-without-return");
    }
    for (auto& child : children) child.join();
    std::puts("Gamescope session: five start/resize/stop cycles, busy rejection, failure, disconnect and foreign/malformed events: PASS");
}
