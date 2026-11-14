#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Execute the locked Venus present loop with a deterministic target/clock.

This checks production control flow, not real GPU timing or device acceptance.
"""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, required=True)
p.add_argument('--build-dir', type=Path, required=True)
a = p.parse_args()
text = (a.source/'src/virtio/vulkan/vn_renderer_vtest.c').read_text()
start = text.index('   uint64_t present_drain_us = 0;', text.index('\nvn_winehua_present('))
end = text.index('\n   if (drain_perf) {\n      const int64_t present_end_ns', start)
body = text[start:end]
out = a.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
code = r'''
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#define vn_log(...) ((void)0)
static uint64_t elapsed_us, ready_us;
static unsigned calls, drains, markers, sleeps;
static int failure;
struct device { void *instance, *renderer, *primary_ring; };
static bool vtest_winehua_present_trace_enabled(void) { return true; }
static int64_t os_time_get_nano(void) { return (int64_t)(elapsed_us+1)*1000; }
static void vn_ring_roundtrip(void *ring) { (void)ring; ++markers; }
static void vn_ring_wait_all(void *ring) { (void)ring; ++drains; }
static void fake_sleep(unsigned us) { elapsed_us+=us; ++sleeps; }
#define usleep fake_sleep
static int vn_renderer_winehua_present(void *renderer, void *present) {
    (void)renderer; (void)present; ++calls;
    return failure ? failure : elapsed_us >= ready_us ? 0 : -EAGAIN;
}
static int run(void) {
    const bool drain_perf=true;
    struct device storage={0}, *dev=&storage;
    unsigned present=0;
''' + body + r'''
    assert(present_drain_attempts==calls && present_renderer_us==0 && present_drain_us==0);
    return result;
}
int main(void) {
    const uint64_t targets[]={0,1000,127000,162000,254000,255000,256000,422000};
    for(unsigned i=0;i<sizeof(targets)/sizeof(*targets);++i) {
        elapsed_us=calls=drains=markers=sleeps=0;ready_us=targets[i];failure=0;
        int result=run();
        assert(result==(ready_us<=255000?0:-EAGAIN));
        assert(drains==1 && markers==1 && calls<=9 && sleeps+1==calls && elapsed_us<=255000);
        if(ready_us>127000)assert(elapsed_us==255000 && calls==9);
    }
    elapsed_us=calls=drains=markers=sleeps=0;failure=-EPROTO;
    assert(run()==-EPROTO && calls==1 && drains==1 && markers==1 && sleeps==0);
    puts("PASS: ready at 0..255ms, bounded exhaustion, single ring drain, permanent failure");
}
'''
source=out/'present-retry.c';source.write_text(code)
binary=out/'present-retry'
subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',source,'-o',binary],check=True,timeout=30)
subprocess.run([binary],check=True,timeout=10)
