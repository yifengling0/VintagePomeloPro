"""Exercise production Wine/Mesa mapping with real shared pages, not memcpy stubs.

Only the Wine address allocator and vtest protocol/platform boundary are stubs.
Mesa source comes from its pinned commit plus both registered overlays. Wine
source comes from the normal overlay replay, including explicit-flush fixes.
"""
import os
from pathlib import Path
import re
import subprocess
import unittest

import wine_shm_state_cache_test as base

ROOT = Path(__file__).resolve().parents[1]

STUBS = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
typedef wchar_t WCHAR;
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
static int wcsnicmp(const WCHAR *a,const WCHAR *b,size_t size) {
    while(size--) { int x=towupper(*a++),y=towupper(*b++);if(x!=y) return x-y;if(!x) break; }
    return 0;
}
typedef size_t SIZE_T;
typedef uintptr_t ULONG_PTR;
typedef int NTSTATUS;
#define DECLSPEC_EXPORT
#define ERR(...) ((void)0)
#define MEM_RELEASE 1
#define MEM_RESERVE 2
#define MEM_COMMIT 4
#define PAGE_READWRITE 1
#define NtCurrentProcess() ((void *)-1)
#define ROUND_SIZE(addr,size,mask) (((size)+(mask))&~(mask))
static size_t host_page_mask;
static int wow64=1, deny_alloc, wrong_high, allocs, frees;
static void *owned[1024];
static size_t owned_size[1024];
static pthread_mutex_t vm_lock=PTHREAD_MUTEX_INITIALIZER;
static int is_wow64(void) { return wow64; }
static NTSTATUS NtAllocateVirtualMemory(void *process,void **ptr,uintptr_t bits,
                                       size_t *size,unsigned type,unsigned prot) {
    assert(bits==1 && type==(MEM_RESERVE|MEM_COMMIT) && prot==PAGE_READWRITE);
    if (deny_alloc) return -1;
    *ptr=mmap(NULL,*size,PROT_READ|PROT_WRITE,
              MAP_PRIVATE|MAP_ANONYMOUS|(wrong_high?0:MAP_32BIT),-1,0);
    if (*ptr==MAP_FAILED) return -1;
    pthread_mutex_lock(&vm_lock);
    size_t i;for(i=0;i<1024;i++) if (!owned[i]) break;
    assert(i<1024);owned[i]=*ptr;owned_size[i]=*size;allocs++;
    pthread_mutex_unlock(&vm_lock);return 0;
}
static NTSTATUS NtFreeVirtualMemory(void *process,void **ptr,size_t *size,unsigned type) {
    assert(type==MEM_RELEASE && !*size);
    pthread_mutex_lock(&vm_lock);
    size_t i;for(i=0;i<1024;i++) if(owned[i]==*ptr) break;
    assert(i<1024);assert(!munmap(*ptr,owned_size[i]));owned[i]=NULL;frees++;
    pthread_mutex_unlock(&vm_lock);return 0;
}
'''

MESA_STUBS = r'''
typedef pthread_once_t once_flag;
#define ONCE_FLAG_INIT PTHREAD_ONCE_INIT
#define call_once(flag,fn) pthread_once(flag,fn)
#define RTLD_NOW 2
#define RTLD_NOLOAD 4
static unsigned lookups, dlopens, dlcloses, ordinary_unmaps, unrefs;
static int absent_library, absent_release;
static void *fake_dlopen(const char *name,int flags) {
    assert(!strcmp(name,"ntdll.so") && flags==(RTLD_NOW|RTLD_NOLOAD));
    dlopens++;return absent_library?NULL:(void *)1;
}
static void *fake_dlsym(void *handle,const char *name) {
    assert(handle==(void *)1);lookups++;
    if (!strcmp(name,"winehua_map_shared_buffer_v1")) return winehua_map_shared_buffer_v1;
    assert(!strcmp(name,"winehua_unmap_shared_buffer_v1"));
    return absent_release?NULL:winehua_unmap_shared_buffer_v1;
}
static void fake_dlclose(void *handle) { assert(handle==(void *)1);dlcloses++; }
#define dlopen fake_dlopen
#define dlsym fake_dlsym
#define dlclose fake_dlclose
#define VIRGL_BIND_DISPLAY_TARGET 1
#define VIRGL_BIND_SCANOUT 2
#define VIRGL_RESOURCE_FLAG_MAP_PERSISTENT 1
#define VIRGL_RESOURCE_FLAG_MAP_COHERENT 2
#define PIPE_MAP_READ_WRITE 3
#define CALLOC_STRUCT(type) calloc(1,sizeof(struct type))
#define FREE free
#define ALIGN(v,a) (((v)+(a)-1)&~((a)-1))
#define p_atomic_set(ptr,v) (*(ptr)=(v))
#define os_mmap mmap
static int counted_munmap(void *ptr,size_t size) { ordinary_unmaps++;return munmap(ptr,size); }
#define os_munmap counted_munmap
enum pipe_texture_target { PIPE_BUFFER,PIPE_TEXTURE_2D };
struct pipe_reference { int count; };
static void pipe_reference_init(struct pipe_reference *p,int n) { p->count=n; }
struct virgl_resource_cache_entry { int unused; };
struct virgl_resource_params {
    unsigned size,bind,format,flags,nr_samples,width,height,depth,array_size,last_level,target;
};
struct sw_displaytarget { int unused; };
struct sw_winsys {
    struct sw_displaytarget *(*displaytarget_create)(void *,unsigned,unsigned,unsigned,unsigned,
                                                    unsigned,const void *,unsigned *);
    void *(*displaytarget_map)(void *,struct sw_displaytarget *,unsigned);
    void (*displaytarget_destroy)(void *,struct sw_displaytarget *);
};
struct virgl_winsys { int unused; };
struct virgl_vtest_winsys { struct virgl_winsys base; struct sw_winsys *sws; unsigned protocol_version; };
static struct virgl_vtest_winsys *virgl_vtest_winsys(struct virgl_winsys *v) { return (void *)v; }
static void *align_malloc(size_t size,unsigned alignment) { return malloc(size); }
static void align_free(void *ptr) { free(ptr); }
static unsigned pipe_to_virgl_format(unsigned format) { return format; }
static int backing_fd,received_fd;
static int virgl_vtest_send_resource_create(struct virgl_vtest_winsys *vtws,int handle,
    unsigned target,unsigned format,unsigned bind,unsigned width,unsigned height,unsigned depth,
    unsigned array,unsigned level,unsigned samples,unsigned size,int *fd) {
    if(vtws->protocol_version>=2 && size) { *fd=dup(backing_fd);received_fd=*fd;assert(*fd>=0); }
    return handle;
}
static int virgl_vtest_winsys_resource_create_blob(struct virgl_winsys *vws,unsigned target,
    unsigned format,unsigned bind,unsigned width,unsigned height,unsigned depth,unsigned array,
    unsigned level,unsigned samples,unsigned flags,unsigned size,int *fd) {
    *fd=dup(backing_fd);received_fd=*fd;assert(*fd>=0);return 99;
}
static void virgl_vtest_send_resource_unref(struct virgl_vtest_winsys *v,unsigned handle) { unrefs++; }
static void virgl_resource_cache_entry_init(struct virgl_resource_cache_entry *e,struct virgl_resource_params p) {}
static unsigned util_format_get_stride(unsigned f,unsigned w) { return w; }
static void util_copy_rect(void *dst,unsigned format,unsigned stride,int x,int y,
                           unsigned w,unsigned h,void *src,unsigned src_stride,int sx,int sy) {}
