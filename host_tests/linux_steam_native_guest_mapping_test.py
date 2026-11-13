"""Exercise real shared/COW mmap under an injected host noexec restriction."""
from pathlib import Path
import os, subprocess, tempfile, unittest

ROOT = Path(__file__).resolve().parents[1]

class GuestMapping(unittest.TestCase):
 def test_guest_files_keep_mapping_semantics(self):
  with tempfile.TemporaryDirectory(prefix='steamohos-guest-map-',dir='/var/tmp') as directory:
   base=Path(directory);fixture=ROOT/'entry/src/main/cpp/linux_steam/gnu_fixture'
   deny=base/'deny.c';deny.write_text(r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <sys/mman.h>
void *mmap(void *a,size_t n,int p,int f,int fd,off_t o) {
 if((p&PROT_EXEC)&&fd>=0){errno=EACCES;return MAP_FAILED;}
 void *(*next)(void*,size_t,int,int,int,off_t)=dlsym(RTLD_NEXT,"mmap");return next(a,n,p,f,fd,o);
}
int mprotect(void *a,size_t n,int p) {
 if(p&PROT_EXEC){errno=EACCES;return -1;}
 int (*next)(void*,size_t,int)=dlsym(RTLD_NEXT,"mprotect");return next(a,n,p);
}
''')
   source=base/'probe.c';source.write_text(r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
 char path[]="/var/tmp/steamohos-guest-XXXXXX";int fd=mkstemp(path);assert(fd>=0);unlink(path);
 long page=sysconf(_SC_PAGESIZE);assert(page>0);assert(!ftruncate(fd,8*page));
 unsigned char elf[20]={0x7f,'E','L','F',2,1};elf[18]=62;assert(pwrite(fd,elf,20,0)==20);
 assert(lseek(fd,7,SEEK_SET)==7);
 void *shared=mmap(NULL,3*page,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_SHARED,fd,0);assert(shared!=MAP_FAILED);
 assert(lseek(fd,0,SEEK_CUR)==7);((char*)shared)[page+123]='S';assert(!msync(shared,3*page,MS_SYNC));
 char c=0;assert(pread(fd,&c,1,page+123)==1&&c=='S');
 assert(!mprotect(shared,page,PROT_READ|PROT_EXEC));
 pid_t child=fork();assert(child>=0);if(!child){assert(!mprotect(shared,page,PROT_READ|PROT_EXEC));_exit(0);}
 int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
 void *private=mmap(NULL,page,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE,fd,page);assert(private!=MAP_FAILED);
 ((char*)private)[123]='P';assert(pread(fd,&c,1,page+123)==1&&c=='S');
 void *moved=mremap(private,page,2*page,MREMAP_MAYMOVE);assert(moved!=MAP_FAILED);
 assert(!mprotect(moved,2*page,PROT_READ|PROT_EXEC));assert(!munmap(moved,2*page));
 // Fixed collisions, partial overwrite and holes must not inherit guest rights.
 assert(mmap(shared,page,PROT_READ|PROT_EXEC,MAP_SHARED|MAP_FIXED_NOREPLACE,fd,0)==MAP_FAILED&&errno==EEXIST);
 assert(mmap((char*)shared+page,page,PROT_READ|PROT_WRITE,MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==(char*)shared+page);
 assert(mprotect(shared,3*page,PROT_READ|PROT_EXEC)==-1&&errno==EACCES);
 assert(!mprotect(shared,page,PROT_READ|PROT_EXEC));
 assert(!mprotect((char*)shared+2*page,page,PROT_READ|PROT_EXEC));assert(!munmap(shared,3*page));
 // ARM64 host ELF, arbitrary data, JIT and inactive sessions retain failures.
 elf[18]=183;assert(pwrite(fd,elf,20,0)==20);
 assert(mmap(NULL,page,PROT_READ|PROT_EXEC,MAP_PRIVATE,fd,0)==MAP_FAILED&&errno==EACCES);
 elf[0]='X';assert(pwrite(fd,elf,20,0)==20);
 assert(mmap(NULL,page,PROT_READ|PROT_EXEC,MAP_PRIVATE,fd,0)==MAP_FAILED&&errno==EACCES);
 void *jit=mmap(NULL,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(jit!=MAP_FAILED);
 assert(mprotect(jit,page,PROT_READ|PROT_EXEC)==-1&&errno==EACCES);assert(!munmap(jit,page));
 elf[0]=0x7f;elf[18]=62;assert(pwrite(fd,elf,20,0)==20);unsetenv("STEAMOHOS_NATIVE_FEX");
 assert(mmap(NULL,page,PROT_READ|PROT_EXEC,MAP_PRIVATE,fd,0)==MAP_FAILED&&errno==EACCES);close(fd);
 puts("PASS shared writeback, COW, offset, fixed collisions, protection, remap, overwrite, fork and host/JIT isolation; no payload copy");
}
''')
   flags=['-O2','-Wall','-Wextra','-Werror']
   adapter=base/'adapter.so'
   subprocess.run(['cc',*flags,'-shared','-fPIC',*[str(fixture/n) for n in ['posix_compat.c','filesystem_compat.c','syscall_compat.S']],'-pthread','-ldl','-o',str(adapter)],check=True)
   subprocess.run(['cc',*flags,'-shared','-fPIC',str(deny),'-ldl','-o',str(base/'deny.so')],check=True)
   subprocess.run(['cc',*flags,str(source),'-o',str(base/'probe')],check=True)
   result=subprocess.run([str(base/'probe')],env={**os.environ,'LD_PRELOAD':f'{adapter}:{base}/deny.so','STEAMOHOS_NATIVE_FEX':'1','STEAMOHOS_NATIVE_MEMORY':'1'},capture_output=True,text=True,timeout=10)
   self.assertEqual(result.returncode,0,result.stdout+result.stderr)
   self.assertIn('original_errno=13 retry_ok=1 copy=0',result.stderr)
   print(result.stdout,end='')

if __name__=='__main__':unittest.main()
