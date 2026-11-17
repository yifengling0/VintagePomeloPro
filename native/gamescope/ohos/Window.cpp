// SPDX-License-Identifier: LGPL-2.1-or-later
#include "OhosWindow.h"
#include <native_window/external_window.h>

// Keep the SDK's Region type and X11's Region typedef in separate translation
// units. Both APIs retain their real declarations and ABI.
namespace gamescope {
namespace { NativeWindow* pendingOutput = nullptr; }
extern "C" __attribute__((visibility("default"))) bool VppGamescopeSetOutputWindow(void* window) {
    if (!window || pendingOutput) return false;
    pendingOutput = static_cast<NativeWindow*>(window);
    return true;
}
NativeWindow* CreateOutputWindow(uint64_t) {
    // Ownership of the IPC-deserialized producer transfers to the backend.
    auto* window = pendingOutput; pendingOutput = nullptr;
    return window;
}
bool SetOutputWindowSize(NativeWindow* window, uint32_t width, uint32_t height) {
    if (!window || !width || !height || width > 8192 || height > 8192) return false;
    // The IPC-deserialized producer retains its buffer geometry. ArkUI changes
    // the consumer rect, so update this producer before querying WSI extent.
    return OH_NativeWindow_NativeWindowHandleOpt(window, SET_BUFFER_GEOMETRY,
        static_cast<int32_t>(width), static_cast<int32_t>(height)) == 0;
}
void DestroyOutputWindow(NativeWindow* window) {
    if (window) OH_NativeWindow_DestroyNativeWindow(window);
}
}
