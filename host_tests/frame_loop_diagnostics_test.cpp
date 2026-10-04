#include "common/frame_loop_diagnostics.h"
#include <cassert>
#include <iostream>

int main() {
    using namespace winehua;
    LoopTimingHistogram histogram;
    assert(histogram.UpperPercentile(99) == 0);
    for (int i = 0; i < 99; ++i) histogram.Add(16667);
    histogram.Add(1000000);
    assert(histogram.count == 100 && histogram.maxUs == 1000000);
    assert(histogram.UpperPercentile(50) == 16750);
    assert(histogram.UpperPercentile(99) == 16750);
    assert(histogram.UpperPercentile(100) == 1000000);

    FrameLoopDiagnosticWindow window;
    assert(!window.Sync(FrameLoopDiagnosticState(), 1000));
    window.NoteTake(100, false, false, false, false, false);
    assert(window.loops == 0 && !window.Due(5000000));
    SetFrameLoopDiagnostics(true);
    const uint64_t enabledState = FrameLoopDiagnosticState();
    SetFrameLoopDiagnostics(true);
    assert(FrameLoopDiagnosticState() == enabledState); // repeated Want does not reset sampling
    assert(window.Sync(enabledState, 1000));
    for (int i = 0; i < 240; ++i) {
        window.NoteTake(100, false, false, false, false, false);
        window.NoteWork(150);
        window.NoteWait(8000);
    }
    assert(window.loops == 240 && window.noFrame == 240 && window.presents == 0);
    assert(window.Due(2001000)); // an idle window is observable without 120 presents
    assert(window.work.sumUs == 36000 && window.wait.sumUs == 1920000);
    window.NotePresent(true, 2001000);
    window.NotePresent(false, 2002000);
    window.NotePresent(true, 2063500);
    assert(window.presents == 2 && window.failed == 1 && window.presentGap.maxUs == 62500);
    const auto source = gProducerDiagnostics.Snapshot();
    window.ResetWindow(2064000, source);
    window.NotePresent(true, 2126000);
    assert(window.presentGap.count == 1 && window.presentGap.maxUs == 62500); // carry interval across windows

    LoopTimingHistogram lock;
    assert(gTakeLockWaitSink == nullptr);
    {
        ScopedTakeLockSink scope(&lock);
        TakeLockTimer timer;
        timer.Acquired();
        assert(lock.count == 1);
        {
            ScopedTakeLockSink disabled(nullptr);
            TakeLockTimer noTimer;
            noTimer.Acquired();
            assert(lock.count == 1);
        }
        assert(gTakeLockWaitSink == &lock);
    }
    assert(gTakeLockWaitSink == nullptr);
    SetFrameLoopDiagnostics(false);
    assert(!window.Sync(FrameLoopDiagnosticState(), 5000000));
    SetFrameLoopDiagnostics(true);
    window.Sync(FrameLoopDiagnosticState(), 9000000);
    window.NotePresent(true, 9000010);
    assert(window.presentGap.count == 0); // disabled time does not become a stall
    NoteProducerCommit(true, false, false, 3840000, 700, 1);
    const auto last = gProducerDiagnostics.Snapshot();
    assert(last.shmTop-source.shmTop == 1 && last.bytes-source.bytes == 3840000);
    assert(last.commitUs-source.commitUs == 700 && last.callbacks-source.callbacks == 1);
    std::cout << "frame_loop_diagnostics_test PASS\n";
}
