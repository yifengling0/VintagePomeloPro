"""Guest Steamworks SysV IPC uses real shared state despite denied kernel APIs."""
import os,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
SOURCE=r'''#define _GNU_SOURCE 1
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(void) {
 const long denied[]={SYS_semget,SYS_semctl,SYS_semop,SYS_semtimedop,SYS_shmget,SYS_shmat,SYS_shmdt,SYS_shmctl};
 struct sock_filter f[19];int n=0;
 f[n++]=(struct sock_filter)BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr));
 for(unsigned i=0;i<sizeof(denied)/sizeof(*denied);++i) {
  f[n++]=(struct sock_filter)BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,denied[i],0,1);
  f[n++]=(struct sock_filter)BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM);
 }
 f[n++]=(struct sock_filter)BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW);
 struct sock_fprog policy={n,f};
 assert(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0));assert(!prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&policy));
 assert(syscall(SYS_semget,0,1,0600)==-1&&errno==EPERM);
 int sem=semget(IPC_PRIVATE,1,0600);assert(sem>0);
 union {int val;} value={3};assert(!semctl(sem,0,SETVAL,value));
 int shm=shmget(IPC_PRIVATE,4096,0600);assert(shm>0);
 char* memory=shmat(shm,0,0);assert(memory!=(void*)-1);strcpy(memory,"before");
 pid_t child=fork();assert(child>=0);
 if(!child){struct sembuf op={0,-1,IPC_NOWAIT};assert(!semop(sem,&op,1));
  char* other=shmat(shm,0,0);assert(other!=(void*)-1&&!strcmp(other,"before"));
  strcpy(other,"after");assert(!shmdt(other));_exit(0);}
 int status;assert(waitpid(child,&status,0)==child&&status==0);
 assert(semctl(sem,0,GETVAL)==2&&!strcmp(memory,"after"));
 assert(!semctl(sem,0,IPC_RMID));assert(!shmctl(shm,IPC_RMID,0));assert(!shmdt(memory));
 assert(semctl(sem,0,GETVAL)==-1&&errno==EINVAL);
 return 0;
}
'''
class GuestIpc(unittest.TestCase):
 def test_kernel_sysv_denied_but_guest_protocol_is_shared_and_removed(self):
  with tempfile.TemporaryDirectory(prefix='native-guest-ipc-',dir='/var/tmp') as name:
   directory=Path(name);source=directory/'probe.c';source.write_text(SOURCE)
   library=directory/'guest.so';binary=directory/'probe'
   common=['cc','-O2','-Wall','-Wextra','-Werror']
   subprocess.run([*common,'-shared','-fPIC',str(ROOT/'entry/src/main/cpp/linux_steam/gnu_fixture/sysvipc_compat.c'),'-pthread','-o',str(library)],check=True)
   subprocess.run([*common,str(source),'-o',str(binary)],check=True)
   result=subprocess.run([str(binary)],env={**os.environ,'LD_PRELOAD':str(library),'STEAMOHOS_SYSVIPC_ROOT':str(directory/'ipc')},capture_output=True,text=True,timeout=10)
   self.assertEqual(result.returncode,0,result.stderr)
if __name__=='__main__':unittest.main()
