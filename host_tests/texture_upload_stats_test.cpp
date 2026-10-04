#include "common/perf_utils.h"
#include "common/texture_upload_stats.h"
#include <cassert>
#include <cstdio>
int main() {
    winehua::TextureUploadStats uploads;
    uploads.Issued(20, 10, 7);
    uploads.Issued(5, 4, 3);
    assert(uploads.calls == 2 && uploads.bytes == 880 && uploads.cpuUs == 10);
    winehua::RendererPerfWindow perf;
    perf.Add(1, 2, uploads.cpuUs, 4, 20, uploads.bytes, false, uploads.calls, true);
    assert(perf.failedSwaps == 1 && perf.displayed == 0);
    assert(perf.uploadBytes == 880 && perf.uploadCalls == 2 && perf.uploadTimedFrames == 1);
    // Static native-only draw/swap does not invent an upload, nor erase failed
    // swap's issued calls. No claim of DMA completion or unique-content FPS.
    perf.Add(1, 2, 0, 4, 6, 0, true, 0, false);
    assert(perf.failedSwaps == 1 && perf.displayed == 1);
    assert(perf.uploadBytes == 880 && perf.uploadCalls == 2 && perf.uploadTimedFrames == 1);
    std::puts("Upload counters preserve issued work across swap failure; static/native frames add zero uploads");
}