struct pipe_box { int unused; };
static void u_box_2d(int x,int y,unsigned w,unsigned h,struct pipe_box *box) {}
static int virgl_vtest_transfer_put(struct virgl_winsys *vws,void *res,const struct pipe_box *box,
                                   unsigned stride,unsigned layer,unsigned offset,unsigned level) { return 0; }
'''

WINE_MAIN = r'''
static int shared_fd(size_t size) {
    int fd=memfd_create("virgl-production-shared-map",MFD_CLOEXEC);
    assert(fd>=0 && !ftruncate(fd,size));return fd;
}
static void *thread_run(void *unused) {
    int fd=shared_fd(4096);
    unsigned char *peer=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    assert(peer!=MAP_FAILED);
    for(unsigned i=0;i<100;i++) {
        unsigned char *p=winehua_map_shared_buffer_v1(fd,4096);
        assert(p && (uintptr_t)p<0x80000000);
        p[123]=(unsigned char)i;assert(peer[123]==(unsigned char)i);
        winehua_unmap_shared_buffer_v1(p);
    }
    assert(!munmap(peer,4096));close(fd);return NULL;
}
int main(int argc,char **argv) {
    assert(argc==2);host_page_mask=getpagesize()-1;
    int fd=shared_fd(8192);
    if (!strcmp(argv[1],"gates")) {
        assert(!winehua_map_shared_buffer_v1(fd,4096) && !allocs);
        setenv("WINEHUA_VIRGL_LOW_MAP","0",1);assert(!winehua_map_shared_buffer_v1(fd,4096));
        setenv("WINEHUA_VIRGL_LOW_MAP","1x",1);assert(!winehua_map_shared_buffer_v1(fd,4096));
        setenv("WINEHUA_VIRGL_LOW_MAP","1",1);wow64=0;assert(!winehua_map_shared_buffer_v1(fd,4096));wow64=1;
        assert(!winehua_map_shared_buffer_v1(-1,4096));assert(!winehua_map_shared_buffer_v1(fd,0));
        assert(!winehua_map_shared_buffer_v1(fd,SIZE_MAX));assert(!winehua_map_shared_buffer_v1(fd,0x7fff0000));
        assert(!allocs);
    } else if (!strcmp(argv[1],"environment")) {
        setenv("WINEHUA_VIRGL_LOW_MAP","0",1);
        winehua_init_shared_map_environment(L"OTHER=1\0PATH=bad\0");
        assert(!strcmp(getenv("WINEHUA_VIRGL_LOW_MAP"),"1"));
        void *default_map=winehua_map_shared_buffer_v1(fd,4096);
        assert(default_map && (uintptr_t)default_map<0x80000000);
        winehua_unmap_shared_buffer_v1(default_map);
        winehua_init_shared_map_environment(L"winehua_virgl_low_map=1\0OTHER=0\0");
        assert(!strcmp(getenv("WINEHUA_VIRGL_LOW_MAP"),"1"));
        winehua_init_shared_map_environment(L"WINEHUA_VIRGL_LOW_MAP=0\0");
        assert(!strcmp(getenv("WINEHUA_VIRGL_LOW_MAP"),"0"));
        assert(!winehua_map_shared_buffer_v1(fd,4096));
        winehua_init_shared_map_environment(L"WINEHUA_VIRGL_LOW_MAP=1x\0");
        assert(!strcmp(getenv("WINEHUA_VIRGL_LOW_MAP"),"0"));
        winehua_init_shared_map_environment(L"WINEHUA_VIRGL_LOW_MAP=\0");
        assert(!strcmp(getenv("WINEHUA_VIRGL_LOW_MAP"),"0"));
        winehua_init_shared_map_environment(NULL);
        assert(!strcmp(getenv("WINEHUA_VIRGL_LOW_MAP"),"1"));
        wow64=0;assert(!winehua_map_shared_buffer_v1(fd,4096));wow64=1;
        assert(allocs==frees);
    } else {
        setenv("WINEHUA_VIRGL_LOW_MAP","1",1);
        if (!strcmp(argv[1],"shared")) {
            unsigned char *peer=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
            assert(peer!=MAP_FAILED);peer[1]=0xa5;peer[8191]=0x37;
            unsigned char *p=winehua_map_shared_buffer_v1(fd,8191);
            assert(p && (uintptr_t)p+8192<=0x80000000 && p!=peer && p[1]==0xa5 && p[8191]==0x37);
            /* No initial/return copy, both views access the SAME backing. */
            close(fd);fd=-1;p[7]=0x42;assert(peer[7]==0x42);peer[8190]=0x73;assert(p[8190]==0x73);
            winehua_unmap_shared_buffer_v1(p);assert(!munmap(peer,8192));
        } else if (!strcmp(argv[1],"failures")) {
            deny_alloc=1;assert(!winehua_map_shared_buffer_v1(fd,4096) && !allocs);deny_alloc=0;
            wrong_high=1;assert(!winehua_map_shared_buffer_v1(fd,4096));wrong_high=0;assert(allocs==frees);
            int closed=dup(fd);close(closed);
            assert(!winehua_map_shared_buffer_v1(closed,4096));assert(allocs==frees);
            /* The callback never consumes the caller's fd on failure. */
            assert(fcntl(fd,F_GETFD)>=0);
            void *p=winehua_map_shared_buffer_v1(fd,4096);assert(p);winehua_unmap_shared_buffer_v1(p);
        } else {
            assert(!strcmp(argv[1],"threads"));pthread_t threads[4];
            for(unsigned i=0;i<4;i++) assert(!pthread_create(&threads[i],NULL,thread_run,NULL));
            for(unsigned i=0;i<4;i++) assert(!pthread_join(threads[i],NULL));
            assert(allocs==400);
        }
    }
    winehua_unmap_shared_buffer_v1(NULL);if(fd>=0) close(fd);
    assert(allocs==frees);puts("Wine shared-map production PASS");
}
'''

MESA_MAIN = r'''
int main(int argc,char **argv) {
    assert(argc==2);host_page_mask=getpagesize()-1;backing_fd=memfd_create("vtest-test",MFD_CLOEXEC);
    assert(backing_fd>=0 && !ftruncate(backing_fd,8192));
    enum pipe_texture_target target=PIPE_BUFFER;
    unsigned protocol=2, flags=0,size=8192;
    bool expect_low=true;
    if (!strcmp(argv[1],"off")) expect_low=false;
    else setenv("WINEHUA_VIRGL_LOW_MAP","1",1);
    if (!strcmp(argv[1],"texture")) { target=PIPE_TEXTURE_2D;expect_low=false; }
    if (!strcmp(argv[1],"no-library")) { absent_library=1;expect_low=false; }
    if (!strcmp(argv[1],"old-wine")) { absent_release=1;expect_low=false; }
    if (!strcmp(argv[1],"low-exhausted")) { deny_alloc=1;expect_low=false; }
    if (!strcmp(argv[1],"native64")) { wow64=0;expect_low=false; }
    if (!strcmp(argv[1],"v0")) { protocol=0;expect_low=false; }
    if (!strcmp(argv[1],"zero")) { size=0;expect_low=false; }
    if (!strcmp(argv[1],"persistent")) flags=VIRGL_RESOURCE_FLAG_MAP_PERSISTENT;
    struct virgl_vtest_winsys ws={.protocol_version=protocol};
    struct virgl_hw_res *res=virgl_vtest_winsys_resource_create(&ws.base,target,NULL,0,4,8192,
                                                              1,1,1,0,0,flags,size);
    assert(res);
    if (size) {
        if (protocol>=2) { assert(fcntl(received_fd,F_GETFD)==-1 && errno==EBADF); }
        assert(!!res->winehua_map_release==expect_low);
        if(expect_low) {
            assert((uintptr_t)res->ptr+size<=0x80000000);
            unsigned char *peer=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_SHARED,backing_fd,0);
            assert(peer!=MAP_FAILED);((unsigned char *)res->ptr)[81]=0x42;assert(peer[81]==0x42);
            peer[6000]=0xf5;assert(((unsigned char *)res->ptr)[6000]==0xf5);munmap(peer,8192);
            assert(allocs==1 && !frees);
        }
    } else assert(!res->ptr && !allocs);
    /* A setting change after creation must not change how the resource dies. */
    setenv("WINEHUA_VIRGL_LOW_MAP","0",1);virgl_hw_res_destroy(&ws,res);
    assert(allocs==frees && unrefs==1);
    assert(ordinary_unmaps==((protocol>=2 && size && !expect_low)?1:0));
    if (lookups) assert(lookups==2 && dlcloses==1);
    close(backing_fd);puts("Mesa create/destroy production PASS");
}
'''

DYNAMIC_MAIN = r'''
int main(int argc,char **argv) {
    assert(argc==2);setenv("WINEHUA_VIRGL_LOW_MAP","1",1);
    void *library=NULL;
    if (strcmp(argv[1],"absent")) {
        library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);assert(library);
    }
    bool direct=library && !!dlsym(library,"winehua_unmap_shared_buffer_v1");
    backing_fd=memfd_create("dynamic-low-map",MFD_CLOEXEC);
    assert(backing_fd>=0 && !ftruncate(backing_fd,8192));
    struct virgl_vtest_winsys ws={.protocol_version=2};
    struct virgl_hw_res *res=virgl_vtest_winsys_resource_create(&ws.base,PIPE_BUFFER,NULL,0,4,
                                                              8192,1,1,1,0,0,0,8192);
    assert(res && !!res->winehua_map_release==direct);
    assert(fcntl(received_fd,F_GETFD)==-1 && errno==EBADF);
    unsigned char *peer=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_SHARED,backing_fd,0);
    assert(peer!=MAP_FAILED);
    if (direct) assert((uintptr_t)res->ptr+8192<=0x80000000);
    ((unsigned char *)res->ptr)[1023]=0x71;assert(peer[1023]==0x71);
    virgl_hw_res_destroy(&ws,res);munmap(peer,8192);close(backing_fd);
    if (library) {
        int (*balance)(void)=dlsym(library,"winehua_test_balance");assert(balance && !balance());
        dlclose(library);
    }
    puts("Real RTLD_LOCAL / NOLOAD ABI discovery PASS");
}
'''


class SharedLowMapTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        base.WineShmStateCacheTest.setUpClass.__func__(cls)
        wine = cls.first_replay['dlls/ntdll/unix/virtual.c'].decode()
        start = wine.index('/* Private native ABI,')
        helpers = wine[start:wine.index('\n#endif', start)]
        env_source = cls.first_replay['dlls/ntdll/unix/env.c'].decode()
        env_helper = base.function(env_source, 'static void winehua_init_shared_map_environment(')
        default_helper = base.function(env_source, 'static void winehua_default_shared_map_environment(')
        env_helper = env_helper.replace('winehua_init_shared_map_environment(', 'winehua_import_shared_map_environment(', 1)
        env_helper += '\n' + default_helper + '''
