"""Exercise production FBO setup and opt-in cache with deterministic GL stubs."""
import argparse
from pathlib import Path
import subprocess,tempfile

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser()
p.add_argument('--source',type=Path,default=ROOT/'thirdparty/virglrenderer')
source=p.parse_args().source
text=(source/'src/vrend/vrend_renderer.c').read_text()
types=text[text.index('/* Begin immutable framebuffer completeness cache types.'):text.index('/* End immutable framebuffer completeness cache types.')]+'\n'
setup=text[text.index('/* Begin immutable framebuffer completeness cache helpers.'):text.index('\nvoid vrend_set_framebuffer_state_no_attach(')]
objects=text[text.index('void\nvrend_renderer_object_destroy('):text.index('\nstatic uint32_t query_stats_index_to_gl_map[]')]
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PIPE_MAX_COLOR_BUFS 8
#define GL_FRAMEBUFFER 1
#define GL_FRAMEBUFFER_COMPLETE 2
#define VIRGL_OBJECT_SURFACE 1
#define VIRGL_ERROR_CTX_ILLEGAL_SURFACE 9
#define VREND_STORAGE_GL_IMMUTABLE (1u<<0)
#define VREND_STORAGE_EGL_IMAGE (1u<<1)
#define VREND_STORAGE_GBM_BUFFER (1u<<2)
#define VREND_STORAGE_GL_MEMOBJ (1u<<3)
#define VREND_STORAGE_D3D_TEXTURE (1u<<4)
#define has_bit(bits,mask) (((bits)&(mask))!=0)
typedef unsigned GLenum,GLuint;
typedef int GLint;
enum virgl_object_type { TYPE_SURFACE=1 };
struct vrend_resource {unsigned storage_bits;void *egl_image,*gbm_bo;bool y_0_top;unsigned target,gl_id;
 struct {unsigned last_level,height0,width0,depth0,nr_samples,format;} base;};
