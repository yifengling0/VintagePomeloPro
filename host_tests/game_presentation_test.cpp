#include "graphics/game_presentation.h"
#include "compositor/frame/geometry.h"
#include <cassert>
#include <cmath>

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
    Configure(true, false, false, 30, true, 99);
    assert(Sharpness()==20 && LimitPeriodNs()==33333334);
    Configure(false, false, false, 0);
    assert(!Enabled() && !Integer() && !Nearest() && LimitPeriodNs() == 0);
}
