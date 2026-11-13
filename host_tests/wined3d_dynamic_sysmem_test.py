"""Replay overlays and exercise real Wine buffer location / partial map code."""
import os
import subprocess
import unittest

import wine_shm_state_cache_test as base

STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef int BOOL, HRESULT, LONG;
typedef unsigned DWORD;
#define TRUE 1
#define FALSE 0
#define TRACE(...) ((void)0)
#define TRACE_(channel) TRACE
#define WARN(...) ((void)0)
#define ERR(...) ((void)0)
#define FAILED(h) ((h)<0)
#define E_OUTOFMEMORY -1
#define E_INVALIDARG -2
#define WINED3D_OK 0
#define WINED3DUSAGE_DYNAMIC 0x200
#define WINED3DUSAGE_CS 0x10000000u
#define WINED3DUSAGE_MANAGED 0x20
#define WINED3DCREATE_SOFTWARE_VERTEXPROCESSING 0x20
#define WINED3D_BIND_CONSTANT_BUFFER 4
#define WINED3D_CONSTANT_BUFFER_ALIGNMENT 16
#define WINED3DFMT_R8_UNORM 1
#define WINED3D_RTYPE_BUFFER 1
#define WINED3D_MULTISAMPLE_NONE 0
#define CLIENT_BO_DISCARDED ((void *)-1)
#define WINED3D_RESOURCE_ACCESS_GPU 1
#define WINED3D_RESOURCE_ACCESS_MAP_W 8
#define WINED3D_BIND_VERTEX_BUFFER 1
#define WINED3D_BIND_INDEX_BUFFER 2
#define WINED3D_BUFFER_USE_BO 2
#define WINED3D_MAP_WRITE 0x40000000u
#define WINED3D_MAP_READ 0x80000000u
#define WINED3D_MAP_NOOVERWRITE 0x1000
#define WINED3D_MAP_DISCARD 0x2000
#define WINED3D_LOCATION_SYSMEM 1
#define WINED3D_LOCATION_BUFFER 2
#define WINED3D_LOCATION_DISCARDED 4
#define WINED3D_LOCATION_CLEARED 8
#define RESOURCE_ALIGNMENT 16
typedef uintptr_t DWORD_PTR;
struct wined3d_range { unsigned offset, size; };
struct wined3d_bo_address { void *buffer_object; uint8_t *addr; };
struct wined3d_d3d_info { int persistent_map; };
struct wined3d_adapter { struct wined3d_d3d_info d3d_info; };
struct wined3d_device { struct wined3d_adapter *adapter; struct { unsigned flags; } create_parms; };
struct wined3d_resource {
    struct wined3d_device *device; unsigned size, usage, map_count, pin_sysmem;
    uint8_t *heap_memory; struct { struct wined3d_bo_address addr; } client;
};
struct wined3d_buffer {
    struct wined3d_resource resource; unsigned locations, flags;
    uint8_t *map_ptr; void *buffer_object;
    unsigned dirty_range_count; size_t dirty_ranges_capacity;
    struct wined3d_range *dirty_ranges;
    unsigned structure_byte_stride; const struct wined3d_buffer_ops *buffer_ops;
};
struct wined3d_buffer_desc { unsigned usage, access, bind_flags, byte_width, structure_byte_stride; };
struct wined3d_box { unsigned left, right; };
struct wined3d_context { struct wined3d_d3d_info *d3d_info; };
struct wined3d_gl_info { int supported[3]; };
enum { ARB_VERTEX_BUFFER_OBJECT, APPLE_FLUSH_BUFFER_RANGE, ARB_MAP_BUFFER_RANGE };
struct wined3d_adapter_gl { struct wined3d_gl_info gl_info; };
static struct wined3d_adapter_gl gl_adapter={{{1,0,1}}};
static struct wined3d_adapter_gl *wined3d_adapter_gl(struct wined3d_adapter *a) { return &gl_adapter; }
struct wined3d_buffer_gl { struct wined3d_buffer b; };
struct wined3d_sub_resource_data { const void *data; };
struct wined3d_parent_ops { int unused; };
struct wined3d_buffer_ops { int unused; };
static const struct wined3d_buffer_ops wined3d_buffer_gl_ops;
static int init_result;
static unsigned init_data_calls;
static unsigned expect_initial_pin;
struct wined3d_format { int unused; };
static struct wined3d_format format;
static struct { bool cb_access_map_w; } wined3d_settings;
static const int buffer_resource_ops;
static const struct wined3d_format *wined3d_get_format(struct wined3d_adapter *a, unsigned f, unsigned b) { return &format; }
static HRESULT resource_init(struct wined3d_resource *r, struct wined3d_device *d, ...) {
    r->size=128; r->device=d; r->usage=WINED3DUSAGE_DYNAMIC;
    return init_result;
}
static void resource_cleanup(struct wined3d_resource *r) {}
static void wined3d_resource_wait_idle(struct wined3d_resource *r) {}
static void buffer_resource_unload(struct wined3d_resource *r) {}
static void wined3d_buffer_init_data(struct wined3d_buffer *b, struct wined3d_device *d,
        const struct wined3d_sub_resource_data *data) {
    init_data_calls++; assert(b->resource.pin_sysmem==expect_initial_pin);
}
static struct wined3d_context ctx;
static uint8_t gpu[128];
static unsigned mapped, uploaded, downloaded, last_offset, last_size;
static int wow64 = 1, wow64_query_ok = 1;
static struct wined3d_buffer *buffer_from_resource(struct wined3d_resource *r) { return (void *)r; }
static unsigned GetEnvironmentVariableA(const char *key, char *out, unsigned capacity) {
    const char *v=getenv(key); if (!v) return 0;
    unsigned size=strlen(v); if (size>=capacity) return size+1;
    memcpy(out,v,size+1); return size;
}
static void *GetCurrentProcess(void) { return NULL; }
static BOOL IsWow64Process(void *p, BOOL *value) { *value=wow64; return wow64_query_ok; }
static struct wined3d_context *context_acquire(struct wined3d_device *d, void *t, unsigned s) {
    ctx.d3d_info=&d->adapter->d3d_info; return &ctx;
}
static void context_release(struct wined3d_context *c) {}
static BOOL wined3d_array_reserve(void **array, size_t *capacity, size_t size, size_t element) {
    if (size>*capacity) { *array=realloc(*array,size*element); *capacity=size; }
    return *array!=NULL;
}
static BOOL wined3d_buffer_prepare_location(struct wined3d_buffer *b, struct wined3d_context *c, unsigned l) {
    if (l==WINED3D_LOCATION_SYSMEM && !b->resource.heap_memory)
        b->resource.heap_memory=malloc(b->resource.size);
    return TRUE;
}
static BOOL wined3d_resource_prepare_sysmem(struct wined3d_resource *r) {
    if (!r->heap_memory) r->heap_memory=malloc(r->size); return r->heap_memory!=NULL;
}
static void wined3d_resource_free_sysmem(struct wined3d_resource *r) { free(r->heap_memory); r->heap_memory=NULL; }
static void wined3d_buffer_acquire_bo_for_write(struct wined3d_buffer *b, struct wined3d_context *c) {}
static void wined3d_buffer_drop_bo(struct wined3d_buffer *b) { abort(); }
static void *wined3d_context_map_bo_address(struct wined3d_context *c, const struct wined3d_bo_address *a,
        unsigned size, unsigned flags) { mapped++; return gpu; }
