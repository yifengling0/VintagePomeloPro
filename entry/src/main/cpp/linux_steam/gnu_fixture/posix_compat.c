// GNU-only adaptations for the app's private X11 endpoint. Never loaded by OHOS.
#define _GNU_SOURCE 1
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <link.h>
#include <sys/syscall.h>
// Linux UAPI assigns 444 on both supported ISAs; older GNU sysroots omit it.
#ifndef SYS_landlock_create_ruleset
#define SYS_landlock_create_ruleset 444
#endif
#ifndef SYS_openat2
#define SYS_openat2 437
#endif
#ifndef SYS_close_range
#define SYS_close_range 436
#endif
#include <unistd.h>
#include <stdarg.h>
#include <stdatomic.h>

static void *(*next_dlopen)(const char *, int);
static void *(*next_dlmopen)(Lmid_t, const char *, int);
static int ModuleImage(struct dl_phdr_info *image, size_t size, void *context) {
    (void)size; (void)context;
    static _Atomic uintptr_t seen[512];
    uintptr_t identity=(uintptr_t)image->dlpi_addr+1;
    for(unsigned i=0;i<512;++i) {
        uintptr_t current=atomic_load(&seen[i]);
        if(current==identity)return 0;
        if(current)continue;
        if(!atomic_compare_exchange_strong(&seen[i],&current,identity)) {--i;continue;}
        uintptr_t end=(uintptr_t)image->dlpi_addr;
        for(unsigned j=0;j<image->dlpi_phnum;++j)if(image->dlpi_phdr[j].p_type==PT_LOAD) {
            uintptr_t limit=image->dlpi_addr+image->dlpi_phdr[j].p_vaddr+image->dlpi_phdr[j].p_memsz;
            if(limit>end)end=limit;
        }
        const char *name=image->dlpi_name&&*image->dlpi_name?image->dlpi_name:"<main>";
        const char *base=strrchr(name,'/');if(base)name=base+1;
        fprintf(stderr,"STEAMOHOS_IMAGE pid=%d base=%p end=%p name=%.160s\n",getpid(),(void*)image->dlpi_addr,(void*)end,name);
        return 0;
    }
    return 0;
}
static void ModuleSnapshot(void) {
    const char *value=getenv("STEAMOHOS_MODULE_DIAGNOSTICS");
    if(value&&!strcmp(value,"1")) {
        int saved=errno;dl_iterate_phdr(ModuleImage,NULL);errno=saved;
    }
}
static int NativeDiagnostic(void) {
    const char *value=getenv("STEAMOHOS_NATIVE_MEMORY");
    return value && !strcmp(value,"1");
}
__attribute__((visibility("hidden"))) extern long (*SteamOhosNextSyscall)(long,...);
// Observe large virtual reservations without changing their address or flags.
// In particular, CEF/V8 needs a CodeRange even when GPU acceleration is off.
static void *(*next_mmap)(void *,size_t,int,int,int,off_t);
void *mmap(void *address,size_t length,int prot,int flags,int fd,off_t offset) {
    if (!next_mmap) next_mmap=dlsym(RTLD_NEXT,"mmap");
    if (!next_mmap) { errno=ENOSYS; return MAP_FAILED; }
    void *result=next_mmap(address,length,prot,flags,fd,offset);
    int saved=errno;
    static _Atomic unsigned diagnostics;
    if (NativeDiagnostic() && length>=64u*1024u*1024u && atomic_fetch_add(&diagnostics,1)<128) {
        struct rlimit limit={0,0};getrlimit(RLIMIT_AS,&limit);
        fprintf(stderr,"STEAMOHOS_MMAP pid=%d length=%zu hint=%p prot=%x flags=%x result=%p errno=%d asLimit=%llu generation=%s\n",
            getpid(),length,address,prot,flags,result,result==MAP_FAILED?saved:0,
            (unsigned long long)limit.rlim_cur,getenv("STEAMOHOS_EXEC_GENERATION")?getenv("STEAMOHOS_EXEC_GENERATION"):"0");
    }
    errno=saved;return result;
}
void *mmap64(void *address,size_t length,int prot,int flags,int fd,off64_t offset) {
    // Both supported GNU targets have 64-bit off_t.
    _Static_assert(sizeof(off_t)==sizeof(off64_t),"native GNU offset ABI");
    return mmap(address,length,prot,flags,fd,(off_t)offset);
}
static int (*next_mprotect)(void *,size_t,int);
int mprotect(void *address,size_t length,int prot) {
    if (!next_mprotect) next_mprotect=dlsym(RTLD_NEXT,"mprotect");
    if (!next_mprotect) { errno=ENOSYS; return -1; }
    int result=next_mprotect(address,length,prot),saved=errno;
    static _Atomic unsigned diagnostics;
    if (NativeDiagnostic() && result<0 && atomic_fetch_add(&diagnostics,1)<64)
        fprintf(stderr,"STEAMOHOS_MPROTECT pid=%d address=%p length=%zu prot=%x errno=%d\n",getpid(),address,length,prot,saved);
    errno=saved;return result;
}
static int (*next_connect)(int, const struct sockaddr *, socklen_t);
static int (*next_bind)(int,const struct sockaddr*,socklen_t);
static int (*next_accept)(int,struct sockaddr*,socklen_t*);
static int (*next_accept4)(int,struct sockaddr*,socklen_t*,int);
extern void SteamOhosObserveSocket(int) __attribute__((weak));
// Observe real local TCP results without changing address or socket semantics.
static int TraceLocalEndpoint(const char *operation, int fd, const struct sockaddr *address,
                              socklen_t length, int result) {
    int saved=errno;
    if(SteamOhosObserveSocket&&address&&(address->sa_family==AF_INET||address->sa_family==AF_INET6)&&
       (result==0||(result<0&&saved==EINPROGRESS)))SteamOhosObserveSocket(fd);
    unsigned port=0;
    int local=0;
    if(address && address->sa_family==AF_INET && length>=sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *ip=(const struct sockaddr_in*)address;
        local=(ntohl(ip->sin_addr.s_addr)>>24)==127;
        port=ntohs(ip->sin_port);
    } else if(address && address->sa_family==AF_INET6 && length>=sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *ip=(const struct sockaddr_in6*)address;
        local=IN6_IS_ADDR_LOOPBACK(&ip->sin6_addr);
        port=ntohs(ip->sin6_port);
    }
    static _Atomic unsigned diagnostics;
    if(local && NativeDiagnostic() && atomic_fetch_add(&diagnostics,1)<64)
        fprintf(stderr,"STEAMOHOS_LOCAL_TCP pid=%d op=%s fd=%d port=%u result=%d errno=%d\n",
                getpid(),operation,fd,port,result,result<0?saved:0);
    errno=saved;
    return result;
}
extern int SteamOhosUnixSocketPath(const char*,char*,size_t);
static int UnixEndpoint(int (*operation)(int,const struct sockaddr*,socklen_t),int fd,const struct sockaddr *address,socklen_t length) {
    const size_t offset=offsetof(struct sockaddr_un,sun_path);
    if(address&&address->sa_family==AF_UNIX&&length>offset&&length<=sizeof(struct sockaddr_un)) {
        const struct sockaddr_un *source=(const struct sockaddr_un*)address;
        if(source->sun_path[0]) {
            char path[sizeof(source->sun_path)+1];size_t count=length-offset;
            memcpy(path,source->sun_path,count);path[count]=0;
            struct sockaddr_un target={.sun_family=AF_UNIX};
            int mapped=SteamOhosUnixSocketPath(path,target.sun_path,sizeof(target.sun_path));
            if(mapped<0)return -1;
            if(mapped)return operation(fd,(const struct sockaddr*)&target,offset+strlen(target.sun_path)+1);
        }
    }
    return operation(fd,address,length);
}

