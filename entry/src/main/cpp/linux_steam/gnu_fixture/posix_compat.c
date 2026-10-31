// GNU-only adaptations for the app's private X11 endpoint. Never loaded by OHOS.
#define _GNU_SOURCE 1
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <fcntl.h>
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
static int NativeFex(void) {
    const char *value=getenv("STEAMOHOS_NATIVE_FEX");return value&&!strcmp(value,"1");
}
#include "native_guest_mapping.h"
static void FexExecPolicy(int prot) {
    if((prot&PROT_EXEC)&&NativeFex()) {
        int saved=errno;(void)prctl(0x6a6974,0UL,0UL,0UL,0UL);errno=saved;
    }
}
static void *(*next_mmap)(void *,size_t,int,int,int,off_t);
void *mmap(void *address,size_t length,int prot,int flags,int fd,off_t offset) {
    if (!next_mmap) next_mmap=dlsym(RTLD_NEXT,"mmap");
    if (!next_mmap) { errno=ENOSYS; return MAP_FAILED; }
    FexExecPolicy(prot);
    void *result=next_mmap(address,length,prot,flags,fd,offset);
    int saved=errno;
    int guest=GuestElfDescriptor(fd);
    if(result==MAP_FAILED&&(saved==EACCES||saved==EPERM)&&(prot&PROT_EXEC)&&guest) {
        result=next_mmap(address,length,prot&~PROT_EXEC,flags,fd,offset);
        int retry_error=errno;
        TraceGuestProtection("mmap",saved,result!=MAP_FAILED);saved=retry_error;
    }
    if(result!=MAP_FAILED&&NativeFex())GuestFileUpdate(result,length,guest);
    static _Atomic unsigned guest_diagnostics;
    if(guest&&NativeDiagnostic()&&atomic_fetch_add(&guest_diagnostics,1)<32) {
        struct stat st={0};(void)fstat(fd,&st);
        fprintf(stderr,"STEAMOHOS_GUEST_MAP fd=%d address=%p length=%zu offset=%lld file_bytes=%lld prot=%x flags=%x errno=%d\n",
            fd,result,length,(long long)offset,(long long)st.st_size,prot,flags,result==MAP_FAILED?saved:0);
    }
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
// Only called for a bounded diagnostic after a real protection failure.
// Read the calling process's own VMA without allocating or dumping full maps.
static void TraceProtectionRegion(void *address) {
    if(!SteamOhosNextSyscall)return;
    int fd=(int)SteamOhosNextSyscall(SYS_openat,AT_FDCWD,"/proc/self/maps",O_RDONLY|O_CLOEXEC,0);
    if(fd<0)return;
    char bytes[4096],line[768];size_t used=0;int done=0;
    for(unsigned block=0;block<64&&!done;++block) {
        long count=SteamOhosNextSyscall(SYS_read,fd,bytes,sizeof(bytes));
        if(count<=0)break;
        for(long i=0;i<count;++i) {
            if(bytes[i]!='\n') {if(used+1<sizeof(line))line[used++]=bytes[i];continue;}
            line[used]=0;unsigned long begin=0,end=0;char permissions[5]={0};
            if(sscanf(line,"%lx-%lx %4s",&begin,&end,permissions)==3&&
               (uintptr_t)address>=begin&&(uintptr_t)address<end) {
                const char *name=strrchr(line,'/');
                const char *kind=strstr(line,"memfd:")?"memfd":name?"file":"anonymous";
                fprintf(stderr,"STEAMOHOS_PROTECTION_REGION pid=%d begin=%lx end=%lx permissions=%s kind=%s object=%.160s guest_file=%d\n",
                    getpid(),begin,end,permissions,kind,name?name+1:"none",GuestFileCovers(address,1));
                done=1;break;
            }
            used=0;
        }
    }
    (void)SteamOhosNextSyscall(SYS_close,fd);
}
static int (*next_mprotect)(void *,size_t,int);
int mprotect(void *address,size_t length,int prot) {
    if (!next_mprotect) next_mprotect=dlsym(RTLD_NEXT,"mprotect");
    if (!next_mprotect) { errno=ENOSYS; return -1; }
    FexExecPolicy(prot);
    int result=next_mprotect(address,length,prot),saved=errno;
    if(result<0&&(saved==EACCES||saved==EPERM)&&(prot&PROT_EXEC)&&GuestFileCovers(address,length)) {
        result=next_mprotect(address,length,prot&~PROT_EXEC);
        int retry_error=errno;
        TraceGuestProtection("mprotect",saved,result==0);saved=retry_error;
    }
    static _Atomic unsigned diagnostics;
    if (NativeDiagnostic() && result<0 && atomic_fetch_add(&diagnostics,1)<64) {
        fprintf(stderr,"STEAMOHOS_MPROTECT pid=%d address=%p length=%zu prot=%x errno=%d\n",getpid(),address,length,prot,saved);
        TraceProtectionRegion(address);
    }
    errno=saved;return result;
}
static int (*next_munmap)(void *,size_t);
int munmap(void *address,size_t length) {
    if(!next_munmap)next_munmap=dlsym(RTLD_NEXT,"munmap");
    if(!next_munmap){errno=ENOSYS;return -1;}
    int result=next_munmap(address,length),saved=errno;
    if(!result&&NativeFex())GuestFileUpdate(address,length,0);
    errno=saved;return result;
}
static void *(*next_mremap)(void *,size_t,size_t,int,...);
void *mremap(void *address,size_t old_length,size_t new_length,int flags,...) {
    if(!next_mremap)next_mremap=dlsym(RTLD_NEXT,"mremap");
    if(!next_mremap){errno=ENOSYS;return MAP_FAILED;}
    void *target=NULL;
    if(flags&MREMAP_FIXED){va_list args;va_start(args,flags);target=va_arg(args,void*);va_end(args);}
    int guest=GuestFileCovers(address,old_length);
    void *result=next_mremap(address,old_length,new_length,flags,target);int saved=errno;
    if(result!=MAP_FAILED&&NativeFex()) {
#ifdef MREMAP_DONTUNMAP
        if(!(flags&MREMAP_DONTUNMAP))
#endif
            GuestFileUpdate(address,old_length,0);
        GuestFileUpdate(result,new_length,guest);
    }
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
            if(operation==next_bind && NativeFex() && NativeDiagnostic()) {
                const char *root=getenv("XDG_RUNTIME_DIR");
                char expected[108];
                int count=root?snprintf(expected,sizeof(expected),"%s/FEX_gdbserver/%d-gdb",root,getpid()):-1;
                if(count>0 && (size_t)count<sizeof(expected) &&
                   length-offset>=(size_t)count &&
                   !memcmp(source->sun_path,expected,(size_t)count) &&
                   (length-offset==(size_t)count || !source->sun_path[count])) {
                    struct sockaddr_un debug={.sun_family=AF_UNIX};
                    int n=snprintf(debug.sun_path+1,sizeof(debug.sun_path)-1,
                        "vpp-fex-gdb-%u-%d",(unsigned)geteuid(),getpid());
                    int result=operation(fd,(const struct sockaddr*)&debug,offset+1+(size_t)n);
                    fprintf(stderr,"STEAMOHOS_NATIVE_GDB abstract=%s result=%d\n",debug.sun_path+1,result);
                    return result;
                }
            }
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
    pthread_atfork(GuestFileForkPrepare,GuestFileForkDone,GuestFileForkDone);
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
    if(NativeFex()&&(!strcmp(name,"libvulkan.so.1")||!strcmp(name,"libvulkan.so"))) {
        const char *code=getenv("STEAMOHOS_CODE_ROOT");
        if(code&&code[0]=='/') {
            int n=snprintf(mapped,capacity,"%s/libsteamohos_native_vulkan.so",code);
            if(n>0&&(size_t)n<capacity)return mapped;
        }
    }
    // The native GLX provider is the same verified Mesa DRI closure as GNU
    // Proton. DRIL is a separate driver entry library in Mesa 25, not a copy
    // of Gallium. Resolve only this exact, host-owned driver directory.
    const char *code_root=getenv("STEAMOHOS_CODE_ROOT");
    const char *glx_root=getenv("STEAMOHOS_NATIVE_GLX_ROOT");
    if(code_root&&glx_root) {
        char expected[PATH_MAX],driver[PATH_MAX];
        int n=snprintf(expected,sizeof(expected),"%s/native-vulkan-driver",code_root);
        int d=snprintf(driver,sizeof(driver),"%s/swrast_dri.so",glx_root);
        if(n>0&&(size_t)n<sizeof(expected)&&d>0&&(size_t)d<sizeof(driver)&&
           !strcmp(glx_root,expected)&&!strcmp(name,driver)) {
            n=snprintf(mapped,capacity,"%s/libdril_dri.so",glx_root);
            if(n>0&&(size_t)n<capacity)return mapped;
        }
    }
    if(NativeFex()&&code_root&&getenv("VTEST_SOCKET_NAME")&&
       (!strcmp(name,"libGL.so.1")||!strcmp(name,"libGL.so"))) {
        int n=snprintf(mapped,capacity,"%s/native-vulkan-driver/libGL.so.1",code_root);
        if(n>0&&(size_t)n<capacity)return mapped;
    }
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
extern long SteamOhosFexOpenAt2(int,const char*,const void*,size_t) __attribute__((weak));
__attribute__((visibility("hidden"))) long SteamOhosOpenAt2(int directory,const char *path,const void *how,size_t size) {
    if(NativeFex()&&SteamOhosFexOpenAt2) {
        long result=SteamOhosFexOpenAt2(directory,path,how,size);int saved=errno;
        static _Atomic unsigned graphics;
        if(NativeDiagnostic()&&path&&(strstr(path,"libvulkan")||strstr(path,"libGL")||
            strstr(path,"libX")||strstr(path,"libxcb")||strstr(path,"libSDL"))&&
            atomic_fetch_add(&graphics,1)<128)
            fprintf(stderr,"STEAMOHOS_NATIVE_GRAPHICS_OPEN pid=%d dirfd=%d path=%.350s result=%ld errno=%d\n",
                getpid(),directory,path,result,result<0?saved:0);
        errno=saved;return result;
    }
    if(NativeDiagnostic()) {
        static _Atomic unsigned logged;
        if(atomic_fetch_add(&logged,1)==0)fputs("STEAMOHOS_CAPABILITY openat2 unsupported\n",stderr);
        errno=ENOSYS;return -1;
    }
    if(!SteamOhosNextSyscall)return SteamOhosSyscallUnavailable();
    return SteamOhosNextSyscall(SYS_openat2,directory,path,how,size);
}

// Linux FEX issues openat/faccessat through syscall@PLT, bypassing the usual
// file wrappers. Reuse their real dirfd + emulated-depot-link semantics for
// this namespace only. All other callers retain the original syscall ABI.
__attribute__((visibility("hidden"))) long SteamOhosFexFileCall(long number,long a,long b,long c,long d,long e,long f) {
    if(NativeFex()) {
        if(number==SYS_openat) {
            int result=openat((int)a,(const char*)b,(int)c,(mode_t)d),saved=errno;
            static _Atomic unsigned reports;
            if(atomic_fetch_add(&reports,1)<64)
                fprintf(stderr,"STEAMOHOS_NATIVE_FEX_OPEN dirfd=%d path=%.300s result=%d errno=%d\n",
                    (int)a,(const char*)b,result,result<0?saved:0);
            errno=saved;return result;
        }
        if(number==SYS_faccessat)return faccessat((int)a,(const char*)b,(int)c,0);
#ifdef SYS_newfstatat
        if(number==SYS_newfstatat)return fstatat((int)a,(const char*)b,(struct stat*)c,(int)d);
#elif defined(SYS_fstatat64)
        if(number==SYS_fstatat64)return fstatat((int)a,(const char*)b,(struct stat*)c,(int)d);
#endif
        if(number==SYS_readlinkat)return readlinkat((int)a,(const char*)b,(char*)c,(size_t)d);
#ifdef SYS_statx
        if(number==SYS_statx)return statx((int)a,(const char*)b,(int)c,(unsigned)d,(struct statx*)e);
#endif
#ifdef SYS_faccessat2
        // FEX falls back to faccessat on the actual unsupported API.
        if(number==SYS_faccessat2){errno=ENOSYS;return -1;}
#endif
    }
    if(!SteamOhosNextSyscall)return SteamOhosSyscallUnavailable();
    return SteamOhosNextSyscall(number,a,b,c,d,e,f);
}

extern long SteamOhosNativeFexExecCall(long,long,long,long,long,long,long) __attribute__((weak));
__attribute__((visibility("hidden"))) long SteamOhosFexExecCall(long number,long a,long b,long c,long d,long e,long f) {
    if(NativeFex()&&SteamOhosNativeFexExecCall)
        return SteamOhosNativeFexExecCall(number,a,b,c,d,e,f);
    if(!SteamOhosNextSyscall)return SteamOhosSyscallUnavailable();
    return SteamOhosNextSyscall(number,a,b,c,d,e,f);
}

extern long SteamOhosNativeFexSigaction(long,long,long,long,long,long,long) __attribute__((weak));
__attribute__((visibility("hidden"))) long SteamOhosFexSignalCall(long number,long a,long b,long c,long d,long e,long f) {
    if(NativeFex()&&SteamOhosNativeFexSigaction)
        return SteamOhosNativeFexSigaction(number,a,b,c,d,e,f);
    if(!SteamOhosNextSyscall)return SteamOhosSyscallUnavailable();
    return SteamOhosNextSyscall(number,a,b,c,d,e,f);
}

// These optional Linux facilities are unavailable in the application sandbox.
// FEX has ordinary fallbacks; only its own process receives ENOSYS here.
__attribute__((visibility("hidden"))) long SteamOhosFexOptionalCall(long number,long a,long b,long c,long d,long e,long f) {
    if(NativeFex()){errno=ENOSYS;return -1;}
    if(!SteamOhosNextSyscall)return SteamOhosSyscallUnavailable();
    return SteamOhosNextSyscall(number,a,b,c,d,e,f);
}
