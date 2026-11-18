// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "Embedded.h"
#include <algorithm>

// Source serials belong to accepted GPU buffers, never repaint requests.
// One pending real frame is the entire display queue owned by FG.
class VppFrameGenerationSchedule {
public:
    void Reset() { *this=VppFrameGenerationSchedule{}; }
    bool Observe(uint64_t key,uint64_t serial,uint64_t now,uint64_t display) {
        if (identity_!=key || (lastSourceNs_ && (now<lastSourceNs_ || now-lastSourceNs_>250000000))) Reset();
        identity_=key;
        fresh_=serial!=serial_ || !lastSourceNs_;
        if (!fresh_) return false;
        if (lastSourceNs_) {
            const auto delta=now-lastSourceNs_;
            intervalNs_=intervalNs_?(intervalNs_*3+delta)/4:delta;
            ++samples_;
        }
        serial_=serial; lastSourceNs_=now;
        canGenerate_=display && samples_>=2 && intervalNs_>=display*17/10 && intervalNs_<=200000000 && !pending_;
        // A new source supersedes a pending frame; never display an older real
        // after a newer generated/real image, and never accumulate debt.
        pending_=false;
        return true;
    }
    bool CanGenerate() const { return fresh_ && canGenerate_; }
    bool Pending() const { return pending_; }
    void Generated(uint64_t now,uint64_t display) {
        pending_=true; dueNs_=now+std::max(display,intervalNs_/2);
    }
    void RealPresented() { pending_=false; dueNs_=0; }
    uint64_t DueNs() const { return pending_?dueNs_:0; }
    uint64_t IntervalNs() const { return intervalNs_; }
    uint32_t Reason(uint64_t display) const {
        if (!display) return VPP_FG_NO_CADENCE;
        if (samples_<2) return VPP_FG_WARMING;
        return intervalNs_>=display*17/10 && intervalNs_<=200000000?VPP_FG_RUNNING:VPP_FG_DISPLAY_RATE;
    }
private:
    uint64_t identity_=0,serial_=0,lastSourceNs_=0,intervalNs_=0,dueNs_=0;
    uint32_t samples_=0;
    bool fresh_=false,canGenerate_=false,pending_=false;
};
