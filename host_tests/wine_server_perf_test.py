#!/usr/bin/env python3
"""Exercise the production diagnostic, including overwritten reply identity."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--wine-src', type=Path, required=True)
args = parser.parse_args()
source = (args.wine_src / 'dlls/ntdll/unix/server.c').read_text(encoding='utf-8')
helper = source.split('/* WINEHUA_SERVER_PERF_BEGIN:', 1)[1].split('/* WINEHUA_SERVER_PERF_END */', 1)[0]
helper = helper.split('*/', 1)[1]
function = source.split('unsigned int server_call_unlocked( void *req_ptr )', 1)[1]
function = 'unsigned int server_call_unlocked( void *req_ptr )' + function.split('\n}\n', 1)[0] + '\n}\n'
fixture = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#define REQ_NB_REQUESTS 4
#define FTRACE_BLOCK_START(...) do {} while (0);
#define FTRACE_BLOCK_END() do {} while (0);
static void trace_sink(const char *fmt, ...) { (void)fmt; }
#define TRACE_(channel) trace_sink
struct __server_request_info {
    union { struct { struct { unsigned int req; } request_header; } req; unsigned int reply; } u;
    const char *name;
};
static unsigned int send_error;
static unsigned int send_request(struct __server_request_info *req) {
    (void)req;
    errno = EIO;
    return send_error;
}
static unsigned int wait_reply(struct __server_request_info *req) {
    req->u.reply = 99999; /* Real server replies overwrite this union. */
    errno = EBUSY;
    return 7;
}
'''
tests = r'''
static void *independent_thread(void *ignored) {
    (void)ignored;
    assert(winehua_server_perf.calls == 0);
    winehua_server_perf_add(2, "thread", 100, 900);
    assert(winehua_server_perf.calls == 1);
    assert(winehua_server_perf.entries[2].total_ns == 800);
    return NULL;
}
int main(void) {
    pthread_t thread;
    struct __server_request_info req = { .u.req.request_header.req = 1, .name = "select" };
    unsetenv("WINEHUA_SERVER_PERF");
    assert(!winehua_server_perf_is_enabled());
    assert(server_call_unlocked(&req) == 7);
    assert(winehua_server_perf.calls == 0);
    winehua_server_perf_enabled = -1;
    setenv("WINEHUA_SERVER_PERF", "1x", 1);
    assert(!winehua_server_perf_is_enabled());
    winehua_server_perf_enabled = -1;
    setenv("WINEHUA_SERVER_PERF", "1", 1);
    req.u.req.request_header.req = 1;
    assert(server_call_unlocked(&req) == 7);
    assert(errno == EBUSY);
    assert(winehua_server_perf.entries[1].calls == 1);
    assert(!strcmp(winehua_server_perf.entries[1].name, "select"));
    assert(req.u.reply == 99999);
    req.u.req.request_header.req = 3;
    req.name = "release_semaphore";
    send_error = 17;
    assert(server_call_unlocked(&req) == 17);
    assert(errno == EIO);
    assert(winehua_server_perf.entries[3].calls == 1);
    memset(&winehua_server_perf, 0, sizeof(winehua_server_perf));
    winehua_server_perf_add(4, "invalid", 100, 200);
    winehua_server_perf_add(1, "zero", 0, 200);
    winehua_server_perf_add(1, "backwards", 200, 100);
    assert(winehua_server_perf.calls == 0);
    winehua_server_perf_add(1, "select", 100, 500);
    winehua_server_perf_add(1, "select", 600, 800);
    assert(winehua_server_perf.entries[1].calls == 2);
    assert(winehua_server_perf.entries[1].total_ns == 600);
    assert(winehua_server_perf.entries[1].max_ns == 400);
    assert(!pthread_create(&thread, NULL, independent_thread, NULL));
    assert(!pthread_join(thread, NULL));
    assert(winehua_server_perf.calls == 2);
    /* Flush only after both the minimum call batch and interval have elapsed. */
    for (unsigned int i=2; i<64; ++i)
        winehua_server_perf_add(1, "select", 1000000200ULL, 1000000300ULL);
    assert(winehua_server_perf.calls == 0);
    assert(winehua_server_perf.entries[1].calls == 0);
    puts("PASS: default off, identity, errno, errors, bounds, TLS, interval");
}
'''
with tempfile.TemporaryDirectory(prefix='wine-server-perf-') as temporary:
    root = Path(temporary)
    (root / 'test.c').write_text(fixture + helper + function + tests, encoding='utf-8')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-o', str(root / 'test'), str(root / 'test.c')], check=True)
    subprocess.run([str(root / 'test')], check=True)
