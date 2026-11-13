/* SPDX-License-Identifier: MIT
 * Opt-in guest diagnostic. Observe bounded libc writes near live stack frames;
 * forward the original call without changing its destination, size or result.
 * Inline engine stores and frames without an RBP chain are outside coverage.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <limits.h>

struct stack_write_thread {
    _Atomic uintptr_t low,high,frame;
    _Atomic int tid;
};
static struct stack_write_thread stack_write_threads[256];
static _Atomic int stack_write_enabled;
static _Atomic unsigned stack_write_reports;
static _Thread_local int stack_write_busy,stack_write_slot=-1;
static void *(*stack_write_memset)(void *,int,size_t);
static void *(*stack_write_memcpy)(void *,const void *,size_t);
static void *(*stack_write_memmove)(void *,const void *,size_t);
static int (*stack_write_create)(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);

static void stack_write_register(void) {
    pthread_attr_t attr;void *base=NULL;size_t size=0;
    stack_write_busy=1;
    if(!pthread_getattr_np(pthread_self(),&attr)) {
        int rc=pthread_attr_getstack(&attr,&base,&size);pthread_attr_destroy(&attr);
        if(!rc&&base&&size>4096&&(uintptr_t)base<=UINTPTR_MAX-size) {
            int tid=(int)syscall(SYS_gettid);
            for(unsigned i=0;i<256;i++) {
                int empty=0;
                if(!atomic_compare_exchange_strong(&stack_write_threads[i].tid,&empty,-1))continue;
                atomic_store(&stack_write_threads[i].low,(uintptr_t)base);
                atomic_store(&stack_write_threads[i].high,(uintptr_t)base+size);
                atomic_store(&stack_write_threads[i].frame,0);
                atomic_store(&stack_write_threads[i].tid,tid);stack_write_slot=(int)i;break;
            }
        }
    }
    stack_write_busy=0;
}
static void stack_write_report(const char *operation,uintptr_t caller,uintptr_t destination,
                               size_t size,int target,uintptr_t frame,const char *reason) {
    if(atomic_fetch_add(&stack_write_reports,1)>=32)return;
    Dl_info info={0};uintptr_t offset=caller;const char *module="unknown";
    if(dladdr((void *)caller,&info)&&info.dli_fbase){
        offset-=(uintptr_t)info.dli_fbase;
        const char *slash=info.dli_fname?strrchr(info.dli_fname,'/'):NULL;
        module=slash?slash+1:info.dli_fname?info.dli_fname:"main";
    }
    char line[512];int count=snprintf(line,sizeof(line),
        "GNU_STACK_WRITE op=%s caller=%.80s+0x%zx destination=%p bytes=%zu writer_tid=%ld target_tid=%d frame=%p reason=%s\n",
        operation,module,(size_t)offset,(void *)destination,size,syscall(SYS_gettid),target,(void *)frame,reason);
    if(count>0&&(size_t)count<sizeof(line))emit(line,(size_t)count);
}
static void stack_write_check(const char *operation,void *destination,size_t size,
                              uintptr_t frame,uintptr_t caller) {
    if(!atomic_load_explicit(&stack_write_enabled,memory_order_relaxed)||stack_write_busy||!size)return;
    int saved=errno;stack_write_busy=1;
    uintptr_t begin=(uintptr_t)destination;
    if(begin<=UINTPTR_MAX-size)for(unsigned i=0;i<256;i++) {
        struct stack_write_thread *entry=&stack_write_threads[i];int tid=atomic_load(&entry->tid);
        uintptr_t low=atomic_load(&entry->low),high=atomic_load(&entry->high),end=begin+size;
        if(tid<=0||end<=low||begin>=high)continue;
        if((int)i==stack_write_slot) {
            atomic_store(&entry->frame,frame);
            /* Observe saved RBP/return slots only; ordinary caller scratch is valid. */
            for(unsigned depth=0;depth<32;depth++) {
                if(frame<low||frame>high-16||(frame&7))break;
                if(begin<frame+16&&end>frame) {
                    stack_write_report(operation,caller,begin,size,tid,frame,"active_frame_overlap");break;
                }
                uintptr_t next=*(const uintptr_t *)frame;
                if(next<=frame||next-frame>1024*1024)break;
                frame=next;
            }
        } else {
            uintptr_t other=atomic_load(&entry->frame);
            if(other>=low+128&&other<=high-128&&begin<other+128&&end>other-128)
                stack_write_report(operation,caller,begin,size,tid,other,"cross_thread_near_frame");
        }
    }
    stack_write_busy=0;errno=saved;
}
void *memset(void *destination,int value,size_t size) {
    stack_write_check("memset",destination,size,(uintptr_t)__builtin_frame_address(0),(uintptr_t)__builtin_return_address(0));
    if(stack_write_memset)return stack_write_memset(destination,value,size);
    volatile unsigned char *out=destination;for(size_t i=0;i<size;i++)out[i]=(unsigned char)value;return destination;
}
void *memcpy(void *destination,const void *source,size_t size) {
    stack_write_check("memcpy",destination,size,(uintptr_t)__builtin_frame_address(0),(uintptr_t)__builtin_return_address(0));
    if(stack_write_memcpy)return stack_write_memcpy(destination,source,size);
    volatile unsigned char *out=destination;const unsigned char *in=source;
    for(size_t i=0;i<size;i++)out[i]=in[i];return destination;
}
void *memmove(void *destination,const void *source,size_t size) {
    stack_write_check("memmove",destination,size,(uintptr_t)__builtin_frame_address(0),(uintptr_t)__builtin_return_address(0));
    if(stack_write_memmove)return stack_write_memmove(destination,source,size);
    volatile unsigned char *out=destination;const unsigned char *in=source;
    if((uintptr_t)out<(uintptr_t)in)for(size_t i=0;i<size;i++)out[i]=in[i];
    else for(size_t i=size;i;i--)out[i-1]=in[i-1];return destination;
}
static void stack_write_unregister(void *unused) {
    (void)unused;if(stack_write_slot>=0)atomic_store(&stack_write_threads[stack_write_slot].tid,0);
}
struct stack_write_start {void *(*entry)(void *);void *argument;};
static void *stack_write_start_thread(void *raw) {
    struct stack_write_start start=*(struct stack_write_start *)raw;free(raw);
    stack_write_register();void *result;
    pthread_cleanup_push(stack_write_unregister,NULL);
    result=start.entry(start.argument);
    pthread_cleanup_pop(1);return result;
}
int pthread_create(pthread_t *thread,const pthread_attr_t *attr,void *(*entry)(void *),void *argument) {
    if(!stack_write_create)stack_write_create=dlsym(RTLD_NEXT,"pthread_create");
    if(!stack_write_create)return EAGAIN;
    if(!atomic_load(&stack_write_enabled))return stack_write_create(thread,attr,entry,argument);
    struct stack_write_start *start=malloc(sizeof(*start));if(!start)return ENOMEM;
    *start=(struct stack_write_start){entry,argument};
    int rc=stack_write_create(thread,attr,stack_write_start_thread,start);if(rc)free(start);return rc;
}
static int stack_write_initialize(void) {
    stack_write_busy=1;
    stack_write_memset=dlsym(RTLD_NEXT,"memset");stack_write_memcpy=dlsym(RTLD_NEXT,"memcpy");
    stack_write_memmove=dlsym(RTLD_NEXT,"memmove");stack_write_create=dlsym(RTLD_NEXT,"pthread_create");
    if(!stack_write_memset||!stack_write_memcpy||!stack_write_memmove||!stack_write_create){stack_write_busy=0;return -1;}
    stack_write_register();if(stack_write_slot<0)return -1;
    atomic_store(&stack_write_enabled,1);
    static const char ready[]="GNU_STACK_WRITE observer_ready=1 max_reports=32 inline_stores_observed=0\n";
    emit(ready,sizeof(ready)-1);
    return 0;
}
