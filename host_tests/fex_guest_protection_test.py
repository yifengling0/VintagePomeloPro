"""Exercise the delivered FEX adapter on a real noexec shared backing file."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class GuestProtection(unittest.TestCase):
 def test_noexec_fallback_preserves_host_policy_and_failure_contract(self):
  if not Path('/dev/shm').is_dir():self.skipTest('Linux shared-memory filesystem required')
  with tempfile.TemporaryDirectory(prefix='fex-guest-protection-',dir='/var/tmp') as folder, tempfile.TemporaryDirectory(prefix='fex-noexec-',dir='/dev/shm') as shared:
   source=Path(folder)/'probe.cpp';binary=Path(folder)/'probe'
   source.write_text(r"""
#include "fex-ohos-guest-protection.h"
#include <cassert>
#include <fcntl.h>
#include <unistd.h>
int main(int argc,char** argv) {
 assert(argc==2);int fd=open(argv[1],O_RDWR|O_CREAT|O_EXCL,0600);assert(fd>=0);
 assert(!ftruncate(fd,4096));const char marker=0x5a;assert(pwrite(fd,&marker,1,0)==1);
 void* p=mmap(nullptr,4096,PROT_NONE,MAP_SHARED,fd,0);assert(p!=MAP_FAILED);
 if(::mprotect(p,4096,PROT_READ|PROT_EXEC)==0)return 77;
 assert(errno==EACCES||errno==EPERM);
 unsetenv("STEAMOHOS_NATIVE_FEX");
 assert(FEX::Ohos::GuestMprotect(p,4096,PROT_READ|PROT_EXEC)==-1&&(errno==EACCES||errno==EPERM));
 setenv("STEAMOHOS_NATIVE_FEX","0",1);
 assert(FEX::Ohos::GuestMprotect(p,4096,PROT_READ|PROT_EXEC)==-1&&(errno==EACCES||errno==EPERM));
 setenv("STEAMOHOS_NATIVE_FEX","1",1);
 assert(!FEX::Ohos::GuestMprotect(p,4096,PROT_READ|PROT_EXEC));
 assert(*static_cast<char*>(p)==marker);
 // The native ARM64-JIT API remains subject to the original kernel policy.
 assert(::mprotect(p,4096,PROT_READ|PROT_EXEC)==-1&&(errno==EACCES||errno==EPERM));
 assert(!FEX::Ohos::GuestMprotect(p,4096,PROT_READ|PROT_WRITE));
 *static_cast<char*>(p)=0x33;char value=0;assert(pread(fd,&value,1,0)==1&&value==0x33);
 assert(FEX::Ohos::GuestMprotect(static_cast<char*>(p)+1,4096,PROT_READ|PROT_EXEC)==-1&&errno==EINVAL);
 assert(!munmap(p,4096));
 assert(FEX::Ohos::GuestMprotect(p,4096,PROT_READ|PROT_EXEC)==-1&&errno==ENOMEM);
 close(fd);fd=open(argv[1],O_RDONLY);assert(fd>=0);
 p=mmap(nullptr,4096,PROT_READ,MAP_SHARED,fd,0);assert(p!=MAP_FAILED);
 // Retrying must still reject a genuinely forbidden shared write.
 assert(FEX::Ohos::GuestMprotect(p,4096,PROT_READ|PROT_WRITE|PROT_EXEC)==-1&&(errno==EACCES||errno==EPERM));
 assert(*static_cast<char*>(p)==0x33);assert(!munmap(p,4096));close(fd);
}
""")
   subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-I',str(ROOT/'scripts/patches'),str(source),'-o',str(binary)],check=True)
   result=subprocess.run([str(binary),str(Path(shared)/'doublemapper')],capture_output=True,text=True,timeout=10)
   if result.returncode==77:self.skipTest('/dev/shm is executable on this host')
   self.assertEqual(result.returncode,0,result.stdout+result.stderr)
if __name__=='__main__':unittest.main()
