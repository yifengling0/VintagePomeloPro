"""Kernel clock data must survive userspace exec; anonymous payloads must not."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class KernelMaps(unittest.TestCase):
 def test_ohos_and_linux_kernel_mappings(self):
  source=r'''#include "native_kernel_maps.h"
#include <cassert>
int main() {
 using steamohos::NativeKernelMaps;
 NativeKernelMaps ohos(0x5000,0x1000);
 assert(ohos.Keep(0x3000,0x4000,"r--p","[kshare]\n"));
 assert(ohos.Keep(0x4000,0x5000,"r--p","\n"));
 assert(ohos.Keep(0x5000,0x9000,"r-xs","[shmm]\n"));
 assert(!ohos.Keep(0x9000,0xa000,"rw-p","\n"));
 NativeKernelMaps ordinary(0x5000,0x1000);
 assert(!ordinary.Keep(0x4000,0x5000,"r--p","\n"));
 for(auto name:{"[anon:kshare]","/tmp/[kshare]","[kshare]suffix"}) {
  NativeKernelMaps invalid(0x5000,0x1000);
  assert(!invalid.Keep(0x3000,0x4000,"r--p",name));
  assert(!invalid.Keep(0x4000,0x5000,"r--p","\n"));
 }
 for(auto permission:{"rw-p","r-xp","r--s","---p"}) {
  NativeKernelMaps invalid(0x5000,0x1000);
  assert(invalid.Keep(0x3000,0x4000,"r--p","[kshare]"));
  assert(!invalid.Keep(0x4000,0x5000,permission,"\n"));
 }
 NativeKernelMaps gap(0x5000,0x1000);
 assert(gap.Keep(0x2000,0x3000,"r--p","[kshare]"));
 assert(!gap.Keep(0x4000,0x5000,"r--p","\n"));
 NativeKernelMaps big(0x5000,0x1000);
 assert(big.Keep(0x1000,0x2000,"r--p","[kshare]"));
 assert(!big.Keep(0x2000,0x5000,"r--p","\n"));
 NativeKernelMaps linux(0x5000,0x1000);
 assert(linux.Keep(0x1000,0x2000,"r--p","[vvar]"));
 assert(linux.Keep(0x2000,0x3000,"r--p","[vvar_vclock]"));
 assert(linux.Keep(0x5000,0x6000,"r-xp","[vdso]"));
 NativeKernelMaps missing(0,0x1000);
 assert(missing.Keep(0x3000,0x4000,"r--p","[kshare]"));
 assert(!missing.Keep(0x4000,0x5000,"r--p","\n"));
}
'''
  with tempfile.TemporaryDirectory(prefix='steamohos-kernel-maps-') as temporary:
   p=Path(temporary);(p/'test.cpp').write_text(source)
   subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(ROOT/'entry/src/main/cpp/linux_steam'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
if __name__=='__main__':unittest.main()
