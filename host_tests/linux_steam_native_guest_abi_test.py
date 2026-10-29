"""Real file API consistency/isolation for the paired FEX guest interpreter."""
from pathlib import Path
import os,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class GuestAbi(unittest.TestCase):
 def test_interpreter_overlay_and_boundaries(self):
  with tempfile.TemporaryDirectory(prefix='native-guest-abi-',dir='/var/tmp') as folder:
   base=Path(folder);root=base/'root';libs=root/'lib/x86_64-linux-gnu';libs.mkdir(parents=True)
   code=base/'code';core=code/'native-guest-core-x86_64';core.mkdir(parents=True)
   (libs/'ld-2.31.so').write_text('old');(libs/'ld-linux-x86-64.so.2').symlink_to('ld-2.31.so')
   (libs/'libc.so.6').write_text('old');(core/'ld-linux-x86-64.so.2').write_text('paired')
   source=base/'probe.c';source.write_text(r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
static void content(int fd,const char *expected) {
 static int call; ++call;assert(fd>=0);char data[32]={0};ssize_t n=read(fd,data,sizeof(data)-1);
 if(n!=(ssize_t)strlen(expected)||strcmp(data,expected))fprintf(stderr,"content call=%d actual=%s expected=%s\n",call,data,expected);
 assert(n==(ssize_t)strlen(expected));
 assert(!strcmp(data,expected));close(fd);
}
int main(void) {
 const char *root=getenv("STEAMOHOS_NATIVE_FEX_ROOTFS"),*core=getenv("STEAMOHOS_NATIVE_GUEST_CORE");
 char path[4096],paired[4096];snprintf(path,sizeof(path),"%s/lib/x86_64-linux-gnu/ld-2.31.so",root);
 snprintf(paired,sizeof(paired),"%s/ld-linux-x86-64.so.2",core);
 content(open(path,O_RDONLY),"paired");
 struct stat expected,actual;assert(!stat(paired,&expected)&&!stat(path,&actual));
 assert(actual.st_dev==expected.st_dev&&actual.st_ino==expected.st_ino);
 char *canonical=realpath(path,NULL);
 if(!canonical||strcmp(canonical,paired))fprintf(stderr,"realpath actual=%s expected=%s errno=%d\n",canonical?canonical:"NULL",paired,errno);
 assert(canonical&&!strcmp(canonical,paired));free(canonical);
 int dir=open(root,O_RDONLY|O_DIRECTORY);assert(dir>=0);
 content(openat(dir,"lib/x86_64-linux-gnu/ld-2.31.so",O_RDONLY),"paired");
 content(syscall(SYS_openat,dir,"lib/x86_64-linux-gnu/ld-2.31.so",O_RDONLY,0),"paired");
 struct {unsigned long long flags,mode,resolve;} how={O_RDONLY,0,0x10};
 content(syscall(SYS_openat2,dir,"/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",&how,sizeof(how)),"paired");
 how.flags=O_PATH;int held=syscall(SYS_openat2,dir,"lib/x86_64-linux-gnu/ld-2.31.so",&how,sizeof(how));
 assert(held>=0&&!fstat(held,&actual)&&actual.st_ino==expected.st_ino);close(held);
 how.flags=O_RDONLY|O_NOFOLLOW;
 assert(syscall(SYS_openat2,dir,"lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",&how,sizeof(how))<0&&errno==ELOOP);
 assert(!fstatat(dir,"lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",&actual,AT_SYMLINK_NOFOLLOW)&&S_ISLNK(actual.st_mode));
 content(openat(dir,"lib/x86_64-linux-gnu/libc.so.6",O_RDONLY),"old");
 setenv("STEAMOHOS_NATIVE_GUEST_CORE","/unrelated",1);content(open(path,O_RDONLY),"old");
 setenv("STEAMOHOS_NATIVE_GUEST_CORE",core,1);unsetenv("STEAMOHOS_NATIVE_FEX");
 content(open(path,O_RDONLY),"old");close(dir);
 puts("PASS paired interpreter: open, dirfd, raw syscall, stat, realpath, held openat2, NOFOLLOW and inactive isolation");
}
''')
   fixture=ROOT/'entry/src/main/cpp/linux_steam/gnu_fixture';flags=['-O2','-Wall','-Wextra','-Werror']
   adapter=base/'adapter.so';probe=base/'probe'
   subprocess.run(['cc',*flags,'-shared','-fPIC',*[str(fixture/n) for n in ['posix_compat.c','filesystem_compat.c','syscall_compat.S']],'-pthread','-ldl','-o',str(adapter)],check=True)
   subprocess.run(['cc',*flags,str(source),'-o',str(probe)],check=True)
   env={**os.environ,'LD_PRELOAD':str(adapter),'STEAMOHOS_NATIVE_FEX':'1',
        'STEAMOHOS_NATIVE_FEX_ROOTFS':str(root),'STEAMOHOS_CODE_ROOT':str(code),'STEAMOHOS_NATIVE_GUEST_CORE':str(core)}
   result=subprocess.run([str(probe)],env=env,capture_output=True,text=True,timeout=10)
   self.assertEqual(result.returncode,0,result.stdout+result.stderr);print(result.stdout,end='')
if __name__=='__main__':unittest.main()
