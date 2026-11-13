#!/usr/bin/env python3
"""Exercise the actual Legacy DXGI instance cache, without a GPU or Wine."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <atomic>
#include <cassert>
#include <stdexcept>
#include <thread>
#include <vector>
#include "util/rc/util_rc.h"
#include "util/util_shared_instance.h"
struct Instance : dxvk::RcObject {
  static std::atomic<int> alive, created;
  static bool fail;
  Instance() { if (fail) throw std::runtime_error("creation"); ++alive; ++created; }
  ~Instance() { --alive; }
};
std::atomic<int> Instance::alive{0}, Instance::created{0};
bool Instance::fail = false;
int main() {
  dxvk::SharedInstance<Instance> cache;
  Instance::fail = true;
  try { cache.acquire(); assert(false); } catch (const std::runtime_error&) {}
  assert(Instance::alive == 0);
  Instance::fail = false;
  auto primary = cache.acquire();
  std::vector<std::thread> workers;
  for (int t=0; t<8; ++t) workers.emplace_back([&] {
    for (int n=0; n<2000; ++n) {
      auto query = cache.acquire();
      assert(query->ptr() == primary->ptr());
    }
  });
  for (auto& worker : workers) worker.join();
  assert(Instance::created == 1 && Instance::alive == 1);
  dxvk::Rc<Instance> device = *primary;
  primary.reset();
  assert(Instance::alive == 1); // A device may outlive its last factory.
  auto restarted = cache.acquire();
  assert(restarted->ptr() != device.ptr());
  assert(Instance::created == 2 && Instance::alive == 2);
  restarted.reset();
  device = nullptr;
  assert(Instance::alive == 0);
  for (int n=0; n<50; ++n) { auto lease = cache.acquire(); }
  assert(Instance::created == 52 && Instance::alive == 0);
}
'''

with tempfile.TemporaryDirectory(prefix='dxgi-shared-test-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(SOURCE)
    subprocess.run(['c++', '-std=c++17', '-pthread', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I' + str(ROOT / 'thirdparty/dxvk/src'), str(path / 'test.cpp'),
                    '-o', str(path / 'test')], check=True, timeout=60)
    subprocess.run([str(path / 'test')], check=True, timeout=30)
print('PASS: concurrent reuse, last-factory release, device lifetime, exception retry, re-entry (ASan/UBSan)')
