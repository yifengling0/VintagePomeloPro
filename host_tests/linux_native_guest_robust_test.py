"""Real robust-list/owner-death contracts under a denied query syscall."""
from pathlib import Path
import os,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'entry/src/main/cpp/linux_steam/gnu_fixture'
FIXTURE=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <linux/filter.h>
#include <linux/futex.h>
#include <linux/seccomp.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
static long (*kernel)(long,...);
static pthread_mutex_t owner;
static void deny_query(void) {
 struct sock_filter code[]={BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
 BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_get_robust_list,0,1),
 BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|ENOSYS),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
 struct sock_fprog filter={sizeof(code)/sizeof(code[0]),code};
 assert(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0));assert(!prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&filter));
}
static void* check(void* arg) {
 (void)arg;struct robust_list_head* real=NULL;size_t size=0;
 assert(!kernel(SYS_get_robust_list,0,&real,&size)&&real&&size==sizeof(*real));
 pthread_mutexattr_t a;pthread_mutex_t mutex;
 assert(!pthread_mutexattr_init(&a));assert(!pthread_mutexattr_setrobust(&a,PTHREAD_MUTEX_ROBUST));
 assert(!pthread_mutex_init(&mutex,&a));pthread_mutexattr_destroy(&a);
 assert(!pthread_mutex_lock(&mutex));
 struct robust_list* before=real->list.next;
 deny_query();
 for(int i=0;i<32;i++) {
  void *found=NULL;size_t length=0;
  assert(!syscall(SYS_get_robust_list,i&1?0:syscall(SYS_gettid),&found,&length));
  assert(found==real&&length==sizeof(*real)&&real->list.next==before);
 }
 void* found=NULL;size_t length=0;
 assert(syscall(SYS_get_robust_list,0,(void*)1,&length)==-1&&errno==EFAULT);
 assert(syscall(SYS_get_robust_list,0,&found,(void*)1)==-1&&errno==EFAULT);
 assert(syscall(SYS_get_robust_list,2147483647,&found,&length)==-1&&errno==ENOSYS);
 assert(!pthread_mutex_unlock(&mutex));assert(!pthread_mutex_destroy(&mutex));
 // Explicit registrations remain exact, including validation failure and clear.
 struct robust_list_head custom={.futex_offset=0};custom.list.next=&custom.list;
 assert(syscall(SYS_set_robust_list,&custom,1)==-1&&errno==EINVAL);
 assert(!syscall(SYS_set_robust_list,&custom,sizeof(custom)));
 assert(!syscall(SYS_get_robust_list,0,&found,&length)&&found==&custom);
 assert(!syscall(SYS_set_robust_list,real,sizeof(*real)));
 assert(!syscall(SYS_get_robust_list,0,&found,&length)&&found==real);
 return NULL;
}
static void* leave_owner(void* p) {
 (void)p;void *real=NULL,*found=NULL;size_t size=0,length=0;
 assert(!kernel(SYS_get_robust_list,0,&real,&size));
 assert(!pthread_mutex_lock(&owner));deny_query();
 assert(!syscall(SYS_get_robust_list,0,&found,&length)&&found==real&&length==size);
 return NULL;
}
int main(void) {
 void* libc=dlopen("libc.so.6",RTLD_NOW);assert(libc);kernel=dlsym(libc,"syscall");assert(kernel);
 pthread_t workers[4];for(int i=0;i<4;i++)assert(!pthread_create(&workers[i],NULL,check,NULL));
 for(int i=0;i<4;i++)assert(!pthread_join(workers[i],NULL));
 pthread_mutexattr_t a;assert(!pthread_mutexattr_init(&a));
 assert(!pthread_mutexattr_setrobust(&a,PTHREAD_MUTEX_ROBUST));assert(!pthread_mutex_init(&owner,&a));
 pthread_mutexattr_destroy(&a);pthread_t t;assert(!pthread_create(&t,NULL,leave_owner,NULL));
 assert(!pthread_join(t,NULL));assert(pthread_mutex_lock(&owner)==EOWNERDEAD);
 assert(!pthread_mutex_consistent(&owner));assert(!pthread_mutex_unlock(&owner));pthread_mutex_destroy(&owner);
 assert(syscall(SYS_getpid)==getpid());assert(syscall(SYS_close,-1)==-1&&errno==EBADF);
 // mmap's sixth argument must still reach libc unchanged.
 FILE* file=tmpfile();assert(file);unsigned char bytes[8192]={0};bytes[4096]=0x5a;
 assert(fwrite(bytes,1,sizeof(bytes),file)==sizeof(bytes)&&!fflush(file));
 void* memory=(void*)syscall(SYS_mmap,0,4096,PROT_READ,MAP_PRIVATE,fileno(file),4096);
 assert(memory!=MAP_FAILED&&*(unsigned char*)memory==0x5a);munmap(memory,4096);fclose(file);
 check(NULL);puts("REAL_GUEST_ROBUST_QUERY_CONTRACT_PASS");return 0;
}
'''
class GuestRobust(unittest.TestCase):
 def test_query_matches_kernel_and_preserves_owner_death_and_syscall_abi(self):
  with tempfile.TemporaryDirectory(prefix='vpp-guest-robust-') as path:
   t=Path(path);lib=t/'guest.so';source=t/'probe.c';exe=t/'probe';source.write_text(FIXTURE)
   subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fPIC','-shared',str(SOURCE/'native_guest_robust.c'),str(SOURCE/'native_guest_syscall.S'),'-ldl','-pthread','-o',str(lib)],check=True)
   subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(source),'-ldl','-pthread','-o',str(exe)],check=True)
   result=subprocess.run([str(exe)],env={**os.environ,'LD_PRELOAD':str(lib)},capture_output=True,text=True,timeout=15)
   self.assertEqual(result.returncode,0,result.stdout+result.stderr)
   self.assertIn('REAL_GUEST_ROBUST_QUERY_CONTRACT_PASS',result.stdout)
if __name__=='__main__':unittest.main()