// Load the verified, unmodified ANGLE modules before CEF forks its GPU/zygote
// children. The document provider may deny new opens later in child startup.
// Keep RTLD_LOCAL handles alive for the process lifetime: forked children reuse
// the images without changing the renderer, creating a context or enabling GPU.
__attribute__((visibility("hidden"))) void SteamOhosPreloadCefGraphics(void) {
    const char *entry=getenv("STEAMOHOS_EXEC_PATH"),*code=getenv("STEAMOHOS_CODE_ROOT");
    const char *external=getenv("STEAMOHOS_EXTERNAL_RUNTIME");
    if(!NativeDiagnostic()||!entry||!code||!external||code[0]!='/')return;
    if(!next_dlopen)next_dlopen=dlsym(RTLD_NEXT,"dlopen");
    if(!next_dlopen)return;
    size_t count=strlen(code),external_length=strlen(external);
    if(external_length<2||strncmp(code,external,external_length)||
       (code[external_length]&&code[external_length]!='/')||
       strncmp(entry,code,count)||strcmp(entry+count,"/steamrtarm64/steamwebhelper")||
       strstr(code,"/../")||strstr(code,"/./"))return;
    static void *modules[2];
    const char *names[]={"libGLESv2.so","libEGL.so"};
    for(unsigned i=0;i<2;++i) {
        char path[PATH_MAX];int n=snprintf(path,sizeof(path),"%s/steamrtarm64/%s",code,names[i]);
        if(n<=0||n>=(int)sizeof(path))continue;
        modules[i]=next_dlopen(path,RTLD_NOW|RTLD_LOCAL);
        const char *error=modules[i]?NULL:dlerror();
        fprintf(stderr,"STEAMOHOS_CEF_EARLY_GRAPHICS pid=%d library=%s loaded=%d error=%.300s\n",
                getpid(),names[i],modules[i]!=NULL,error?error:"");
    }
}
__attribute__((constructor)) static void Initialize(void) {
    SteamOhosNextSyscall = dlsym(RTLD_NEXT, "syscall");
    next_bind = dlsym(RTLD_NEXT,"bind");
    next_accept = dlsym(RTLD_NEXT,"accept");
    next_accept4 = dlsym(RTLD_NEXT,"accept4");
    next_connect = dlsym(RTLD_NEXT, "connect");
    next_dlopen = dlsym(RTLD_NEXT, "dlopen");
    next_dlmopen = dlsym(RTLD_NEXT, "dlmopen");
    ModuleSnapshot();
}
int connect(int fd, const struct sockaddr *address, socklen_t length) {
    if (!next_connect) { errno = ENOSYS; return -1; }
    static const char expected[] = "/tmp/.X11-unix/X57";
    const size_t offset = offsetof(struct sockaddr_un, sun_path);
    if (address && address->sa_family == AF_UNIX && length > offset && length <= sizeof(struct sockaddr_un)) {
        const struct sockaddr_un *original = (const struct sockaddr_un *)address;
        const char *path = original->sun_path;
        size_t available = length - offset;
        if (!path[0]) { ++path; --available; } // Linux abstract X11 socket.
        if (available >= sizeof(expected)-1 && !memcmp(path, expected, sizeof(expected)-1) &&
            (available == sizeof(expected)-1 || path[sizeof(expected)-1] == '\0')) {
            const char *private_path = getenv("STEAMOHOS_X11_SOCKET");
            struct sockaddr_un target = {.sun_family = AF_UNIX};
            if (!private_path || private_path[0] != '/' || strlen(private_path) >= sizeof(target.sun_path)) {
                errno = EINVAL; return -1;
            }
            strcpy(target.sun_path, private_path);
            return next_connect(fd, (const struct sockaddr *)&target, offset + strlen(private_path) + 1);
        }
    }
    int result=UnixEndpoint(next_connect,fd,address,length);
    return TraceLocalEndpoint("connect",fd,address,length,result);
}

