"""Exercise the production preload with a denied empty-path kernel contract."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class EmptyPathStat(unittest.TestCase):
    def test_real_fd_identity_and_errors(self):
        with tempfile.TemporaryDirectory(prefix='steamohos-empty-stat-', dir='/var/tmp') as temp:
            base = Path(temp)
            adapter = base / 'adapter.so'
            subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra', '-Werror',
                            str(ROOT / 'entry/src/main/cpp/linux_steam/gnu_fixture/filesystem_compat.c'),
                            '-ldl', '-pthread', '-o', str(adapter)], check=True)
            # Inject the observed OHOS EFAULT below our real production adapter.
            shim = base / 'deny.c'
            shim.write_text(r'''#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
static int denied(int fd) {
 if(!getenv("DENY_FSTAT_MEMFD"))return 0;
 char path[80],target[160];snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);
 ssize_t size=readlink(path,target,sizeof(target)-1);
 if(size<0)return 0;target[size]=0;return strstr(target,"memfd:")!=NULL;
}
int fstat(int fd,struct stat *st) {
 if(denied(fd)){errno=EACCES;return -1;}return syscall(SYS_fstat,fd,st);
}
int fstat64(int fd,struct stat64 *st){return fstat(fd,(struct stat*)st);}
int fstatat(int fd,const char *p,struct stat *st,int flags) {
 if(!*p&&(flags&AT_EMPTY_PATH)&&!(flags&~(AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW|AT_NO_AUTOMOUNT))) {
  if(fcntl(fd,F_GETFD)<0)return -1;
  errno=EFAULT;return -1;
 }
 return syscall(SYS_newfstatat,fd,p,st,flags);
}
int fstatat64(int fd,const char *p,struct stat64 *st,int flags){return fstatat(fd,p,(struct stat*)st,flags);}
int __fxstatat(int v,int fd,const char *p,struct stat *st,int flags){
 if(v!=0&&v!=1){errno=EINVAL;return -1;}return fstatat(fd,p,st,flags);
}
int __fxstatat64(int v,int fd,const char *p,struct stat64 *st,int flags){return __fxstatat(v,fd,p,(struct stat*)st,flags);}
''')
            deny = base / 'deny.so'
            subprocess.run(['cc', '-shared', '-fPIC', '-O2', str(shim), '-o', str(deny)], check=True)
            source = base / 'run.c'
            source.write_text(r'''#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
extern int __fxstatat(int,int,const char*,struct stat*,int);
extern int __fxstatat64(int,int,const char*,struct stat64*,int);
int main(void) {
 char path[]="/var/tmp/steamohos-empty-fd-XXXXXX";int fd=mkstemp(path);assert(fd>=0);
 assert(!unlink(path)&&!ftruncate(fd,12345));
 struct stat actual,reference;struct stat64 large;
 assert(!fstat(fd,&reference));
 assert(fstatat(fd,"",&actual,AT_EMPTY_PATH)==-1&&errno==EFAULT);
 assert(!setenv("STEAMOHOS_NATIVE_FEX","1",1));
 for(int flags=AT_EMPTY_PATH;flags<= (AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW);flags+=AT_SYMLINK_NOFOLLOW) {
  assert(!fstatat(fd,"",&actual,flags));
  assert(actual.st_ino==reference.st_ino&&actual.st_dev==reference.st_dev&&actual.st_size==12345);
 }
 assert(!fstatat64(fd,"",&large,AT_EMPTY_PATH|AT_NO_AUTOMOUNT)&&large.st_size==12345);
 assert(!__fxstatat(1,fd,"",&actual,AT_EMPTY_PATH)&&actual.st_ino==reference.st_ino);
 assert(!__fxstatat64(1,fd,"",&large,AT_EMPTY_PATH)&&large.st_ino==reference.st_ino);
 assert(__fxstatat(99,fd,"",&actual,AT_EMPTY_PATH)==-1&&errno==EINVAL);
 int badflags=AT_EMPTY_PATH|0x80000000;
 int expected=syscall(SYS_newfstatat,fd,"",&reference,badflags),saved=errno;
 int result=fstatat(fd,"",&actual,badflags);
 assert(result==expected&&(result==0||errno==saved));
 assert(fstatat(fd,"",&actual,0)==-1&&errno==ENOENT);
 assert(fstatat(fd,"missing",&actual,AT_EMPTY_PATH)==-1&&errno==ENOTDIR);
 assert(!close(fd));assert(fstatat(fd,"",&actual,AT_EMPTY_PATH)==-1&&errno==EBADF);
 fd=memfd_create("empty-stat-contract",MFD_CLOEXEC);assert(fd>=0);
 assert(!ftruncate(fd,4567)&&!fstat(fd,&reference));
 assert(!fstatat(fd,"",&actual,AT_EMPTY_PATH)&&actual.st_size==4567&&actual.st_ino==reference.st_ino);
 assert(!setenv("DENY_FSTAT_MEMFD","1",1));
 assert(fstat(fd,&actual)==-1&&errno==EACCES);
 assert(fstatat(fd,"",&actual,AT_EMPTY_PATH)==-1&&errno==EFAULT);
 assert(fstatat64(fd,"",&large,AT_EMPTY_PATH)==-1&&errno==EFAULT);
 assert(__fxstatat(1,fd,"",&actual,AT_EMPTY_PATH)==-1&&errno==EFAULT);
 assert(__fxstatat64(1,fd,"",&large,AT_EMPTY_PATH)==-1&&errno==EFAULT);
 close(fd);return 0;
}
''')
            runner = base / 'run'
            subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(runner)], check=True)
            env = dict(os.environ, LD_PRELOAD=str(adapter) + ':' + str(deny))
            env.pop('STEAMOHOS_NATIVE_FEX', None)
            result = subprocess.run([str(runner)], env=env, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr[-3000:])


if __name__ == '__main__':
    unittest.main()
