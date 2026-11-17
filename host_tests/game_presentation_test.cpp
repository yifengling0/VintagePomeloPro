#include "graphics/game_presentation.h"
#include "compositor/frame/geometry.h"
#include <cassert>
#include <cmath>
#include <iostream>

// Supply 120 display ticks with upward-rounded nanoseconds to avoid comparing
// the ceil-rounded 60 FPS limit against a display period rounded down by 1 ns.
static void CheckDisplayTicks(int fps, int expected) {
    using namespace winehua::game;
    Configure(true, false, false, fps);
    FrameDeadline compositor, producer;
    int compositions = 0, productions = 0;
    constexpr uint64_t displayPeriod = 8'333'334;
    for (uint64_t tick = 0; tick < 120; ++tick) {
        const auto now = tick * displayPeriod;
        compositions += compositor.Ready(now, CompositionPeriodNs());
        productions += producer.Ready(now, ProducerPeriodNs(displayPeriod));
    }
    assert(compositions == expected && productions == expected);
}

int main() {
    using namespace winehua::game;
    FitRect fit;
    Configure(false, false, false, 30);
    assert(LimitPeriodNs() == 0 && ProducerPeriodNs(16666667) == 16666667);
    assert(ComputeFitRect(1920, 1080, 800, 600, fit) && fit.dstH == 1080);
    Configure(true, false, true, 30);
    assert(Nearest() && Integer());
    assert(ComputeFitRect(1920, 1080, 800, 600, fit));
    assert(fit.dstW == 800 && fit.dstH == 600 && fit.offX == 560 && fit.offY == 240);
    assert(FitUnmapDisplayX(fit, FitMapDisplayX(fit, 400)) == 400);
    assert(FitUnmapDisplayY(fit, FitMapDisplayY(fit, 300)) == 300);
    assert(ComputeFitRect(400, 300, 800, 600, fit) && fit.dstW == 400 && fit.dstH == 300);
    assert(ProducerPeriodNs(16666667) == 33333334);
    assert(ProducerPeriodNs(100000000) == 100000000);
    FrameDeadline deadline;
    assert(deadline.Ready(100, 10));
    assert(!deadline.Ready(109, 10));
    assert(deadline.Ready(110, 10));
    assert(deadline.Ready(500, 10));
    assert(!deadline.Ready(501, 10)); // late frames have no catch-up debt
    assert(deadline.Ready(501, 0));
    Configure(true, false, false, 60, true, 20);
    assert(Fsr() && !Nearest() && !Integer() && Sharpness()==20);
    assert(LimitPeriodNs()==16666667); // sharpness must not contaminate FPS
    Configure(true, false, false, 0, true, -10);
    assert(Sharpness()==0 && LimitPeriodNs()==0);
    assert(CompositionPeriodNs()==0);
    for (uint64_t period : {8'333'333ull, 11'111'111ull, 16'666'667ull}) {
        assert(ProducerPeriodNs(period)==period); // actual 120/90/60 Hz grant
    }
    Configure(true, false, false, 30, true, 99);
    assert(Sharpness()==20 && LimitPeriodNs()==33333334);
    assert(CompositionPeriodNs()==33333334 && ProducerPeriodNs(8333333)==33333334);
    CheckDisplayTicks(0, 120);
    CheckDisplayTicks(60, 60);
    CheckDisplayTicks(30, 30);
    Configure(true, false, false, 60);
    assert(CompositionPeriodNs()==16666667 && ProducerPeriodNs(8333333)==16666667);
    assert(ProducerPeriodNs(50000000)==50000000); // a limit cannot accelerate the display
    AppliedPolicy applied;
    const auto previous = Snapshot();
    assert(applied.Changed(previous));
    applied.Presented(previous);
    assert(!applied.Changed(previous));
    Configure(true, false, false, 30, true, 5);
    const auto live = Snapshot();
    assert(applied.Changed(live)); // repaint even if a game emits no new frame
    assert(ProducerPeriodNs(8333333)==33333334); // update producer without a refresh-rate change
    assert(applied.Changed(live)); // pause/EAGAIN must not consume the update
    applied.Presented(live);
    assert(!applied.Changed(live));
    Configure(true, false, true, 0);
    assert(applied.Changed(Snapshot()));
    assert(ComputeFitRect(1920, 1080, 800, 600, fit) && fit.dstW == 800);
    assert(FitUnmapDisplayX(fit, FitMapDisplayX(fit, 400)) == 400);
    Configure(true, false, false, 0);
    assert(ComputeFitRect(1920, 1080, 800, 600, fit) && fit.dstW == 1440);
    assert(FitUnmapDisplayX(fit, FitMapDisplayX(fit, 400)) == 400);
    Configure(false, false, false, 0);
    assert(!Enabled() && !Integer() && !Nearest() && LimitPeriodNs() == 0);
    assert(CompositionPeriodNs()==0 && ProducerPeriodNs(8333333)==8333333);
    std::cout << "PASS: default accepts 120 display ticks; explicit 60/30 limits pace both compositor and producer\n";
}
