// Exercise the production cadence observer; mock only the system VSync API.
#include "graphics/display_cadence.h"
#include <cassert>
#include <iostream>
struct OH_NativeVSync {};
static OH_NativeVSync connection;
static int created, destroyed, requests, queries;
static bool unavailable;
static long long granted, visible;
OH_NativeVSync* OH_NativeVSync_Create(const char*, unsigned) {
    ++created;
    return unavailable ? nullptr : &connection;
}
void OH_NativeVSync_Destroy(OH_NativeVSync* value) { assert(value == &connection); ++destroyed; }
int OH_NativeVSync_SetExpectedFrameRateRange(OH_NativeVSync* value, OH_NativeVSync_ExpectedRateRange* range) {
    assert(value == &connection && range->min == 60 && range->max == 120 && range->expected == 120);
    return 0;
}
int OH_NativeVSync_RequestFrame(OH_NativeVSync*, OH_NativeVSync_FrameCallback callback, void* data) {
    ++requests;
    assert(data == nullptr);
    // The SDK updates its cache on a callback; mere GetPeriod polling is stale.
    visible = granted;
    callback(0, data);
    return 0;
}
int OH_NativeVSync_GetPeriod(OH_NativeVSync*, long long* period) { ++queries; *period = visible; return 0; }
int main() {
    {
        winehua::DisplayCadence cadence;
        assert(cadence.PeriodNs() == 16666667);
        cadence.Initialize(); cadence.Initialize();
        assert(created == 1);
        granted = 8333333;
        assert(cadence.Refresh(0) && cadence.PeriodNs() == 8333333);
        granted = 16666667;
        assert(!cadence.Refresh(50000) && cadence.PeriodNs() == 8333333);
        assert(cadence.Refresh(100000) && cadence.PeriodNs() == 16666667);
        granted = 0;
        assert(!cadence.Refresh(200000) && cadence.PeriodNs() == 16666667);
        granted = 10000000;
        assert(cadence.Refresh(300000) && cadence.PeriodNs() == 10000000);
        assert(requests == 5 && queries == 4);
    }
    assert(destroyed == 1);
    unavailable = true;
    {
        winehua::DisplayCadence cadence;
        cadence.Initialize();
        assert(!cadence.Refresh(0) && cadence.PeriodNs() == 16666667);
    }
    assert(destroyed == 1);
    std::cout << "PASS: actual 120/60/100Hz transitions, callback-driven period, idle queries and unavailable VSync\n";
}
