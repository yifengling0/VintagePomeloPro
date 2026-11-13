"""Run the real launcher policy against healthy and denied memfd metadata."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeJitCapability(unittest.TestCase):
    def test_kernel_contract_and_user_precedence(self):
        with tempfile.TemporaryDirectory(prefix='native-jit-capability-', dir='/var/tmp') as temp:
            source = Path(temp) / 'run.cpp'
            source.write_text(r'''
#include "native_jit_capability.h"
#include <cassert>
#include <cstring>
#include <fcntl.h>
static int failure, calls, lastFd=-1;
extern "C" int __real_memfd_create(const char*,unsigned);
extern "C" int __real_fstat(int,struct stat*);
extern "C" int __wrap_memfd_create(const char *name,unsigned flags) {
 assert(!strcmp(name,"steamohos-jit-capability")&&flags==MFD_CLOEXEC);++calls;
 if(failure==ENOSYS){errno=ENOSYS;return -1;}
 lastFd=__real_memfd_create(name,flags);assert(lastFd>=0);return lastFd;
}
extern "C" int __wrap_fstat(int fd,struct stat *status) {
 if(failure){errno=failure;return -1;}return __real_fstat(fd,status);
}
int main() {
 using namespace steamohos::fex;
 unsetenv("DOTNET_EnableWriteXorExecute");unsetenv("COMPlus_EnableWriteXorExecute");
 errno=E2BIG;assert(ApplyNativeJitCapability()==NativeJitPolicy::Unchanged);
 assert(errno==E2BIG&&!getenv("DOTNET_EnableWriteXorExecute"));
 assert(fcntl(lastFd,F_GETFD)==-1&&errno==EBADF);
 for(int error:{EACCES,EFAULT}) {
  failure=error;errno=E2BIG;
  assert(ApplyNativeJitCapability()==NativeJitPolicy::MetadataDenied&&errno==E2BIG);
  assert(!strcmp(getenv("DOTNET_EnableWriteXorExecute"),"0"));
  assert(fcntl(lastFd,F_GETFD)==-1&&errno==EBADF);
  unsetenv("DOTNET_EnableWriteXorExecute");
 }
 for(int error:{EIO,ENOSYS}) {
  failure=error;errno=E2BIG;
  assert(ApplyNativeJitCapability()==NativeJitPolicy::Unchanged&&errno==E2BIG);
  assert(!getenv("DOTNET_EnableWriteXorExecute"));
 }
 for(const char *key:{"DOTNET_EnableWriteXorExecute","COMPlus_EnableWriteXorExecute"}) {
  for(const char *value:{"0","1",""}) {
   assert(!setenv(key,value,1));int before=calls;
   assert(ApplyNativeJitCapability()==NativeJitPolicy::Unchanged&&calls==before);
   assert(!strcmp(getenv(key),value));unsetenv(key);
  }
 }
}
''')
            binary = Path(temp) / 'run'
            subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-include', 'initializer_list',
                            '-I', str(ROOT / 'entry/src/main/cpp/linux_steam/gnu_fixture'),
                            str(source), '-Wl,--wrap=memfd_create,--wrap=fstat',
                            '-o', str(binary)], check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
