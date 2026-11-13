# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run actual prepared protocol/visibility functions with independent clients.

Host contract tests only; these doubles do not qualify a GPU or game.
"""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT/'thirdparty/virglrenderer'
TASK = ROOT/'build/host_tests/venus-precise-context'


def function(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}', start)+3]


PREAMBLE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#define __OHOS__ 1
#define UNUSED __attribute__((unused))
#include "vtest/vtest_protocol.h"
#define VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK 0xff
#define VIRGL_RENDERER_CONTEXT_FLAG_OHOS_NATIVE_MEMORY (1u << 16)
#define VIRGL_RENDERER_CONTEXT_FLAG_OHOS_PRECISE_SHADOW (1u << 17)
struct input { int (*read)(struct input *, void *, size_t); };
struct vtest_context {
    struct input *input;
    bool context_initialized;
    uint32_t native_memory_flags,capset_id;
    int out_fd;
};
static struct vtest_context *current;
static uint32_t request,reply[4];
static int read_input(struct input *input,void *out,size_t size) {
    (void)input;assert(size==sizeof(request));memcpy(out,&request,size);return size;
}
static struct vtest_context *vtest_get_current_context(void) {return current;}
static int vtest_block_write(int fd,const void *data,size_t size) {
    (void)fd;assert(size==sizeof(reply));memcpy(reply,data,size);return size;
}
static int vtest_lazy_init_context(struct vtest_context *ctx) {
    ctx->context_initialized=true;return 0;
}
'''

MAIN = r'''
static void query(uint32_t param,bool supported) {
    request=param;assert(!vtest_get_param(0));
    assert(reply[2]==supported && reply[3]==(supported?1u:0u));
}
static int init(uint32_t flags) {request=4|flags;return vtest_context_init(0);}
int main(void) {
    struct input input={read_input};
    struct vtest_context a={.input=&input},b={.input=&input},c={.input=&input};
    current=&a;query(VCMD_PARAM_OHOS_PRECISE_SHADOW_V1,true);
    assert(a.native_memory_flags==VIRGL_RENDERER_CONTEXT_FLAG_OHOS_PRECISE_SHADOW);
    assert(!init(a.native_memory_flags));
    query(VCMD_PARAM_OHOS_NATIVE_MEMORY_V1,false);
    query(VCMD_PARAM_OHOS_PRECISE_SHADOW_V1,true);
    assert(init(0)==-EINVAL);
    current=&b;assert(!init(0));
    query(VCMD_PARAM_OHOS_PRECISE_SHADOW_V1,false);
    assert(!b.native_memory_flags && b.context_initialized);
    current=&c;
    query(VCMD_PARAM_OHOS_NATIVE_MEMORY_V1,true);
    query(VCMD_PARAM_OHOS_PRECISE_SHADOW_V1,true);
    query(VCMD_PARAM_OHOS_NATIVE_MEMORY_V1,true);
    assert(c.native_memory_flags==((1u<<16)|(1u<<17)));
    assert(!init(c.native_memory_flags));
    query(VCMD_PARAM_OHOS_NATIVE_MEMORY_V1,true);
    assert(c.native_memory_flags==((1u<<16)|(1u<<17)));
    request=4|(1u<<18);assert(vtest_context_init(0)==-EINVAL);
    return 0;
}
'''

class ContextContract(unittest.TestCase):
    def compile_run(self, name, text):
        TASK.mkdir(parents=True, exist_ok=True)
        source=TASK/(name+'.c');binary=TASK/name
        source.write_text(text)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                        '-I'+str(SOURCE),source,'-o',binary],check=True,timeout=30)
        subprocess.run([binary],check=True,timeout=10)

    def test_actual_negotiation_has_no_cross_client_or_late_mode_change(self):
        source=(SOURCE/'vtest/vtest_renderer.c').read_text()
        self.compile_run('negotiation', PREAMBLE+
                         function(source,'int vtest_get_param(')+
                         function(source,'int vtest_context_init(')+MAIN)

    def test_actual_outer_entry_accepts_only_venus_negotiation_flags(self):
        source=(SOURCE/'src/virglrenderer.c').read_text()
        begin=source.index('   /* unsupported flags */',source.index('int virgl_renderer_context_create_with_flags('))
        end=source.index('\n   ctx = virgl_context_lookup(',begin)
        self.compile_run('entry',r'''
#include <assert.h>
#include <stdint.h>
#include <errno.h>
#define __OHOS__ 1
#define VIRTGPU_DRM_CAPSET_VENUS 4
#define VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK 0xff
#define VIRGL_RENDERER_CONTEXT_FLAG_OHOS_NATIVE_MEMORY (1u << 16)
#define VIRGL_RENDERER_CONTEXT_FLAG_OHOS_PRECISE_SHADOW (1u << 17)
static int validate(uint32_t ctx_flags) {
    uint32_t capset_id=ctx_flags & VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK;
'''+source[begin:end]+r'''
    return 0;
}
int main(void) {
    for(unsigned combination=0;combination<4;++combination) {
        uint32_t flags=combination<<16;
        assert(!validate(4|flags));
        assert(validate(2|flags)==(flags?EINVAL:0));
    }
    assert(validate(4|(1u<<18))==EINVAL);
    return 0;
}
''')

    def test_actual_fence_refresh_preserves_precise_clients_unpublished_cpu_bytes(self):
        source=(SOURCE/'src/venus/vkr_device_memory.c').read_text()
        preamble=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>
#define __OHOS__ 1
struct vkr_context {bool ohos_precise_shadow;unsigned copies;};
static atomic_uint_fast64_t vkr_ohos_shadow_from_host_sync_count;
static const char *mode;
static const char *os_get_option(const char *name) {(void)name;return mode;}
static bool vkr_ohos_shadow_trace_enabled(void) {return false;}
#define vkr_log(...) ((void)0)
static void vkr_device_memory_sync_shadows(struct vkr_context *ctx,bool to_host) {
    assert(!to_host);ctx->copies++;
}
'''
        main=r'''
int main(void) {
    struct vkr_context precise={.ohos_precise_shadow=true},legacy={0};
    vkr_device_memory_sync_shadows_from_host(&precise);
    vkr_device_memory_sync_shadows_from_host(&legacy);
    assert(!precise.copies && legacy.copies==1);
    mode="full";
    vkr_device_memory_sync_shadows_from_host(&precise);
    vkr_device_memory_sync_shadows_from_host(&legacy);
    assert(!precise.copies && legacy.copies==2);
    mode="precise";
    vkr_device_memory_sync_shadows_from_host(&legacy);
    assert(legacy.copies==2);
    return 0;
}
'''
        self.compile_run('visibility',preamble+
                         function(source,'void\nvkr_device_memory_sync_shadows_from_host(')+main)


if __name__ == '__main__':
    unittest.main()
