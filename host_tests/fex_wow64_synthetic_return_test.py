"""Run the real WOW64 syscall handler and shadow-return helper at API boundaries.

This is a host regression, not execution or performance measurement of ARM JIT
code. CALL/RET boundary operations model BranchOps.cpp's documented pair layout.
The native calls, context replacement, and JIT locks are controlled test seams.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PIN = '86ff33bbe2'
MODULE = 'Source/Windows/WOW64/Module.cpp'
HEADER = 'Source/Windows/Common/CallRetStack.h'
PATCH = ROOT / 'scripts/patches/fex-wow64-synthetic-return.patch'


def pinned(path):
    return subprocess.check_output(['git', '-C', str(ROOT / 'thirdparty/fex'),
                                    'show', f'{PIN}:{path}'], text=True)


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


STUBS = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
namespace FEXCore {
namespace X86State { enum { REG_RAX, REG_RSP }; }
namespace HLE { struct SyscallArguments {}; }
namespace Core {
struct InternalThreadState;
struct CpuStateFrame {
  struct { uint64_t gregs[2]{}, rip{}, callret_sp{}; } State;
  InternalThreadState* Thread;
};
struct InternalThreadState {
  static constexpr uint64_t CALLRET_STACK_SIZE = 0x400000;
  void* CallRetStackBase;
  CpuStateFrame* CurrentFrame;
};
}
}
using UINT32 = uint32_t;
using ULONG32 = uint32_t;
using UINT = unsigned;
using unixlib_handle_t = uint64_t;
static void* ULongToPtr(uint32_t p) { return reinterpret_cast<void*>(uintptr_t(p)); }
namespace BridgeInstrs { void* UnixCall = (void*)0x1234; void* Syscall = (void*)0x4321; }
static FEXCore::Core::CpuStateFrame frame;
static FEXCore::Core::InternalThreadState thread;
static uint32_t* guest;
static int unlocks, locks, native_calls, mode;
static uint64_t initial_sp;
static int GetTLS() { return 0; }
namespace Context {
static void UnlockJITContext(int) { ++unlocks; }
static void LockJITContext(int) { ++locks; }
}
static void Wow64ProcessPendingCrossProcessItems() {}
static uint64_t NativeCall() {
  ++native_calls;
  if (mode == 1) { frame.State.rip = 0x789a; frame.State.callret_sp += 32; }
  if (mode == 2) { frame.State.callret_sp += 32; } // nested callback left a new prediction stack
  if (mode == 3) { // callback consumed and restored its own entries
    frame.State.callret_sp -= 16;
    auto p = reinterpret_cast<uint64_t*>(frame.State.callret_sp);
    p[0] = 0x5566; p[1] = 0xaabb;
    frame.State.callret_sp += 16;
  }
  if (mode == 4) { // code-cache invalidation cleared the prediction entry
    reinterpret_cast<uint64_t*>(frame.State.callret_sp)[0] = 0;
  }
  return 0x2468;
}
static uint64_t WineUnixCall(uint64_t handle, unsigned id, void* args) {
  assert(handle == 0x1020304050607080 && id == 7 && args == (void*)0x3456);
  return NativeCall();
}
static uint64_t Wow64SystemServiceEx(unsigned id, unsigned* args) {
  assert(id == 9 && args == guest + 2); return NativeCall();
}
'''

MAIN = r'''
static void push(uint64_t rip) {
  frame.State.callret_sp -= 16;
  auto p = reinterpret_cast<uint64_t*>(frame.State.callret_sp);
  p[0] = rip; p[1] = 0xbeef;
}
static bool pop(uint64_t rip) {
  auto p = reinterpret_cast<uint64_t*>(frame.State.callret_sp);
  bool match = p[0] == rip;
  frame.State.callret_sp += 16;
  return match;
}
static void entry(bool unixcall = true) {
  guest[0] = 0x1122;
  uint64_t handle = 0x1020304050607080;
  memcpy(guest + 1, &handle, sizeof(handle));
  guest[3] = 7; guest[4] = 0x3456;
  frame.State.rip = uint64_t(unixcall ? BridgeInstrs::UnixCall : BridgeInstrs::Syscall);
  frame.State.gregs[FEXCore::X86State::REG_RSP] = uintptr_t(guest);
  frame.State.gregs[FEXCore::X86State::REG_RAX] = 9;
}
int main(int argc, char** argv) {
  assert(argc == 2);
  auto page = size_t(sysconf(_SC_PAGESIZE));
  auto size = thread.CALLRET_STACK_SIZE;
  auto base = mmap(nullptr, size + 2 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(base != MAP_FAILED);
  thread.CallRetStackBase = static_cast<char*>(base) + page;
  assert(!mprotect(thread.CallRetStackBase, size, PROT_READ | PROT_WRITE));
  thread.CurrentFrame = &frame; frame.Thread = &thread;
  frame.State.callret_sp = initial_sp = uintptr_t(thread.CallRetStackBase) + size / 4;
  guest = static_cast<uint32_t*>(mmap(nullptr, page, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0));
  assert(guest != MAP_FAILED && uintptr_t(guest) < UINT32_MAX);
  if (!strcmp(argv[1], "old-leak")) {
    for (int i = 0; i < 60000; ++i) {
      push(0x3344); push(0x1122); entry(); OldHandleSyscallImpl(&frame, nullptr);
      assert(!pop(0x3344));
    }
    assert(initial_sp - frame.State.callret_sp == 60000 * 16);
    printf("old: 60000 synthetic returns leaked 960000 bytes; 60000 outer prediction misses\n");
  } else if (!strcmp(argv[1], "loop")) {
    for (int i = 0; i < 200000; ++i) {
      push(0x3344); push(0x1122); entry(i % 2 == 0); HandleSyscallImpl(&frame, nullptr);
      assert(frame.State.rip == 0x1122 && frame.State.gregs[0] == 0x2468);
      assert(frame.State.gregs[1] == uintptr_t(guest) + (i % 2 == 0 ? 20 : 4));
      assert(pop(0x3344) && frame.State.callret_sp == initial_sp);
    }
    assert(native_calls == 200000 && locks == native_calls && unlocks == locks);
    puts("candidate: 200000 UnixCall/syscall returns balanced; outer predictions preserved");
  } else if (!strcmp(argv[1], "context")) {
    push(0x1122); entry(); mode = 1; auto sp = frame.State.callret_sp;
    HandleSyscallImpl(&frame, nullptr);
    assert(frame.State.rip == 0x789a && frame.State.callret_sp == sp + 32);
    assert(frame.State.gregs[0] == 9 && frame.State.gregs[1] == uintptr_t(guest));
  } else if (!strcmp(argv[1], "moved")) {
    push(0x1122); entry(); mode = 2; auto sp = frame.State.callret_sp;
    HandleSyscallImpl(&frame, nullptr);
    assert(frame.State.rip == 0x1122 && frame.State.callret_sp == sp + 32);
  } else if (!strcmp(argv[1], "callback")) {
    push(0x3344); push(0x1122); entry(); mode = 3; HandleSyscallImpl(&frame, nullptr);
    assert(pop(0x3344) && frame.State.callret_sp == initial_sp);
  } else if (!strcmp(argv[1], "invalidate")) {
    push(0x1122); entry(); mode = 4; auto sp = frame.State.callret_sp;
    HandleSyscallImpl(&frame, nullptr); assert(frame.State.callret_sp == sp);
  } else if (!strcmp(argv[1], "mismatch")) {
    push(0x9999); entry(); auto sp = frame.State.callret_sp;
    HandleSyscallImpl(&frame, nullptr); assert(frame.State.callret_sp == sp);
  } else if (!strcmp(argv[1], "bounds")) {
    auto lower = uintptr_t(thread.CallRetStackBase);
    for (auto sp : {lower - 16, lower + size, lower + 1, lower + size - 8, UINT64_MAX}) {
      frame.State.callret_sp = sp;
      assert(!FEX::Windows::CallRetStack::CompleteSyntheticReturn(&thread, sp, 0x1122));
      assert(frame.State.callret_sp == sp);
    }
    frame.State.callret_sp = lower + size - 16;
    reinterpret_cast<uint64_t*>(frame.State.callret_sp)[0] = 0x1122;
    assert(FEX::Windows::CallRetStack::CompleteSyntheticReturn(&thread, frame.State.callret_sp, 0x1122));
    assert(frame.State.callret_sp == lower + size);
  } else if (!strcmp(argv[1], "empty")) {
    assert(!FEX::Windows::CallRetStack::CompleteSyntheticReturn(&thread, initial_sp, 0x1122));
    assert(!FEX::Windows::CallRetStack::CompleteSyntheticReturn(&thread, initial_sp, 0));
    assert(frame.State.callret_sp == initial_sp);
  } else { assert(false); }
  munmap(guest, page); munmap(base, size + 2 * page);
}
'''


class SyntheticReturn(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='fex-synthetic-return-')
        cls.stage = Path(cls.temp.name)
        for path in (MODULE, HEADER):
            target = cls.stage / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(pinned(path))
        cls.old = function(pinned(MODULE), 'static uint64_t HandleSyscallImpl(')
        subprocess.run(['patch', '-p1', '--batch', '-i', str(PATCH)], cwd=cls.stage, check=True, capture_output=True)
        cls.after = {p: (cls.stage / p).read_text() for p in (MODULE, HEADER)}
        helper = function(cls.after[HEADER], 'bool CompleteSyntheticReturn(')
        new = function(cls.after[MODULE], 'static uint64_t HandleSyscallImpl(')
        code = STUBS + '\nnamespace FEX::Windows::CallRetStack {\n' + helper + '\n}\n'
        code += cls.old.replace('HandleSyscallImpl(', 'OldHandleSyscallImpl(') + '\n' + new + '\n' + MAIN
        code = '#include <initializer_list>\n' + code
        cpp = cls.stage / 'regression.cpp'
        cpp.write_text(code)
        cls.exe = cls.stage / 'regression'
        sanitizers = os.environ.get('FEX_TEST_SANITIZERS', '')
        extra = ['-fno-omit-frame-pointer', '-fsanitize=' + sanitizers] if sanitizers else []
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O2', '-Wall', '-Wextra', *extra,
                        str(cpp), '-o', str(cls.exe)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_handler_and_return_boundaries(self):
        for case in ('old-leak', 'loop', 'context', 'moved', 'callback', 'invalidate', 'mismatch', 'bounds', 'empty'):
            with self.subTest(case=case):
                result = subprocess.run([str(self.exe), case], check=True, capture_output=True, text=True, timeout=10)
                print(result.stdout, end='')

    def test_overlay_replay_and_layout(self):
        # Replay twice. The helper must not modify the JIT or target a cached host PC.
        for _ in range(2):
            subprocess.run(['patch', '-p1', '-R', '--batch', '-i', str(PATCH)], cwd=self.stage, check=True, capture_output=True)
            for p in (MODULE, HEADER):
                self.assertEqual((self.stage / p).read_text(), pinned(p))
            subprocess.run(['patch', '-p1', '--batch', '-i', str(PATCH)], cwd=self.stage, check=True, capture_output=True)
            for p in (MODULE, HEADER):
                self.assertEqual((self.stage / p).read_text(), self.after[p])
        branches = pinned('FEXCore/Source/Interface/Core/JIT/BranchOps.cpp')
        self.assertIn('REG_CALLRET_SP, -0x10)', branches)
        self.assertIn('REG_CALLRET_SP, 0x10)', branches)


if __name__ == '__main__':
    unittest.main(verbosity=2)
