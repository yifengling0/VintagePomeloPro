// Keep x86 guest files genuinely file-backed on a host noexec filesystem.
// FEX tracks the guest's executable permission itself; these pages are read by
// its translator, never executed by the ARM64 CPU. Host/JIT pages are excluded.
#ifndef STEAMOHOS_NATIVE_GUEST_MAPPING_H
#define STEAMOHOS_NATIVE_GUEST_MAPPING_H
#include <pthread.h>

static int GuestElfDescriptor(int fd) {
    if(fd<0||!NativeFex())return 0;
    struct stat st;unsigned char elf[20];
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||pread(fd,elf,sizeof(elf),0)!=sizeof(elf)||
       memcmp(elf,"\177ELF",4)||elf[5]!=1)return 0;
    unsigned machine=elf[18]|((unsigned)elf[19]<<8);
    return (elf[4]==1&&machine==3)||(elf[4]==2&&machine==62);
}
// Bounded, allocation-free VMA metadata. If the table fills we forget affected
// entries, conservatively retaining host permission failures for those ranges.
struct GuestFileRange {uintptr_t begin,end;};
static struct GuestFileRange guest_file_ranges[2048];
static pthread_mutex_t guest_file_mutex=PTHREAD_MUTEX_INITIALIZER;
static int GuestFileEnd(void *base,size_t length,uintptr_t *end) {
    const long query=sysconf(_SC_PAGESIZE);
    if(query<=0)return 0;
    const size_t page=(size_t)query;
    if(!length||length>SIZE_MAX-(page-1))return 0;
    size_t rounded=(length+page-1)&~(page-1);
    if((uintptr_t)base>UINTPTR_MAX-rounded)return 0;
    *end=(uintptr_t)base+rounded;return 1;
}
static void GuestFileInsert(uintptr_t begin,uintptr_t end) {
    for(unsigned i=0;i<2048;++i)if(guest_file_ranges[i].begin==guest_file_ranges[i].end) {
        guest_file_ranges[i]=(struct GuestFileRange){begin,end};return;
    }
}
static void GuestFileUpdate(void *base,size_t length,int guest) {
    uintptr_t begin=(uintptr_t)base,end;
    if(!GuestFileEnd(base,length,&end))return;
    pthread_mutex_lock(&guest_file_mutex);
    for(unsigned i=0;i<2048;++i) {
        struct GuestFileRange old=guest_file_ranges[i];
        if(old.begin==old.end||old.end<=begin||old.begin>=end)continue;
        if(old.begin<begin&&old.end>end) {
            guest_file_ranges[i].end=begin;GuestFileInsert(end,old.end);
        } else if(old.begin<begin)guest_file_ranges[i].end=begin;
        else if(old.end>end)guest_file_ranges[i].begin=end;
        else guest_file_ranges[i]=(struct GuestFileRange){0,0};
    }
    if(guest)GuestFileInsert(begin,end);
    pthread_mutex_unlock(&guest_file_mutex);
}
static int GuestFileCovers(void *base,size_t length) {
    uintptr_t begin=(uintptr_t)base,end;
    if(!NativeFex()||!GuestFileEnd(base,length,&end))return 0;
    pthread_mutex_lock(&guest_file_mutex);
    uintptr_t cursor=begin;
    while(cursor<end) {
        uintptr_t covered=cursor;
        for(unsigned i=0;i<2048;++i)
            if(guest_file_ranges[i].begin<=cursor&&guest_file_ranges[i].end>covered)
                covered=guest_file_ranges[i].end;
        if(covered==cursor)break;
        cursor=covered;
    }
    pthread_mutex_unlock(&guest_file_mutex);return cursor>=end;
}
static void GuestFileForkPrepare(void) {pthread_mutex_lock(&guest_file_mutex);}
static void GuestFileForkDone(void) {pthread_mutex_unlock(&guest_file_mutex);}
static void TraceGuestProtection(const char *operation,int original,int result) {
    if(!NativeDiagnostic())return;
    static _Atomic unsigned reports;
    if(atomic_fetch_add(&reports,1)<32)
        fprintf(stderr,"STEAMOHOS_GUEST_FILE op=%s original_errno=%d retry_ok=%d copy=0 shared_flags_preserved=1\n",
                operation,original,result);
}
#endif
