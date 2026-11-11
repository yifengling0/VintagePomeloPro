"""Real rtld/fork/dirfd tests for native Steam FEX routing, without CPU emulation.

The small host FEX stand-in tests launch contracts, NOT x86 execution or GPU.
It has no execute bit, starts a sibling FEXServer and issues syscall(openat).
"""
from pathlib import Path
import gzip, json, os, re, shutil, signal, socket, struct, subprocess, tempfile, threading, time, unittest
ROOT=Path(__file__).resolve().parents[1]
NATIVE=ROOT/'entry/src/main/cpp/linux_steam'
def fex_config(text):
 # Match upstream Config.cpp: scalar Config items are visited in order;
 # StringArrayType appends repeated properties. json.loads without a pairs
 # hook hides duplicate Env keys and previously accepted an unsupported array.
 def pairs(items):
  result={}
  for key,value in items:
   if key=='Env':
    if not isinstance(value,str):raise ValueError('FEX Env requires repeated scalar strings')
    result.setdefault(key,[]).append(value)
   else:result[key]=value
  return result
 return json.loads(text,object_pairs_hook=pairs)
class NativeFex(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory(prefix='steamohos-native-fex-',dir='/var/tmp')
  cls.base=Path(cls.tmp.name);cls.code=cls.base/'signed';cls.code.mkdir()
  cls.library=cls.base/'library';cls.common=cls.library/'steamapps/common';cls.common.mkdir(parents=True)
  cls.tool=cls.common/'FEX-Emu';(cls.tool/'usr/bin').mkdir(parents=True)
  for name in ['fex-compat-tool','FEXCompatTool']:(cls.tool/name).write_text('never executed Python or depot wrapper\n')
  cls.runtime=cls.common/'SteamLinuxRuntime_soldier';cls.runtime.mkdir()
  (cls.runtime/'_v2-entry-point').write_text('never executed pressure-vessel\n')
  cls.scout=cls.common/'SteamLinuxRuntime';cls.scout.mkdir()
  (cls.scout/'scout-on-soldier-entry-point-v2').write_text('never executed scout wrapper\n')
  cls.rootfs=cls.runtime/'soldier_platform_1/files'
  (cls.rootfs/'usr/lib/x86_64-linux-gnu').mkdir(parents=True)
  (cls.rootfs/'usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2').write_bytes(b'\x7fELF\x02\x01'+bytes(12)+b'\x3e\x00')
  (cls.rootfs/'usr/lib/data').write_text('guest-data')
  (cls.rootfs/'lib').write_bytes(b'STEAMOHOS_DEPOT_SYMLINK_V1\n/usr/lib')
  # A real OHOS emulated symlink record, not a host symlink.
  (cls.rootfs/'absolute-link').write_bytes(b'STEAMOHOS_DEPOT_SYMLINK_V1\n/usr/lib')
  (cls.rootfs/'escape-link').write_bytes(b'STEAMOHOS_DEPOT_SYMLINK_V1\n/../../outside')
  cls.game=cls.common/'Generic Game';cls.game.mkdir();cls.binary=cls.game/'game.x86_64'
  cls.binary.write_bytes(b'\x7fELF\x02\x01'+bytes(12)+b'\x3e\x00')
  cls.compat=cls.library/'steamapps/compatdata/2923300'
  (cls.base/'ipc').mkdir()
  obj=[]
  for name in ['posix_compat.c','filesystem_compat.c','sysvipc_compat.c','socket_metadata.c','popen_compat.c','exec_compat.cpp','syscall_compat.S']:
   source=NATIVE/'gnu_fixture'/name;target=cls.base/(name+'.o');obj.append(str(target))
   subprocess.run(['g++' if name.endswith('.cpp') else 'cc','-O2','-Wall','-Wextra','-Werror','-fPIC','-c',str(source),'-o',str(target)],check=True)
  for name in ['native_loader.cpp','native_loader_entry.S']:
   target=cls.base/(name+'.o');obj.append(str(target))
   subprocess.run(['g++','-O2','-Wall','-Wextra','-Werror','-fPIC','-c',str(NATIVE/name),'-o',str(target)],check=True)
  subprocess.run(['g++','-shared',*obj,'-ldl','-pthread','-o',str(cls.code/'libsteamohos_posix.so')],check=True)
  subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror',str(NATIVE/'gnu_fixture/steamohos_fex.cpp'),'-o',str(cls.code/'steamohos_fex')],check=True)
  caller=cls.base/'caller.c';caller.write_text('#include <unistd.h>\n#include <stdio.h>\nint main(int n,char**v){if(n<2)return 64;execv(v[1],v+1);perror("caller exec");return 77;}')
  subprocess.run(['cc',str(caller),'-o',str(cls.base/'caller')],check=True)
  source=cls.base/'fex.c';source.write_text(r"""
#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ucontext.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/prctl.h>
// Bypass the interposed libc syscall() just as FEX's host SVC does.
static long inline_syscall6(long nr,long a,long b,long c,long d,long e,long f) {
 register long r10 __asm__("r10")=d;
 register long r8 __asm__("r8")=e;
 register long r9 __asm__("r9")=f;
 long result;
 __asm__ volatile("syscall":"=a"(result):"a"(nr),"D"(a),"S"(b),"d"(c),
                  "r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory");
 return result;
}
static int preinit_ok;
static volatile sig_atomic_t raw_signal_seen;
static void raw_signal(int s,siginfo_t* i,void* c){(void)i;(void)c;raw_signal_seen=s;}
static void* expected_alt_stack;
static void raw_alt_signal(int s,siginfo_t* i,void* c){
 (void)i;assert(s==SIGUSR1);ucontext_t* u=c;
 assert(u->uc_stack.ss_sp==expected_alt_stack&&u->uc_stack.ss_size==65536);
 assert(u->uc_stack.ss_flags==0);raw_signal_seen++;
}

static void before_constructors(void) {
 int fd=openat(AT_FDCWD,"/dev/null",O_RDONLY);assert(fd>=0);close(fd);
 struct stat st;assert(!fstatat(AT_FDCWD,"/dev/null",&st,0)&&S_ISCHR(st.st_mode));
 preinit_ok=1;
}
__attribute__((section(".preinit_array"),used)) static void (*early)(void)=before_constructors;
static void check_file_syscalls(void) {
 const char *root=getenv("FEX_ROOTFS");assert(root);
 char path[4096],target[128];snprintf(path,sizeof(path),"%s/absolute-link",root);
 int dir=syscall(SYS_openat,AT_FDCWD,root,O_RDONLY|O_DIRECTORY,0);assert(dir>=0);
 struct stat st;
#ifdef SYS_newfstatat
 assert(!syscall(SYS_newfstatat,dir,"absolute-link",&st,AT_SYMLINK_NOFOLLOW)&&S_ISLNK(st.st_mode));
 assert(!syscall(SYS_newfstatat,dir,"absolute-link",&st,0)&&S_ISDIR(st.st_mode));
#endif
 assert(syscall(SYS_readlinkat,dir,"absolute-link",target,sizeof(target))==8&&!memcmp(target,"/usr/lib",8));
#ifdef SYS_statx
 struct statx sx;assert(!syscall(SYS_statx,dir,"absolute-link",AT_SYMLINK_NOFOLLOW,STATX_TYPE,&sx)&&S_ISLNK(sx.stx_mode));
#endif
 struct OpenHow {unsigned long long flags,mode,resolve;} how={O_RDONLY|O_CLOEXEC,0,0x10};
 int opened=syscall(SYS_openat2,dir,"absolute-link/data",&how,sizeof(how));assert(opened>=0);
 char bytes[16]={0};assert(read(opened,bytes,10)==10&&!strcmp(bytes,"guest-data"));close(opened);
 opened=syscall(SYS_openat2,dir,"../../absolute-link/data",&how,sizeof(how));assert(opened>=0);close(opened);
 assert(syscall(SYS_openat2,dir,"escape-link",&how,sizeof(how))<0&&errno==ENOENT);
 how.flags=O_PATH;opened=syscall(SYS_openat2,dir,"absolute-link/data",&how,sizeof(how));assert(opened>=0);
 assert(!(fcntl(opened,F_GETFD)&FD_CLOEXEC));close(opened);
 how.flags=O_RDONLY|O_NOFOLLOW;
 assert(syscall(SYS_openat2,dir,"absolute-link",&how,sizeof(how))<0&&errno==ELOOP);
 how.flags=O_RDONLY;how.resolve=8;
 assert(syscall(SYS_openat2,dir,"absolute-link/data",&how,sizeof(how))<0&&errno==ENOSYS);
 how.resolve=0x10;assert(syscall(SYS_openat2,dir,"data",&how,8)<0&&errno==EINVAL);
 struct {struct OpenHow how;unsigned long long extra;} extended={how,1};
 assert(syscall(SYS_openat2,dir,"data",&extended,sizeof(extended))<0&&errno==E2BIG);
 int wrong=open("/",O_PATH|O_DIRECTORY);assert(wrong>=0);
 assert(syscall(SYS_openat2,wrong,"etc/passwd",&how,sizeof(how))<0&&errno==ENOSYS);close(wrong);
 int fd=openat(dir,"absolute-link/data",O_RDONLY);assert(fd>=0);close(fd);close(dir);
}
int main(int n,char**v) {
 assert(preinit_ok);
 if(n==4&&!strcmp(v[1],"reexec-result")) {
  assert(!strcmp(v[2],"argument with spaces")&&!strcmp(v[3],"literal;$not-a-command"));
  assert(!getenv("LD_PRELOAD")&&!getenv("LD_AUDIT"));
  puts("FEX_SYSCALL_REEXEC_ARGV_ENV_OK");return 29;
 }
 // The host adapter must be loaded (raw-syscall tests below), but ARM64
 // preload/search paths must not be present in the future guest environment.
 // Even an empty inherited entry shadows FEX's appended guest Config.Env.
 assert(!getenv("LD_PRELOAD"));
 assert(!getenv("LD_AUDIT"));
 assert(strstr(getenv("LD_LIBRARY_PATH"),"/lib/x86_64-linux-gnu"));
 if(getenv("EXPECT_GUEST_PATH")) {
  assert(!strcmp(getenv("PATH"),getenv("EXPECT_GUEST_PATH")));
  pid_t utility=fork();assert(utility>=0);
  if(!utility){execlp("true","true",NULL);_exit(87);}
  int utilityStatus;assert(waitpid(utility,&utilityStatus,0)==utility);
  assert(WIFEXITED(utilityStatus)&&WEXITSTATUS(utilityStatus)==0);
  puts("GUEST_STANDARD_PATH_EXEC_OK");
 }
 if(!getenv("EXPECT_GUEST_AUDIO"))assert(!strstr(getenv("LD_LIBRARY_PATH"),getenv("STEAMOHOS_CODE_ROOT")));
 if(getenv("EXPECT_GUEST_AUDIO"))assert(!strcmp(getenv("ALSA_PLUGIN_DIR"),getenv("EXPECT_GUEST_AUDIO")));
 if(getenv("EXPECT_GUEST_LIBRARY"))assert(strstr(getenv("LD_LIBRARY_PATH"),getenv("EXPECT_GUEST_LIBRARY")));
 if(getenv("EXPECT_NATIVE_DISPLAY"))assert(!strcmp(getenv("DISPLAY"),getenv("EXPECT_NATIVE_DISPLAY")));
 check_file_syscalls();
 if(n==3&&!strcmp(v[1],"--wait_pipe")){unsigned long long ok=0;assert(write(atoi(v[2]),&ok,8)==8);return 0;}
 assert(n>=2);assert(!strcmp(getenv("SteamAppId"),"2923300"));
 assert(getenv("STEAMOHOS_AUDIO_BOOTSTRAP_SOCKET"));
 assert(!strcmp(getenv("SDL_VIDEODRIVER"),getenv("EXPECT_SDL_DRIVER")?getenv("EXPECT_SDL_DRIVER"):"x11"));
 assert(!strcmp(getenv("SDL_VIDEO_DRIVER"),getenv("EXPECT_SDL_DRIVER")?getenv("EXPECT_SDL_DRIVER"):"x11"));
 assert(!strcmp(getenv("STEAM_COMPAT_DATA_PATH"),getenv("STEAMOHOS_NATIVE_FEX_IPC")));
 assert(strstr(getenv("STEAM_COMPAT_DATA_PATH"),"/ipc/f"));
 assert(strlen(getenv("STEAM_COMPAT_DATA_PATH"))+strlen("/fex-emu/Server/4294967295.FEXServer.Socket")<108);
 assert(!getenv("STEAMOHOS_SOFTWARE_GL"));assert(!getenv("LIBGL_ALWAYS_SOFTWARE"));
 if(getenv("EXPECT_NATIVE_GLX")) {
  const char* root=getenv("EXPECT_NATIVE_GLX");
  assert(!strcmp(getenv("LIBGL_DRIVERS_PATH"),root));
  assert(!strcmp(getenv("STEAMOHOS_NATIVE_GLX_ROOT"),root));
  void* gl=dlopen("libGL.so.1",RTLD_NOW|RTLD_LOCAL);assert(gl);
  int(*marker)(void)=dlsym(gl,"native_route_marker");assert(marker&&marker()==811);dlclose(gl);
  char driver[4096];snprintf(driver,sizeof(driver),"%s/swrast_dri.so",root);
  gl=dlopen(driver,RTLD_NOW|RTLD_LOCAL);assert(gl);
  marker=dlsym(gl,"native_route_marker");assert(marker&&marker()==812);dlclose(gl);
 }
 if(getenv("EXPECT_VENUS_TRANSPORT")) {
  assert(!strcmp(getenv("VN_DEBUG"),"vtest"));
  assert(!strcmp(getenv("VN_PERF"),"no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring"));
  assert(!strcmp(getenv("VN_WINEHUA_STRONG_RING_BARRIER"),"1"));
  assert(!strcmp(getenv("VN_WINEHUA_REMOTE_MEMORY_SYNC"),"1"));
  assert(!strcmp(getenv("VN_WINEHUA_PERSISTENT_MAP_SYNC"),"1"));
  assert(!strcmp(getenv("VN_WINEHUA_DIRECT_FENCE_WAIT"),"1"));
 }
 if(getenv("EXPECTED_EXTERNAL_ROOT"))assert(!strcmp(getenv("STEAMOHOS_EXTERNAL_RUNTIME"),getenv("EXPECTED_EXTERNAL_ROOT")));
 char path[4096],data[16]={0};snprintf(path,sizeof(path),"%s/absolute-link",getenv("FEX_ROOTFS"));
 int root=open(path,O_RDONLY|O_DIRECTORY);assert(root>=0);
 int fd=syscall(SYS_openat,root,"data",O_RDONLY,0);assert(fd>=0);
 assert(read(fd,data,10)==10&&!strcmp(data,"guest-data"));close(fd);close(root);
 snprintf(path,sizeof(path),"%s/escape-link",getenv("FEX_ROOTFS"));
 assert(syscall(SYS_openat,AT_FDCWD,path,O_RDONLY,0)<0&&errno==EACCES);
 int pipes[2];assert(!pipe(pipes));pid_t child=fork();assert(child>=0);
 if(!child){close(pipes[0]);char server[4096],number[20];snprintf(server,sizeof(server),"%s/usr/bin/FEXServer",getenv("STEAMOHOS_NATIVE_FEX_TOOL"));snprintf(number,sizeof(number),"%d",pipes[1]);execl(server,server,"--wait_pipe",number,NULL);_exit(87);}
 close(pipes[1]);unsigned long long ready=99;assert(read(pipes[0],&ready,8)==8&&ready==0);close(pipes[0]);int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
 puts("FEX_SERVER_REAL_HANDSHAKE_OK");
 printf("CONFIG=%s/Config.json\n",getenv("FEX_APP_CONFIG_LOCATION"));
 fflush(stdout);
 if(n>2&&(!strcmp(v[2],"reexec-at")||!strcmp(v[2],"reexec-ve"))) {
  char* args[]={v[0],"reexec-result","argument with spaces","literal;$not-a-command",NULL};
  extern char** environ;
  assert(syscall(SYS_execveat,-1,"relative",args,environ,0)==-1&&errno==EBADF);
  assert(syscall(SYS_execveat,AT_FDCWD,"/proc/self/exe",args,environ,0x40000000)==-1&&errno==EINVAL);
  if(!strcmp(v[2],"reexec-at"))syscall(SYS_execveat,AT_FDCWD,"/proc/self/exe",args,environ,0);
  else syscall(SYS_execve,"/proc/self/exe",args,environ);
  perror("FEX syscall reexec");return 88;
 }
 if(n>2&&!strcmp(v[2],"optional-handles")) {
  struct sock_filter filter[]={
   BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
   BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_name_to_handle_at,0,1),
   BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_TRAP),
   BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_open_by_handle_at,0,1),
   BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_TRAP),
   BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
  struct sock_fprog policy={sizeof(filter)/sizeof(filter[0]),filter};
  assert(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0));
  assert(!prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&policy));
  unsigned char unchanged[32];memset(unchanged,0x5a,sizeof(unchanged));int mount=0x5555;
  assert(syscall(SYS_name_to_handle_at,AT_FDCWD,"/",unchanged,&mount,0)==-1&&errno==ENOSYS);
  assert(syscall(SYS_open_by_handle_at,-1,unchanged,0)==-1&&errno==ENOSYS);
  assert(mount==0x5555);for(unsigned i=0;i<sizeof(unchanged);++i)assert(unchanged[i]==0x5a);
  puts("INLINE_OPTIONAL_HANDLES_UNSUPPORTED_AND_BUFFERS_PRESERVED");return 0;
 }
 if(n>2&&(!strcmp(v[2],"optional-robust")||!strcmp(v[2],"optional-robust-raw"))) {
  const int registered=!strcmp(v[2],"optional-robust-raw");
  struct KernelAction {void* handler;unsigned long flags;void* restorer;unsigned long mask;} original,action,queried;
  if(registered) {
   assert(!syscall(SYS_rt_sigaction,SIGSYS,NULL,&original,8));action=original;
   action.handler=(void*)raw_signal;action.flags|=SA_SIGINFO|SA_ONSTACK;
   assert(!syscall(SYS_rt_sigaction,SIGSYS,&action,NULL,8));
  }
  struct sock_filter filter[]={
   BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
   BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_get_robust_list,0,1),
   BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_TRAP),
   BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
  struct sock_fprog policy={sizeof(filter)/sizeof(filter[0]),filter};
  assert(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0));
  assert(!prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&policy));
  void* head=(void*)0x12345678;size_t length=0x5678;
  assert(inline_syscall6(SYS_get_robust_list,0,(long)&head,(long)&length,0,0,0)==-ENOSYS);
  assert(inline_syscall6(SYS_get_robust_list,getpid(),(long)&head,(long)&length,0,0,0)==-ENOSYS);
  assert(inline_syscall6(SYS_get_robust_list,2147483647,(long)&head,(long)&length,0,0,0)==-ENOSYS);
  assert(head==(void*)0x12345678&&length==0x5678);
  assert(inline_syscall6(SYS_getpid,0,0,0,0,0,0)==getpid());
  if(registered) {
   assert(!raw_signal_seen);
   assert(!syscall(SYS_rt_sigaction,SIGSYS,NULL,&queried,8));
   assert(!memcmp(&action,&queried,sizeof(action)));
   raise(SIGSYS);assert(raw_signal_seen==SIGSYS);
   assert(!syscall(SYS_rt_sigaction,SIGSYS,&original,NULL,8));
   puts("REGISTERED_SIGSYS_PRESERVED_AND_OPTIONAL_TRAP_RESUMED");
  }
  puts("INLINE_OPTIONAL_ROBUST_UNSUPPORTED_AND_BUFFERS_PRESERVED");return 0;
 }
 if(n>2&&!strcmp(v[2],"optional-numa")) {
  struct sock_filter filter[]={
   BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
   BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_get_mempolicy,0,1),
   BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_TRAP),
   BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_set_mempolicy,0,1),
   BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_TRAP),
   BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_mbind,0,1),
   BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_TRAP),
   BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_migrate_pages,0,1),
   BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_TRAP),
   BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,SYS_move_pages,0,1),
   BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_TRAP),
   BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
  struct sock_fprog policy={sizeof(filter)/sizeof(filter[0]),filter};
  assert(!prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0));
  assert(!prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&policy));
  unsigned long mask=0x5555;int mode=0x1234,status=0x5678;
  char memory[64];memset(memory,0x5a,sizeof(memory));void* pages[]={memory};
  assert(inline_syscall6(SYS_get_mempolicy,(long)&mode,(long)&mask,8,0,0,0)==-ENOSYS);
  assert(inline_syscall6(SYS_set_mempolicy,0,(long)&mask,8,0,0,0)==-ENOSYS);
  assert(inline_syscall6(SYS_mbind,(long)memory,sizeof(memory),0,(long)&mask,8,0)==-ENOSYS);
  assert(inline_syscall6(SYS_migrate_pages,0,8,(long)&mask,(long)&mask,0,0)==-ENOSYS);
  assert(inline_syscall6(SYS_move_pages,0,1,(long)pages,0,(long)&status,0)==-ENOSYS);
  assert(mask==0x5555&&mode==0x1234&&status==0x5678&&pages[0]==memory);
  for(unsigned i=0;i<sizeof(memory);++i)assert(memory[i]==0x5a);
  assert(inline_syscall6(SYS_getpid,0,0,0,0,0,0)==getpid());
  puts("INLINE_OPTIONAL_NUMA_UNSUPPORTED_AND_BUFFERS_PRESERVED");return 0;
 }
 if(n>2&&!strcmp(v[2],"hold")){for(;;)pause();}
 if(n>2&&!strcmp(v[2],"raw-alt-signal")) {
  void* memory=malloc(65536);assert(memory);expected_alt_stack=memory;
  stack_t alt={.ss_sp=memory,.ss_flags=0,.ss_size=65536},saved;
  assert(!sigaltstack(&alt,&saved));
  struct sigaction handler={0},old;handler.sa_sigaction=raw_alt_signal;
  handler.sa_flags=SA_SIGINFO|SA_ONSTACK;sigemptyset(&handler.sa_mask);
  assert(!sigaction(SIGUSR1,&handler,&old));
  struct KernelAction {void* handler;unsigned long flags;void* restorer;unsigned long mask;} action,previous,queried;
  assert(!syscall(SYS_rt_sigaction,SIGUSR1,NULL,&action,8));
  assert(!syscall(SYS_rt_sigaction,SIGUSR1,&action,&previous,8));
  assert(!memcmp(&action,&previous,sizeof(action)));
  raise(SIGUSR1);raise(SIGUSR1);assert(raw_signal_seen==2);
  assert(!syscall(SYS_rt_sigaction,SIGUSR1,NULL,&queried,8));
  assert(!memcmp(&action,&queried,sizeof(action)));
  assert(!sigaltstack(NULL,&alt)&&alt.ss_sp==memory&&alt.ss_flags==0);
  assert(!sigaction(SIGUSR1,&old,NULL));assert(!sigaltstack(&saved,NULL));free(memory);
  puts("RAW_ALT_SIGNAL_REPEATED_AND_RESTORED");return 0;
 }
 if(n>2&&!strcmp(v[2],"raw-signal")) {
  struct KernelAction {void* handler;unsigned long flags;void* restorer;unsigned long mask;} original,action,previous,queried;
  assert(!syscall(SYS_rt_sigaction,SIGSEGV,NULL,&original,8));action=original;
  action.handler=(void*)raw_signal;action.flags|=SA_SIGINFO;
  assert(!syscall(SYS_rt_sigaction,SIGSEGV,&action,&previous,8));
  assert(previous.handler==original.handler&&previous.flags==original.flags);
  raise(SIGSEGV);assert(raw_signal_seen==SIGSEGV);
  assert(!syscall(SYS_rt_sigaction,SIGSEGV,NULL,&queried,8));
  assert(queried.handler==action.handler&&queried.flags==action.flags&&queried.mask==action.mask&&queried.restorer==action.restorer);
  assert(!syscall(SYS_rt_sigaction,SIGSEGV,&original,NULL,8));
  puts("RAW_SIGNAL_RESUMED_AND_RESTORED");fflush(stdout);raise(SIGSEGV);return 99;
 }
  if(n>2&&!strcmp(v[2],"fault")){raise(SIGSEGV);return 99;}
 if(getenv("EXPECT_SCRIPT")) {
  int offset=getenv("EXPECT_INTERPRETER_ARG")?1:0;
  assert(n==5+offset&&strstr(v[1],"/usr/bin/bash"));
  if(offset)assert(!strcmp(v[2],getenv("EXPECT_INTERPRETER_ARG")));
  assert(!strcmp(v[2+offset],getenv("EXPECT_SCRIPT")));
  assert(!strcmp(v[3+offset],"argument with spaces")&&!strcmp(v[4+offset],"literal;$not-a-command"));
 } else assert(n==4&&!strcmp(v[2],"argument with spaces")&&!strcmp(v[3],"literal;$not-a-command"));
 fd=open(getenv("FEX_APP_CONFIG"),O_RDONLY);assert(fd>=0);close(fd);
 return 23;
}
""")
  subprocess.run(['cc',str(source),'-o',str(cls.tool/'usr/bin/FEX')],check=True)
  shutil.copyfile(cls.tool/'usr/bin/FEX',cls.tool/'usr/bin/FEXServer')
  for f in (cls.tool/'usr/bin').iterdir():f.chmod(0o660)
  interp=re.search(r'interpreter: (.*?)\]',subprocess.check_output(['readelf','-l',str(cls.base/'caller')],text=True)).group(1)
  shutil.copyfile(interp,cls.code/Path(interp).name)
  for name in ['libc.so.6','libstdc++.so.6','libgcc_s.so.1','libm.so.6']:
   shutil.copyfile(subprocess.check_output(['g++','-print-file-name='+name],text=True).strip(),cls.code/name)
  cls.env={**os.environ,'LD_PRELOAD':str(cls.code/'libsteamohos_posix.so'),'STEAMOHOS_CODE_ROOT':str(cls.code),
   'STEAMOHOS_LIBRARY_ROOT':str(cls.library),'STEAMOHOS_IPC_ROOT':str(cls.base/'ipc'),'STEAM_COMPAT_DATA_PATH':str(cls.compat),
   'STEAM_COMPAT_INSTALL_PATH':str(cls.game),'SteamAppId':'2923300','STEAMOHOS_SOFTWARE_GL':'1','LIBGL_ALWAYS_SOFTWARE':'1',
   'STEAMOHOS_AUDIO_BOOTSTRAP_SOCKET':str(cls.base/'audio'),'GLIBC_TUNABLES':'glibc.pthread.rseq=0'}
  cls.env.pop('STEAMOHOS_NATIVE_FEX',None)
  cls.control=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
  cls.endpoint=str(cls.base/'observe.sock');cls.control.bind(cls.endpoint);cls.control.listen(8)
  cls.env.update(STEAMOHOS_LAUNCH_SOCKET=cls.endpoint,STEAMOHOS_LAUNCH_NONCE='a'*64)
  def read(conn,size):
   value=b''
   while len(value)<size:
    more=conn.recv(size-len(value))
    if not more: return None
    value+=more
   return value
  def observe(conn):
   with conn:
    prefix=read(conn,4)
    if prefix is None:return
    frame=read(conn,struct.unpack('!I',prefix)[0])
    assert frame and b'HMOSDECK-NATIVE/1' in frame
    conn.sendall(struct.pack('!III',0x484d5231,1,0))
    while read(conn,16):pass
  def accept():
   while True:
    try:conn,_=cls.control.accept()
    except OSError:return
    threading.Thread(target=observe,args=(conn,),daemon=True).start()
  threading.Thread(target=accept,daemon=True).start()
 @classmethod
 def tearDownClass(cls):cls.control.close();cls.tmp.cleanup()
 def command(self,*extra):
  return [str(self.base/'caller'),str(self.tool/'fex-compat-tool'),'waitforexitandrun','--',str(self.runtime/'_v2-entry-point'),'--verb=waitforexitandrun','--',str(self.scout/'scout-on-soldier-entry-point-v2'),str(self.binary),*extra]
 def run_command(self,args=None,env=None):
  return subprocess.run(args or self.command('argument with spaces','literal;$not-a-command'),env=env or self.env,capture_output=True,text=True,timeout=15)
 def guest_audio_core(self,complete):
  core=self.code/'native-guest-core-x86_64';core.mkdir()
  names=['ld-linux-x86-64.so.2','libc.so.6','libm.so.6','libdl.so.2','libpthread.so.0','librt.so.1','libutil.so.1','libresolv.so.2','libanl.so.1','libnss_dns.so.2','libnss_files.so.2','libgcc_s.so.1','libstdc++.so.6']
  if complete:names.extend(['libasound_module_pcm_hmosdeck.so','libsteamohos_native_ipc.so'])
  for name in names:(core/name).write_bytes(b'\x7fELF\x02\x01'+bytes(12)+b'\x3e\x00')
  (core/'manifest.json').write_text('{}')
  return core

 def test_kernel_checked_local_copy(self):
  source=self.base/'user-copy.c';source.write_text(r"""
#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
int main(void){
 long (*copy)(void*,const void*,unsigned long)=dlsym(RTLD_DEFAULT,"SteamOhosLocalCopy");assert(copy);
 unsigned char *a=mmap(0,12288,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 unsigned char *b=mmap(0,12288,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(a!=MAP_FAILED&&b!=MAP_FAILED);
 memset(a,0x5a,12288);assert(copy(b,a,12288)==12288&&!memcmp(a,b,12288));
 assert(!mprotect(a+4096,4096,PROT_NONE));errno=0;assert(copy(b,a+4096,1)==-1&&errno==EFAULT);
 errno=0;assert(copy(a+4096,b,1)==-1&&errno==EFAULT);
 assert(copy(b,a,8192)==4096);assert(!mprotect(a+4096,4096,PROT_READ));
 assert(copy(b,a,8192)==8192);errno=0;assert(copy((void*)1,a,1)==-1&&errno==EFAULT);
 errno=0;assert(copy(b,(void*)(UINTPTR_MAX-1),8)==-1&&errno==EFAULT);
 assert(copy(0,0,0)==0);puts("COPY_BOTH_EFAULT_PARTIAL_READONLY_AND_ZERO");return 0;
}
""")
  binary=self.base/'user-copy';subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',source,'-ldl','-o',binary],check=True)
  result=subprocess.run([binary],env={**os.environ,'LD_PRELOAD':str(self.code/'libsteamohos_posix.so')},capture_output=True,text=True,timeout=10)
  self.assertEqual(result.returncode,0,result.stderr);self.assertIn('COPY_BOTH_EFAULT_PARTIAL_READONLY_AND_ZERO',result.stdout)

 def test_runtime_script_preserves_argv_without_host_shell(self):
  directory=self.rootfs/'usr/bin';directory.mkdir(exist_ok=True)
  (directory/'bash').write_bytes(self.binary.read_bytes())
  (self.rootfs/'bin').write_bytes(b'STEAMOHOS_DEPOT_SYMLINK_V1\n/usr/bin')
  script=self.game/'launcher with spaces'
  for suffix in ['', ' -e', ' -e -u']:
   with self.subTest(suffix=suffix):
    script.write_text('#!/bin/bash'+suffix+'\nnever evaluate this on host\n')
    args=[str(script) if x==str(self.binary) else x for x in self.command('argument with spaces','literal;$not-a-command')]
    env={**self.env,'EXPECT_SCRIPT':str(script)}
    if suffix:env['EXPECT_INTERPRETER_ARG']=suffix.strip()
    result=self.run_command(args,env)
    self.assertEqual(result.returncode,23,result.stdout+result.stderr)
 def test_invalid_script_and_missing_interpreter_fail_before_fex(self):
  script=self.game/'invalid-launcher'
  for content,stage in [('#!/missing/interpreter\nexit 0\n','script_interpreter_missing_or_invalid'),
                        ('#!/bin/../bin/bash\n','game_not_x86_elf_or_script'),
                        ('#!/bin/bash\x00\n','game_not_x86_elf_or_script'),
                        ('#!/bin/bash'+' '*260+'\n','game_not_x86_elf_or_script'),
                        ('not an executable\n','game_not_x86_elf_or_script')]:
   with self.subTest(content=content[:30]):
    script.write_text(content)
    args=[str(script) if x==str(self.binary) else x for x in self.command('argument with spaces','literal;$not-a-command')]
    result=self.run_command(args)
    self.assertNotEqual(result.returncode,23)
    self.assertIn('stage='+stage,result.stderr)
 def test_guest_audio_uses_guest_plugin_with_original_bootstrap(self):
  core=self.guest_audio_core(True)
  try:
   result=self.run_command(env={**self.env,'EXPECT_GUEST_AUDIO':str(core)})
   self.assertEqual(result.returncode,23,result.stdout+result.stderr)
   self.assertIn('audio_thunk=0',result.stderr);self.assertIn('guest_alsa=1',result.stderr)
  finally:shutil.rmtree(core)
 def test_guest_ipc_preload_is_only_in_fex_guest_configuration(self):
  core=self.guest_audio_core(True);process=None
  try:
   process=subprocess.Popen(self.command('hold'),env={**self.env,'EXPECT_GUEST_AUDIO':str(core)},stdout=subprocess.PIPE,stderr=subprocess.PIPE)
   import select
   content=b'';deadline=time.monotonic()+10
   while b'CONFIG=' not in content or not content.endswith(b'\n'):
    self.assertLess(time.monotonic(),deadline)
    if select.select([process.stdout],[],[],1)[0]:
     block=os.read(process.stdout.fileno(),1);self.assertTrue(block);content+=block
   document=fex_config(Path(content.decode().split('CONFIG=',1)[1].strip()).read_text())
   self.assertIn('LD_PRELOAD='+str(core/'libsteamohos_native_ipc.so'),document['Config']['Env'])
   self.assertEqual(len(document['Config']['Env']),5)
   self.assertEqual(sum(v.startswith('PATH=') for v in document['Config']['Env']),1)
   process.send_signal(signal.SIGTERM);process.communicate(timeout=10)
  finally:
   if process and process.poll() is None:process.kill();process.communicate(timeout=5)
   shutil.rmtree(core)
 def test_incomplete_guest_audio_is_actionable_before_fex(self):
  core=self.guest_audio_core(False)
  try:
   result=self.run_command();self.assertEqual(result.returncode,126)
   self.assertIn('guest_audio_plugin_missing',result.stderr)
   self.assertNotIn('FEX_SERVER_REAL_HANDSHAKE_OK',result.stdout)
  finally:shutil.rmtree(core)
 def test_real_loader_server_and_exit(self):
  result=self.run_command();self.assertEqual(result.returncode,23,result.stdout+result.stderr)
  self.assertIn('FEX_SERVER_REAL_HANDSHAKE_OK',result.stdout)
  self.assertIn('status=exited code=23',result.stderr)
  self.assertFalse(list((self.compat/'fex-emu').glob('hmosdeck-*')))
 def test_native_exit_diagnostic_preserves_kernel_status_and_scope(self):
  source=self.base/'exit-probe.c';binary=self.base/'exit-probe'
  source.write_text(r'''#include <assert.h>
#include <errno.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(int n,char**v){
 (void)v;assert(syscall(SYS_getpid)==getpid());
 assert(syscall(SYS_close,-1)==-1&&errno==EBADF);
 syscall(n>1?SYS_exit:SYS_exit_group,37);return 99;
}
''')
  subprocess.run(['cc','-O2',str(source),'-o',str(binary)],check=True)
  base={**os.environ,'LD_PRELOAD':str(self.code/'libsteamohos_posix.so')}
  for native,trace in [('0','1'),('1','0'),('1','1')]:
   for args in [[],['thread-exit']]:
    result=subprocess.run([str(binary),*args],env={**base,'STEAMOHOS_NATIVE_FEX':native,
        'STEAMOHOS_NATIVE_EXIT_TRACE':trace},capture_output=True,text=True,timeout=5)
    self.assertEqual(result.returncode,37,result.stderr)
    if native==trace=='1':
     self.assertEqual(result.stderr.count('STEAMOHOS_NATIVE_EXIT '),1)
     self.assertIn('syscall='+('exit' if args else 'exit_group')+' status=37',result.stderr)
     self.assertRegex(result.stderr,r'module=exit-probe map_offset=0x[1-9a-f][0-9a-f]*')
    else:self.assertNotIn('STEAMOHOS_NATIVE_EXIT ',result.stderr)
 def test_nested_fex_syscall_exec_uses_validated_host_loader(self):
  for mode in ['reexec-at','reexec-ve']:
   with self.subTest(mode=mode):
    result=self.run_command(self.command(mode))
    self.assertEqual(result.returncode,29,result.stdout+result.stderr)
    self.assertIn('FEX_SYSCALL_REEXEC_ARGV_ENV_OK',result.stdout)
    self.assertIn('status=exited code=29',result.stderr)
 def test_host_and_guest_linker_environments_are_separate(self):
  game_libraries=str(self.game/'native-libs')
  result=self.run_command(env={**self.env,'LD_LIBRARY_PATH':str(self.code)+':'+game_libraries,
   'EXPECT_GUEST_LIBRARY':game_libraries})
  self.assertEqual(result.returncode,23,result.stdout+result.stderr)
  self.assertIn('FEX_SERVER_REAL_HANDSHAKE_OK',result.stdout)
 def test_guest_path_exec_preserves_game_order_and_excludes_host_tools(self):
  for supplied,expected in [
   (str(self.game)+"/bin:"+str(self.code)+"/steamrtarm64:/system/bin:/bin",str(self.game)+"/bin:/bin:/usr/local/bin:/usr/bin"),
   ("","/usr/local/bin:/usr/bin:/bin"),
   ("/usr/bin:/bin:/usr/bin","/usr/bin:/bin:/usr/local/bin")]:
   with self.subTest(path=supplied):
    result=self.run_command(env={**self.env,'PATH':supplied,'EXPECT_GUEST_PATH':expected})
    self.assertEqual(result.returncode,23,result.stdout+result.stderr)
    self.assertIn('GUEST_STANDARD_PATH_EXEC_OK',result.stdout)
 def test_32_bit_thunks_use_fex_base_directory_contract(self):
  guest=self.tool/'usr/share/fex-emu/GuestThunks_32'
  host=self.tool/'usr/lib/aarch64-linux-gnu/fex-emu/HostThunks_32'
  guest.mkdir(parents=True);host.mkdir(parents=True)
  original=self.binary.read_bytes()
  machine=183 if os.uname().machine=='aarch64' else 62
  elf=lambda bits,arch:b'\x7fELF'+bytes([bits,1])+bytes(12)+arch.to_bytes(2,'little')
  database=self.tool/'usr/share/fex-emu/ThunksDB.json';database.write_text('{"DB":{}}')
  for name in ['GL','vulkan','asound','wayland-client']:
   (host/('lib'+name+'-host.so')).write_bytes(elf(2,machine))
   (guest/('lib'+name+'-guest.so')).write_bytes(elf(1,3))
  self.binary.write_bytes(elf(1,3))
  process=None
  try:
   process=subprocess.Popen(self.command('hold'),env=self.env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
   import select
   content=b'';deadline=time.monotonic()+10
   while b'CONFIG=' not in content or not content.endswith(b'\n'):
    self.assertLess(time.monotonic(),deadline)
    if select.select([process.stdout],[],[],1)[0]:
     block=os.read(process.stdout.fileno(),1);self.assertTrue(block);content+=block
   config=Path(content.decode().split('CONFIG=',1)[1].strip())
   document=json.loads(config.read_text())
   self.assertEqual(document['Config']['ThunkGuestLibs'],str(guest).removesuffix('_32'))
   self.assertEqual(document['ThunksDB'],dict(GL='1',Vulkan='1',asound='1',WaylandClient='1'))
   process.send_signal(signal.SIGTERM);out,err=process.communicate(timeout=10)
   self.assertIn(b'guest_bits=32',err)
  finally:
   if process and process.poll() is None:process.kill();process.communicate(timeout=5)
   self.binary.write_bytes(original);shutil.rmtree(guest);shutil.rmtree(host);database.unlink()
 def test_compatdata_cannot_escape_library(self):
  result=self.run_command(env={**self.env,'STEAM_COMPAT_DATA_PATH':str(self.base/'userdata')})
  self.assertEqual(result.returncode,126);self.assertIn('compatdata_outside_library',result.stderr)
  self.assertFalse((self.base/'userdata').exists())
 def test_native_vulkan_uses_wine_venus_transport_contract(self):
  result=self.run_command(env={**self.env,'VTEST_SOCKET_NAME':'/tmp/native-vtest.socket','EXPECT_VENUS_TRANSPORT':'1'})
  self.assertEqual(result.returncode,23,result.stdout+result.stderr)
 def test_native_glx_uses_matching_host_driver_and_dril_entry(self):
  directory=self.code/'native-vulkan-driver';directory.mkdir()
  source=self.base/'native_glx_marker.c'
  try:
   for name,marker in [('libGL.so.1',811),('libdril_dri.so',812)]:
    source.write_text('int native_route_marker(void){return '+str(marker)+';}')
    subprocess.run(['cc','-shared','-fPIC',str(source),'-o',str(directory/name)],check=True)
   result=self.run_command(env={**self.env,'VTEST_SOCKET_NAME':'/tmp/native-vtest.socket','EXPECT_NATIVE_GLX':str(directory)})
   self.assertEqual(result.returncode,23,result.stdout+result.stderr)
  finally:shutil.rmtree(directory)
 def test_terminal_diagnostic_preserves_fatal_signal(self):
  code=self.base/'releases/test/runtime';code.parent.mkdir(parents=True,exist_ok=True)
  code.symlink_to(self.code,target_is_directory=True)
  try:
   result=self.run_command(self.command('fault'),env={**self.env,'STEAMOHOS_CODE_ROOT':str(code),
    'STEAMOHOS_NATIVE_MEMORY':'1','STEAMOHOS_EXTERNAL_RUNTIME':str(code)})
   self.assertEqual(result.returncode,139,result.stdout+result.stderr)
   self.assertIn('STEAMOHOS_FEX_TERMINAL_FAULT signal=0xb',result.stderr)
   self.assertIn('status=exited code=139 signal=11',result.stderr)
  finally:code.unlink()
 def test_raw_signal_diagnostic_preserves_handler_and_query(self):
  code=self.base/'releases/test/runtime';code.parent.mkdir(parents=True,exist_ok=True)
  code.symlink_to(self.code,target_is_directory=True)
  try:
   result=self.run_command(self.command('raw-signal'),env={**self.env,'STEAMOHOS_CODE_ROOT':str(code),
    'STEAMOHOS_NATIVE_MEMORY':'1','STEAMOHOS_EXTERNAL_RUNTIME':str(code)})
   self.assertEqual(result.returncode,139,result.stdout+result.stderr)
   self.assertIn('RAW_SIGNAL_RESUMED_AND_RESTORED',result.stdout)
   self.assertIn('STEAMOHOS_FEX_SIGNAL signal=0xb',result.stderr)
   self.assertIn('STEAMOHOS_FEX_TERMINAL_FAULT signal=0xb',result.stderr)
  finally:code.unlink()
 def test_file_handle_seccomp_uses_linux_unsupported_contract(self):
  result=self.run_command(self.command('optional-handles'))
  self.assertEqual(result.returncode,0,result.stdout+result.stderr)
  self.assertIn('INLINE_OPTIONAL_HANDLES_UNSUPPORTED_AND_BUFFERS_PRESERVED',result.stdout)
 def test_robust_list_seccomp_reports_unsupported_without_fabricating_a_list(self):
  result=self.run_command(self.command('optional-robust'))
  self.assertEqual(result.returncode,0,result.stdout+result.stderr)
  self.assertIn('INLINE_OPTIONAL_ROBUST_UNSUPPORTED_AND_BUFFERS_PRESERVED',result.stdout)
 def test_optional_trap_after_fex_registers_sigsys_handler(self):
  environment={**self.env};environment.pop('STEAMOHOS_NATIVE_MEMORY',None)
  result=self.run_command(self.command('optional-robust-raw'),env=environment)
  self.assertEqual(result.returncode,0,result.stdout+result.stderr)
  self.assertIn('REGISTERED_SIGSYS_PRESERVED_AND_OPTIONAL_TRAP_RESUMED',result.stdout)
 def test_numa_seccomp_uses_linux_unsupported_contract(self):
  result=self.run_command(self.command('optional-numa'))
  self.assertEqual(result.returncode,0,result.stdout+result.stderr)
  self.assertIn('INLINE_OPTIONAL_NUMA_UNSUPPORTED_AND_BUFFERS_PRESERVED',result.stdout)
 def test_native_display_is_inherited_by_fex_and_server(self):
  result=self.run_command(env={**self.env,'DISPLAY':':57','STEAMOHOS_NATIVE_X11_DISPLAY':'127.0.0.1:22222','EXPECT_NATIVE_DISPLAY':'127.0.0.1:22222'})
  self.assertEqual(result.returncode,23,result.stdout+result.stderr)
 def test_explicit_sdl_backend_is_preserved(self):
  environment={**self.env,'SDL_VIDEODRIVER':'wayland','SDL_VIDEO_DRIVER':'wayland','EXPECT_SDL_DRIVER':'wayland'}
  result=self.run_command(env=environment)
  self.assertEqual(result.returncode,23,result.stdout+result.stderr)
 def test_raw_alt_stack_handler_without_diagnostics(self):
  environment={**self.env};environment.pop('STEAMOHOS_NATIVE_MEMORY',None)
  result=self.run_command(self.command('raw-alt-signal'),env=environment)
  self.assertEqual(result.returncode,0,result.stdout+result.stderr)
  self.assertIn('RAW_ALT_SIGNAL_REPEATED_AND_RESTORED',result.stdout)
  self.assertNotIn('STEAMOHOS_FEX_SIGNAL',result.stderr)
 def test_existing_cloud_mapping_is_accepted(self):
  private=self.base/'ipc/hmosdeck-cloud-v2-compatdata/2923300';private.mkdir(parents=True,exist_ok=True)
  prefix=self.base/'cloud-wine';prefix.mkdir(exist_ok=True)
  # The existing Cloud layer maps only a recognised persisted compatibility
  # choice, not every arbitrary private directory or native tool.
  (self.library/'config').mkdir(exist_ok=True)
  config=self.library/'config/config.vdf'
  config.write_text('"InstallConfigStore" { "Software" { "Valve" { "Steam" { "CompatToolMapping" { "0" { "name" "hmosdeck_proton" } } } } } }')
  result=self.run_command(env={**self.env,'STEAMOHOS_WINE_PREFIX':str(prefix),'STEAMOHOS_STEAM_ROOT':str(self.library)})
  self.assertEqual(result.returncode,23,result.stdout+result.stderr)
  self.assertIn('FEX_SERVER_REAL_HANDSHAKE_OK',result.stdout)
  self.assertTrue((private/'fex-emu').is_dir())
 def test_compatdata_symlink_outside_library_is_rejected(self):
  outside=self.base/'outside-compat';outside.mkdir(exist_ok=True)
  link=self.library/'steamapps/compatdata/4294967295'
  link.parent.mkdir(exist_ok=True);link.symlink_to(outside,target_is_directory=True)
  try:
   result=self.run_command(env={**self.env,'STEAM_COMPAT_DATA_PATH':str(link)})
   self.assertEqual(result.returncode,126);self.assertIn('compatdata_outside_library',result.stderr)
  finally:link.unlink()
 def sparse_runtime(self,manifest):
  runtime=self.common/'SteamLinuxRuntime_sniper'
  runtime.mkdir(exist_ok=True)
  (runtime/'_v2-entry-point').write_text('never executed pressure-vessel\n')
  platform=runtime/'sniper_platform_test';root=platform/'files'
  (root/'lib/x86_64-linux-gnu').mkdir(parents=True,exist_ok=True)
  loader=root/'lib/x86_64-linux-gnu/ld-version.so'
  loader.write_bytes(b'\x7fELF\x02\x01'+bytes(12)+b'\x3e\x00')
  (root/'lib/data').write_text('guest-data')
  (root/'absolute-link').write_bytes(b'STEAMOHOS_DEPOT_SYMLINK_V1\n/usr/lib')
  (root/'escape-link').write_bytes(b'STEAMOHOS_DEPOT_SYMLINK_V1\n/../../outside')
  with gzip.open(platform/'usr-mtree.txt.gz','wt') as out:out.write(manifest)
  command=self.command('argument with spaces','literal;$not-a-command')
  command[4]=str(runtime/'_v2-entry-point')
  return runtime,root,command
 def test_minimized_runtime_restores_metadata_without_copying_payload(self):
  runtime,root,command=self.sparse_runtime('#mtree\n. type=dir\n./lib64/ld-linux-x86-64.so.2 type=link link=/lib/x86_64-linux-gnu/ld-version.so\n./lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 type=link link=ld-version.so\n')
  try:
   for attempt in range(2):
    result=self.run_command(command);self.assertEqual(result.returncode,23,result.stdout+result.stderr)
    self.assertIn('payload_copied=0',result.stderr)
    if attempt:self.assertIn('created=0',result.stderr)
   self.assertTrue((root/'usr').is_symlink())
   self.assertEqual(os.readlink(root/'usr'),'.')
   self.assertEqual((root/'lib/x86_64-linux-gnu/ld-version.so').stat().st_size,20)
  finally:shutil.rmtree(runtime)
 def test_mtree_escape_is_rejected_before_any_writes(self):
  runtime,root,command=self.sparse_runtime('#mtree\n. type=dir\n./new-link type=link link=lib\n./../escape type=link link=lib\n')
  try:
   result=self.run_command(command);self.assertEqual(result.returncode,126,result.stdout+result.stderr)
   self.assertIn('runtime_mtree_traversal',result.stderr)
   self.assertFalse((root/'new-link').exists())
   self.assertFalse((root/'usr').exists())
  finally:shutil.rmtree(runtime)
 def test_external_fex_maps_only_the_same_installation(self):
  code=self.base/'releases/test/runtime';code.parent.mkdir(parents=True,exist_ok=True)
  code.symlink_to(self.code,target_is_directory=True)
  try:
   result=self.run_command(env={**self.env,'STEAMOHOS_CODE_ROOT':str(code),
    'STEAMOHOS_NATIVE_MEMORY':'1','STEAMOHOS_EXTERNAL_RUNTIME':str(code),'EXPECTED_EXTERNAL_ROOT':str(self.base)})
   self.assertEqual(result.returncode,23,result.stdout+result.stderr)
  finally:code.unlink()
 def test_external_fex_does_not_expand_an_unrelated_runtime(self):
  result=self.run_command(env={**self.env,'STEAMOHOS_NATIVE_MEMORY':'1','STEAMOHOS_EXTERNAL_RUNTIME':str(self.code)})
  self.assertEqual(result.returncode,126,result.stdout+result.stderr)
  self.assertIn('fex_external_runtime_scope',result.stderr)
 def test_unknown_runtime_is_not_silently_stripped(self):
  command=self.command();command[5]='--verb=unknown'
  result=self.run_command(command);self.assertEqual(result.returncode,126);self.assertIn('runtime_verb_missing',result.stderr)
 def test_missing_server_reports_failure(self):
  server=self.tool/'usr/bin/FEXServer';saved=server.with_suffix('.saved');server.rename(saved)
  try:
   result=self.run_command();self.assertEqual(result.returncode,126);self.assertIn('fex_server_elf_missing',result.stderr)
  finally:saved.rename(server)
 def test_external_script_and_helper_without_capability_are_rejected(self):
  for name in ['usr/bin/FEX','other-script']:
   path=self.tool/name
   if not path.exists():path.write_text('#!/bin/sh\nexit 0\n')
   result=self.run_command([str(self.base/'caller'),str(path)])
   self.assertEqual(result.returncode,77,result.stdout+result.stderr)
 def test_cancel_preserves_configuration_and_signal_status(self):
  process=subprocess.Popen(self.command('hold'),env=self.env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
  try:
   import select
   deadline=time.monotonic()+10;lines=[]
   # Read a byte at a time: TextIO buffering must not hide the second line.
   content=b''
   while b'CONFIG=' not in content or not content.endswith(b'\n'):
    self.assertLess(time.monotonic(),deadline)
    ready,_,_=select.select([process.stdout],[],[],1)
    if ready:
     block=os.read(process.stdout.fileno(),1)
     self.assertTrue(block);content+=block
   path=Path(content.decode().split('CONFIG=',1)[1].strip());document=fex_config(path.read_text())
   self.assertEqual(document['Config']['RootFS'],str(self.rootfs))
   self.assertIn('LD_PRELOAD=',document['Config']['Env'])
   self.assertIn('STEAM_COMPAT_DATA_PATH='+str(self.compat),document['Config']['Env'])
   process.send_signal(signal.SIGTERM)
   out,err=process.communicate(timeout=10)
   self.assertEqual(process.returncode,128+signal.SIGTERM,content.decode()+out+err)
   self.assertFalse(path.parent.exists())
  finally:
   if process.poll() is None:process.kill();process.communicate(timeout=5)
 def test_steam_appinfo_and_user_fex_settings_survive(self):
  process=subprocess.Popen(self.command('hold'),env={**self.env,'STEAM_FEX_TSOENABLED':'0',
   'STEAM_FEX_MULTIBLOCK':'1','STEAM_COMPAT_FEX_CONFIG':'TSOEnabled:1 Multiblock:0 ThunksDB_Vulkan:1'},
   stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
  try:
   import select
   content=b'';deadline=time.monotonic()+10
   while b'CONFIG=' not in content or not content.endswith(b'\n'):
    self.assertLess(time.monotonic(),deadline)
    if select.select([process.stdout],[],[],1)[0]:
     block=os.read(process.stdout.fileno(),1);self.assertTrue(block);content+=block
   config=Path(content.decode().split('CONFIG=',1)[1].strip())
   data=json.loads((config.parent/'app.json').read_text())
   self.assertEqual(data['Config'],dict(TSOEnabled='1',Multiblock='0'))
   self.assertEqual(data['ThunksDB'],dict(GL='0'))
   # An absent Vulkan thunk remains disabled in the base config despite ':1'.
   self.assertEqual(json.loads(config.read_text())['ThunksDB']['Vulkan'],'0')
   process.send_signal(signal.SIGTERM);process.communicate(timeout=10)
   self.assertFalse(config.parent.exists())
  finally:
   if process.poll() is None:process.kill();process.communicate(timeout=5)
if __name__=='__main__':unittest.main()
