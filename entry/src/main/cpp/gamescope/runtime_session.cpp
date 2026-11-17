// SPDX-License-Identifier: LGPL-2.1-or-later
#include "runtime_session.h"
#include "spawn_broker.h"
#include "process_backend.h"
#include "output_connection.h"
#include "BrokerTransport.h"
#include <memory>
#include <mutex>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

namespace winehua::gamescope {
namespace {
using namespace ::gamescope::ohos;
class Session {
public:
    ~Session() {
        { std::lock_guard lock(ioMutex_); if (endpoint_ >= 0) shutdown(endpoint_, SHUT_RDWR); }
        if (monitor_.joinable()) monitor_.join();
    }
    int Start(const SessionConfig& config) {
        int control[2], spawn[2];
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, control)) return errno;
        if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, spawn)) {
            close(control[0]); close(control[1]); return errno;
        }
        auto fail = [&] {
            for (int fd : {control[0], control[1], spawn[0], spawn[1]}) close(fd);
        };
        if (send(control[0], &config, sizeof(config), MSG_NOSIGNAL) != sizeof(config)) {
            fail(); return EIO;
        }
        int32_t pid = -1;
        const int code = StartOutputChild(config.surfaceId, control[1], spawn[1], &pid, outputConnection_);
        if (code != 0 || pid <= 0) { fail(); return code ? code : ECHILD; }
        close(control[1]); close(spawn[1]);
        endpoint_ = control[0];
        try {
            broker_ = std::make_unique<SpawnBroker>(spawn[0], config.runtimeDirectory,
                std::string(config.runtimeDirectory)+"/xwayland.log");
            status_.pid = pid; status_.active = true; status_.stage = "starting";
            monitor_ = std::thread([this] { Monitor(); });
        } catch (...) {
            // Closing the session endpoint asks the shim to stop. Never claim
            // a successful session if its lifecycle observer could not start.
            shutdown(endpoint_, SHUT_RDWR); close(endpoint_); endpoint_ = -1;
            if (!broker_) close(spawn[0]);
            broker_.reset(); return ENOMEM;
        }
        return 0;
    }
    RuntimeSessionStatus Snapshot() { std::lock_guard lock(stateMutex_); return status_; }
    bool Send(const SessionCommand& command) {
        std::lock_guard lock(ioMutex_);
        return endpoint_ >= 0 && send(endpoint_, &command, sizeof(command), MSG_NOSIGNAL | MSG_DONTWAIT) == sizeof(command);
    }
private:
    void Monitor() {
        bool returned = false;
        for (;;) {
            if (!WaitReadable(endpoint_, 250)) {
                if (errno == ETIMEDOUT) continue;
                break;
            }
            SessionEvent event;
            const auto received = recv(endpoint_, &event, sizeof(event), MSG_DONTWAIT | MSG_TRUNC);
            if (received <= 0) break;
            if (received != sizeof(event) || event.magic != SessionMagic || event.version != 1 ||
                event.size != sizeof(event) || event.pid != status_.pid ||
                !memchr(event.detail, 0, sizeof(event.detail))) {
                std::lock_guard lock(stateMutex_);
                status_.stage = "protocol-error"; status_.error = EPROTO;
                status_.detail = "invalid-session-event";
                break;
            }
            {
                std::lock_guard lock(stateMutex_);
                status_.error = event.error; status_.detail = event.detail;
                switch (event.stage) {
                case SessionStage::Failed: status_.stage = "failed"; break;
                case SessionStage::Loaded: status_.stage = "loaded"; break;
                case SessionStage::OutputReady: status_.outputReady = true; status_.stage = "output-ready"; break;
                case SessionStage::DisplayReady:
                    status_.displayReady = true; status_.display = event.detail; status_.stage = "display-ready"; break;
                case SessionStage::Presented: status_.presented = true; status_.stage = "presented"; break;
                case SessionStage::Returned: returned = true; status_.stage = "returned"; break;
                default: status_.stage = "protocol-error"; status_.error = EPROTO; break;
                }
            }
        }
        {
            std::lock_guard lock(ioMutex_);
            close(endpoint_); endpoint_ = -1;
        }
        broker_->Stop();
        std::lock_guard lock(stateMutex_);
        status_.active = false;
        if (!returned && status_.stage != "failed" && status_.stage != "protocol-error") {
            status_.stage = "disconnected"; status_.error = ECONNRESET;
            status_.detail = "session-closed-without-return";
        }
    }
    std::mutex stateMutex_, ioMutex_;
    int endpoint_ = -1;
    RuntimeSessionStatus status_;
    std::unique_ptr<SpawnBroker> broker_;
    std::unique_ptr<OutputConnection> outputConnection_;
    std::thread monitor_;
};
std::mutex sessionMutex;
std::shared_ptr<Session> currentSession;
}
int StartSession(const ::gamescope::ohos::SessionConfig& config) {
    if (!ValidateSessionConfig(config) || config.mode != SessionMode::Compositor) return EINVAL;
    if (!ProcessBackendAvailable()) return ENOTSUP;
    std::lock_guard lock(sessionMutex);
    if (currentSession && currentSession->Snapshot().active) return EBUSY;
    auto session = std::make_shared<Session>();
    const int result = session->Start(config);
    if (!result) currentSession = std::move(session);
    return result;
}
bool RequestSessionStop() {
    std::lock_guard lock(sessionMutex);
    SessionCommand command;
    return currentSession && currentSession->Send(command);
}
bool UpdateSessionOutput(uint32_t width, uint32_t height, bool visible) {
    if (!width || !height || width > 8192 || height > 8192) return false;
    std::lock_guard lock(sessionMutex);
    SessionCommand command; command.kind = CommandKind::OutputState;
    command.width = width; command.height = height; command.visible = visible;
    return currentSession && currentSession->Send(command);
}
RuntimeSessionStatus SessionStatus() {
    std::lock_guard lock(sessionMutex);
    return currentSession ? currentSession->Snapshot() : RuntimeSessionStatus{};
}
}
