"""Compile the real flush function; verify bytes, slice bounds and old-code rejection."""
from pathlib import Path
import os,subprocess,tempfile

repo=Path(__file__).resolve().parents[1]
source=repo/'thirdparty/mesa/src/gallium/drivers/zink/zink_resource.c'
text=source.read_text()
start=text.index('static void\nzink_transfer_flush_region(',text.index('static void\nzink_image_subdata('))
end=text.index('\n/* used to determine',start)
function=text[start:end]
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#define ASSERTED
#define PIPE_MAP_WRITE 1
#define PIPE_BUFFER 0
#define VK_SUCCESS 0
typedef uint64_t VkDeviceSize;
typedef struct { uint64_t offset,size; } VkMappedMemoryRange;
struct pipe_box { int x,y,z,width,height,depth; };
struct pipe_resource { unsigned target,format,width0; };
struct pipe_transfer { struct pipe_resource *resource; unsigned usage,stride,layer_stride; struct pipe_box box; };
struct object { bool coherent,is_buffer; uint64_t offset,size; };
struct zink_resource { struct {struct pipe_resource b;} base; struct object *obj; };
struct zink_transfer { struct {struct pipe_transfer b;} base; struct pipe_resource *staging_res; unsigned offset,depthPitch; };
struct zink_context { int unused; };
struct zink_screen { int dev; };
struct pipe_context { struct zink_screen *screen; };
static struct zink_context context;
static unsigned calls,copies;
static bool enabled=true;
static VkMappedMemoryRange actual;
static struct zink_context *zink_context(struct pipe_context *p) {(void)p;return &context;}
static struct zink_screen *zink_screen(struct zink_screen *p) {return p;}
static struct zink_resource *zink_resource(struct pipe_resource *p) {return (void*)p;}
static const char *os_get_option(const char *s) {(void)s;return enabled?"1":NULL;}
static VkMappedMemoryRange zink_resource_init_mem_range(struct zink_screen *s,struct object *o,uint64_t offset,uint64_t size) {
 (void)s; assert(offset>=o->offset && size<=o->size && offset-o->offset<=o->size-size);
 return (VkMappedMemoryRange){offset,size};
}
static int flush(int dev,unsigned n,const VkMappedMemoryRange *r) {(void)dev;assert(n==1);actual=*r;calls++;return VK_SUCCESS;}
#define VKSCR(n) flush
#define mesa_loge(s) abort()
static unsigned util_format_get_blocksize(unsigned f) {(void)f;return 4;}
static unsigned util_format_get_2d_size(unsigned f,unsigned stride,unsigned y) {(void)f;return stride*y;}
static unsigned util_format_get_stride(unsigned f,unsigned x) {(void)f;return 4*x;}
static void zink_copy_buffer(struct zink_context *c,struct zink_resource *d,struct zink_resource *s,uint64_t a,uint64_t b,uint64_t n) {(void)c;(void)d;(void)s;(void)a;(void)b;(void)n;copies++;}
static void zink_transfer_copy_bufimage(struct zink_context *c,struct zink_resource *d,struct zink_resource *s,struct zink_transfer *t) {(void)c;(void)d;(void)s;(void)t;copies++;}
'''
main=r'''
int main(void) {
 struct zink_screen screen={0}; struct pipe_context ctx={&screen};
 struct object image_obj={true,false,0,65536},stage_obj={true,true,4096,1024};
 struct zink_resource image={{{1,0,64}},&image_obj},stage={{{PIPE_BUFFER,0,16}},&stage_obj};
 struct zink_transfer t={0}; t.base.b.resource=&image.base.b;t.base.b.usage=PIPE_MAP_WRITE;t.staging_res=&stage.base.b;
 struct pipe_box b={0,0,0,2,2,1};
 zink_transfer_flush_region(&ctx,&t.base.b,&b);assert(calls==1&&actual.offset==4096&&actual.size==16&&copies==1);
 /* Row/layer and compressed-block bytes must all be published. */
 stage.base.b.width0=96;b.width=4;b.height=3;b.depth=2;
 zink_transfer_flush_region(&ctx,&t.base.b,&b);assert(actual.offset==4096&&actual.size==96);
 stage.base.b.width0=32;b.width=8;b.height=8;b.depth=1;
 zink_transfer_flush_region(&ctx,&t.base.b,&b);assert(actual.offset==4096&&actual.size==32);
 /* Image origin is not a byte offset into the staging allocation. */
 t.base.b.box=(struct pipe_box){17,23,2,8,8,1};
 zink_transfer_flush_region(&ctx,&t.base.b,&b);assert(actual.offset==4096&&actual.size==32);
 /* Buffer ranges retain byte offsets and neighboring slices. */
 image.base.b.target=PIPE_BUFFER;image_obj.is_buffer=true;image_obj.offset=8192;image_obj.size=4096;
 t.staging_res=NULL;t.base.b.box.x=96;b.x=16;b.width=24;
 zink_transfer_flush_region(&ctx,&t.base.b,&b);assert(actual.offset==8304&&actual.size==24);
 t.staging_res=&stage.base.b;t.offset=80;
 zink_transfer_flush_region(&ctx,&t.base.b,&b);assert(actual.offset==4192&&actual.size==24);
 enabled=false;unsigned before=calls;
 zink_transfer_flush_region(&ctx,&t.base.b,&b);assert(calls==before);
 stage_obj.coherent=false;
 zink_transfer_flush_region(&ctx,&t.base.b,&b);assert(calls==before+1&&actual.offset==4096&&actual.size==1024);
 t.base.b.usage=0;before=calls;
 zink_transfer_flush_region(&ctx,&t.base.b,&b);assert(calls==before);
 return 0;
}
'''
old='''         const bool slice = remote_upload && m->obj->is_buffer;
         VkMappedMemoryRange range = zink_resource_init_mem_range(screen, m->obj,
            m->obj->offset + (slice ? src_offset : 0), slice ? size : m->obj->size);'''
a=function.index('         VkDeviceSize upload_offset')
b=function.index('\n         if (VKSCR(FlushMappedMemoryRanges)',a)
mutant=function[:a]+old+function[b:]
with tempfile.TemporaryDirectory(prefix='zink-upload-contract-') as directory:
    root=Path(directory)
    for name,body,flags,success in [('production',function,[],True),('sanitized',function,['-fsanitize=address,undefined'],True),('old-range',mutant,[],False)]:
        src=root/(name+'.c');exe=root/name;src.write_text(prefix+body+main)
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-Wno-sign-compare','-O1','-g',*flags,str(src),'-o',str(exe)],check=True)
        result=subprocess.run([str(exe)],capture_output=True)
        assert (result.returncode==0)==success,(name,result.returncode,result.stderr.decode())
        print(name,'PASS' if success else 'rejected as required')