static void wined3d_context_unmap_bo_address(struct wined3d_context *c, const struct wined3d_bo_address *a,
        unsigned n, const struct wined3d_range *r) {}
static void wined3d_context_copy_bo_address(struct wined3d_context *c, const struct wined3d_bo_address *dst,
        const struct wined3d_bo_address *src, unsigned n, const struct wined3d_range *ranges, unsigned flags) {
    for (unsigned i=0;i<n;i++) {
        unsigned off=ranges[i].offset, size=ranges[i].size; assert(off+size<=sizeof(gpu));
        memcpy((dst->buffer_object?gpu:dst->addr)+off,(src->buffer_object?gpu:src->addr)+off,size);
        if (dst->buffer_object) { uploaded+=size; last_offset=off; last_size=size; } else downloaded+=size;
    }
}
'''

MAIN = r'''
int main(int argc,char **argv) {
    struct wined3d_adapter adapter={0}; struct wined3d_device device={.adapter=&adapter};
    struct wined3d_buffer_desc desc={.usage=WINED3DUSAGE_DYNAMIC,.access=9,.bind_flags=1,.byte_width=128};
    if (!strcmp(argv[1],"initial")) {
        setenv("WINEHUA_DYNAMIC_BUFFER_SYSMEM","1",1);
        struct wined3d_buffer_gl created={0};
        struct wined3d_sub_resource_data data={gpu};
#ifndef _WIN64
        expect_initial_pin=1;
#endif
        assert(!wined3d_buffer_gl_init(&created,&device,&desc,&data,NULL,NULL));
        assert(init_data_calls==1 && created.b.locations==WINED3D_LOCATION_CLEARED);
        free(created.b.dirty_ranges); puts("initial publication ordering PASS"); return 0;
    }
    if (!strcmp(argv[1],"policy")) {
        unsetenv("WINEHUA_DYNAMIC_BUFFER_SYSMEM"); assert(!winehua_dynamic_buffer_sysmem(&device,&desc));
        const char *bad[]={"0","","true","10","11"};
        for (unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) {
            setenv("WINEHUA_DYNAMIC_BUFFER_SYSMEM",bad[i],1); assert(!winehua_dynamic_buffer_sysmem(&device,&desc));
        }
        setenv("WINEHUA_DYNAMIC_BUFFER_SYSMEM","1",1);
        struct wined3d_buffer_gl created={0};
        init_result=-42;
        assert(wined3d_buffer_gl_init(&created,&device,&desc,NULL,NULL,NULL)==-42);
        assert(!created.b.resource.pin_sysmem);
        init_result=0;
        assert(!wined3d_buffer_gl_init(&created,&device,&desc,NULL,NULL,NULL));
        assert(created.b.flags&WINED3D_BUFFER_USE_BO);
        assert(created.b.locations==WINED3D_LOCATION_CLEARED);
        free(created.b.dirty_ranges);
#ifdef _WIN64
        assert(!winehua_dynamic_buffer_sysmem(&device,&desc));
        assert(!created.b.resource.pin_sysmem);
#else
        assert(winehua_dynamic_buffer_sysmem(&device,&desc));
        assert(created.b.resource.pin_sysmem);
        desc.bind_flags=2; assert(winehua_dynamic_buffer_sysmem(&device,&desc));
        desc.bind_flags=3; assert(winehua_dynamic_buffer_sysmem(&device,&desc));
        unsigned badbinds[]={0,4,8,1|8,2|16};
        for(unsigned i=0;i<sizeof(badbinds)/sizeof(*badbinds);i++) {
            desc.bind_flags=badbinds[i]; assert(!winehua_dynamic_buffer_sysmem(&device,&desc));
        }
        desc.bind_flags=1; adapter.d3d_info.persistent_map=1; assert(!winehua_dynamic_buffer_sysmem(&device,&desc));
        adapter.d3d_info.persistent_map=0; desc.usage=0; assert(!winehua_dynamic_buffer_sysmem(&device,&desc));
        desc.usage=WINED3DUSAGE_DYNAMIC; desc.access=1; assert(!winehua_dynamic_buffer_sysmem(&device,&desc));
        desc.access=8; assert(!winehua_dynamic_buffer_sysmem(&device,&desc));
        desc.access=9; wow64=0; assert(!winehua_dynamic_buffer_sysmem(&device,&desc));
        wow64=1; wow64_query_ok=0; assert(!winehua_dynamic_buffer_sysmem(&device,&desc));
#endif
        puts("policy PASS"); return 0;
    }
    struct wined3d_buffer b={.resource={.device=&device,.size=128,.usage=WINED3DUSAGE_DYNAMIC,.pin_sysmem=1},
        .locations=WINED3D_LOCATION_BUFFER,.flags=WINED3D_BUFFER_USE_BO,.buffer_object=gpu};
    memset(gpu,0x37,sizeof(gpu));
    struct wined3d_box box={16,20}; void *ptr;
    assert(!buffer_resource_sub_resource_map(&b.resource,0,&ptr,&box,WINED3D_MAP_WRITE|WINED3D_MAP_NOOVERWRITE));
    assert(downloaded==128 && !mapped); memset(ptr,0xa1,4);
    assert(!buffer_resource_sub_resource_unmap(&b.resource,0));
    assert(wined3d_buffer_load_location(&b,&ctx,WINED3D_LOCATION_BUFFER));
    assert(uploaded==4 && last_offset==16 && last_size==4 && gpu[16]==0xa1 && gpu[15]==0x37 && gpu[20]==0x37);
    box.left=80;box.right=83;
    assert(!buffer_resource_sub_resource_map(&b.resource,0,&ptr,&box,WINED3D_MAP_WRITE|WINED3D_MAP_NOOVERWRITE));
    memset(ptr,0xb2,3); assert(!buffer_resource_sub_resource_unmap(&b.resource,0));
    assert(wined3d_buffer_load_location(&b,&ctx,WINED3D_LOCATION_BUFFER));
    assert(uploaded==7 && downloaded==128 && !mapped && gpu[16]==0xa1 && gpu[80]==0xb2);
    /* Nested partial locks keep earlier pointers valid and publish both ranges. */
    box.left=24;box.right=26; assert(!buffer_resource_sub_resource_map(&b.resource,0,&ptr,&box,WINED3D_MAP_WRITE));
    uint8_t *first=ptr; box.left=96;box.right=98;
    assert(!buffer_resource_sub_resource_map(&b.resource,0,&ptr,&box,WINED3D_MAP_WRITE|WINED3D_MAP_NOOVERWRITE));
    memset(first,0xc3,2);memset(ptr,0xd4,2);
    assert(!buffer_resource_sub_resource_unmap(&b.resource,0)); assert(b.resource.map_count==1);
    assert(!buffer_resource_sub_resource_unmap(&b.resource,0));
    assert(wined3d_buffer_load_location(&b,&ctx,WINED3D_LOCATION_BUFFER));
    assert(uploaded==11 && gpu[24]==0xc3 && gpu[96]==0xd4 && !mapped);
    /* Existing invalidation reloads new GPU contents before a CPU read. */
    gpu[7]=0xee;wined3d_buffer_invalidate_location(&b,WINED3D_LOCATION_SYSMEM);
    box.left=0;box.right=128;
    assert(!buffer_resource_sub_resource_map(&b.resource,0,&ptr,&box,WINED3D_MAP_READ));
    assert(((uint8_t *)ptr)[7]==0xee && downloaded==256);
    assert(!buffer_resource_sub_resource_unmap(&b.resource,0));
    /* DISCARD continues publishing the complete buffer, as required upstream. */
    assert(!buffer_resource_sub_resource_map(&b.resource,0,&ptr,&box,WINED3D_MAP_WRITE|WINED3D_MAP_DISCARD));
    memset(ptr,0xf5,128);assert(!buffer_resource_sub_resource_unmap(&b.resource,0));
    assert(wined3d_buffer_load_location(&b,&ctx,WINED3D_LOCATION_BUFFER));
    assert(uploaded==139 && last_size==128 && gpu[0]==0xf5 && gpu[127]==0xf5 && !mapped);
    free(b.resource.heap_memory);free(b.dirty_ranges);puts("partial/nested/read/discard PASS");
}
'''


class DynamicSysmemTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        base.WineShmStateCacheTest.setUpClass.__func__(cls)
        source=cls.first_replay['dlls/wined3d/buffer.c'].decode()
        signatures=('static bool winehua_dynamic_buffer_sysmem(', 'static HRESULT wined3d_buffer_init(',
            'HRESULT wined3d_buffer_gl_init(',
            'static inline void buffer_clear_dirty_areas(', 'static BOOL buffer_is_fully_dirty(',
            'static void buffer_invalidate_bo_range(', 'void wined3d_buffer_validate_location(',
            'static void wined3d_buffer_invalidate_range(', 'void wined3d_buffer_invalidate_location(',
            'static void wined3d_buffer_evict_sysmem(', 'BOOL wined3d_buffer_load_location(',
            'static HRESULT buffer_resource_sub_resource_map(', 'static HRESULT buffer_resource_sub_resource_unmap(')
        bodies=''.join(base.function(source,s) for s in signatures)
        cls.binaries={}
        for arch in ('wow64','win64'):
            src=cls.folder/(arch+'-sysmem.c'); binary=src.with_suffix('')
            src.write_text(STUBS+bodies+MAIN)
            subprocess.run(['gcc','-D_POSIX_C_SOURCE=200809L',*(['-D_WIN64'] if arch=='win64' else []),
                '-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function',
                '-Wno-unused-variable','-Wno-misleading-indentation','-fsanitize=address,undefined',
                '-fno-pie','-no-pie',str(src),'-o',str(binary)],check=True)
            cls.binaries[arch]=binary

    def run_case(self,arch,case):
        r=subprocess.run([str(self.binaries[arch]),case],capture_output=True,text=True,timeout=10,
            env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:halt_on_error=1'))
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_explicit_opt_in_and_narrow_buffer_policy(self): self.run_case('wow64','policy')
    def test_win64_remains_unchanged(self): self.run_case('win64','policy')
    def test_partial_nested_read_and_discard_keep_publication(self): self.run_case('wow64','publication')
    def test_initial_data_observes_policy_before_submission(self):
        for arch in ('wow64','win64'): self.run_case(arch,'initial')
    def test_overlay_replays_twice(self): self.assertEqual(self.first_replay,self.second_replay)


if __name__=='__main__': unittest.main(argv=[__file__]+base.remaining)
