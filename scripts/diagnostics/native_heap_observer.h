/* Diagnostic-only GNU allocator evidence. Included only by the private builder.
 * No allocation interception, environment/configuration dump or silent trimming.
 */
#include <malloc.h>
#include <pthread.h>
#include <time.h>
#include <execinfo.h>

/* GNU diagnostic binary only. Forward directly to glibc's allocator so that
 * resolving a symbol cannot recursively allocate during the first malloc.
 * Record only the first 128 allocations of at least 1 MiB in CEF. No payload
 * bytes, allocation policy, free behaviour or application state is changed. */
extern void* __libc_malloc(size_t);
extern void* __libc_calloc(size_t,size_t);
extern void* __libc_realloc(void*,size_t);
static int steamohos_heap_observer_role(void);
static _Atomic int steamohos_heap_collecting;
static _Thread_local int steamohos_alloc_observing;
struct steamohos_heap_site { _Atomic uintptr_t caller; _Atomic size_t bytes,count; };
static struct steamohos_heap_site steamohos_heap_sites[256];
static void steamohos_heap_allocation(const char* operation,size_t bytes,void* result,uintptr_t caller) {
    /* Avoid even touching dynamic TLS while rtld is allocating its initial
     * TLS blocks. Small-allocation accounting starts at the thread checkpoint. */
    int collecting=atomic_load(&steamohos_heap_collecting);
    if(bytes<1024u*1024u&&!collecting)return;
    if(steamohos_alloc_observing)return;
    int saved=errno;steamohos_alloc_observing=1;
    if(collecting) {
        for(unsigned i=0;i<256;i++) {
            struct steamohos_heap_site* site=&steamohos_heap_sites[i];uintptr_t known=atomic_load(&site->caller);
            if(!known&&!atomic_compare_exchange_strong(&site->caller,&known,caller)){--i;continue;}
            if(!known||known==caller){atomic_fetch_add(&site->bytes,bytes);atomic_fetch_add(&site->count,1);break;}
        }
    }
    static _Atomic unsigned records;
    if(bytes>=1024u*1024u&&steamohos_heap_observer_role()&&atomic_fetch_add(&records,1)<128) {
        void* stack[10];int count=backtrace(stack,10);
        char line[1536];int used=snprintf(line,sizeof(line),"GNU_ALLOC pid=%d op=%s bytes=%zu result=%p stack=",getpid(),operation,bytes,result);
        for(int i=2;i<count&&used>0&&used<(int)sizeof(line)-100;++i) {
            Dl_info info={0};const char* name="unknown";uintptr_t offset=(uintptr_t)stack[i];
            if(dladdr(stack[i],&info)&&info.dli_fbase) {
                const char* base=info.dli_fname?strrchr(info.dli_fname,'/'):NULL;
                name=base?base+1:info.dli_fname?info.dli_fname:"main";
                offset-=(uintptr_t)info.dli_fbase;
            }
            used+=snprintf(line+used,sizeof(line)-(size_t)used," %.80s+0x%zx",name,(size_t)offset);
        }
        if(used>0&&used<(int)sizeof(line)-1){line[used++]='\n';ssize_t written=write(STDERR_FILENO,line,(size_t)used);(void)written;}
    }
    steamohos_alloc_observing=0;errno=saved;
}
void* malloc(size_t bytes) {
    void* result=__libc_malloc(bytes);steamohos_heap_allocation("malloc",bytes,result,(uintptr_t)__builtin_return_address(0));return result;
}
void* calloc(size_t count,size_t bytes) {
    void* result=__libc_calloc(count,bytes);
    if(!count||bytes<=SIZE_MAX/count)steamohos_heap_allocation("calloc",count*bytes,result,(uintptr_t)__builtin_return_address(0));
    return result;
}
void* realloc(void* old,size_t bytes) {
    void* result=__libc_realloc(old,bytes);steamohos_heap_allocation("realloc",bytes,result,(uintptr_t)__builtin_return_address(0));return result;
}
static void* steamohos_heap_observe(void* unused) {
    (void)unused;
    const pid_t owner=getpid();
    for(unsigned sample=0;sample<12;++sample) {
        struct timespec pause={10,0};while(nanosleep(&pause,&pause)&&errno==EINTR){}
        if(getpid()!=owner)break;
        struct mallinfo2 m=mallinfo2();
        fprintf(stderr,"GNU_HEAP pid=%d sample=%u arena=%zu used=%zu free=%zu mmap=%zu mmap_count=%zu top=%zu\n",
            owner,sample,m.arena,m.uordblks,m.fordblks,m.hblkhd,m.hblks,m.keepcost);
        if(sample<6) {
            steamohos_alloc_observing=1;
            for(unsigned i=0;i<256;++i) {
                struct steamohos_heap_site* site=&steamohos_heap_sites[i];size_t bytes=atomic_load(&site->bytes);
                if(bytes<16u*1024u*1024u)continue;
                uintptr_t caller=atomic_load(&site->caller);Dl_info info={0};const char* name="unknown";uintptr_t offset=caller;
                if(dladdr((void*)caller,&info)&&info.dli_fbase){const char* base=info.dli_fname?strrchr(info.dli_fname,'/'):NULL;name=base?base+1:info.dli_fname?info.dli_fname:"main";offset-=(uintptr_t)info.dli_fbase;}
                fprintf(stderr,"GNU_HEAP_SITE pid=%d sample=%u module=%.80s offset=0x%zx requested=%zu calls=%zu cumulative_not_live=1\n",owner,sample,name,(size_t)offset,bytes,atomic_load(&site->count));
            }
            steamohos_alloc_observing=0;
        }
        if(sample==5) {
            /* Explicit, per-session diagnostic marker only. Do not install a
             * recurring trim loop or change application allocation semantics. */
            const char* ipc=getenv("STEAMOHOS_IPC_ROOT");char marker[4096];
            int n=ipc?snprintf(marker,sizeof(marker),"%s/heap-trim-diagnostic",ipc):-1;
            if(n>0&&n<(int)sizeof(marker)&&!access(marker,F_OK)) {
                int trimmed=malloc_trim(0);
                fprintf(stderr,"GNU_HEAP pid=%d trim_diagnostic=1 returned=%d\n",owner,trimmed);
            }
        }
    }
    return NULL;
}
static int steamohos_heap_observer_role(void) {
    const char* path=getenv("STEAMOHOS_EXEC_PATH");
    const char* base=path?strrchr(path,'/'):NULL;
    /* Steam self-execs before creating its normal worker threads. Starting a
     * sampler there would change the single-thread exec precondition. */
    return base&&!strcmp(base,"/steamwebhelper");
}
/* Never create a thread in a constructor: Chromium's freshly exec'd zygote
 * must begin single-threaded. Start only after the application has itself
 * started twelve threads. The at-fork child inherits the started flag, while
 * POSIX removes the sampler thread, so we do not add a worker to that zygote. */
