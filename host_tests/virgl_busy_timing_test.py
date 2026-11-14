"""Exercise the production handler with injected phase costs and failures."""
import argparse
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--source', type=Path, required=True)
args = p.parse_args()
text = (args.source / 'vtest/vtest_renderer.c').read_text()
helpers = text[text.index('enum winehua_busy_stage {'):text.index('/* End complete busy-wait diagnostics. */')]
function = text[text.index('int vtest_resource_busy_wait(UNUSED'):text.index('\nint vtest_resource_busy_wait_nop(')]
prefix = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#define UNUSED __attribute__((unused))
#ifndef WINEHUA_VTEST_BUSY_DIAGNOSTIC
#define WINEHUA_VTEST_BUSY_DIAGNOSTIC 0
#endif
#include "vtest_protocol.h"
static uint64_t now_ns = 1000000000ull;
static unsigned clocks, finish_calls, polls, fd_waits, writes, summaries;
static int read_failure, finish_failure, write_failure, poll_fd = 9;
static bool enabled;
static uint32_t flags, handle, wire[3];
static int fake_clock_gettime(clockid_t clock, struct timespec *ts) {
   assert(clock == CLOCK_MONOTONIC); clocks++;
   ts->tv_sec = now_ns / 1000000000ull; ts->tv_nsec = now_ns % 1000000000ull;
   return 0;
}
#define clock_gettime fake_clock_gettime
struct vtest_input { int (*read)(struct vtest_input *, void *, int); };
struct vtest_context { struct vtest_input *input; bool context_initialized;
   uint32_t ctx_id, implicit_fence_submitted; int out_fd; };
