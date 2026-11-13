"""Exercise CEF's legacy stat ABI and graphics reuse after fork."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNNER = r'''#define _GNU_SOURCE 1
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern int __fxstatat64(int,int,const char*,struct stat64*,int);
extern int __fxstatat(int,int,const char*,struct stat*,int);
#if defined(__aarch64__)
#define VER 0
#else
#define VER 1
#endif
int main(int argc,char **argv) {
 assert(argc==2);int preload=atoi(argv[1]);
 const char *code=getenv("STEAMOHOS_CODE_ROOT");char path[4096];
 for(int i=0;i<2;++i) {
  snprintf(path,sizeof(path),"%s/steamrtarm64/%s",code,i?"libEGL.so":"libGLESv2.so");
  void *module=dlopen(path,RTLD_NOW|RTLD_NOLOAD);assert((module!=NULL)==preload);
  if(module)assert(!dlclose(module));
 }
 if(preload) {
  pid_t child=fork();assert(child>=0);
  if(!child) {
   for(int i=0;i<2;++i) {
    snprintf(path,sizeof(path),"%s/steamrtarm64/%s",code,i?"libEGL.so":"libGLESv2.so");
    assert(!unlink(path));
    void *module=dlopen(path,RTLD_NOW|RTLD_LOCAL);assert(module);
    int (*marker)(void)=dlsym(module,"cef_test_marker");assert(marker&&marker()==42);
    assert(!dlclose(module));
   }
   _exit(0);
  }
  int exitStatus;assert(waitpid(child,&exitStatus,0)==child&&WIFEXITED(exitStatus)&&WEXITSTATUS(exitStatus)==0);
 }
 int fd=open("/tmp/cef-legacy-stat",O_CREAT|O_RDWR|O_EXCL,0600);assert(fd>=0);
 assert(write(fd,"old-abi",7)==7);struct stat64 legacy;struct stat status;
 assert(!__fxstatat64(VER,AT_FDCWD,"/tmp/cef-legacy-stat",&legacy,0)&&legacy.st_size==7);
 assert(!__fxstatat(VER,AT_FDCWD,"/tmp/cef-legacy-stat",&status,0)&&status.st_size==7);
 assert(__fxstatat64(-123,AT_FDCWD,"/tmp/cef-legacy-stat",&legacy,0)==-1&&errno==EINVAL);
 int directory=open("/proc/self/fd",O_RDONLY|O_DIRECTORY);assert(directory>=0);
 char number[24];snprintf(number,sizeof(number),"%d",fd);
 assert(!setenv("CEF_TEST_PROC_DENIED","1",1));
 assert(__fxstatat64(-123,directory,number,&legacy,0)==-1);
 int unrelated=open(getenv("STEAMOHOS_TMP_ROOT"),O_RDONLY|O_DIRECTORY);assert(unrelated>=0);
 assert(__fxstatat64(VER,unrelated,number,&legacy,0)==-1&&errno==ENOENT);assert(!close(unrelated));
 assert(!__fxstatat64(VER,directory,number,&legacy,0)&&S_ISREG(legacy.st_mode)&&legacy.st_size==7);
 assert(!__fxstatat(VER,directory,number,&status,0)&&S_ISREG(status.st_mode)&&status.st_size==7);
 assert(!fstatat(directory,number,&status,0)&&S_ISREG(status.st_mode));
 assert(!fstatat64(directory,number,&legacy,0)&&legacy.st_ino==status.st_ino);
 assert(__fxstatat64(VER,directory,number,&legacy,AT_SYMLINK_NOFOLLOW)==-1&&errno==ENOENT);
 assert(__fxstatat64(VER,directory,number,&legacy,0x40000000)==-1);
 assert(!unsetenv("STEAMOHOS_NATIVE_MEMORY"));
 assert(__fxstatat64(VER,directory,number,&legacy,0)==-1&&errno==ENOENT);
 assert(!setenv("STEAMOHOS_NATIVE_MEMORY","1",1));assert(!close(fd));
 assert(__fxstatat64(VER,directory,number,&legacy,0)==-1&&errno==ENOENT);
 assert(!close(directory));assert(!unlink("/tmp/cef-legacy-stat"));
 puts("PASS legacy ABI, aliases, real own-fd metadata, errors and inherited graphics");
}
'''
SHIM = r'''#define _GNU_SOURCE 1
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
static int denied(const char *p) {
 return getenv("CEF_TEST_PROC_DENIED")&&p&&*p&&strspn(p,"0123456789")==strlen(p);
}
#define AT(name,type) \
 int name(int fd,const char *p,type *b,int f) { \
  if(denied(p)){errno=ENOENT;return -1;} \
  int (*next)(int,const char*,type*,int)=dlsym(RTLD_NEXT,#name);return next(fd,p,b,f); \
 }
#define LEGACY(name,type) \
 int name(int v,int fd,const char *p,type *b,int f) { \
  if(denied(p)){errno=ENOENT;return -1;} \
  int (*next)(int,int,const char*,type*,int)=dlsym(RTLD_NEXT,#name);return next(v,fd,p,b,f); \
 }
AT(fstatat,struct stat)
AT(fstatat64,struct stat64)
LEGACY(__fxstatat,struct stat)
LEGACY(__fxstatat64,struct stat64)
'''

class CefStartup(unittest.TestCase):
 def test_legacy_abi_and_pre_fork_graphics(self):
  with tempfile.TemporaryDirectory(prefix='cef-startup-',dir='/var/tmp') as temp:
   base=Path(temp);fixture=ROOT/'entry/src/main/cpp/linux_steam/gnu_fixture'
   library=base/'compat.so';shim=base/'denied.so';runner=base/'runner'
   for name,content in [('runner.c',RUNNER),('denied.c',SHIM),('graphics.c','int cef_test_marker(void){return 42;}')]:
    (base/name).write_text(content)
   flags=['-O2','-Wall','-Wextra','-Werror']
   subprocess.run(['cc',*flags,'-shared','-fPIC',*[str(fixture/n) for n in ['posix_compat.c','filesystem_compat.c','syscall_compat.S']],'-pthread','-ldl','-o',str(library)],check=True)
   subprocess.run(['cc',*flags,'-shared','-fPIC',str(base/'denied.c'),'-ldl','-o',str(shim)],check=True)
   subprocess.run(['cc',*flags,str(base/'runner.c'),'-ldl','-o',str(runner)],check=True)
   for scenario in ['webhelper','unrelated','outside']:
    case=base/scenario;code=case/'runtime';(code/'steamrtarm64').mkdir(parents=True)
    (case/'tmp').mkdir()
    for name in ['libGLESv2.so','libEGL.so']:
     subprocess.run(['cc',*flags,'-shared','-fPIC',str(base/'graphics.c'),'-o',str(code/'steamrtarm64'/name)],check=True)
    env=os.environ.copy();env.update(LD_PRELOAD=str(library)+':'+str(shim),
      STEAMOHOS_NATIVE_MEMORY='1',STEAMOHOS_EXTERNAL_RUNTIME=str(case),
      STEAMOHOS_CODE_ROOT=str(code),STEAMOHOS_TMP_ROOT=str(case/'tmp'),
      STEAMOHOS_EXEC_PATH=str(code/'steamrtarm64'/('steam' if scenario=='unrelated' else 'steamwebhelper')))
    if scenario=='outside':env['STEAMOHOS_EXTERNAL_RUNTIME']=str(case/'other')
    result=subprocess.run([str(runner),'1' if scenario=='webhelper' else '0'],env=env,capture_output=True,text=True,timeout=10)
    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
    self.assertIn('STEAMOHOS_PROC_FSTAT_RECOVERED',result.stderr)
    self.assertEqual(result.stderr.count('STEAMOHOS_CEF_EARLY_GRAPHICS'),2 if scenario=='webhelper' else 0)
    print(scenario,result.stdout.strip())

if __name__=='__main__':unittest.main()
