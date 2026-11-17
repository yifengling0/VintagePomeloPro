// SPDX-License-Identifier: LGPL-2.1-or-later
#include "process_backend.h"
#include "proc/ncp_dispatch.h"
#include "phone_adapter/phone_adapter.h"
#include "phone_adapter/phone_process.h"
#include <atomic>

namespace winehua::gamescope {
namespace { std::atomic<bool> phoneReady{false}; }
int PrepareProcessBackend() {
    if (!PhoneAdapter_IsPhoneMode()) return NCP_NO_ERROR;
    const int result = Phone_PrepareDirectForkServer();
    phoneReady.store(result == NCP_NO_ERROR);
    return result;
}
bool ProcessBackendAvailable() {
    return !PhoneAdapter_IsPhoneMode() ||
        (phoneReady.load() && Phone_GetDirectForkServerPid() > 0);
}
const char* ProcessBackendName() {
    return PhoneAdapter_IsPhoneMode() ? "phone-clean-fork" : "system-ncp";
}
bool StandaloneOutputSupported() { return !PhoneAdapter_IsPhoneMode(); }
int StartRuntimeChild(const char* entry, NativeChildProcess_Args args, int32_t* pid) {
    if (!pid) return NCP_ERR_INVALID_PARAM;
    *pid = -1;
    if (PhoneAdapter_IsPhoneMode()) {
        if (!ProcessBackendAvailable()) return NCP_ERR_NOT_SUPPORTED;
        // The clean server passes SCM_RIGHTS copies, with no consumption of
        // caller FDs. Do not use the legacy adapter's late fork for Vulkan.
        return Phone_StartViaDirectForkServer(entry, args, pid);
    }
    NativeChildProcess_Options options{};
    options.isolationMode = NCP_ISOLATION_MODE_NORMAL;
    return WineHua_StartNativeChildProcess(entry, args, options, pid);
}
} // namespace winehua::gamescope
