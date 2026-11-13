"""Run production native wait and durable-prefix predicate without Harmony SDKs."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]

def function(source, signature):
    start = source.index(signature)
    body = source.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

class NativeStartupWait(unittest.TestCase):
    def test_completion_without_exit_callback_and_cancel_deadline(self):
        source = (ROOT/'entry/src/main/cpp/wine/wine_launch.cpp').read_text()
        predicate = re.search(r'WaitForLaunch\(p, (\[&\]\(\) \{.*?\}), 120000\)', source, re.S).group(1)
        harness = r"""
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>
#include "wine/wineboot_wait.h"
static std::atomic<bool> launcherExited{false}, workerRunning{false};
static std::atomic<int> workerExitCode{-1};
std::vector<WineProcessEntry> GetProcessListSnapshot() {
    WineProcessEntry entry{};entry.pid=20;entry.exeBasename="wineboot.exe";
    entry.startTimestampMs=1;entry.running=workerRunning;entry.exitCode=workerExitCode;
    return {entry};
}
bool IsLaunchChildExited(pid_t) { return launcherExited; }
struct LaunchParams { std::string prefixDir; };
static std::atomic<bool> current{true}, initialized{true};
static bool LaunchCurrent(const LaunchParams*) { return current; }
static bool IsWinePrefixInitialized(const std::string&) { return initialized; }
"""
        harness += function(source, 'static bool FileContainsExactRecord(')
        harness += function(source, 'static bool WinebootDispatchFailed(')
        harness += '\ntemplate<class Probe> ' + function(source, 'static bool WaitForLaunch(')
        harness += '\nstatic bool wait_prefix(const LaunchParams* p, const std::string& winebootStatus, int timeout) {\n'
        harness += 'const std::string dispatchStatus=winebootStatus+".dispatch"; const pid_t childPid=10;\n'
        harness += 'const winehua::WinebootAttempt attempt({}); bool dispatchFailed=false;\n'
        harness += 'const bool completed=WaitForLaunch(p, ' + predicate + ', timeout); return completed && !dispatchFailed; }\n'
        harness += r"""
static void write_record(const std::string& path, const char* record) {
    FILE* f=fopen(path.c_str(),"wb"); assert(f); fputs(record,f); assert(!fclose(f));
}
int main(int argc, char** argv) {
    assert(argc==2); LaunchParams params; const LaunchParams* p=&params; const std::string path=argv[1];
    write_record(path,"wineboot-init-request");
    assert(!wait_prefix(p,path,1)); // request/early registry never counts as complete
    write_record(path,"wineboot-init-ok-extra");assert(!wait_prefix(p,path,1));
    write_record(path,"wineboot-init-ok");initialized=false;
    assert(!wait_prefix(p,path,1)); // success token still requires the complete durable prefix
    initialized=true;assert(wait_prefix(p,path,500)); // no process-exit callback exists in this process
    write_record(path,"wineboot-init-ok\r\n");assert(wait_prefix(p,path,500));
    write_record(path,"wineboot-init-request");
    const auto start=std::chrono::steady_clock::now();
    std::thread cancel([] { usleep(10000);current=false; });
    assert(!wait_prefix(p,path,90000));cancel.join();
    assert(std::chrono::steady_clock::now()-start<std::chrono::seconds(1));
    current=true;
    write_record(path+".dispatch","wineboot-create-failed-c0000001");launcherExited=true;
    workerRunning=true; // wrapper exit must not abort an active registration worker
    assert(!wait_prefix(p,path,1));
    workerRunning=false;
    const auto failed=std::chrono::steady_clock::now();
    assert(!wait_prefix(p,path,120000));
    assert(std::chrono::steady_clock::now()-failed<std::chrono::seconds(1));
    write_record(path+".dispatch","wineboot-process-exit-00000000");
    assert(!WinebootDispatchFailed(path+".dispatch")); // exit zero does not imply durable completion
    workerExitCode=1; // a known nonzero worker exit is concrete failure even without a dispatch file
    const auto workerFailed=std::chrono::steady_clock::now();
    assert(!wait_prefix(p,path,120000));
    assert(std::chrono::steady_clock::now()-workerFailed<std::chrono::seconds(1));
    workerExitCode=-1;
    const auto deadline=std::chrono::steady_clock::now();
    assert(!WaitForLaunch(p,[]{return false;},20));
    assert(std::chrono::steady_clock::now()-deadline<std::chrono::seconds(1));
    puts("PASS production prefix completion: lost exit callback, incomplete/invalid marker, cancellation and deadline");
}
"""
        with tempfile.TemporaryDirectory(prefix='wine-startup-wait-') as tmp:
            root=Path(tmp); cpp=root/'test.cpp'; exe=root/'test'
            cpp.write_text(harness)
            subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-pthread','-I'+str(ROOT/'host_tests/process_stubs'),'-I'+str(ROOT/'entry/src/main/cpp'),str(cpp),'-o',str(exe)],check=True)
            subprocess.run([str(exe),str(root/'status')],check=True,timeout=5)

if __name__ == '__main__': unittest.main()