int pthread_create(pthread_t* thread,const pthread_attr_t* attr,void*(*entry)(void*),void* arg) {
    typedef int(*Create)(pthread_t*,const pthread_attr_t*,void*(*)(void*),void*);
    static _Atomic(Create) cached_create;
    Create real_create=atomic_load(&cached_create);
    if(!real_create) {
        real_create=(Create)dlsym(RTLD_NEXT,"pthread_create");
        if(real_create)atomic_store(&cached_create,real_create);
    }
    if(!real_create)return EAGAIN;
    int result=real_create(thread,attr,entry,arg),saved=errno;
    static _Atomic unsigned started;
    static _Atomic int observing;
    if(!result&&steamohos_heap_observer_role()&&atomic_fetch_add(&started,1)>=11&&!atomic_exchange(&observing,1)) {
        atomic_store(&steamohos_heap_collecting,1);
        pthread_attr_t probe_attr;pthread_t probe;
        if(!pthread_attr_init(&probe_attr)) {
            (void)pthread_attr_setstacksize(&probe_attr,128*1024);
            (void)pthread_attr_setdetachstate(&probe_attr,PTHREAD_CREATE_DETACHED);
            int error=real_create(&probe,&probe_attr,steamohos_heap_observe,NULL);
            pthread_attr_destroy(&probe_attr);
            fprintf(stderr,"GNU_HEAP observer pid=%d role=steamwebhelper thread_error=%d\n",getpid(),error);
        }
    }
    errno=saved;return result;
}
