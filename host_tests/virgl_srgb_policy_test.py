"""Replay actual VirGL framebuffer and clear functions. GL calls are recorded.
This is a source semantics test, not a GPU blend/pixel certification.
Pass --source to the clean pinned source with managed patches applied.
"""
from pathlib import Path
import argparse, subprocess, tempfile
R=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,default=R/'thirdparty/virglrenderer')
args=p.parse_args()
source=(args.source/'src/vrend/vrend_renderer.c').read_text()
def function(name):
    pos=source.index(name+'(');a=source.rfind('\nstatic',0,pos)+1
    b=source.index('{',pos);depth=1;e=b+1
    while depth:
        depth+=(source[e]=='{')-(source[e]=='}');e+=1
    return source[a:e]
stub=r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#define GL_NONE 0
#define GL_FRAMEBUFFER_SRGB_EXT 1
#define GL_COLOR_ATTACHMENT0 0
#define GL_COLOR_ATTACHMENT1 1
#define GL_COLOR_ATTACHMENT2 2
#define GL_COLOR_ATTACHMENT3 3
#define GL_COLOR_ATTACHMENT4 4
#define GL_COLOR_ATTACHMENT5 5
#define GL_COLOR_ATTACHMENT6 6
#define GL_COLOR_ATTACHMENT7 7
#define VREND_DEBUG(...) ((void)0)
#define virgl_info(...) ((void)0)
typedef unsigned GLenum;
enum {VIRGL_FORMAT_R8G8B8A8_UNORM=1,VIRGL_FORMAT_R8G8B8X8_UNORM,
 VIRGL_FORMAT_B8G8R8A8_UNORM,VIRGL_FORMAT_B8G8R8X8_UNORM,
 VIRGL_FORMAT_R8G8B8A8_SRGB=11,VIRGL_FORMAT_R8G8B8X8_SRGB,
 VIRGL_FORMAT_B8G8R8A8_SRGB,VIRGL_FORMAT_B8G8R8X8_SRGB};
enum vrend_unorm_srgb_write_policy {VREND_UNORM_SRGB_WRITE_UNDECIDED,
 VREND_UNORM_SRGB_WRITE_ENCODE_XRGB,VREND_UNORM_SRGB_WRITE_PRESERVE};
struct vrend_resource {struct {unsigned format;} base; bool views;};
struct vrend_surface {unsigned format;struct vrend_resource *texture;};
struct vrend_sub_context {unsigned nr_cbufs;struct vrend_surface *surf[8];
 bool framebuffer_srgb_enabled;uint8_t swizzle_output_rgb_to_bgr,needs_manual_srgb_encode_bitmask;
 enum vrend_unorm_srgb_write_policy unorm_srgb_write_policy;};