struct vrend_surface {struct vrend_resource *texture;unsigned format,gl_id,level,first_layer,last_layer,nr_samples;};
'''
state=r'''
struct vrend_sub_context {
#if WINEHUA_VIRGL_FBO_STATUS_CACHE
 struct winehua_fbo_status_cache fbo_status_cache;
#endif
 unsigned fb_id,nr_cbufs,fb_height,viewport_state_dirty;
 bool fbo_origin_upper_left,shader_dirty,blend_state_dirty;
 struct vrend_surface *surf[PIPE_MAX_COLOR_BUFS],*zsurf;
 void *object_hash;
};
struct vrend_context {struct vrend_sub_context *sub;const char *debug_name;};
static struct vrend_surface *table[128];
static unsigned queries,binds,attachments,emits,errors,warnings,status=GL_FRAMEBUFFER_COMPLETE;
static void glBindFramebuffer(GLenum target,GLuint id){assert(target==GL_FRAMEBUFFER&&id==17);binds++;}
static GLenum glCheckFramebufferStatus(GLenum target){assert(target==GL_FRAMEBUFFER);queries++;return status;}
static void *vrend_object_lookup(void *table_id,unsigned h,int type){(void)table_id;assert(type==VIRGL_OBJECT_SURFACE);return h<128?table[h]:NULL;}
static void vrend_object_remove(void *table_id,unsigned h,int type){(void)table_id;(void)type;if(h<128)table[h]=NULL;}
static unsigned vrend_object_insert(void *table_id,void *data,unsigned h,enum virgl_object_type type){(void)table_id;(void)type;assert(h<128);table[h]=data;return h;}
static void vrend_surface_reference(struct vrend_surface **p,struct vrend_surface *v){*p=v;}
static void vrend_hw_set_zsurf_texture(struct vrend_context *ctx){(void)ctx;attachments++;}
static void vrend_hw_set_color_surface(struct vrend_sub_context *ctx,unsigned i){(void)ctx;(void)i;attachments++;}
static void vrend_hw_emit_framebuffer_state(struct vrend_sub_context *ctx){(void)ctx;emits++;}
static unsigned u_minify(unsigned v,unsigned l){return v>>l;}
static void vrend_report_context_error(struct vrend_context *c,unsigned type,unsigned h){(void)c;(void)h;assert(type==VIRGL_ERROR_CTX_ILLEGAL_SURFACE);errors++;}
static void virgl_error(const char *fmt,...){(void)fmt;warnings++;}
'''
tests=r'''
int main(void){
 struct vrend_resource resources[96]={0};struct vrend_surface surfaces[96]={0};
 for(unsigned i=1;i<96;i++){resources[i].storage_bits=VREND_STORAGE_GL_IMMUTABLE;resources[i].base.height0=720;
  surfaces[i].texture=&resources[i];surfaces[i].gl_id=i;table[i]=&surfaces[i];}
 struct vrend_sub_context sub={.fb_id=17},other={.fb_id=17};struct vrend_context ctx={&sub,"test"};
 uint32_t colors[8]={1};
 vrend_set_framebuffer_state(&ctx,1,colors,0);assert(queries==1&&emits==1&&attachments==1&&sub.fb_height==720);
 vrend_set_framebuffer_state(&ctx,1,colors,0);assert(queries==(WINEHUA_VIRGL_FBO_STATUS_CACHE?1:2)&&binds==2&&emits==2);
#if WINEHUA_VIRGL_FBO_STATUS_CACHE
 assert(sub.fbo_status_cache.count==1); /* Hits must not fill the bounded ring. */
#endif
 unsigned old=queries;colors[0]=120;
 vrend_set_framebuffer_state(&ctx,1,colors,0);assert(queries==old&&errors==1);
 colors[0]=1;
 vrend_renderer_object_destroy(&ctx,1);assert(!table[1]);
 assert(vrend_renderer_object_insert(&ctx,&surfaces[1],1,TYPE_SURFACE)==1);
 vrend_set_framebuffer_state(&ctx,1,colors,0);assert(queries==old+1);
 colors[1]=2;vrend_set_framebuffer_state(&ctx,2,colors,0);old=queries;
 colors[0]=2;colors[1]=1;vrend_set_framebuffer_state(&ctx,2,colors,0);assert(queries==old+1);
 old=queries;vrend_set_framebuffer_state(&ctx,0,NULL,3);assert(queries==old+1);
 vrend_set_framebuffer_state(&ctx,0,NULL,3);assert(queries==old+(WINEHUA_VIRGL_FBO_STATUS_CACHE?1:2));
 old=queries;vrend_set_framebuffer_state(&ctx,0,NULL,0);assert(queries==old);
 /* Mutable, imported and mipmapped attachments never populate the cache. */
 for(unsigned kind=0;kind<9;kind++){
  winehua_fbo_status_reset(&sub);resources[1].storage_bits=VREND_STORAGE_GL_IMMUTABLE;
  resources[1].egl_image=resources[1].gbm_bo=NULL;resources[1].base.last_level=surfaces[1].level=0;
  switch(kind){case 0:resources[1].storage_bits=0;break;
   case 1:resources[1].storage_bits|=VREND_STORAGE_EGL_IMAGE;break;
   case 2:resources[1].storage_bits|=VREND_STORAGE_GBM_BUFFER;break;
   case 3:resources[1].storage_bits|=VREND_STORAGE_GL_MEMOBJ;break;
   case 4:resources[1].storage_bits|=VREND_STORAGE_D3D_TEXTURE;break;
   case 5:resources[1].egl_image=&ctx;break;case 6:resources[1].gbm_bo=&ctx;break;
   case 7:resources[1].base.last_level=1;break;case 8:surfaces[1].level=1;break;}
  colors[0]=1;old=queries;vrend_set_framebuffer_state(&ctx,1,colors,0);vrend_set_framebuffer_state(&ctx,1,colors,0);assert(queries==old+2);
 }
 resources[1].base.last_level=surfaces[1].level=0;winehua_fbo_status_reset(&sub);status=123;
 old=queries;unsigned logs=warnings;vrend_set_framebuffer_state(&ctx,1,colors,0);vrend_set_framebuffer_state(&ctx,1,colors,0);
 assert(queries==old+2&&warnings>logs); /* Incomplete results are always re-checked/reported. */
 status=GL_FRAMEBUFFER_COMPLETE;winehua_fbo_status_reset(&sub);
 for(unsigned i=1;i<=33;i++){colors[0]=i;vrend_set_framebuffer_state(&ctx,1,colors,0);}
#if WINEHUA_VIRGL_FBO_STATUS_CACHE
 assert(sub.fbo_status_cache.count==32);
#endif
 old=queries;colors[0]=1;vrend_set_framebuffer_state(&ctx,1,colors,0);assert(queries==old+1); /* Evicted. */
 ctx.sub=&other;old=queries;vrend_set_framebuffer_state(&ctx,1,colors,0);assert(queries==old+1);
 colors[0]=0;old=queries;unsigned failures=errors;vrend_set_framebuffer_state(&ctx,1,colors,0);assert(errors==failures+1&&queries==old);
 puts("PASS production FBO: first/hit/order/depth/empty, invalid handles, insertion/destruction invalidation, external/mip/mutable exclusions, incomplete retries, bounded eviction and subcontext isolation");
}
'''
with tempfile.TemporaryDirectory(prefix='virgl-fbo-') as tmp:
    tmp=Path(tmp);src=tmp/'test.c'
    src.write_text(prefix+types+state+setup+objects+tests)
    for mode in (0,1):
        out=tmp/f'test-{mode}'
        subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function',
            '-fsanitize=address,undefined','-fno-omit-frame-pointer',f'-DWINEHUA_VIRGL_FBO_STATUS_CACHE={mode}',str(src),'-o',str(out)],check=True)
        subprocess.run([str(out)],check=True,timeout=15)
