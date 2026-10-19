"""Exercise the production pipe reader with a trace flood and a late error."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
production = (ROOT / 'entry/src/main/cpp/proc/wine_child.cpp').read_text()
start = production.index('struct stderr_ctx ')
end = production.index('\n#undef LOG_DOMAIN', start)
reader = production[start:end]
fixture = r'''
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <thread>
#include <string>
#include <vector>
#include <cassert>
#include "stderr_hilog_budget.h"
static unsigned mirrored, errors;
static void test_mirror(const char* line) {
    ++mirrored;
    if (strstr(line, "err:late-fatal")) ++errors;
}
#define OH_LOG_INFO(domain, fmt, line) test_mirror(line)
#define OH_LOG_WARN(...) ((void)0)
static int test_clock(clockid_t, timespec* value) { value->tv_sec=100; value->tv_nsec=0; return 0; }
#define clock_gettime test_clock
__READER__
#undef clock_gettime
static void run(bool full) {
    if(full) setenv("WINEHUA_STDERR_HILOG", "full", 1);
    else unsetenv("WINEHUA_STDERR_HILOG");
    mirrored=errors=0;
    int pipes[2]; assert(pipe(pipes)==0);
    char filename[]="/tmp/vp-stderr-test-XXXXXX";
    int file=mkstemp(filename); assert(file>=0);
    std::thread drain([&]{ stderr_reader_thread(new stderr_ctx{pipes[0],file}); });
    std::string payload;
    for(int i=0;i<20000;++i) payload += "[SMC] normal trace flood\n";
    payload += "err:late-fatal must still reach system log\n";
    size_t offset=0;
    while(offset<payload.size()) {
        const ssize_t n=write(pipes[1],payload.data()+offset,payload.size()-offset);
        if(n<0&&errno==EINTR)continue;
        assert(n>0); offset+=n;
    }
    close(pipes[1]); drain.join();
    int saved=open(filename,O_RDONLY); assert(saved>=0);
    std::string actual; char buf[4096]; ssize_t n;
    while((n=read(saved,buf,sizeof(buf)))>0) actual.append(buf,n);
    close(saved); unlink(filename);
    assert(actual==payload); // The raw diagnostic file must never lose bytes.
    assert(errors==1); // Normal traces must not spend the error allowance.
    if(full) assert(mirrored>=20001);
    else assert(mirrored==33);
}
int main() {
    run(false); run(true);
    winehua::StderrHilogBudget budget;
    budget.BeginSecond(0);
    for(int i=0;i<100000;++i) budget.Allow("[SMC] repeat");
    assert(budget.BeginSecond(0)==0);
    assert(budget.BeginSecond(1)==100000-32);
    assert(budget.Allow("new interval"));
    for(int i=0;i<8;++i)assert(budget.Allow("Unhandled exception"));
    assert(!budget.Allow("Unhandled exception"));
    puts("PASS production stderr pipe: full file, bounded mirror, late error, rollover, explicit full mode");
}
'''.replace('__READER__', reader)
with tempfile.TemporaryDirectory(prefix='vpp-stderr-test-') as temp:
    source = Path(temp) / 'test.cpp'
    binary = Path(temp) / 'test'
    source.write_text(fixture)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O2', '-pthread',
                    '-I', str(ROOT / 'entry/src/main/cpp/proc'), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=15)
