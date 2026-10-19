// Explicit diagnostic executable. Its signal handler is never installed in Steam.
#define _GNU_SOURCE 1
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

static void text(const char *value) {
    size_t length=0; while(value[length] && length<1024) ++length;
    while(length) { ssize_t n=write(STDERR_FILENO,value,length); if(n<=0)return; value+=n;length-=(size_t)n; }
}
static void hex(uintptr_t value) {
    char bytes[2+sizeof(value)*2+1];size_t n=sizeof(bytes)-1;bytes[n]=0;
    do {bytes[--n]="0123456789abcdef"[value&15];value>>=4;}while(value);
    bytes[--n]='x';bytes[--n]='0';text(bytes+n);
}
static void fault(int signal,siginfo_t *information,void *context) {
    const ucontext_t *u=context;
    text("STEAMOHOS_LOADER_FAULT signal=");hex((uintptr_t)signal);
    text(" address=");hex((uintptr_t)information->si_addr);
#if defined(__aarch64__)
    text(" pc=");hex(u->uc_mcontext.pc);text(" lr=");hex(u->uc_mcontext.regs[30]);
    text(" sp=");hex(u->uc_mcontext.sp);
#elif defined(__x86_64__)
    text(" pc=");hex(u->uc_mcontext.gregs[REG_RIP]);text(" sp=");hex(u->uc_mcontext.gregs[REG_RSP]);
#endif
    text("\n");
    // Read immutable loader identity only; do not call dladdr or allocate from
    // a fault that may have interrupted rtld while holding its locks.
    const struct link_map *map=_r_debug.r_map;
    for(unsigned i=0;map && i<128;++i,map=map->l_next) {
        text("STEAMOHOS_LOADER_IMAGE base=");hex(map->l_addr);text(" name=");
        if(map->l_name)text(map->l_name);
        text("\n");
    }
    _exit(128+signal);
}

static void proc_descriptors(void) {
    const char *temporary=getenv("STEAMOHOS_TMP_ROOT");
    char path[4096];if(!temporary || snprintf(path,sizeof(path),"%s/loader-proc-XXXXXX",temporary)>=(int)sizeof(path))return;
    int file=mkstemp(path);if(file<0)return;
    unlink(path);
    int directory=open("/proc/self/fd",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    char number[32],absolute[96];snprintf(number,sizeof(number),"%d",file);
    snprintf(absolute,sizeof(absolute),"/proc/self/fd/%d",file);
    struct stat status;
    int direct=fstat(file,&status),direct_error=errno;
    int relative=fstatat(directory,number,&status,0),relative_error=errno;
    int full=stat(absolute,&status),full_error=errno;
    int raw=(int)syscall(SYS_newfstatat,directory,number,&status,0),raw_error=errno;
    fprintf(stderr,"STEAMOHOS_PROC_FD pid=%d fd=%d directory=%d fstat=%d/%d relative=%d/%d absolute=%d/%d raw=%d/%d\n",
            getpid(),file,directory,direct,direct?direct_error:0,relative,relative?relative_error:0,
            full,full?full_error:0,raw,raw?raw_error:0);
    if(directory>=0)close(directory);
    close(file);
}

int main(void) {
    proc_descriptors();
    pid_t child=fork();if(child<0)return 1;
    if(!child) {
        struct sigaction action={0};action.sa_sigaction=fault;action.sa_flags=SA_SIGINFO|SA_RESETHAND;
        sigemptyset(&action.sa_mask);
        if(sigaction(SIGSEGV,&action,NULL)||sigaction(SIGBUS,&action,NULL))_exit(2);
        fprintf(stderr,"STEAMOHOS_LOADER_PROBE begin pid=%d library=libvulkan.so.1\n",getpid());
        void *library=dlopen("libvulkan.so.1",RTLD_NOW|RTLD_LOCAL);
        if(!library){fprintf(stderr,"STEAMOHOS_LOADER_PROBE dlerror=%s\n",dlerror());_exit(3);}
        fprintf(stderr,"STEAMOHOS_LOADER_PROBE loaded=1\n");
        void *symbol=dlsym(library,"vkGetInstanceProcAddr");
        fprintf(stderr,"STEAMOHOS_LOADER_PROBE instance_proc_addr=%p\n",symbol);
        dlclose(library);_exit(symbol?0:4);
    }
    int status;while(waitpid(child,&status,0)<0){if(errno!=EINTR)return 1;}
    fprintf(stderr,"STEAMOHOS_LOADER_PROBE child_exit=%d child_signal=%d\n",
            WIFEXITED(status)?WEXITSTATUS(status):-1,WIFSIGNALED(status)?WTERMSIG(status):0);
    return WIFEXITED(status)&&WEXITSTATUS(status)==0?0:1;
}
