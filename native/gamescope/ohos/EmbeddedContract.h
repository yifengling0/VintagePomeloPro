// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "Embedded.h"
#include <cstring>

constexpr uint32_t VppGamescopeMaxLayers = 64;

inline bool VppGamescopeFinite(float value) {
    // Core uses fast-math; classify bits so NaN/Inf are still rejected.
    uint32_t bits; std::memcpy(&bits,&value,sizeof(bits));
    return (bits & 0x7f800000u)!=0x7f800000u;
}
inline bool VppGamescopeValidFrame(const VppGamescopeFrame& frame) {
    if (frame.size!=sizeof(frame) || !frame.layers || !frame.count || frame.count>VppGamescopeMaxLayers ||
        !frame.width || !frame.height || frame.width>8192 || frame.height>8192 || frame.filter>3 || frame.sharpness>20 ||
        frame.frameGeneration>1 || frame.historyReset>1 ||
        (frame.displayPeriodNs && (frame.displayPeriodNs<1000000 || frame.displayPeriodNs>1000000000))) return false;
    uint64_t shmBytes=0;
    for (uint32_t i=0;i<frame.count;++i) {
        const auto& l=frame.layers[i];
        if (!l.width || !l.height || l.width>8192 || l.height>8192 || l.vulkan>1 || l.opaque>1 || l.upscale>1 ||
            !VppGamescopeFinite(l.x) || !VppGamescopeFinite(l.y) || !VppGamescopeFinite(l.w) || !VppGamescopeFinite(l.h) ||
            l.w<=0 || l.h<=0 || l.w>16384 || l.h>16384 ||
            !VppGamescopeFinite(l.u0) || !VppGamescopeFinite(l.u1) || !VppGamescopeFinite(l.v0) || !VppGamescopeFinite(l.v2) ||
            l.u1==0 || l.v2==0 || l.u0<0 || l.u0>1 || l.v0<0 || l.v0>1 ||
            l.u0+l.u1<0 || l.u0+l.u1>1 || l.v0+l.v2<0 || l.v0+l.v2>1) return false;
        if (l.native) {
            if (l.pixels) return false;
            for (uint32_t j=0;j<i;++j) if (frame.layers[j].native==l.native) return false;
        } else {
            const uint64_t bytes=uint64_t(l.width)*l.height*4;
            if (!l.pixels || l.pixelBytes<bytes) return false;
            shmBytes+=bytes; if (shmBytes>32u*1024*1024) return false;
        }
    }
    return true;
}