static struct { uint32_t implicit_fence_completed; } renderer;
static int input_read(UNUSED struct vtest_input *input, void *buf, int size) {
   assert(size == 8); now_ns += 3000000;
   uint32_t request[] = {handle, flags}; memcpy(buf, request, sizeof(request));
   return read_failure ? -1 : size;
}
static struct vtest_input input = { input_read };
static struct vtest_context ctx = { &input, true, 7, 10, 33 };
static struct vtest_context *vtest_get_current_context(void) { return &ctx; }
static bool winehua_vtest_perf_summary_enabled(void) { return enabled; }
static void winehua_diag(const char *fmt, ...) {
   if (strstr(fmt, "busy summary")) summaries++;
}
static int virgl_renderer_context_finish(uint32_t id) {
   assert(id == 7); finish_calls++; now_ns += 50000000;
   return finish_failure;
}
static int virgl_renderer_get_poll_fd(void) { return poll_fd; }
static void vtest_wait_for_fd_read(int fd) {
   assert(fd == 9); fd_waits++; now_ns += 20000000;
}
static void virgl_renderer_poll(void) {
   polls++; now_ns += 1000000;
   renderer.implicit_fence_completed = ctx.implicit_fence_submitted;
}
static int vtest_block_write(int fd, const void *buf, int size) {
   assert(fd == 33); writes++; now_ns += 1000000;
   if (write_failure == (int)writes) return -13;
   if (writes == 1) { assert(size == 8); memcpy(wire, buf, 8); }
   else { assert(writes == 2 && size == 4); memcpy(wire+2, buf, 4); }
   return size;
}
'''
tests = r'''
static void reset(void) {
   memset(winehua_busy_totals, 0, sizeof(winehua_busy_totals));
   winehua_busy_window_ns = 0;
   clocks = finish_calls = polls = fd_waits = writes = summaries = 0;
   read_failure = finish_failure = write_failure = 0;
   poll_fd = 9; handle = 123; flags = 1; enabled = true;
   ctx.context_initialized = true; ctx.implicit_fence_submitted = 10;
   renderer.implicit_fence_completed = 9;
   memset(wire, 0, sizeof(wire));
}
static void reply(uint32_t busy) {
   assert(writes == 2 && wire[0] == 1 && wire[1] == VCMD_RESOURCE_BUSY_WAIT && wire[2] == busy);
}
int main(void) {
   assert(!setenv("VTEST_SYNC_GL_FINISH", "1", 1));
   reset();
   assert(!vtest_resource_busy_wait(2)); reply(0);
   struct winehua_busy_totals *t = &winehua_busy_totals[1];
   assert(finish_calls == 1 && polls == 1 && fd_waits == 1);
   assert(t->calls == 1 && t->finishes == 1 && !t->errors);
   assert(t->ns[WINEHUA_BUSY_READ] == 3000000 && t->ns[WINEHUA_BUSY_FINISH] == 50000000);
   assert(t->ns[WINEHUA_BUSY_POLL] == 21000000 && t->ns[WINEHUA_BUSY_REPLY] == 2000000);
   assert(t->total_ns == 76000000 && t->max_ns == 76000000);
   reset(); enabled = false;
   assert(!vtest_resource_busy_wait(2)); reply(0);
#if WINEHUA_VTEST_BUSY_DIAGNOSTIC
   assert(clocks && winehua_busy_totals[1].calls == 1 && finish_calls == 1 && polls == 1);
#else
   assert(!clocks && !winehua_busy_totals[1].calls && finish_calls == 1 && polls == 1);
#endif
   reset(); flags = 0;
   assert(!vtest_resource_busy_wait(2)); reply(1);
   assert(!finish_calls && !polls && winehua_busy_totals[0].calls == 1);
   reset(); renderer.implicit_fence_completed = 10;
   assert(!vtest_resource_busy_wait(2)); reply(0);
   assert(finish_calls == 1 && !polls); /* Do not skip finish on a cached fence. */
   reset(); assert(!setenv("VTEST_SYNC_GL_FINISH", "0", 1));
   assert(!vtest_resource_busy_wait(2) && finish_calls == 1);
   reset(); unsetenv("VTEST_SYNC_GL_FINISH"); poll_fd = -1;
   assert(!vtest_resource_busy_wait(2)); reply(0);
   assert(!finish_calls && polls == 1 && !fd_waits);
   assert(!setenv("VTEST_SYNC_GL_FINISH", "1", 1));
   reset(); finish_failure = -5;
   assert(vtest_resource_busy_wait(2) == -5 && !polls && !writes);
   assert(winehua_busy_totals[1].errors == 1 && winehua_busy_totals[1].ns[WINEHUA_BUSY_FINISH] == 50000000);
   for (int failure = 1; failure <= 2; failure++) {
      reset(); write_failure = failure;
      assert(vtest_resource_busy_wait(2) == -13 && writes == (unsigned)failure);
      assert(winehua_busy_totals[1].errors == 1);
      assert(winehua_busy_totals[1].ns[WINEHUA_BUSY_REPLY] == (uint64_t)failure * 1000000);
   }
   reset(); read_failure = 1;
   assert(vtest_resource_busy_wait(2) == -1 && !finish_calls && !writes);
   reset(); ctx.context_initialized = false;
   assert(vtest_resource_busy_wait(2) == -1 && !finish_calls && !writes);
   reset(); ctx.context_initialized = false; handle = flags = 0;
   assert(!vtest_resource_busy_wait(2)); reply(1); /* Protocol probe preserved. */
   reset(); flags = 0; ctx.implicit_fence_submitted = 0; renderer.implicit_fence_completed = UINT32_MAX;
   assert(!vtest_resource_busy_wait(2)); reply(1); /* Signed wrap comparison preserved. */
   reset();
   for (int i = 0; i < 50; i++) {
      writes = 0; renderer.implicit_fence_completed = 9;
      assert(!vtest_resource_busy_wait(2));
   }
   assert(!summaries);
   now_ns += 5000000000ull; writes = 0;
   assert(!vtest_resource_busy_wait(2));
   assert(summaries == 1 && !winehua_busy_totals[1].calls);
   puts("PASS production busy handler: full finish/read/poll/reply timings, disabled clocks, wire/sync parity, failures, probe/wrap and bounded summaries");
}
'''
with tempfile.TemporaryDirectory(prefix='virgl-busy-timing-') as tmp:
    source = Path(tmp) / 'test.c'
    source.write_text(prefix + helpers + function + tests)
    for diagnostic in (0, 1):
        out = Path(tmp) / ('test-' + str(diagnostic))
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-DWINEHUA_VTEST_BUSY_DIAGNOSTIC=' + str(diagnostic),
                    '-I', str(args.source / 'vtest'), str(source), '-o', str(out)], check=True)
        subprocess.run([str(out)], check=True, timeout=15)
