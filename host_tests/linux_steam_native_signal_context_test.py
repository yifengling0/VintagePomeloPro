"""Signal-stack ABI boundary checks; no game or CPU-emulation claim."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class SignalContext(unittest.TestCase):
 def test_empty_context_recovery_and_conservative_boundaries(self):
  with tempfile.TemporaryDirectory(prefix='native-signal-',dir='/var/tmp') as folder:
   source=Path(folder)/'probe.cpp';binary=Path(folder)/'probe'
   source.write_text(r'''#include "native_signal_context.h"
#include "native_seccomp.h"
#include <cassert>
#include <cstring>
#include <initializer_list>
int main(){
 using steamohos::fex::UnsupportedSandboxCall;
 siginfo_t info{};ucontext_t context{};memset(&context,0x5a,sizeof(context));
 const auto originalContext=context;
 info.si_code=1;info.si_arch=0xc000003eu;info.si_syscall=SYS_name_to_handle_at;
 assert(!UnsupportedSandboxCall(nullptr,&context));
 assert(!UnsupportedSandboxCall(&info,nullptr));
 info.si_code=SI_USER;assert(!UnsupportedSandboxCall(&info,&context));
 info.si_code=1;info.si_arch=0;assert(!UnsupportedSandboxCall(&info,&context));
 info.si_arch=0xc000003eu;info.si_syscall=SYS_write;
 assert(!UnsupportedSandboxCall(&info,&context));
 assert(!memcmp(&context,&originalContext,sizeof(context)));
 for(int syscallNumber:{SYS_name_to_handle_at,SYS_open_by_handle_at,SYS_get_mempolicy,
                       SYS_set_mempolicy,SYS_mbind,SYS_migrate_pages,SYS_move_pages,SYS_get_robust_list}) {
  info.si_syscall=syscallNumber;context=originalContext;
  assert(UnsupportedSandboxCall(&info,&context));
  auto expected=originalContext;expected.uc_mcontext.gregs[REG_RAX]=-ENOSYS;
  assert(!memcmp(&context,&expected,sizeof(context)));
 }
 using steamohos::fex::RepairEmptySignalStack;
 stack_t kernel{};kernel.ss_sp=reinterpret_cast<void*>(0x10000);kernel.ss_size=0x10000;kernel.ss_flags=SS_ONSTACK;
 stack_t saved{};assert(RepairEmptySignalStack(saved,kernel,0x30000));
 assert(saved.ss_sp==kernel.ss_sp&&saved.ss_size==kernel.ss_size&&saved.ss_flags==0);
 saved={};assert(RepairEmptySignalStack(saved,kernel,0x10000)&&saved.ss_flags==SS_ONSTACK);
 saved={};assert(RepairEmptySignalStack(saved,kernel,0x1ffff)&&saved.ss_flags==SS_ONSTACK);
 saved={};assert(RepairEmptySignalStack(saved,kernel,0x20000)&&saved.ss_flags==0);
 stack_t original=saved;assert(!RepairEmptySignalStack(saved,kernel,0x11000));assert(!memcmp(&saved,&original,sizeof(saved)));
 saved={};saved.ss_flags=SS_DISABLE;original=saved;assert(!RepairEmptySignalStack(saved,kernel,0));assert(!memcmp(&saved,&original,sizeof(saved)));
 saved={};saved.ss_size=1;assert(!RepairEmptySignalStack(saved,kernel,0)&&saved.ss_size==1&&!saved.ss_sp);
 saved={};kernel.ss_flags=SS_DISABLE;assert(!RepairEmptySignalStack(saved,kernel,0));
 kernel.ss_flags=0;assert(!RepairEmptySignalStack(saved,kernel,0));
 kernel.ss_flags=static_cast<int>(1u<<31);assert(!RepairEmptySignalStack(saved,kernel,0));
 kernel.ss_flags=SS_ONSTACK;kernel.ss_sp=reinterpret_cast<void*>(UINTPTR_MAX-15);kernel.ss_size=32;
 assert(!RepairEmptySignalStack(saved,kernel,0)&&!saved.ss_sp&&!saved.ss_size&&!saved.ss_flags);
 kernel.ss_sp=nullptr;kernel.ss_size=65536;assert(!RepairEmptySignalStack(saved,kernel,0));
 kernel.ss_sp=reinterpret_cast<void*>(0x10000);kernel.ss_size=0;assert(!RepairEmptySignalStack(saved,kernel,0));
}
''')
   subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-I',str(ROOT/'entry/src/main/cpp/linux_steam/gnu_fixture'),str(source),'-o',str(binary)],check=True)
   subprocess.run([str(binary)],check=True,timeout=5)
if __name__=='__main__':unittest.main()
