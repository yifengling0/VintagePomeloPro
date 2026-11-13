"""Execute the actual diagnostic reporter and WOW64 exit hook with host stubs."""
from pathlib import Path
import re
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
patch = (repo / 'scripts/patches/fex-wow64-exact-store-diagnostics.patch').read_text()
added = '\n'.join(line[1:] for line in patch.splitlines() if line.startswith('+') and not line.startswith('+++'))
assert 'ExactStoreStatsReporterInstance' not in patch
assert 'std::fprintf' not in patch and '#include <cstdio>' not in patch
assert 'WindowsExactFloatStoreEnabled && WindowsExactFloatStoreStatsEnabled' in added
start = added.index('static void ReportExactStoreStats(bool Final) {')
end = added.index('\n#endif', start)
reporter = added[start:end]
start = added.index('void BTCpuProcessTerm(')
end = added.index('\n}', start) + 2
hook = added[start:end]
post_image = '\n'.join(line[1:] for line in patch.splitlines()
    if line.startswith(('+', ' ')) and not line.startswith('+++'))
syscall_windows = re.findall(r'Context::UnlockJITContext\(TLS\);.*?Context::LockJITContext\(TLS\);',
    post_image, re.DOTALL)
assert len(syscall_windows) == 2
assert all('ReportExactStoreStats(false);' in window for window in syscall_windows)
unix_window = syscall_windows[0]
source = '''#include <atomic>
#include <chrono>
#include <cstdint>
#include <cassert>
#include <string>
#include <vector>
#include <sstream>
using HANDLE = void*; using BOOL = int; using ULONG = unsigned long;
uint64_t FEXExactStoreCounters[6] {};
std::atomic<bool> FEXExactStoreStatsEverEnabled {};
std::vector<std::string> Outputs;
bool JITLocked = true;
unsigned GetCurrentProcessId() { return 7; }
void __wine_dbg_output(const char* text) { assert(!JITLocked); Outputs.emplace_back(text); }
namespace Context {
void UnlockJITContext(int) { assert(JITLocked); JITLocked = false; }
void LockJITContext(int) { assert(!JITLocked); JITLocked = true; }
}
unsigned WineUnixCall(unsigned, unsigned, void*) { assert(!JITLocked); return 42; }
void* ULongToPtr(unsigned value) { return reinterpret_cast<void*>(static_cast<uintptr_t>(value)); }
namespace fextl::fmt {
template<class... T> std::string format(const char*, T... args) {
  std::ostringstream out; ((out << args << ' '), ...); return out.str();
}
}
''' + reporter + '\n' + hook + '''
uint64_t SimulatedUnixCall() {
  int TLS = 0;
  struct { unsigned Handle = 1, ID = 2, Args = 3; } Values;
  auto StackArgs = &Values;
  uint64_t ReturnRAX = 0;
''' + unix_window + '''
  return ReturnRAX;
}
int main() {
  BTCpuProcessTerm(nullptr, false, 0);
  assert(Outputs.empty());
  FEXExactStoreStatsEverEnabled = true;
  FEXExactStoreCounters[0] = 100; FEXExactStoreCounters[1] = 90; FEXExactStoreCounters[2] = 10;
  FEXExactStoreCounters[3] = 50; FEXExactStoreCounters[4] = 25; FEXExactStoreCounters[5] = 25;
  // Execute the actual production unlock/call/report/relock span. Ordinary
  // guest syscalls never need to exit ExecuteThread to produce live output.
  assert(SimulatedUnixCall() == 42 && JITLocked);
  assert(Outputs.size() == 1 && Outputs[0] == "7 0 1 100 90 10 50 25 25 ");
  for (int i = 0; i < 1000; ++i) ReportExactStoreStats(false);
  assert(Outputs.size() == 1);
  BTCpuProcessTerm(reinterpret_cast<void*>(1), false, 0);
  BTCpuProcessTerm(nullptr, true, 0);
  assert(Outputs.size() == 1);
  // An in-flight update is explicitly marked inconsistent, without spinning.
  ++FEXExactStoreCounters[0];
  JITLocked = false;
  BTCpuProcessTerm(nullptr, false, 0);
  assert(Outputs.size() == 2 && Outputs[1] == "7 1 0 101 90 10 50 25 25 ");
  BTCpuProcessTerm(nullptr, false, 0);
  assert(Outputs.size() == 2);
}
'''
with tempfile.TemporaryDirectory(prefix='fex-stats-hook-') as tmp:
    cpp = Path(tmp) / 'reporter.cpp'
    cpp.write_text(source)
    exe = Path(tmp) / 'test'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-parameter', '-DFEX_WOW64_EXACT_STORE_DIAGNOSTICS=1',
        str(cpp), '-o', str(exe)], check=True, timeout=30)
    subprocess.run([str(exe)], check=True, timeout=10)
print('PASS: unlocked syscall output, rate limit, before-self exit, idempotence, and inconsistent snapshot')
