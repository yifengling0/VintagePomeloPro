#include "direct/direct_frame_stats.h"
#include <cassert>
#include <cmath>
#include <iostream>
int main() {
    winehua::direct::DirectFrameAcceptance image;
    image.Acquired();
    assert(!image.Presented(false, true)); // output failed, retry remains fresh
    assert(!image.Presented(true, false)); // hidden image is not displayed
    assert(image.Presented(true, true));
    for (int i = 0; i < 60; ++i) assert(!image.Presented(true, true));
    image.Acquired(); // buffer sequence may be reused for new content
    assert(image.Presented(true, true));
    winehua::direct::DirectFrameRate rate;
    double fps = -1;
    assert(!rate.Sample(0, fps));
    for (int i = 0; i < 24; ++i) rate.Record();
    assert(!rate.Sample(999999, fps));
    assert(rate.Sample(1000000, fps) && std::abs(fps - 24) < 0.001);
    assert(rate.Sample(2000000, fps) && fps == 0); // idle never retains old FPS
    for (int i = 0; i < 11; ++i) rate.Record();
    assert(rate.Sample(3000000, fps) && std::abs(fps - 11) < 0.001);
    std::cout << "PASS: Direct FPS counts fresh visible images, retries and idle\n";
}
