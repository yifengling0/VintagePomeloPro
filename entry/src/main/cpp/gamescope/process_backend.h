// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <AbilityKit/native_child_process.h>

namespace winehua::gamescope {
// Called during Ability.onCreate, before ArkUI/GPU initialization. A phone
// compositor must never fork the already-running desktop graphics process.
int PrepareProcessBackend();
bool ProcessBackendAvailable();
const char* ProcessBackendName();
// Platform route only; GPU-game qualification is a separate capability.
bool StandaloneOutputSupported();
// Caller retains every supplied FD on both device classes.
int StartRuntimeChild(const char* entry, NativeChildProcess_Args args, int32_t* pid);
} // namespace winehua::gamescope
