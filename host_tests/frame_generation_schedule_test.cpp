#include "FrameGenerationSchedule.h"
#include "graphics/game_presentation.h"
#include <cassert>
#include <cstdio>
int main() {
    for (uint64_t period:{16666667ull,8333334ull}) {
        VppFrameGenerationSchedule schedule;
        const uint64_t start=1000000000;
        for (uint64_t frame=1;frame<10;++frame) {
            const auto now=start+frame*period*2;
            assert(schedule.Observe(1,frame,now,period));
            assert(schedule.CanGenerate()==(frame>=3));
            if (schedule.CanGenerate()) {
                schedule.Generated(now,period);
                assert(schedule.Pending() && schedule.DueNs()==now+period);
                // A display tick is not a new source frame.
                assert(!schedule.Observe(1,frame,now+period,period));
                assert(!schedule.CanGenerate());
                assert(schedule.Reason(period)==VPP_FG_RUNNING);
                schedule.RealPresented(); assert(!schedule.Pending());
            }
        }
        // Saturated source and absent native cadence must never generate.
        schedule.Reset();
        for(uint64_t frame=1;frame<30;++frame) { schedule.Observe(1,frame,start+frame*period,period); assert(!schedule.CanGenerate()); }
        schedule.Observe(2,30,start+31*period,period); assert(!schedule.CanGenerate());
        schedule.Observe(2,31,start+100*period,period); assert(!schedule.CanGenerate());
        schedule.Reset();
        for(uint64_t frame=1;frame<8;++frame) { schedule.Observe(1,frame,start+frame*period*2,0); assert(!schedule.CanGenerate()); }
    }
    using namespace winehua::game;
    Configure(true,false,false,30,false,10,true);
    assert(FrameGeneration() && CompositionPeriodNs()==0 && ProducerPeriodNs(8333334)==33333334);
    Configure(true,false,false,0,false,10,true);
    assert(ProducerPeriodNs(8333334)==8333334); // no implicit source limit
    Configure(true,false,false,60);
    assert(!FrameGeneration() && CompositionPeriodNs()==16666667);
    puts("PASS FG schedule: 30->60 / 60->120, duplicate ticks, saturation, reset, explicit source caps");
}