// OHOS third-party apps cannot create filesystem symlinks. Keep data in its
// real Steam directory and route explicit dlopen paths to signed original ELF.
// Ordinary dependency resolution still uses GNU rtld's signed --library-path.
static const char *SignedLibrary(const char *name, char *mapped, size_t capacity) {
    if (!name) return name;
    const char *software=getenv("STEAMOHOS_SOFTWARE_GL");
    const char *runtime=getenv("STEAMOHOS_CODE_ROOT");
    if(software && !strcmp(software,"1") && runtime && runtime[0]=='/' &&
       (!strcmp(name,"libGL.so.1") || !strcmp(name,"libGLX.so.0") || !strcmp(name,"libGLX_indirect.so.0"))) {
        int size=snprintf(mapped,capacity,"%s/libsteamohos_software_gl.so",runtime);
        if(size>0 && (size_t)size<capacity)return mapped;
    }
    const char *root = getenv("STEAMOHOS_STEAM_ROOT");
    const char *code = getenv("STEAMOHOS_CODE_ROOT");
    if (!root || !code || root[0] != '/' || code[0] != '/') return name;
    const char *relative = name;
    size_t length = strlen(root);
    if (!strncmp(name, root, length) && name[length] == '/') relative = name + length + 1;
    else if (!strncmp(name, "./steamrtarm64/", 15)) relative = name + 2;
    if (strncmp(relative, "steamrtarm64/", 13) || strstr(relative, "/../") || strstr(relative, "/./") ||
        !strcmp(relative + strlen(relative) - 1, "/")) return name;
    int result = snprintf(mapped, capacity, "%s/%s", code, relative);
    if (result < 0 || (size_t)result >= capacity) return name;
    return mapped;
}
void *dlopen(const char *name, int flags) {
    if (!next_dlopen) { errno = ENOSYS; return NULL; }
    char mapped[PATH_MAX];
    const char *target = SignedLibrary(name, mapped, sizeof(mapped));
    static _Atomic unsigned diagnostics;
    if (NativeDiagnostic() && atomic_fetch_add(&diagnostics,1)<50)
        fprintf(stderr,"STEAMOHOS_DLOPEN_SCOPE flags=0x%x input=%s\n",flags,name ? name : "<main>");
    void *result = next_dlopen(target, flags);
    if(result)ModuleSnapshot();
    if (result && NativeDiagnostic() && name &&
        (strstr(name,"libvulkan") || strstr(name,"libEGL") || strstr(name,"libGLESv2"))) {
        int saved=errno; struct link_map *loaded=NULL;
        if(!dlinfo(result,RTLD_DI_LINKMAP,&loaded) && loaded)
            fprintf(stderr,"STEAMOHOS_GRAPHICS_LIBRARY pid=%d name=%.100s loaded=%.500s base=%p\n",
                getpid(),name,loaded->l_name,(void*)loaded->l_addr);
        errno=saved;
    }

    // Do not consume dlerror(): the original caller must receive that error.
    static _Atomic unsigned failures;
    if (!result && target && atomic_fetch_add(&failures,1)<20) fprintf(stderr, "STEAMOHOS_DLOPEN_FAILED input=%s target=%s\n", name, target);
    return result;
}
void *dlmopen(Lmid_t namespace_id, const char *name, int flags) {
    if (!next_dlmopen) { errno = ENOSYS; return NULL; }
    char mapped[PATH_MAX];
    const char *target=SignedLibrary(name,mapped,sizeof(mapped));
    static _Atomic unsigned diagnostics;
    int diagnostic=NativeDiagnostic() && atomic_fetch_add(&diagnostics,1)<50;
    if(diagnostic) fprintf(stderr,"STEAMOHOS_DLMOPEN_SCOPE namespace=%ld flags=0x%x input=%s\n",(long)namespace_id,flags,name?name:"<main>");
    void *result=next_dlmopen(namespace_id,target,flags);
    if(diagnostic) fprintf(stderr,"STEAMOHOS_DLMOPEN_RETURN ok=%d\n",result!=NULL);
    return result;
}


