// SPDX-License-Identifier: LGPL-2.1-or-later
#include "../entry/src/main/cpp/gamescope/process_backend.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace {
bool phone = false, prepareAllowed = false;
int prepareCalls = 0, systemCalls = 0, forkCalls = 0, serverPid = -1;
void CheckLaunch(const char* entry, NativeChildProcess_Args args) {
    assert(strcmp(entry, "libgamescope_child.so:Main") == 0);
    assert(args.fdList.head && args.fdList.head->fd >= 0);
    assert(fcntl(args.fdList.head->fd, F_GETFD) >= 0);
}
}
extern "C" bool PhoneAdapter_IsPhoneMode() { return phone; }
int Phone_PrepareDirectForkServer() {
    ++prepareCalls; if (prepareAllowed) serverPid = 1234;
    return prepareAllowed ? NCP_NO_ERROR : NCP_ERR_NOT_SUPPORTED;
}
int32_t Phone_GetDirectForkServerPid() { return serverPid; }
Ability_NativeChildProcess_ErrCode Phone_StartViaDirectForkServer(
    const char* entry, NativeChildProcess_Args args, int32_t* pid) {
    ++forkCalls; CheckLaunch(entry, args); *pid = 5678; return NCP_NO_ERROR;
}
extern "C" Ability_NativeChildProcess_ErrCode WineHua_StartNativeChildProcess(
    const char* entry, NativeChildProcess_Args args, NativeChildProcess_Options options, int32_t* pid) {
    ++systemCalls; CheckLaunch(entry, args); assert(options.isolationMode == NCP_ISOLATION_MODE_NORMAL);
    *pid = 8765; return NCP_NO_ERROR;
}
int main() {
    using namespace winehua::gamescope;
    int pipeFds[2]; assert(pipe(pipeFds) == 0);
    NativeChildProcess_Fd node{}; node.fdName = const_cast<char*>("gs_session"); node.fd = pipeFds[0];
    NativeChildProcess_Args args{}; args.entryParams = const_cast<char*>("gamescope-v1"); args.fdList.head = &node;
    int32_t pid = -1;
    assert(PrepareProcessBackend() == 0 && prepareCalls == 0);
    assert(ProcessBackendAvailable());
    assert(StandaloneOutputSupported());
    assert(StartRuntimeChild("libgamescope_child.so:Main", args, &pid) == 0 && pid == 8765);
    assert(systemCalls == 1 && forkCalls == 0);
    phone = true;
    assert(!StandaloneOutputSupported());
    assert(!ProcessBackendAvailable());
    assert(StartRuntimeChild("libgamescope_child.so:Main", args, &pid) == NCP_ERR_NOT_SUPPORTED && pid == -1);
    assert(PrepareProcessBackend() == NCP_ERR_NOT_SUPPORTED);
    assert(StartRuntimeChild("libgamescope_child.so:Main", args, &pid) == NCP_ERR_NOT_SUPPORTED);
    assert(systemCalls == 1 && forkCalls == 0); // Never try phone system NCP/late fork.
    prepareAllowed = true;
    assert(PrepareProcessBackend() == 0 && ProcessBackendAvailable());
    assert(!StandaloneOutputSupported()); // Fork success never enables phone output.
    assert(StartRuntimeChild("libgamescope_child.so:Main", args, &pid) == 0 && pid == 5678);
    assert(systemCalls == 1 && forkCalls == 1);
    assert(fcntl(pipeFds[0], F_GETFD) >= 0); // Retain caller FD ownership.
    serverPid = -1; // Server death cannot silently fall back to late fork/NCP.
    assert(!ProcessBackendAvailable());
    assert(StartRuntimeChild("libgamescope_child.so:Main", args, &pid) == NCP_ERR_NOT_SUPPORTED);
    assert(systemCalls == 1 && forkCalls == 1);
    close(pipeFds[0]); close(pipeFds[1]);
    std::puts("Gamescope tablet NCP and early phone fork routing/failure isolation: PASS");
}
