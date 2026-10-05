"""Production opt-in accumulators and manual API hooks, with clock/driver boundaries stubbed.

These tests measure host overhead and semantics, not OHOS/FEX/GPU performance.
"""
import os
from pathlib import Path
import re
import subprocess
import unittest

import wine_shm_state_cache_test as base

BOUNDARY = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <time.h>
static __thread volatile int perf_enabled;
static __thread uint64_t fake_us=100, perf_clock_calls;
static __thread int perf_clock_fail;
static __thread unsigned perf_log_count;
static __thread char perf_log[1024];
static __thread unsigned perf_log_max, perf_log_truncated;
static uint64_t host_ns(void) {
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;
}
static int fake_clock_gettime(int id,struct timespec *t) {
    (void)id; ++perf_clock_calls; if(perf_clock_fail)return -1; t->tv_sec=fake_us/1000000; t->tv_nsec=(fake_us%1000000)*1000; ++fake_us; return 0;
}
static struct { struct { uintptr_t UniqueProcess,UniqueThread; } ClientId; } perf_teb={{7,9}};
#define NtCurrentTeb() (&perf_teb)
#define HandleToULong(x) ((unsigned long)(x))
#define clock_gettime fake_clock_gettime
#define DECLSPEC_NOINLINE __attribute__((noinline))
#define WINE_DECLARE_DEBUG_CHANNEL(x)
#define TRACE_ON(x) perf_enabled
#define TRACE_(x) perf_capture
static void perf_capture(const char *fmt,...) {
    va_list args; va_start(args,fmt); int n=vsnprintf(perf_log,sizeof(perf_log),fmt,args); va_end(args); ++perf_log_count;
    if(n<0 || (unsigned)n>=sizeof(perf_log)) ++perf_log_truncated;
    if(n>0 && (unsigned)n>perf_log_max)perf_log_max=n;
}
__attribute__((constructor)) static void perf_environment(void) {
    perf_enabled=getenv("WINEHUA_TEST_PERF") && strcmp(getenv("WINEHUA_TEST_PERF"),"0");
}
'''


def production_helpers(source, header=''):
    begin=source.index('/* winehua_perf implementation begin */')
    end=source.index('/* winehua_perf implementation end */')+len('/* winehua_perf implementation end */')
    return BOUNDARY+header+source[begin:end]+'\n'


def production_function(source, signature):
    """Include production's tightly scoped 0035 copy macro, when present."""
    body=base.function(source,signature)
    pos=source.index(signature)
    line=source[:pos].splitlines()[-1]
    if line.startswith('#define memcpy('):
        assert source[pos+len(body):].startswith('#undef memcpy')
        body=line+'\n'+body+'#undef memcpy\n'
    return body


HELPER_MAIN = r'''
static void reset_perf(void) {
    memset(&winehua_perf_state,0,sizeof(winehua_perf_state)); fake_us=100; perf_clock_calls=perf_log_count=0;
}
static void *thread_test(void *p) {
    (void)p; perf_enabled=1; reset_perf();
    uint64_t t=winehua_perf_begin(); winehua_perf_end(0,t,7,3,0);
    assert(winehua_perf_state.counters[0].count==1 && winehua_perf_state.counters[0].copied==3);
    assert(perf_log_count==0); return NULL;
}
static volatile unsigned sink;
__attribute__((noinline)) static void work(unsigned n) { sink+=n; }
__attribute__((noinline)) static void instrumented(unsigned n) {
    uint64_t t=winehua_perf_begin(); work(n); winehua_perf_end(0,t,64,64,0);
}
int main(int argc,char **argv) {
    assert(argc==2); reset_perf(); perf_enabled=0;
    if (!strcmp(argv[1],"off")) {
        for (unsigned i=0;i<10000;++i) { uint64_t t=winehua_perf_begin(); winehua_perf_end(0,t,64,64,0); }
        assert(perf_clock_calls==0 && perf_log_count==0);
        assert(!winehua_perf_state.window_start && !winehua_perf_state.counters[0].count);
    } else if (!strcmp(argv[1],"summary")) {
        perf_enabled=1;
        uint64_t t=winehua_perf_begin(); winehua_perf_end(0,t,100,20,0);
        assert(perf_clock_calls==2 && !perf_log_count);
        assert(winehua_perf_state.counters[0].count==1 && winehua_perf_state.counters[0].total_us==1);
        for (unsigned r=1;r<=4;++r) { t=winehua_perf_begin(); winehua_perf_end(0,t,10,2,r); }
        fake_us=1000098; t=winehua_perf_begin(); winehua_perf_end(0,t,0,0,0); assert(!perf_log_count);
        t=winehua_perf_begin(); winehua_perf_end(0,t,0,0,0); assert(perf_log_count==1);
        assert(strstr(perf_log,"pid=7 tid=9 window_us=1000001"));
        assert(strstr(perf_log,"=7/7/1/140/28/3/1/1/1/1"));
        assert(!winehua_perf_state.counters[0].count);
        t=winehua_perf_begin(); winehua_perf_end(0,t,0,0,0); assert(perf_log_count==1);
        fake_us+=2000000; t=winehua_perf_begin(); winehua_perf_end(0,t,0,0,0); assert(perf_log_count==2);
        puts(perf_log);
    } else if (!strcmp(argv[1],"long")) {
        perf_enabled=1; uint64_t t=winehua_perf_begin(); fake_us+=5000000;
        winehua_perf_end(0,t,5,0,1); assert(perf_log_count==1);
        assert(strstr(perf_log,"=1/5000001/5000001/5/0/0/1/0/0/0"));
    } else if (!strcmp(argv[1],"backward")) {
        perf_enabled=1; uint64_t t=winehua_perf_begin(); fake_us=1;
        winehua_perf_end(0,t,1,1,0); assert(!winehua_perf_state.counters[0].count && !perf_log_count);
    } else if (!strcmp(argv[1],"clock-failure")) {
        perf_enabled=1;perf_clock_fail=1;
        uint64_t t=winehua_perf_begin();assert(t==0);perf_clock_fail=0;
        fake_us=9000000000000ull;winehua_perf_end(0,t,1,1,0);
        assert(!winehua_perf_state.counters[0].count && !perf_log_count);
        t=winehua_perf_begin();perf_clock_fail=1;winehua_perf_end(0,t,1,1,0);perf_clock_fail=0;
        assert(!winehua_perf_state.counters[0].count && !perf_log_count);
        t=winehua_perf_begin();winehua_perf_end(0,t,1,1,0);
        assert(winehua_perf_state.counters[0].count==1 && !perf_log_count);
    } else if (!strcmp(argv[1],"threads")) {
        pthread_t a,b; perf_enabled=1;
        uint64_t t=winehua_perf_begin(); winehua_perf_end(0,t,99,11,0);
        assert(!pthread_create(&a,NULL,thread_test,NULL) && !pthread_create(&b,NULL,thread_test,NULL));
        pthread_join(a,NULL); pthread_join(b,NULL);
        assert(winehua_perf_state.counters[0].count==1 && winehua_perf_state.counters[0].copied==11);
    } else if (!strcmp(argv[1],"saturation")) {
        perf_enabled=1;
        winehua_perf_state.counters[0].requested=UINT64_MAX-1;
        uint64_t t=winehua_perf_begin(); winehua_perf_end(0,t,10,0,0);
        assert(winehua_perf_state.counters[0].requested==UINT64_MAX);
    } else if (!strcmp(argv[1],"all-saturated")) {
        perf_enabled=1;
        memset(winehua_perf_state.counters,0xff,sizeof(winehua_perf_state.counters));
        winehua_perf_state.window_start=101;fake_us=1000101;
        uint64_t t=winehua_perf_begin();winehua_perf_end(0,t,1,1,0);
        assert(perf_log_count==WINEHUA_PERF_COUNT && !perf_log_truncated && perf_log_max<512);
        assert(strchr(perf_log,'\n'));
        t=winehua_perf_begin();winehua_perf_end(0,t,1,1,0);assert(perf_log_count==WINEHUA_PERF_COUNT);
    } else if (!strcmp(argv[1],"benchmark")) {
        const unsigned n=2000000; uint64_t t=host_ns();
        for (unsigned i=0;i<n;++i) work(i);
        uint64_t bare=host_ns()-t; t=host_ns();
        for (unsigned i=0;i<n;++i) instrumented(i);
        uint64_t off=host_ns()-t;
        assert(!perf_clock_calls && !perf_log_count);
        printf("host default-off: bare=%.2f ns/op, instrumented=%.2f ns/op, delta=%.2f ns/op (%u ops; no device inference)\n",
               (double)bare/n,(double)off/n,((double)off-bare)/n,n);
    } else return 2;
    return 0;
}
'''

class PerfTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        base.WineShmStateCacheTest.setUpClass.__func__(cls)
        cls.unix=cls.first_replay['dlls/opengl32/unix_wgl.c'].decode()
        cls.binaries={}
        cls.compile('unix',production_helpers(cls.unix)+HELPER_MAIN)

    @classmethod
    def compile(cls,name,body):
        c=cls.folder/(name+'-perf.c');c.write_text(body);binary=c.with_suffix('')
        subprocess.run(['gcc','-D_POSIX_C_SOURCE=200809L','-std=c11','-O2','-Wall','-Wextra','-Werror',
            '-Wno-unused-function','-Wno-unused-variable','-fsanitize=address,undefined','-fno-pie','-no-pie',
            str(c),'-pthread','-o',str(binary)],check=True)
        cls.binaries[name]=binary

    def run_case(self,name,case=None,enabled=False):
        command=[str(self.binaries[name])]+([case] if case else [])
        r=subprocess.run(command,capture_output=True,text=True,timeout=20,
            env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:halt_on_error=1',WINEHUA_TEST_PERF=str(int(enabled))))
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
        return r

    def test_production_accumulator_semantics(self):
        for scope in ('unix',):
            for case in ('off','summary','long','backward','clock-failure','threads','saturation','all-saturated'):
                with self.subTest(scope=scope,case=case):self.run_case(scope,case)

    def test_default_off_host_cost(self):
        print(self.run_case('unix','benchmark').stdout.strip())

    def test_actual_unix_hook_coverage(self):
        self.assertEqual(self.unix.count('winehua_perf_end(WINEHUA_PERF_MAP_DRIVER,'),4)
        self.assertEqual(self.unix.count('winehua_perf_end(WINEHUA_PERF_GL_FLUSH,'),3)
        self.assertEqual(self.unix.count('winehua_perf_end(WINEHUA_PERF_GL_UNMAP,'),2)
        self.assertEqual(self.first_replay,self.second_replay)


if __name__=='__main__':
    unittest.main(argv=[__file__]+base.remaining)
