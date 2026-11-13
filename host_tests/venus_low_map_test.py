from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
s = (root/'thirdparty/mesa/src/virtio/vulkan/vn_renderer_vtest.c').read_text()

def method(signature):
    start=s.index(signature)
    brace=s.index('{',start)
    depth=1
    end=brace+1
    while depth:
        depth+=(s[end]=='{')-(s[end]=='}')
        end+=1
    return s[start:end]

helpers=s[s.index('#if defined(__OHOS__) && defined(__aarch64__)\nstatic once_flag winehua_venus_low_map_once'):s.index('static void *\nvtest_bo_map(')]
code=r'''
#define _GNU_SOURCE
#define __OHOS__
#define __aarch64__
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <threads.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>
static int scenario, allocated, released, regular_unmapped, closed, unrefs;
static size_t low_size;
static char map_failure[512], diagnostic[512];
static int test_fstat(int fd, struct stat *st) {
    (void)fd; (void)st; errno=EACCES; return -1;
}
static void test_log(void *instance, const char *format, ...) {
    (void)instance;
    char message[512]; va_list args;
    va_start(args,format);vsnprintf(message,sizeof(message),format,args);va_end(args);
    if(strstr(format,"failed to mmap"))strcpy(map_failure,message);
    if(strstr(format,"winehua venus map:"))strcpy(diagnostic,message);
}
static void *low_alloc(int fd, size_t size) {
    if(scenario==4||scenario==7)return NULL;
    ++allocated;low_size=size;
    void *p=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_32BIT,fd,0);
    assert(p!=MAP_FAILED && (uintptr_t)p+size<0x80000000);return p;
}
static void low_release(void *p) {++released;assert(munmap(p,low_size)==0);}
static void *test_dlopen(const char *name,int flags) {
    assert(!strcmp(name,"ntdll.so")&&(flags&RTLD_NOLOAD));return scenario==2?NULL:(void*)1;
}
static void *test_dlsym(void *lib,const char *name) {
    assert(lib==(void*)1);
    if(strstr(name,"unmap"))return scenario==3?NULL:(void*)low_release;
    return (void*)low_alloc;
}
static int test_dlclose(void *lib){assert(lib==(void*)1);return 0;}
static int test_munmap(void *p,size_t size){++regular_unmapped;return munmap(p,size);}
static int test_close(int fd){++closed;return close(fd);}
#define dlopen test_dlopen
#define dlsym test_dlsym
#define dlclose test_dlclose
#define munmap test_munmap
#define close test_close
#define fstat test_fstat
#define os_get_option getenv
#define vn_log test_log
#define VCMD_BLOB_FLAG_MAPPABLE 1
#define VCMD_BLOB_FLAG_SHAREABLE 2
struct vn_renderer{int unused;};
struct vn_renderer_bo{void *mmap_ptr;size_t mmap_size;unsigned res_id;};
struct vtest_bo{struct vn_renderer_bo base;uint32_t blob_flags;int res_fd;void(*winehua_map_release)(void*);};
struct vtest{struct vn_renderer base;void *instance;mtx_t sock_mutex;};
static void vtest_vcmd_resource_unref(struct vtest *v,unsigned id){(void)v;assert(id==99);++unrefs;}
'''
code+=helpers+method('static void *\nvtest_bo_map(')+'\n'+method('static bool\nvtest_bo_destroy(')+r'''
int main(int argc,char **argv){
    assert(argc==2);scenario=atoi(argv[1]);
    setenv("WINEHUA_VENUS_LOW_MAP",scenario==1?"0":"1",1);
    setenv("WINEHUA_VENUS_MAP_DIAG",scenario==6||scenario==8?"1":"0",1);
    struct vtest v={0};mtx_init(&v.sock_mutex,mtx_plain);
    int fd=memfd_create("ownership-test",0);assert(fd>=0&&ftruncate(fd,4096)==0);
    void *alias=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);assert(alias!=MAP_FAILED);
    struct vtest_bo bo={.base={.mmap_size=4096,.res_id=99},.blob_flags=1,.res_fd=fd};
    if(scenario==5){bo.blob_flags=0;}
    if(scenario==6){test_close(fd);closed=0;bo.res_fd=-1;}
    void *p=vtest_bo_map(&v.base,&bo.base);
    if(scenario==6)assert(strstr(map_failure,strerror(EBADF)));
    if(scenario==8)assert(strstr(diagnostic,"wine_owned=1")&&strstr(diagnostic,"fstat_errno=13"));
    if(scenario==5||scenario==6){assert(!p&&!bo.winehua_map_release&&!allocated);}
    else {
        assert(p);((unsigned*)p)[0]=0x87654321;assert(((unsigned*)alias)[0]==0x87654321);
        ((unsigned*)alias)[1]=0xabcdef12;assert(((unsigned*)p)[1]==0xabcdef12);
        assert(vtest_bo_map(&v.base,&bo.base)==p);
        if(scenario==0||scenario==8)assert(allocated==1&&bo.winehua_map_release);
        else assert(allocated==0&&!bo.winehua_map_release);
    }
    /* A later environment change must never change the allocator owner. */
    setenv("WINEHUA_VENUS_LOW_MAP",scenario==0?"0":"1",1);
    assert(vtest_bo_destroy(&v.base,&bo.base));
    assert(!bo.base.mmap_ptr&&!bo.winehua_map_release&&unrefs==1);
    assert(closed==(scenario==6?0:1));
    assert(released==((scenario==0||scenario==8)?1:0));
    assert(regular_unmapped==((scenario!=0&&scenario!=8&&scenario!=5&&scenario!=6)?1:0));
    munmap(alias,4096);mtx_destroy(&v.sock_mutex);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d)
    (p/'test.c').write_text(code)
    subprocess.run(['cc','-std=gnu11','-O2','-pthread',str(p/'test.c'),'-o',str(p/'test')],check=True)
    for i in range(9):subprocess.run([str(p/'test'),str(i)],check=True)
print('PASS: production map/destroy: shared writes, cached map, owned release, setting changes, disabled, missing library/symbol, failed allocator, nonmappable, mmap failure, allocator rejection, denied fstat and original mmap error')