static struct {bool use_gles,egl_image_srgb_import;} vrend_state={true,true};
enum {feat_srgb_write_control};
static bool control=true;
static bool has_feature(unsigned f){(void)f;return control;}
static bool util_format_is_srgb(unsigned f){return f>=11&&f<=14;}
static bool vrend_resource_supports_view(const struct vrend_resource *r,unsigned f){(void)f;return r->views;}
static bool vrend_format_is_bgra(unsigned f){return f==3||f==4||f==13||f==14;}
static bool vrend_format_is_emulated_alpha(unsigned f){(void)f;return false;}
static void glReadBuffer(unsigned n){(void)n;}
static void glEnable(unsigned n){(void)n;}
static void glDisable(unsigned n){(void)n;}
static void glDrawBuffers(unsigned n,const unsigned *b){(void)n;(void)b;}
static unsigned checks;
#define CHECK(c) do{++checks;assert(c);}while(0)
"""
driver=r"""
static void bind(struct vrend_sub_context *s,struct vrend_surface *a,struct vrend_surface *b){
 s->surf[0]=a;s->surf[1]=b;s->nr_cbufs=b?2:1;vrend_hw_emit_framebuffer_state(s);
}
static void expect(struct vrend_sub_context *s,struct vrend_surface *a,bool encode){
 bind(s,a,NULL);CHECK(((s->framebuffer_srgb_enabled||s->needs_manual_srgb_encode_bitmask)!=0) == encode);
 float v[4]={.25f,.1f,.8f,.375f};vrend_clear_color(s,a,v);
 CHECK(v[3]==.375f);
 CHECK(fabsf(v[0]-(s->needs_manual_srgb_encode_bitmask ? vrend_color_encode_as_srgb(.25f):.25f))<.00001f);
}
int main(void){
 struct vrend_resource r={{VIRGL_FORMAT_R8G8B8A8_SRGB},true};
 struct vrend_resource b={{VIRGL_FORMAT_B8G8R8A8_SRGB},true};
 struct vrend_surface rgba={VIRGL_FORMAT_R8G8B8A8_UNORM,&r},xrgb={VIRGL_FORMAT_R8G8B8X8_UNORM,&r};
 struct vrend_surface bgra={VIRGL_FORMAT_B8G8R8A8_UNORM,&b},srgb={VIRGL_FORMAT_R8G8B8A8_SRGB,&r};
 struct vrend_sub_context preserve={0},encode={0},bg={0};
 expect(&preserve,&rgba,false);expect(&preserve,&xrgb,false);expect(&bg,&bgra,false);
 expect(&encode,&xrgb,true);expect(&encode,&rgba,false);expect(&encode,&xrgb,true);
 bind(&encode,&srgb,NULL);CHECK(encode.framebuffer_srgb_enabled);CHECK(!encode.needs_manual_srgb_encode_bitmask);
 // Native sRGB blending stays in GL; shader output is not encoded twice.
 float color[4]={.25f,.1f,.8f,.3f};vrend_clear_color(&encode,&srgb,color);CHECK(color[0]==.25f&&color[3]==.3f);
 r.views=false;bind(&encode,&srgb,NULL);CHECK(encode.framebuffer_srgb_enabled&&!encode.needs_manual_srgb_encode_bitmask);
 // Mixed imported attachments: retain raw UNORM and convert sRGB attachment 1.
 bind(&preserve,&rgba,&srgb);CHECK(!preserve.framebuffer_srgb_enabled);CHECK(preserve.needs_manual_srgb_encode_bitmask==2);
 float raw[4]={.25f,.1f,.8f,.3f},linear[4]={.25f,.1f,.8f,.3f};
 vrend_clear_color(&preserve,&rgba,raw);vrend_clear_color(&preserve,&srgb,linear);
 CHECK(raw[0]==.25f&&linear[0]>.5f&&raw[3]==linear[3]);
 vrend_state.egl_image_srgb_import=false;bind(&encode,&srgb,NULL);
 CHECK(!encode.framebuffer_srgb_enabled&&encode.needs_manual_srgb_encode_bitmask==1);
 control=false;vrend_state.egl_image_srgb_import=true;bind(&encode,&srgb,NULL);CHECK(!encode.needs_manual_srgb_encode_bitmask);
 vrend_state.egl_image_srgb_import=false;bind(&encode,&srgb,NULL);CHECK(encode.needs_manual_srgb_encode_bitmask==1);
 r.views=true;control=true;bind(&preserve,&rgba,NULL);CHECK(!preserve.needs_manual_srgb_encode_bitmask);
 printf("VirGL actual framebuffer/clear: %u checks passed\n",checks);return 0;
}
"""
names=['vrend_is_unorm_surface_of_srgb_resource','vrend_classify_unorm_srgb_write_policy',
       'vrend_surface_wants_srgb_encode','vrend_surface_has_srgb_storage','vrend_surface_needs_manual_srgb',
       'vrend_hw_emit_framebuffer_state','vrend_color_encode_as_srgb','vrend_clear_color']
clear_start=source.index('   if ((buffers & PIPE_CLEAR_COLOR) == PIPE_CLEAR_COLOR0')
clear_end=source.index('   if (buffers & PIPE_CLEAR_DEPTH)',clear_start)
clear_stub=r"""
#include <string.h>
static void bind(struct vrend_sub_context *, struct vrend_surface *, struct vrend_surface *);
#define PIPE_CLEAR_COLOR0 4
#define PIPE_CLEAR_COLOR 1020
#define GL_COLOR 100
#define GL_COLOR_BUFFER_BIT 1
union pipe_color_union {float f[4];unsigned ui[4];int i[4];};
static unsigned selected[8],kind[8];static union pipe_color_union cleared[8];
static bool util_format_is_pure_uint(unsigned f){return f==31;}
static bool util_format_is_pure_sint(unsigned f){return f==32;}
static bool util_format_is_pure_integer(unsigned f){return f==31||f==32;}
static unsigned u_bit_scan(uint32_t *mask){unsigned bit=__builtin_ctz(*mask);*mask&=*mask-1;return bit;}
static void glClearBufferfv(unsigned target,unsigned i,const float *v){assert(target==GL_COLOR);++selected[i];kind[i]=1;memcpy(cleared[i].f,v,16);}
static void glClearBufferuiv(unsigned target,unsigned i,const unsigned *v){assert(target==GL_COLOR);++selected[i];kind[i]=2;memcpy(cleared[i].ui,v,16);}
static void glClearBufferiv(unsigned target,unsigned i,const int *v){assert(target==GL_COLOR);++selected[i];kind[i]=3;memcpy(cleared[i].i,v,16);}
static void vrend_clear_prepare(struct vrend_sub_context *s,struct vrend_surface *surf,unsigned b,float *c,double d,unsigned st){
 (void)b;(void)d;(void)st;vrend_clear_color(s,surf,c);
}
static unsigned clear_actual(struct vrend_sub_context *sub_ctx,unsigned buffers,const union pipe_color_union *color){
 unsigned bits=0,stencil=0;double depth=0;float colorf[4];memcpy(colorf,color->f,16);
"""+source[clear_start:clear_end]+r"""
 return bits;
}
static void mrt_checks(void){
 struct vrend_resource r={{VIRGL_FORMAT_R8G8B8A8_SRGB},false};
 struct vrend_surface raw={VIRGL_FORMAT_R8G8B8A8_UNORM,&r},srgb={VIRGL_FORMAT_R8G8B8A8_SRGB,&r};
 struct vrend_sub_context s={0};control=true;vrend_state.egl_image_srgb_import=true;
 bind(&s,&raw,&srgb);union pipe_color_union c={{.25f,.1f,.8f,.375f}};
 CHECK(clear_actual(&s,12,&c)==0);CHECK(selected[0]==1&&selected[1]==1);
 CHECK(cleared[0].f[0]==.25f&&cleared[1].f[0]>.5f&&cleared[1].f[3]==.375f);
 memset(selected,0,sizeof(selected));CHECK(clear_actual(&s,8,&c)==0);
 CHECK(selected[0]==0&&selected[1]==1&&cleared[1].f[3]==.375f);
 memset(selected,0,sizeof(selected));s.nr_cbufs=1;CHECK(clear_actual(&s,4,&c)==GL_COLOR_BUFFER_BIT);
 CHECK(selected[0]==0); // common float clear keeps glClear fast path
 raw.format=31;c.ui[0]=0xff123456;CHECK(clear_actual(&s,4,&c)==0);CHECK(kind[0]==2&&cleared[0].ui[0]==0xff123456);
 raw.format=32;c.i[0]=-12345;CHECK(clear_actual(&s,4,&c)==0);CHECK(kind[0]==3&&cleared[0].i[0]==-12345);
}
"""
driver=driver.replace(' printf("VirGL actual',' mrt_checks();\n printf("VirGL actual')
with tempfile.TemporaryDirectory(prefix='virgl-srgb-') as d:
    d=Path(d);c=d/'policy.c';binary=d/'test'
    c.write_text(stub+'\n'+'\n\n'.join(function(n) for n in names)+clear_stub+driver)
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',str(c),'-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
