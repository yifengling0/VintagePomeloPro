#!/usr/bin/env python3
"""Compile the production Venus present loop with deterministic ring/socket stubs."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "thirdparty/mesa/src/virtio/vulkan/vn_renderer_vtest.c"


def main():
    source = SOURCE.read_text()
    start = source.index("   uint64_t present_drain_us = 0;", source.index("\nvn_winehua_present("))
    end = source.index("\n   if (drain_perf) {\n      const int64_t present_end_ns", start)
    loop = source[start:end]
    harness = r'''
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
struct device { void *primary_ring, *renderer, *instance; };
struct request { unsigned serial; };
static char events[64];
static unsigned count, calls, transient, delays, drained, waited;
static int final_result;
static const struct request *same_request;
static bool trace;
static void event(char value) { events[count++] = value; }
static int64_t os_time_get_nano(void) { static int64_t ns; return ns += 1000; }
static bool vtest_winehua_present_trace_enabled(void) { return trace; }
static void vn_ring_roundtrip(void *ring) { assert(ring); ++drained; event('D'); }
static void vn_ring_wait_all(void *ring) { assert(ring && drained == 1); ++waited; event('W'); }
static void vn_log(void *instance, const char *fmt, ...) { assert(instance && fmt); }
static int vn_renderer_winehua_present(void *renderer, const struct request *present) {
    assert(renderer && drained == 1 && waited == 1);
    if (same_request) assert(same_request == present);
    same_request = present;
    event('P');
    return calls++ < transient ? -EAGAIN : final_result;
}
static int usleep(unsigned us) { assert(us == (1000u << delays)); ++delays; event('S'); return 0; }
static int run(bool drain_perf) {
    struct device device = {(void*)1, (void*)2, (void*)3}, *dev = &device;
    const unsigned serial = 17;
    const struct request present = {serial};
    (void)serial;
    /* PRODUCTION_LOOP */
    assert(present_drain_attempts == calls);
    if (drain_perf) assert(present_renderer_us == calls && present_drain_us >= 1);
    return result;
}
static void check(unsigned retries, int result, bool perf, bool tracing) {
    memset(events, 0, sizeof(events));
    count = calls = delays = drained = waited = 0;
    same_request = NULL;
    transient = retries; final_result = result; trace = tracing;
    assert(run(perf) == (retries >= 8 ? -EAGAIN : result));
    assert(drained == 1 && waited == 1);
    assert(events[0] == 'D' && events[1] == 'W' && events[2] == 'P');
    assert(calls == (retries >= 8 ? 8 : retries + 1));
    assert(delays == (retries >= 8 ? 8 : retries));
}
int main(void) {
    check(0, 0, false, false);
    check(6, 0, false, false); /* exact target attaches on the seventh request */
    check(8, 0, false, false); /* bounded retry exhaustion */
    check(0, -EIO, false, false); /* permanent errors are not retried */
    check(3, -EIO, true, true);
    puts("Venus production retry: ordering, one drain, same request, bounds and permanent errors PASS");
    return 0;
}
'''.replace("    /* PRODUCTION_LOOP */", loop)
    with tempfile.TemporaryDirectory(prefix="vp-venus-retry-") as directory:
        c = pathlib.Path(directory) / "retry.c"
        binary = pathlib.Path(directory) / "retry"
        c.write_text(harness)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
