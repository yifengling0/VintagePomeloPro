// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <cstdint>
struct NativeWindow;
namespace gamescope {
NativeWindow* CreateOutputWindow(uint64_t surfaceId);
bool SetOutputWindowSize(NativeWindow* window, uint32_t width, uint32_t height);
void DestroyOutputWindow(NativeWindow* window);
}
