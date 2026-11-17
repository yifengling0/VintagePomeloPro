// SPDX-License-Identifier: LGPL-2.1-or-later
#include "OhosWindow.h"
#include <native_window/external_window.h>
#include <native_buffer/native_buffer.h>

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

// These SDK calls live outside X11 translation units (both define Region).
void OhosNativeBufferUnreference(void* buffer) {
    OH_NativeBuffer_Unreference(static_cast<OH_NativeBuffer*>(buffer));
}
bool OhosNativeBufferReference(void* buffer) {
    return OH_NativeBuffer_Reference(static_cast<OH_NativeBuffer*>(buffer)) == 0;
}
void* OhosCloneOutputWindow(void* value) {
    uint64_t id = 0; OHNativeWindow* window = nullptr;
    if (OH_NativeWindow_GetSurfaceId(static_cast<OHNativeWindow*>(value), &id) ||
        OH_NativeWindow_CreateNativeWindowFromSurfaceId(id, &window)) return nullptr;
    return window;
}
