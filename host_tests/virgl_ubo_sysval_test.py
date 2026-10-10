#!/usr/bin/env python3
"""Compile the production UBO policy, shader emitter and sysval uploader.

Mock GL records limits and uploads, while actual device pixel probes exercise
the real Mesa/renderer/driver implementation. No copied policy implementation.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RENDERER = ROOT / "thirdparty/virglrenderer/src/vrend/vrend_renderer.c"
SHADER = ROOT / "thirdparty/virglrenderer/src/vrend/vrend_shader.c"


def function(source, name):
    match = re.search(r"static (?:void\s*\n|void )" + name + r"\s*\(", source)
    assert match, name
    start = source.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def main():
    renderer = RENDERER.read_text()
    shader = SHADER.read_text()
    start = renderer.index("   /* VirglBlock consumes one real block per stage.")
    end = renderer.index("   vrend_state.egl_image_srgb_import =", start)
    policy = "static void policy(void) {\n" + renderer[start:end] + "}\n"
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#define MIN2(a,b) ((a)<(b)?(a):(b))
#define MAX2(a,b) ((a)>(b)?(a):(b))
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define BIT(x) (1u<<(x))
#define VIRGL_NUM_CLIP_PLANES 8
#define VREND_POLYGON_STIPPLE_SIZE 32
#define PIPE_SHADER_TYPES 6
#define PIPE_SHADER_COMPUTE 5
#define PIPE_SHADER_FRAGMENT 1
#define GL_INVALID_INDEX UINT32_MAX
#define GL_UNIFORM_BUFFER 99
typedef unsigned GLuint; typedef int GLint; typedef float GLfloat;
enum { GL_MAX_VERTEX_UNIFORM_BLOCKS, GL_MAX_FRAGMENT_UNIFORM_BLOCKS,
 GL_MAX_GEOMETRY_UNIFORM_BLOCKS, GL_MAX_TESS_CONTROL_UNIFORM_BLOCKS,
 GL_MAX_TESS_EVALUATION_UNIFORM_BLOCKS, GL_MAX_COMPUTE_UNIFORM_BLOCKS,
 GL_MAX_UNIFORM_BUFFER_BINDINGS, GL_MAX_COMBINED_UNIFORM_BLOCKS };
enum { feat_ubo, feat_geometry_shader, feat_tessellation, feat_compute_shader };
enum { UNIFORM_WINSYS_ADJUST_Y, UNIFORM_CLIP_PLANE, UNIFORM_ALPHA_REF_VAL,
 UNIFORM_PSTIPPLE_SAMPLER, UNIFORM_DRAWID_BASE };
static int limits[8], features[4];
static struct { bool use_gles, use_plain_sysval_uniforms; unsigned uniform_block_limit; } vrend_state;
static bool has_feature(int f) {return features[f];}
static void glGetIntegerv(int p, GLint *v) {*v=limits[p];}
static void winehua_gl_caps_log(const char *f,...) {(void)f;}
struct vrend_strbuf {char data[4096];};
static void strbuf_append(struct vrend_strbuf *b,const char *s) {strcat(b->data,s);}
static void strbuf_appendf(struct vrend_strbuf *b,const char *f,...) {
 va_list ap;va_start(ap,f);vsnprintf(b->data+strlen(b->data),sizeof(b->data)-strlen(b->data),f,ap);va_end(ap);
}
struct sysval_uniform_block {
 GLfloat clipp[8][4];GLuint stipple_pattern[32][4];
 GLfloat winsys_adjust_y,alpha_ref_val,clip_plane_enabled;GLint drawid_base;
};
struct vrend_linked_shader_program {
 void *ss[6];GLint plain_sysval_locs[6][6];
 GLuint virgl_block_bind,ubo_sysval_buffer_id;uint32_t sysvalue_data_cookie;
};
struct vrend_sub_context {struct vrend_linked_shader_program *prog;
 uint32_t sysvalue_data_cookie;struct sysval_uniform_block sysvalue_data;};
static int calls,active_stage,counts[6][6];static float floats[6][6];
static void vrend_set_active_pipeline_stage(struct vrend_linked_shader_program *p,int s) {(void)p;active_stage=s;}
static void glUniform4fv(GLint l,int n,const GLfloat *v) {assert(l==0&&n==8&&v[0]==3);counts[active_stage][l]++;calls++;}
static void glUniform1uiv(GLint l,int n,const GLuint *v) {assert(l==1&&n==32&&v[0]==17&&v[31]==48);counts[active_stage][l]++;calls++;}
static void glUniform1f(GLint l,GLfloat v) {assert(l==2||l==3);floats[active_stage][l]=v;counts[active_stage][l]++;calls++;}
static void glUniform1i(GLint l,GLint v) {assert((l==4&&v==1)||(l==5&&v==7));counts[active_stage][l]++;calls++;}
static void glBindBuffer(int t,GLuint b) {(void)t;(void)b;assert(0&&"unexpected internal UBO");}
static void glBufferSubData(int t,int o,size_t n,const void *d) {(void)t;(void)o;(void)n;(void)d;assert(0);}
'''
    code += policy
    code += function(shader, "emit_required_sysval_uniforms") + "\n"
    code += function(renderer, "vrend_fill_sysval_uniform_block") + "\n"
    code += r'''
static void reset(void) {
 memset(&vrend_state,0,sizeof(vrend_state));vrend_state.use_gles=true;
 for(int i=0;i<6;i++)limits[i]=12;limits[6]=limits[7]=72;
 for(int i=0;i<4;i++)features[i]=1;unsetenv("WINEHUA_VIRGL_UBO_FIX");
}
int main(void) {
 reset();policy();assert(vrend_state.use_plain_sysval_uniforms&&vrend_state.uniform_block_limit==12);
 setenv("WINEHUA_VIRGL_UBO_FIX","0",1);policy();assert(!vrend_state.use_plain_sysval_uniforms&&vrend_state.uniform_block_limit==11);
 reset();limits[1]=8;policy();assert(!vrend_state.use_plain_sysval_uniforms&&vrend_state.uniform_block_limit==7);
 reset();limits[6]=71;policy();assert(vrend_state.uniform_block_limit==11);
 reset();limits[7]=60;policy();assert(vrend_state.uniform_block_limit==10);
 reset();vrend_state.use_gles=false;policy();assert(!vrend_state.use_plain_sysval_uniforms&&vrend_state.uniform_block_limit==11);
 reset();features[1]=features[2]=features[3]=0;limits[6]=limits[7]=24;policy();assert(vrend_state.uniform_block_limit==12);
 reset();features[0]=0;policy();assert(!vrend_state.use_plain_sysval_uniforms&&vrend_state.uniform_block_limit==0);
 struct vrend_strbuf b={{0}};emit_required_sysval_uniforms(&b,31,true);
 assert(!strstr(b.data,"VirglBlock")&&strstr(b.data,"uniform float winsys_adjust_y")&&strstr(b.data,"uniform bool clip_plane_enabled")&&strstr(b.data,"uniform int drawid_base"));
 memset(&b,0,sizeof(b));emit_required_sysval_uniforms(&b,1,true);assert(!strstr(b.data,"clipp")&&strstr(b.data,"winsys_adjust_y"));
 memset(&b,0,sizeof(b));emit_required_sysval_uniforms(&b,31,false);assert(strstr(b.data,"layout (std140) uniform VirglBlock"));
 memset(&b,0,sizeof(b));emit_required_sysval_uniforms(&b,0,true);assert(!b.data[0]);
 struct vrend_linked_shader_program p={0};struct vrend_sub_context c={0};c.prog=&p;
 reset();policy();c.sysvalue_data_cookie=1;p.sysvalue_data_cookie=UINT32_MAX;
 c.sysvalue_data.clipp[0][0]=3;c.sysvalue_data.winsys_adjust_y=-1;c.sysvalue_data.alpha_ref_val=.5f;c.sysvalue_data.clip_plane_enabled=1;c.sysvalue_data.drawid_base=7;
 for(int i=0;i<32;i++)c.sysvalue_data.stipple_pattern[i][0]=17+i;
 for(int s=0;s<2;s++){p.ss[s]=&p;for(int i=0;i<6;i++)p.plain_sysval_locs[s][i]=i;}
 vrend_fill_sysval_uniform_block(&c);assert(calls==12&&active_stage==PIPE_SHADER_FRAGMENT);
 assert(floats[0][2]==-1&&floats[1][3]==.5f);vrend_fill_sysval_uniform_block(&c);assert(calls==12);
 c.sysvalue_data_cookie++;p.plain_sysval_locs[1][0]=-1;vrend_fill_sysval_uniform_block(&c);assert(calls==23);
 memset(p.ss,0,sizeof(p.ss));p.ss[5]=&p;for(int i=0;i<6;i++)p.plain_sysval_locs[5][i]=i;
 c.sysvalue_data_cookie++;vrend_fill_sysval_uniform_block(&c);assert(calls==29&&active_stage==5);
 puts("PASS: production UBO stage/combined limits, helper emission, typed upload, cookie and compute tests");
}
'''
    with tempfile.TemporaryDirectory(prefix="virgl-ubo-") as folder:
        path = Path(folder)
        (path / "test.c").write_text(code)
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra",
                        "-Wno-misleading-indentation", "-Werror", str(path / "test.c"),
                        "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    main()
