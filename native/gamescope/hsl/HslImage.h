// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "rendervulkan.hpp"
struct wl_display;
struct wlr_buffer;
bool hsl_image_init(wl_display* display);
bool hsl_image_is_buffer(wlr_buffer* buffer);
gamescope::OwningRc<CVulkanTexture> hsl_image_snapshot(wlr_buffer* buffer);
void hsl_image_collect();
void hsl_image_shutdown();
