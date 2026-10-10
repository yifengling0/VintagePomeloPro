#!/usr/bin/env python3
"""Exercise production condition resolution / lifecycle with mock GL results.

The device probe separately verifies actual draw, clear, blit and copy pixels.
"""
import re, subprocess, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT/'thirdparty/virglrenderer/src/vrend/vrend_renderer.c'

def function(s, name):
    m=re.search(r'(?:static bool|void) '+name+r'\(',s)
    assert m,name
    begin=s.index('{',m.start()); end=begin+1; depth=1
    while depth:
        depth+=(s[end]=='{')-(s[end]=='}'); end+=1
    return s[m.start():end]

def main():
    s=SRC.read_text()
    for name, gate in [('vrend_clear','sub_ctx->cond_render_skip'),
                       ('vrend_clear_surface','render_condition_enabled && sub_ctx->cond_render_skip'),
                       ('vrend_renderer_blit','info->render_condition_enable && ctx->sub->cond_render_skip')]:
        assert gate in function(s,name)
    start=s.index('int vrend_draw_vbo('); end=s.index('\n}',start)
    assert 'if (sub_ctx->cond_render_skip)\n      return 0;' in s[start:end]
    assert 'cond_render_skip' not in function(s,'vrend_renderer_resource_copy_region')
    code=r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef unsigned GLuint; typedef unsigned GLenum;
enum {GL_QUERY_RESULT_AVAILABLE,GL_QUERY_RESULT,
 GL_QUERY_WAIT, GL_QUERY_NO_WAIT,GL_QUERY_BY_REGION_WAIT,GL_QUERY_BY_REGION_NO_WAIT,
 GL_QUERY_WAIT_INVERTED,GL_QUERY_NO_WAIT_INVERTED,
 GL_QUERY_BY_REGION_WAIT_INVERTED,GL_QUERY_BY_REGION_NO_WAIT_INVERTED};
enum {PIPE_RENDER_COND_WAIT,PIPE_RENDER_COND_NO_WAIT,PIPE_RENDER_COND_BY_REGION_WAIT,PIPE_RENDER_COND_BY_REGION_NO_WAIT};
enum {feat_gl_conditional_render,feat_nv_conditional_render,feat_conditional_render_inverted,VIRGL_OBJECT_QUERY};
static struct {bool emulate_conditional_render;} vrend_state;
struct vrend_query {GLuint id;};
struct sub {GLuint cond_render_q_id;GLenum cond_render_gl_mode;bool cond_render_skip;void*object_hash;};
struct vrend_context {struct sub*sub;bool ctx_switch_pending;};
static struct vrend_query query={7};
static unsigned available,result,availability_reads,result_reads,begins,ends,switches;static int native;
static bool has_feature(int f){return f==feat_gl_conditional_render&&native;}
static void *vrend_object_lookup(void*h,unsigned id,int type){(void)h;(void)type;return id==1?&query:NULL;}
static void glGetQueryObjectuiv(GLuint q,GLenum what,GLuint*out){assert(q==7);if(what==GL_QUERY_RESULT_AVAILABLE){availability_reads++;*out=available;}else{result_reads++;*out=result;}}
static void glBeginConditionalRender(GLuint q,GLenum m){(void)m;assert(q==7);begins++;}
static void glBeginConditionalRenderNV(GLuint q,GLenum m){glBeginConditionalRender(q,m);}
static void glEndConditionalRender(void){ends++;}
static void glEndConditionalRenderNV(void){ends++;}
static void vrend_finish_context_switch(struct vrend_context*c){c->ctx_switch_pending=false;switches++;}
#define virgl_warn(...) ((void)0)
"""
    code+=function(s,'vrend_emulated_condition_skip')+'\n'+function(s,'vrend_render_condition')
    code+=r"""
int main(void){
 struct sub sub={0};struct vrend_context ctx={&sub,true};vrend_state.emulate_conditional_render=true;
 for(unsigned mode=0;mode<4;mode++)for(unsigned inv=0;inv<2;inv++)for(unsigned value=0;value<2;value++){
  availability_reads=result_reads=0;available=1;result=value;
  vrend_render_condition(&ctx,1,inv,mode);assert(sub.cond_render_skip==((value==0)!=inv));assert(result_reads==1);
  assert(availability_reads==(mode==PIPE_RENDER_COND_NO_WAIT||mode==PIPE_RENDER_COND_BY_REGION_NO_WAIT));
  assert(!sub.cond_render_q_id&&!sub.cond_render_gl_mode);
  vrend_render_condition(&ctx,0,false,0);assert(!sub.cond_render_skip);
 }
 for(unsigned inv=0;inv<2;inv++)for(unsigned mode=1;mode<4;mode+=2){
  available=0;result=0;result_reads=0;vrend_render_condition(&ctx,1,inv,mode);
  assert(!sub.cond_render_skip&&!result_reads);
 }
 assert(!begins&&!ends&&switches==1);
 available=1;result=0;vrend_render_condition(&ctx,1,false,0);assert(sub.cond_render_skip);
 vrend_render_condition(&ctx,99,false,0);assert(sub.cond_render_skip); /* bad handle doesn't replace state */
 vrend_render_condition(&ctx,1,false,99);assert(sub.cond_render_skip);
 vrend_render_condition(&ctx,0,false,0);assert(!sub.cond_render_skip);
 vrend_state.emulate_conditional_render=false;native=1;vrend_render_condition(&ctx,1,false,0);
 assert(begins==1&&sub.cond_render_q_id==7&&!sub.cond_render_skip);
 vrend_render_condition(&ctx,0,false,0);assert(ends==1&&!sub.cond_render_q_id);
 puts("PASS: production WAIT/NO_WAIT, inverted, region, unresolved, reset, context and native paths; GPU operation gates");
}
"""
    with tempfile.TemporaryDirectory(prefix='virgl-condition-') as folder:
        p=Path(folder);(p/'test.c').write_text(code)
        subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
        subprocess.run([str(p/'test')],check=True)
if __name__=='__main__':main()