// The sandbox prohibits the kernel query. The paired GNU libc exposes its
// real current-thread robust list; foreign thread/process queries stay denied.
__attribute__((visibility("hidden"))) long (*SteamOhosNextSyscall)(long,...);
__attribute__((visibility("hidden"))) long SteamOhosSyscallUnavailable(void) {errno=ENOSYS;return -1;}
__attribute__((visibility("hidden"))) long SteamOhosQueryRobustList(pid_t tid,void **head,size_t *length) {
    if(!NativeDiagnostic()) {
        if(!SteamOhosNextSyscall)return SteamOhosSyscallUnavailable();
        return SteamOhosNextSyscall(SYS_get_robust_list,tid,head,length);
    }
    typedef int (*Query)(pid_t,void**,size_t*);
    Query query=(Query)dlsym(RTLD_NEXT,"__steamohos_get_robust_list");
    if(!query){errno=ENOSYS;return -1;}
    int result=query(tid,head,length);
    fprintf(stderr,"STEAMOHOS_CAPABILITY get_robust_list self_query=%d result=%d\n",tid==0||tid==gettid(),result);
    return result;
}


// Chromium probes for Landlock even when its own sandbox is disabled. This
// app sandbox forbids the query; use the real unsupported-kernel fallback.
__attribute__((visibility("hidden"))) long SteamOhosLandlockCreateRuleset(const void *attributes,size_t size,unsigned flags) {
    if(NativeDiagnostic()) {
        fprintf(stderr,"STEAMOHOS_CAPABILITY landlock_create_ruleset unsupported\n");
        errno=ENOSYS;return -1;
    }
    if(!SteamOhosNextSyscall)return SteamOhosSyscallUnavailable();
    return SteamOhosNextSyscall(SYS_landlock_create_ruleset,attributes,size,flags);
}

