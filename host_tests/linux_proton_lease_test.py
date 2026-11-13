#!/usr/bin/env python3
import subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class Lease(unittest.TestCase):
 def test_kernel_ownership_and_process_death(self):
  with tempfile.TemporaryDirectory(prefix='vpp-prefix-lease-') as temp:
   root=Path(temp);code=root/'test.cpp';code.write_text(r"""
#include "runtime_lease.h"
#include <cassert>
#include <sys/wait.h>
int main(int n,char**v){assert(n==2);std::string root=v[1];
 {steamohos::RuntimeLease first;assert(first.Acquire(root));
  steamohos::RuntimeLease competing;assert(!competing.Acquire(root));
  int status;pid_t child=fork();assert(child>=0);
  if(!child){steamohos::RuntimeLease remote;_exit(remote.Acquire(root)?1:0);}
  assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));}
 // Duplicate/SCM_RIGHTS-style handoff keeps the same OFD locked after
 // the preparing owner goes away. A separate open must remain excluded.
 int handoff=-1;
 {steamohos::RuntimeLease preparing;assert(preparing.Acquire(root));handoff=dup(preparing.Descriptor());}
 {steamohos::RuntimeLease worker;assert(worker.Adopt(handoff,root));close(handoff);
  steamohos::RuntimeLease competitor;assert(!competitor.Acquire(root));}
 {steamohos::RuntimeLease after;assert(after.Acquire(root));}
 int pipefd[2];assert(!pipe(pipefd));pid_t child=fork();assert(child>=0);
 if(!child){close(pipefd[0]);steamohos::RuntimeLease abandoned;assert(abandoned.Acquire(root));char b=1;assert(write(pipefd[1],&b,1)==1);for(;;)pause();}
 close(pipefd[1]);char b;assert(read(pipefd[0],&b,1)==1);close(pipefd[0]);assert(!kill(child,SIGKILL));int status;assert(waitpid(child,&status,0)==child);
 {steamohos::RuntimeLease recovered;assert(recovered.Acquire(root));}
 assert(!unlink((root+"/.proton-maintenance.lock").c_str()));assert(!symlink("/dev/null",(root+"/.proton-maintenance.lock").c_str()));
 steamohos::RuntimeLease unsafe;assert(!unsafe.Acquire(root));return 0;
}
""")
   subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'entry/src/main/cpp/linux_steam'),str(code),'-o',str(root/'test')],check=True)
   subprocess.run([root/'test',root],check=True,timeout=10)
if __name__=='__main__':unittest.main()
