#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

// FEX's x86-64 set_robust_list is a real kernel registration. OHOS denies
// its query, even for self. Never return the ARM64 host pthread's list or a
// fabricated empty head. Discover the guest glibc head through a temporary
// robust mutex's actual linkage, without private pthread/TLS offsets.
__attribute__((visibility("hidden"))) long (*SteamOhosGuestNextSyscall)(long,...);
static __thread struct robust_list_head *registered_head;
static __thread int explicit_registration;
static ptrdiff_t glibc_head_offset;
static struct robust_list_head *DiscoverGlibcHead(void);

__attribute__((visibility("hidden"))) long SteamOhosGuestNoSyscall(void) {
    errno=ENOSYS;return -1;
}
__attribute__((visibility("hidden"))) void *SteamOhosGuestResolveSyscall(void) {
    void *next=dlsym(RTLD_NEXT,"syscall");
    SteamOhosGuestNextSyscall=next;
    return next?next:(void*)SteamOhosGuestNoSyscall;
}
__attribute__((constructor)) static void InitializeGuestRobust(void) {
    (void)SteamOhosGuestResolveSyscall();
    // Discover once during module initialization. Later queries, including
    // those on other threads, need no mutex operation or guessed TLS offset.
    struct robust_list_head *head=DiscoverGlibcHead();
    ptrdiff_t offset=head?(ptrdiff_t)((uintptr_t)head-(uintptr_t)pthread_self()):0;
    if(head&&offset>0&&offset<16384){glibc_head_offset=offset;registered_head=head;}
}

static struct robust_list_head *DiscoverGlibcHead(void) {
#if defined(__GLIBC__) && defined(__x86_64__)
    pthread_mutexattr_t attributes;
    pthread_mutex_t mutex;
    if(pthread_mutexattr_init(&attributes))return NULL;
    int result=pthread_mutexattr_setrobust(&attributes,PTHREAD_MUTEX_ROBUST);
    if(!result)result=pthread_mutex_init(&mutex,&attributes);
    pthread_mutexattr_destroy(&attributes);
    if(result)return NULL;
    result=pthread_mutex_lock(&mutex);
    struct robust_list_head *head=NULL;
    if(!result) {
        // NPTL inserts at the front: prev is &self->robust_head, including
        // when other robust mutexes are already held. Validate both links
        // and the real futex offset before accepting this glibc ABI.
        head=(void*)mutex.__data.__list.__prev;
        if(!head||head->list.next!=(void*)&mutex.__data.__list.__next||
           head->futex_offset!=(long)offsetof(pthread_mutex_t,__data.__lock)-
                               (long)offsetof(pthread_mutex_t,__data.__list.__next))head=NULL;
        (void)pthread_mutex_unlock(&mutex);
    }
    (void)pthread_mutex_destroy(&mutex);
    return head;
#else
    return NULL;
#endif
}

static struct robust_list_head *CurrentGlibcHead(void) {
    if(explicit_registration||registered_head)return registered_head;
    if(!glibc_head_offset)return NULL;
    struct robust_list_head *head=(void*)((char*)pthread_self()+glibc_head_offset),copy;
    struct iovec local={&copy,sizeof(copy)},remote={head,sizeof(copy)};
    long read=SteamOhosGuestNextSyscall(SYS_process_vm_readv,
        SteamOhosGuestNextSyscall(SYS_getpid),&local,1,&remote,1,0);
    if(read!=(long)sizeof(copy)||copy.futex_offset!=
       (long)offsetof(pthread_mutex_t,__data.__lock)-
       (long)offsetof(pthread_mutex_t,__data.__list.__next))return NULL;
    registered_head=head;
    return head;
}

__attribute__((visibility("hidden"))) long SteamOhosGuestSetRobust(long number,void *head,size_t length) {
    if(!SteamOhosGuestNextSyscall)SteamOhosGuestResolveSyscall();
    if(!SteamOhosGuestNextSyscall)return SteamOhosGuestNoSyscall();
    long result=SteamOhosGuestNextSyscall(number,head,length);
    if(!result){registered_head=head;explicit_registration=1;}
    return result;
}

__attribute__((visibility("hidden"))) long SteamOhosGuestQueryRobust(long number,pid_t tid,void **output,size_t *length) {
    if(!SteamOhosGuestNextSyscall)SteamOhosGuestResolveSyscall();
    if(!SteamOhosGuestNextSyscall)return SteamOhosGuestNoSyscall();
    long result=SteamOhosGuestNextSyscall(number,tid,output,length);
    if(result==0)return result;
    const int failure=errno;
    if((failure!=ENOSYS&&failure!=EPERM&&failure!=EACCES)||
       (tid!=0&&tid!=(pid_t)SteamOhosGuestNextSyscall(SYS_gettid)))return result;
    struct robust_list_head *head=CurrentGlibcHead();
    if(!head&&!explicit_registration){errno=failure;return -1;}
    const size_t size=sizeof(*head);
    // Kernel-checked writes preserve EFAULT for invalid output pointers;
    // there is no signal handler or unchecked user-pointer dereference.
    struct iovec from[2]={{&head,sizeof(head)},{(void*)&size,sizeof(size)}};
    struct iovec to[2]={{output,sizeof(head)},{length,sizeof(size)}};
    long written=SteamOhosGuestNextSyscall(SYS_process_vm_writev,
        SteamOhosGuestNextSyscall(SYS_getpid),from,2,to,2,0);
    if(written!=(long)(sizeof(head)+sizeof(size))){if(written>=0)errno=EFAULT;return -1;}
    return 0;
}