static void winehua_init_shared_map_environment(const WCHAR *env) {
    winehua_import_shared_map_environment(env);
    winehua_default_shared_map_environment();
}
'''
        # Use only the files touched by registered Mesa overlays, from its pin.
        patch_script = (ROOT / 'scripts/apply_mesa_ohos_patches.sh').read_text()
        patches = [ROOT / 'patches/mesa' / name for name in re.findall(
            r'\$SCRIPT_DIR/\.\./patches/mesa/([^"\n]+)', patch_script)]
        files, added = set(), set()
        for patch in patches:
            files.update(re.findall(r'^\+\+\+ b/(.+)$', patch.read_text(), re.M))
            added.update(re.findall(r'^--- /dev/null\n\+\+\+ b/(.+)$', patch.read_text(), re.M))
        tree = cls.folder / 'mesa'
        tree.mkdir()
        (tree / 'meson.build').write_text('# disposable pinned overlay fixture\n')
        for relative in files - added:
            path = tree / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(subprocess.check_output(['git', 'show', 'HEAD:' + relative],
                                                     cwd=ROOT / 'thirdparty/mesa'))
        subprocess.run(['git', 'init', '-q', str(tree)], check=True)
        command = ['bash', str(ROOT / 'scripts/apply_mesa_ohos_patches.sh'), str(tree)]
        subprocess.run(command, check=True, capture_output=True)
        cls.mesa_first = {r: (tree / r).read_bytes() for r in files}
        subprocess.run(command, check=True, capture_output=True)
        cls.mesa_second = {r: (tree / r).read_bytes() for r in files}
        relative = 'src/gallium/winsys/virgl/vtest/virgl_vtest_winsys'
        source = cls.mesa_first[relative + '.c'].decode()
        header = cls.mesa_first[relative + '.h'].decode()
        begin = header.index('struct virgl_hw_res {')
        resource = header[begin:header.index('\n};', begin) + 4]
        begin = source.index('static once_flag winehua_low_map_once')
        mesa_helpers = source[begin:source.index('static inline bool can_cache', begin)]
        # Keep native platform branches, omit only their enclosing initial #if.
        mesa_helpers = mesa_helpers.replace('\n#endif\n\nstatic bool', '\n\nstatic bool', 1)
        bodies = ''.join(base.function(source, signature) for signature in (
            'static struct virgl_hw_res *\nvirgl_vtest_winsys_resource_create(',
            'static void virgl_hw_res_destroy('))
        cls.binaries = {}
        for kind, content in (
            ('wine', STUBS + helpers + env_helper + WINE_MAIN),
            ('mesa', STUBS + helpers + MESA_STUBS + resource + mesa_helpers + bodies + MESA_MAIN)):
            path = cls.folder / (kind + '-shared-map.c')
            path.write_text(content)
            binary = path.with_suffix('')
            subprocess.run(['gcc', '-D__OHOS__', '-D__aarch64__', '-D_WIN64', '-std=c11',
                '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unused-function',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-pthread', str(path),
                '-o', str(binary)], check=True)
            cls.binaries[kind] = binary
        # Also validate the actual dynamic loader and export visibility. ntdll
        # is local, paired v1 symbols are optional, and NOLOAD must not load it.
        cls.libraries = []
        for variant in ('new', 'old'):
            path = cls.folder / (variant + '-ntdll.c')
            library_helpers = helpers
            if variant == 'old':
                library_helpers = library_helpers.replace('winehua_unmap_shared_buffer_v1', 'old_release')
            library_stubs = STUBS.replace('#define DECLSPEC_EXPORT',
                '#define DECLSPEC_EXPORT __attribute__((visibility("default")))')
            library_stubs = library_stubs.replace('static size_t host_page_mask;',
                                                  'static size_t host_page_mask=4095;')
            path.write_text(library_stubs + library_helpers +
                '__attribute__((visibility("default"))) int winehua_test_balance(void) { return allocs-frees; }\n')
            library = cls.folder / (variant + '-ntdll.so')
            subprocess.run(['gcc', '-std=c11', '-O2', '-fPIC', '-shared', '-fvisibility=hidden',
                '-Wl,-soname,ntdll.so', '-pthread', str(path), '-o', str(library)], check=True)
            cls.libraries.append(library)
        dynamic_stubs = re.sub(r'^#define (dlopen|dlsym|dlclose|RTLD_NOW|RTLD_NOLOAD) .+\n', '',
                               MESA_STUBS, flags=re.M)
        path = cls.folder / 'dynamic-mesa.c'
        path.write_text(STUBS + '#include <dlfcn.h>\n' + helpers + dynamic_stubs + resource +
                        mesa_helpers + bodies + DYNAMIC_MAIN)
        cls.dynamic_binary = path.with_suffix('')
        subprocess.run(['gcc', '-D__OHOS__', '-D__aarch64__', '-std=c11', '-O2', '-pthread',
                        str(path), '-ldl', '-o', str(cls.dynamic_binary)], check=True)

    def run_case(self, kind, case):
        environment = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:halt_on_error=1')
        environment.pop('WINEHUA_VIRGL_LOW_MAP', None)
        result = subprocess.run([str(self.binaries[kind]), case], capture_output=True,
                                text=True, env=environment, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_default_native64_and_size_gates(self): self.run_case('wine', 'gates')
    def test_bat_child_environment_is_imported_before_mesa(self):
        self.run_case('wine', 'environment')
        source = self.first_replay['dlls/ntdll/unix/env.c'].decode()
        init = base.function(source, 'static void init_peb(')
        self.assertLess(init.index('winehua_init_shared_map_environment('), init.index('peb->'))
        self.assertLess(init.index('winehua_init_shared_map_environment('),
                        init.index('winehua_default_shared_map_environment('))
        self.assertLess(init.index('winehua_default_shared_map_environment('),
                        init.index('NtCurrentTeb()->WowTebOffset'))
    def test_same_pages_bidirectional_and_after_fd_close(self): self.run_case('wine', 'shared')
    def test_owned_reservation_released_on_failures(self): self.run_case('wine', 'failures')
    def test_parallel_map_release_cycles(self): self.run_case('wine', 'threads')
    def test_mesa_real_create_destroy(self): self.run_case('mesa', 'direct')
    def test_mesa_persistent_lifetime(self): self.run_case('mesa', 'persistent')
    def test_mesa_old_peer_and_low_exhaustion_fallback(self):
        for case in ('no-library', 'old-wine', 'low-exhausted'):
            with self.subTest(case=case): self.run_case('mesa', case)
    def test_mesa_opt_in_scope_and_zero_size(self):
        for case in ('off', 'texture', 'native64', 'v0', 'zero'):
            with self.subTest(case=case): self.run_case('mesa', case)
    def test_both_overlay_chains_replay_twice(self):
        self.assertEqual(self.first_replay, self.second_replay)
        self.assertEqual(self.mesa_first, self.mesa_second)
    def test_actual_rtld_local_noload_and_paired_exports(self):
        for library in (*self.libraries, 'absent'):
            with self.subTest(library=str(library)):
                result = subprocess.run([str(self.dynamic_binary), str(library)],
                    capture_output=True, text=True, timeout=15,
                    env=dict(os.environ, LD_LIBRARY_PATH=str(self.folder)))
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__': unittest.main(argv=[__file__] + base.remaining)
