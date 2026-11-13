"""Test real diagnostic accounting and pinned patch replay, not ARM JIT speed."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'scripts/patches/fex-windows-jit-cost.patch'
HEADER = 'FEXCore/include/FEXCore/Utils/JITCompileDiagnostics.h'
FILES = ['FEXCore/include/FEXCore/Debug/InternalThreadState.h',
         'FEXCore/Source/Interface/Context/Context.h',
         'FEXCore/Source/Interface/Core/Core.cpp',
         'FEXCore/Source/Interface/Core/Dispatcher/Dispatcher.cpp']

CPP = r'''
#include "FEXCore/Utils/JITCompileDiagnostics.h"
#include <cassert>
#include <vector>
using namespace FEXCore;
struct Clock {
  static inline uint64_t Value{};
  static inline unsigned Reads{};
  static uint64_t Now() { ++Reads; return Value; }
};
struct Reporter {
  static inline bool Locked{};
  static inline std::vector<JITDiagnosticWindow> Reports;
  static void Report(const JITDiagnosticWindow& w) { assert(!Locked); Reports.push_back(w); }
};
using Scope = JITDiagnosticScope<Clock, Reporter>;
struct Lock {
  Lock() { Reporter::Locked = true; }
  ~Lock() { Reporter::Locked = false; }
};
int main() {
  // Default-off scopes perform no clock reads or output.
  { Scope disabled(nullptr, JITDiagnosticPhase::Entry); Clock::Value = 5'000'000'000; }
  assert(Clock::Reads == 0 && Reporter::Reports.empty());
  JITCompileDiagnostics stats;
  Clock::Value = 0;
  {
    Scope entry(&stats, JITDiagnosticPhase::Entry);
    Lock lock;
    { Scope front(&stats, JITDiagnosticPhase::Frontend); Clock::Value += 7'000'000; }
    { Scope back(&stats, JITDiagnosticPhase::Backend); Clock::Value += 3'000'000; }
    entry.SetOutcome(JITDiagnosticOutcome::Compiled);
  }
  assert(stats.Window.Calls[0] == 1 && stats.Window.Calls[1] == 1 && stats.Window.Calls[2] == 1);
  assert(stats.Window.TotalNS[0] == 10'000'000 && stats.Window.TotalNS[1] == 7'000'000);
  assert(stats.Window.TotalNS[2] == 3'000'000 && stats.Window.Outcomes[0] == 1);
  assert(Reporter::Reports.empty() && stats.ActiveEntries == 0);
  stats.Window.CacheClears = 2;
  stats.Window.CacheRollovers = 1;
  // Nested callbacks cannot reset the outer scope's accounting window.
  Clock::Value = 900'000'000;
  {
    Scope outer(&stats, JITDiagnosticPhase::Entry);
    Lock lock;
    {
      Scope nested(&stats, JITDiagnosticPhase::Entry);
      nested.SetOutcome(JITDiagnosticOutcome::CacheHit);
      Clock::Value = 1'100'000'000;
    }
    assert(Reporter::Reports.empty());
    outer.SetOutcome(JITDiagnosticOutcome::Raced);
    Clock::Value = 1'200'000'000;
  }
  assert(Reporter::Reports.size() == 1);
  const auto& w = Reporter::Reports.back();
  assert(w.BeginNS == 0 && w.EndNS == 1'200'000'000 && w.Calls[0] == 3);
  assert(w.Outcomes[0] == 1 && w.Outcomes[1] == 1 && w.Outcomes[2] == 1);
  assert(w.CacheClears == 2 && w.CacheRollovers == 1 && w.MaxNS[0] == 300'000'000);
  // Entry times are inclusive and can overlap: they are not CPU utilization.
  assert(w.TotalNS[0] == 510'000'000);
  assert(stats.Window.Calls[0] == 0 && stats.Window.CacheClears == 0 && stats.ActiveEntries == 0);
  Clock::Value = 1'300'000'000;
  { Scope failed(&stats, JITDiagnosticPhase::Entry); Clock::Value += 10; }
  { Scope single(&stats, JITDiagnosticPhase::Entry); single.SetOutcome(JITDiagnosticOutcome::SingleStep); }
  assert(stats.Window.Outcomes[3] == 1 && stats.Window.Outcomes[4] == 1);
  assert(Reporter::Reports.size() == 1);
  // Clock reversal is guarded and does not cause a spurious large duration.
  { Scope entry(&stats, JITDiagnosticPhase::Entry); Clock::Value = 1; }
  assert(Reporter::Reports.size() == 1 && stats.Window.MaxNS[0] == 10);
  JITCompileDiagnostics other;
  assert(other.Window.Calls[0] == 0 && !other.Begun);
}
'''

class JITCostTests(unittest.TestCase):
    def test_real_accounting_and_replay(self):
        with tempfile.TemporaryDirectory() as folder:
            work = Path(folder)
            originals = {}
            for name in FILES:
                value = subprocess.check_output(['git', '-C', str(ROOT / 'thirdparty/fex'),
                    'show', '86ff33bbe2:' + name])
                path = work / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(value)
                originals[name] = value
            subprocess.run(['patch', '-p1', '-i', str(PATCH)], cwd=work, check=True, capture_output=True)
            changed = {name: (work / name).read_bytes() for name in FILES + [HEADER]}
            for _ in range(2):
                subprocess.run(['patch', '-p1', '-R', '-i', str(PATCH)], cwd=work, check=True, capture_output=True)
                for name, value in originals.items():
                    self.assertEqual((work / name).read_bytes(), value)
                self.assertFalse((work / HEADER).exists())
                subprocess.run(['patch', '-p1', '-i', str(PATCH)], cwd=work, check=True, capture_output=True)
                for name, value in changed.items():
                    self.assertEqual((work / name).read_bytes(), value)
            (work / 'test.cpp').write_text(CPP)
            subprocess.run(['g++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                '-I' + str(work / 'FEXCore/include'), str(work / 'test.cpp'), '-o', str(work / 'test')],
                check=True, capture_output=True)
            subprocess.run([str(work / 'test')], check=True)

if __name__ == '__main__':
    unittest.main()
