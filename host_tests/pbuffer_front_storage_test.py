#!/usr/bin/env python3
"""Execute Wine's production flush_context with both swap-storage contracts."""
from pathlib import Path
import os,subprocess,tempfile,shutil
root=Path(__file__).resolve().parents[1]
names=['include/wine/opengl_driver.h','dlls/opengl32/unix_wgl.c','dlls/winewayland.drv/opengl_readback.c']
with tempfile.TemporaryDirectory(prefix='vp-storage-contract-') as tmp:
 tree=Path(tmp)
 for name in names:
  p=tree/name;p.parent.mkdir(parents=True,exist_ok=True)
  shutil.copyfile(root/'thirdparty/wine-valve'/name,p)
 patch=(root/'patches/wine/0051-opengl-preserve-pbuffer-front-storage.patch').read_bytes()
 check=subprocess.run(['patch','-p1','--batch','--force','--fuzz=0','-R','--dry-run'],cwd=tree,input=patch,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 if check.returncode:
  subprocess.run(['patch','-p1','--batch','--fuzz=0'],cwd=tree,input=patch,check=True,stdout=subprocess.PIPE)
 source=(tree/names[1]).read_text()
 start=source.index('static void flush_context(');body=source.index('{',start);depth=1;end=body+1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 function=source[start:end]
 code=r"""
#include <assert.h>
#include <stdbool.h>
typedef int BOOL;typedef unsigned UINT,GLenum;typedef void *HWND;
typedef struct {int right,bottom;} RECT;
#define NULL ((void*)0)
#define GL_FLUSH_PRESENT 8
#define GL_FLUSH_FORCE_SWAP 16
#define GL_BACK_LEFT 0x402
#define GL_FRONT_LEFT 0x400
#define GL_READ_FRAMEBUFFER 0x8ca8
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x100
#define GL_STENCIL_BUFFER_BIT 0x400
#define GL_NEAREST 0x2600
#define WARN(...) ((void)0)
struct client {HWND hwnd;};
struct opengl_drawable {struct client *client;BOOL swap_preserves_buffers;GLenum buffer_map[1];};
struct context {int draw_fbo,read_fbo;struct {GLenum read_buffer;} pixel_mode;int base;};
struct opengl_funcs {BOOL (*p_wgl_context_flush)(int*,void(*)(void),UINT);void(*p_glBindFramebuffer)(GLenum,int);void(*p_glReadBuffer)(GLenum);void(*p_glBlitFramebuffer)(int,int,int,int,int,int,int,int,GLenum,GLenum);};
typedef struct {const struct opengl_funcs *glTable;} TEB;
static struct context ctx,*current;
static struct opengl_drawable drawable,*bound;
static int front,swaps,copies,flushes,switch_draw;
static struct context *get_current_context(TEB*t,struct opengl_drawable **read,struct opengl_drawable **draw)
{(void)t;*read=*draw=bound;return current;}
static int context_draws_front(struct context*c){(void)c;return front;}
static void pop_default_fbo(TEB*t){(void)t;}
static void pop_default_fbo_buffers(TEB*t){(void)t;}
static UINT NtUserGetDpiForWindow(HWND h){(void)h;return 96;}
static void NtUserGetClientRect(HWND h,RECT*r,UINT dpi){assert(h);assert(dpi==96);r->right=320;r->bottom=240;}
static GLenum drawable_buffer_from_buffer(struct opengl_drawable*d,GLenum b){(void)d;return b;}
static BOOL context_flush(int*b,void(*f)(void),UINT flags){assert(b);if(f)f();if(flags&GL_FLUSH_FORCE_SWAP)swaps++;if(switch_draw)drawable.swap_preserves_buffers=0;return 1;}
static void bind(GLenum t,int n){(void)t;(void)n;}
static void read_buffer(GLenum b){(void)b;}
static void blit(int a,int b,int c,int d,int e,int f,int g,int h,GLenum mask,GLenum filter){(void)a;(void)b;(void)e;(void)f;assert(c==320&&d==240&&g==320&&h==240);assert(mask==(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT));assert(filter==GL_NEAREST);copies++;}
static void flush(void){flushes++;}
"""+function+r"""
static void reset(void){current=&ctx;bound=&drawable;front=1;swaps=copies=flushes=switch_draw=0;drawable.swap_preserves_buffers=0;}
int main(void){
 struct client client={(void*)1};drawable.client=&client;drawable.buffer_map[0]=GL_BACK_LEFT;
 struct opengl_funcs funcs={context_flush,bind,read_buffer,blit};TEB teb={&funcs};
 reset();flush_context(&teb,flush);assert(swaps==1&&copies==1&&flushes==1);
 reset();drawable.swap_preserves_buffers=1;flush_context(&teb,flush);assert(swaps==1&&!copies&&flushes==1);
 reset();front=0;flush_context(&teb,flush);assert(!swaps&&!copies&&flushes==1);
 reset();drawable.swap_preserves_buffers=1;switch_draw=1;flush_context(&teb,flush);assert(swaps==1&&!copies);
 reset();current=0;bound=0;flush_context(&teb,flush);assert(!swaps&&!copies&&flushes==1);
 return 0;
}
"""
 c=tree/'test.c';c.write_text(code);exe=tree/'test'
 subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: native flip copies, preserving pbuffer does not copy, presentation retained, callback lifetime and fallback')
