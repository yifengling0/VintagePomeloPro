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
    {
        VppFrameGenerationBudget budget;
        constexpr uint64_t start=1000000000, gpu=12000000, limit=6000000;
        budget.Completed(start,gpu,limit);
        budget.Completed(start+1,gpu,limit);
        assert(budget.Allow(start+2));
        budget.Completed(start+2,gpu,limit);
        assert(!budget.Allow(start+999999999));
        assert(budget.Allow(start+1000000002));
        // A failed probe backs off. Ordinary display ticks never renew it.
        budget.Completed(start+1000000002,gpu,limit);
        assert(!budget.Allow(start+3000000001));
        assert(budget.Allow(start+3000000002));
        budget.Completed(start+3000000002,gpu,limit);
        assert(!budget.Allow(start+7000000001));
        assert(budget.Allow(start+7000000002));
        // A later light scene recovers without a toggle or session rebuild.
        budget.Completed(start+7000000002,4000000,limit);
        assert(budget.Allow(start+7000000003) && !budget.CoolingDown());
        budget.Suspend(start);
        assert(budget.Allow(start-1)); // monotonic clock/session reset
        budget.Suspend(start);
        budget.Reset();
        assert(budget.Allow(start) && !budget.CoolingDown());
        // Sustained overload is bounded to occasional probes, not 60/s.
        unsigned attempts=0;
        for(uint64_t now=start;now<start+20000000000ull;now+=16666667) {
            if (budget.Allow(now)) { ++attempts; budget.Completed(now,gpu,limit); }
        }
        assert(attempts<12 && attempts>3);
    }
    {
        VppFrameGenerationSchedule schedule;
        constexpr uint64_t display=8333333, source=33333332;
        for (uint64_t frame=1;frame<=4;++frame) schedule.Observe(1,frame,frame*source,display);
        assert(schedule.CanGenerate());
        assert(schedule.GpuBudgetNs(display)==13333332);
        assert(7700000<schedule.GpuBudgetNs(display)); // Mini 30->60 is not a 120 FPS compute budget
        assert(14000000>schedule.GpuBudgetNs(display));
        assert(schedule.GpuBudgetNs(0)==0);
        schedule.Reset();
        for (uint64_t frame=1;frame<=4;++frame) schedule.Observe(1,frame,frame*display*2,display);
        assert(schedule.GpuBudgetNs(display)==6666666); // 60->120 keeps the tighter budget
    }
    Configure(true,false,false,30,false,10,true);
    assert(FrameGeneration() && CompositionPeriodNs()==0 && ProducerPeriodNs(8333334)==33333334);
    Configure(true,false,false,0,false,10,true);
    assert(ProducerPeriodNs(8333334)==8333334); // no implicit source limit
    Configure(true,false,false,60);
    assert(!FrameGeneration() && CompositionPeriodNs()==16666667);
    puts("PASS FG schedule: 30->60 / 60->120, duplicate ticks, saturation, reset, explicit source caps");
}