// OHOS denies this optional bulk-close syscall. Reporting it as unavailable
// lets lsof run its existing individual-close fallback, closing the real FDs.
__attribute__((visibility("hidden"))) long SteamOhosCloseRange(unsigned first,unsigned last,int flags) {
    if(NativeDiagnostic()) {
        fprintf(stderr,"STEAMOHOS_CAPABILITY close_range unsupported\n");
        errno=ENOSYS;return -1;
    }
    if(!SteamOhosNextSyscall)return SteamOhosSyscallUnavailable();
    return SteamOhosNextSyscall(SYS_close_range,first,last,flags);
}

int bind(int fd,const struct sockaddr *address,socklen_t length) {
    if(!next_bind){errno=ENOSYS;return -1;}
    int result=UnixEndpoint(next_bind,fd,address,length);
    return TraceLocalEndpoint("bind",fd,address,length,result);
}
int accept(int fd,struct sockaddr *address,socklen_t *length) {
    if(!next_accept){errno=ENOSYS;return -1;}
    int result=next_accept(fd,address,length),saved=errno;
    if(result>=0&&SteamOhosObserveSocket)SteamOhosObserveSocket(result);
    errno=saved;return result;
}
int accept4(int fd,struct sockaddr *address,socklen_t *length,int flags) {
    if(!next_accept4){errno=ENOSYS;return -1;}
    int result=next_accept4(fd,address,length,flags),saved=errno;
    if(result>=0&&SteamOhosObserveSocket)SteamOhosObserveSocket(result);
    errno=saved;return result;
}

// Steam probes this newer optional API when preparing depot files. OHOS traps
// it instead of returning ENOSYS. Preserve the unsupported-kernel contract;
// never weaken open_how.resolve restrictions by pretending openat is openat2.
__attribute__((visibility("hidden"))) long SteamOhosOpenAt2(int directory,const char *path,const void *how,size_t size) {
    if(NativeDiagnostic()) {
        static _Atomic unsigned logged;
        if(atomic_fetch_add(&logged,1)==0)fputs("STEAMOHOS_CAPABILITY openat2 unsupported\n",stderr);
        errno=ENOSYS;return -1;
    }
    if(!SteamOhosNextSyscall)return SteamOhosSyscallUnavailable();
    return SteamOhosNextSyscall(SYS_openat2,directory,path,how,size);
}
