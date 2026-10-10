#!/usr/bin/env python3
"""Execute production legacy alpha-state functions; reproduce the missing enable."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'thirdparty/wine-valve/dlls/wined3d/glsl_shader.c').read_text()
def function(signature):
    start = source.rindex(signature) # Skip the earlier forward declaration.
    return source[start:source.index('\n}\n', start) + 3]

prefix = r'''
#include <assert.h>
#include <stdio.h>
typedef float GLfloat;
typedef int GLint;
#define GL_ALPHA_TEST 0xbc0
#define WINED3D_CMP_NEVER 1
#define WINED3D_CMP_ALWAYS 8
#define WINED3D_PUSH_CONSTANTS_PS_FFP 0
#define WINED3D_GLSL_130 0
#define WINED3D_GL_LEGACY_CONTEXT 1
#define WINED3D_SHADER_TYPE_PIXEL 0
#define WINED3D_SHADER_CONST_PS_ALPHA_TEST 1
#define WINED3D_RS_SHADEMODE 1
#define STATE_RENDER(x) (x)
#define checkGLcall(s) ((void)0)
struct wined3d_ffp_ps_constants {float alpha_test_ref;};
struct wined3d_buffer {struct wined3d_ffp_ps_constants constants;};
struct wined3d_device {struct wined3d_buffer *push_constants[1];};
struct wined3d_context {struct wined3d_device *device;unsigned shader_update_mask,constant_update_mask;};
struct wined3d_state {struct {int alpha_func;} extra_ps_args;};
struct wined3d_gl_info {int supported[2];struct {struct {void(*p_glEnable)(int);void(*p_glDisable)(int);void(*p_glAlphaFunc)(int,float);}gl;}gl_ops;};
struct wined3d_context_gl {const struct wined3d_gl_info *gl_info;struct wined3d_context c;};
static int enabled,calls,last_func;
static float last_ref;
static void enable(int p){assert(p==GL_ALPHA_TEST);enabled=1;calls++;}
static void disable(int p){assert(p==GL_ALPHA_TEST);enabled=0;calls++;}
static void alpha(int f,float ref){last_func=f;last_ref=ref;calls++;}
static int wined3d_gl_compare_func(int func){return func;}
static void *wined3d_buffer_load_sysmem(struct wined3d_buffer*b,struct wined3d_context*c){(void)c;return &b->constants;}
static void state_shademode(struct wined3d_context*c,const struct wined3d_state*s,int id){(void)c;(void)s;(void)id;}
'''
tests = r'''
int main(void){
 struct wined3d_gl_info gl={.supported={1,1},.gl_ops={{enable,disable,alpha}}};
 struct wined3d_buffer buffer={{.5f}};
 struct wined3d_device device={{&buffer}};
 struct wined3d_context_gl context={&gl,{&device,1,0}};
 struct wined3d_state state={{5}};
 enabled=0;shader_glsl_update_legacy_states(&context,&state);
 if(!enabled){fprintf(stderr,"legacy test remained disabled\n");return 1;}
 assert(last_func==5&&last_ref==.5f);
 state.extra_ps_args.alpha_func=WINED3D_CMP_ALWAYS;
 shader_glsl_update_legacy_states(&context,&state);assert(!enabled);
 for(int func=1;func<8;func++){
  enabled=0;state.extra_ps_args.alpha_func=func;
  shader_glsl_update_legacy_states(&context,&state);assert(enabled&&last_func==func);
 }
 /* A blit disables the GL state; restoring a dirty pixel-shader state must enable it. */
 disable(GL_ALPHA_TEST);shader_glsl_update_legacy_states(&context,&state);assert(enabled);
 /* Constants can change the reference without changing the shader. */
 context.c.shader_update_mask=0;context.c.constant_update_mask=1;buffer.constants.alpha_test_ref=.75f;
 shader_glsl_update_legacy_states(&context,&state);assert(enabled&&last_ref==.75f);
 device.push_constants[0]=0;shader_glsl_update_legacy_states(&context,&state);assert(last_ref==0);
 /* Core contexts must not receive removed legacy API calls. */
 gl.supported[WINED3D_GL_LEGACY_CONTEXT]=0;int before=calls;
 shader_glsl_update_legacy_states(&context,&state);assert(calls==before);
 return 0;
}
'''
fixed = function('static void glsl_fragment_pipe_alpha_test_func(')
start = fixed.index('    /* On a compatibility context')
end = fixed.index('    if ((func = wined3d_gl_compare_func', start)
previous = fixed[:start] + fixed[end:]
with tempfile.TemporaryDirectory(prefix='vp-alpha-state-') as tmp:
    for name, body, expected in [('fixed', fixed, 0), ('previous', previous, 1)]:
        path = Path(tmp) / (name + '.c')
        path.write_text(prefix + body + function('static void shader_glsl_update_legacy_states(') + tests)
        exe = path.with_suffix('')
        subprocess.run([os.environ.get('CC','cc'), '-std=c11','-Wall','-Wextra','-Werror',
                        str(path), '-o', str(exe)], check=True)
        run = subprocess.run([str(exe)], capture_output=True)
        assert run.returncode == expected, run.stderr.decode()
print('PASS: enable/disable, NEVER/ALWAYS, all comparisons, changed ref, blit restore and core guard; previous failure reproduced')
