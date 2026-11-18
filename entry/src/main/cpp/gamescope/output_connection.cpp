// SPDX-License-Identifier: LGPL-2.1-or-later
#include "output_connection.h"
#include "proc/ncp_dispatch.h"
#include "phone_adapter/phone_adapter.h"
#include <IPCKit/ipc_kit.h>
#include <native_window/external_window.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cerrno>

namespace winehua::gamescope {
namespace {
// The SDK callback has no context argument. Keep at most one pending callback,
// including a timed-out one; a late callback can never configure a new session.
struct Launch {
    bool received = false;
    int error = NCP_ERR_TIMEOUT;
    OHIPCRemoteProxy* proxy = nullptr;
};
std::mutex callbackMutex;
std::mutex launchMutex;
std::condition_variable callbackReady;
Launch* waiting = nullptr;
bool pending = false;
void Started(int error, OHIPCRemoteProxy* proxy) {
    std::lock_guard lock(callbackMutex);
    if (waiting) {
        waiting->received = true; waiting->error = error; waiting->proxy = proxy;
        callbackReady.notify_one();
    } else {
        pending = false;
        if (proxy) OH_IPCRemoteProxy_Destroy(proxy);
    }
}
struct Parcel {
    OHIPCParcel* value = OH_IPCParcel_Create();
    ~Parcel() { if (value) OH_IPCParcel_Destroy(value); }
};
}
OutputConnection::~OutputConnection() { if (proxy_) OH_IPCRemoteProxy_Destroy(proxy_); }
int StartOutputChild(uint64_t surfaceId, int sessionFd, int spawnFd,
                     int32_t* pid, std::unique_ptr<OutputConnection>& connection) {
    if (!pid || !surfaceId || sessionFd < 0 || spawnFd < 0) return EINVAL;
    *pid = -1;
    // A phone fork child cannot receive a NativeWindow through this tablet
    // IPC bootstrap. Phone Game presentation needs its own qualified route.
    if (PhoneAdapter_IsPhoneMode()) return ENOTSUP;
    std::lock_guard launchLock(launchMutex);
    OHNativeWindow* window = nullptr;
    int error = OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &window);
    if (error || !window) return ENODEV;
    Parcel request, reply;
    bool valid = request.value && reply.value &&
        OH_IPCParcel_WriteInt32(request.value, 1) == 0 &&
        OH_IPCParcel_WriteFileDescriptor(request.value, sessionFd) == 0 &&
        OH_IPCParcel_WriteFileDescriptor(request.value, spawnFd) == 0 &&
        OH_NativeWindow_WriteToParcel(window, request.value) == 0;
    OH_NativeWindow_DestroyNativeWindow(window);
    if (!valid) return EIO;
    Launch launch;
    {
        std::lock_guard lock(callbackMutex);
        if (pending) return EBUSY;
        pending = true; waiting = &launch;
    }
    error = WineHua_CreateNativeChildProcess("libgamescope_child.so", Started);
    if (error) {
        std::lock_guard lock(callbackMutex); waiting = nullptr; pending = false;
        return error;
    }
    {
        std::unique_lock lock(callbackMutex);
        if (!callbackReady.wait_for(lock, std::chrono::seconds(15), [&] { return launch.received; })) {
            waiting = nullptr; return ETIMEDOUT;
        }
        waiting = nullptr; pending = false;
    }
    std::unique_ptr<OutputConnection> owner;
    if (launch.proxy) owner = std::make_unique<OutputConnection>(launch.proxy);
    if (launch.error || !launch.proxy) return launch.error ? launch.error : EPIPE;
    int32_t result = EPROTO, childPid = -1;
    if (OH_IPCRemoteProxy_SendRequest(launch.proxy, 1, request.value, reply.value, nullptr) != 0 ||
        OH_IPCParcel_ReadInt32(reply.value, &result) != 0 ||
        OH_IPCParcel_ReadInt32(reply.value, &childPid) != 0) return EIO;
    if (result || childPid <= 0) return result ? result : ECHILD;
    *pid = childPid; connection = std::move(owner);
    return 0;
}
}
