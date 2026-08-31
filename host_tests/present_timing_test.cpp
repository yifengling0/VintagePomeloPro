#include "graphics/present_timing.h"
#include "graphics/present_pacing.h"
#include <cassert>
#include <iostream>

int main()
{
    assert(winehua::PresentPacingPeriodNs(16666667) == 16166667);
    assert(winehua::QueuePresentPacingPeriodNs(16666667) == 16666667);
    assert(winehua::QueuePresentPacingPeriodNs(0) ==
           winehua::kDefaultPresentFramePeriodNs);
    winehua::PresentTimingWindow window;
    assert(window.Count() == 0 && window.CpuCsv().empty());
    for (size_t i = 0; i < window.kSize; ++i)
        assert(window.Add(i, (i + 1) * 1000000, 1, 2, 3, 4) == (i == 119));
    assert(window.Count() == 120);
    assert(window.RequestUs() == 120 && window.DrawUs() == 240);
    assert(window.PublishUs() == 360 && window.RestoreUs() == 480);
    assert(window.CpuCsv().find("0,1,2,3,") == 0);
    assert(window.IntervalCsv().find("0,1000,1000,") == 0);
    assert(!window.Add(7, 123000000, 4, 3, 2, 1));
    assert(window.Count() == 1 && window.IntervalCsv() == "3000");
    assert(window.RequestUs() == 4 && window.CpuCsv() == "7");
    window.Reset();
    assert(!window.Add(5, 10000000, 0, 0, 0, 0));
    assert(window.IntervalCsv() == "0");
    assert(!window.Add(5, 9000000, 0, 0, 0, 0));
    assert(window.IntervalCsv() == "0,0");
    std::cout << "present_timing_test PASS\n";
}
