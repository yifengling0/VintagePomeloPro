/* Desktop conditional rendering over GLES: pixel checks, no version override. */
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static PFNGLGENQUERIESPROC GenQueries;
static PFNGLDELETEQUERIESPROC DeleteQueries;
static PFNGLBEGINQUERYPROC BeginQuery;
static PFNGLENDQUERYPROC EndQuery;
static PFNGLBEGINCONDITIONALRENDERPROC BeginConditional;
static PFNGLENDCONDITIONALRENDERPROC EndConditional;
static PFNGLGENFRAMEBUFFERSPROC GenFramebuffers;
static PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
static PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D;
static PFNGLBLITFRAMEBUFFERPROC BlitFramebuffer;
static PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers;
static int failures,checks;
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l){return DefWindowProcA(h,m,w,l);}
static void check(FILE*out,const char*name,unsigned mode,int positive,int r,int g,int b){
 unsigned char p[4]={0};glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);GLenum err=glGetError();
 int ok=!err&&abs(p[0]-r)<3&&abs(p[1]-g)<3&&abs(p[2]-b)<3;checks++;failures+=!ok;
 fprintf(out,"pixel\t%s\tmode=%x\tpositive=%d\t%d,%d,%d\texpected=%d,%d,%d\tpass=%d\terr=%x\n",name,mode,positive,p[0],p[1],p[2],r,g,b,ok,err);fflush(out);
}
int main(int argc,char**argv){
 FILE*out=fopen(argc>1?argv[1]:"C:\\vp-conditional.tsv","w");if(!out)return 2;
 WNDCLASSA wc={0};wc.hInstance=GetModuleHandleA(NULL);wc.lpfnWndProc=proc;wc.lpszClassName="VPConditional";wc.style=CS_OWNDC;RegisterClassA(&wc);
 HWND h=CreateWindowA(wc.lpszClassName,"GPU conditional rendering",WS_OVERLAPPEDWINDOW|WS_VISIBLE,20,20,320,240,0,0,wc.hInstance,0);
 HDC dc=GetDC(h);PIXELFORMATDESCRIPTOR pf={sizeof(pf),1,PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER,PFD_TYPE_RGBA,32};
 if(!SetPixelFormat(dc,ChoosePixelFormat(dc,&pf),&pf))return 3;
 HGLRC ctx=wglCreateContext(dc);if(!ctx||!wglMakeCurrent(dc,ctx))return 4;
 fprintf(out,"renderer\t%s\t%s\n",glGetString(GL_VERSION),glGetString(GL_RENDERER));
 const char*ext=(const char*)glGetString(GL_EXTENSIONS);
 fprintf(out,"extension\tNV_conditional_render=%d\tframebuffer_sRGB=%d\n",!!strstr(ext,"GL_NV_conditional_render"),!!strstr(ext,"GL_EXT_framebuffer_sRGB"));fflush(out);
 if(!strstr(ext,"GL_NV_conditional_render")){fprintf(out,"unsupported\n");fclose(out);return 5;}
#define LOAD(v,n) v=(void*)wglGetProcAddress(n);if(!v){fprintf(out,"missing\t%s\n",n);return 6;}
 LOAD(GenQueries,"glGenQueries");LOAD(DeleteQueries,"glDeleteQueries");LOAD(BeginQuery,"glBeginQuery");LOAD(EndQuery,"glEndQuery");
 LOAD(BeginConditional,"glBeginConditionalRenderNV");LOAD(EndConditional,"glEndConditionalRenderNV");
 LOAD(GenFramebuffers,"glGenFramebuffers");LOAD(BindFramebuffer,"glBindFramebuffer");
 LOAD(FramebufferTexture2D,"glFramebufferTexture2D");LOAD(BlitFramebuffer,"glBlitFramebuffer");LOAD(DeleteFramebuffers,"glDeleteFramebuffers");
 glViewport(0,0,64,64);glDisable(GL_DITHER);glDisable(GL_BLEND);glDisable(GL_DEPTH_TEST);
 GLuint query,tex,fbo;GenQueries(1,&query);glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
 glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
 GenFramebuffers(1,&fbo);BindFramebuffer(GL_FRAMEBUFFER,fbo);FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
 glClearColor(0,0,1,1);glClear(GL_COLOR_BUFFER_BIT);BindFramebuffer(GL_FRAMEBUFFER,0);
 const GLenum modes[]={GL_QUERY_WAIT,GL_QUERY_NO_WAIT,GL_QUERY_BY_REGION_WAIT,GL_QUERY_BY_REGION_NO_WAIT};
 for(int m=0;m<4;m++)for(int positive=0;positive<2;positive++)for(int op=0;op<3;op++){
  BindFramebuffer(GL_FRAMEBUFFER,0);glClearColor(0,1,0,1);glClear(GL_COLOR_BUFFER_BIT);
  glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);BeginQuery(GL_SAMPLES_PASSED,query);if(positive)glRectf(-1,-1,1,1);EndQuery(GL_SAMPLES_PASSED);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
  /* A ready-query NO_WAIT result must be respected. Unavailable behavior is
   * deterministic in the host mock test, timing-dependent on real hardware. */
  glFinish();BeginConditional(query,modes[m]);
  if(op==0){glColor4f(1,0,0,1);glRectf(-1,-1,1,1);}
  if(op==1){glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);}
  if(op==2){BindFramebuffer(GL_READ_FRAMEBUFFER,fbo);BindFramebuffer(GL_DRAW_FRAMEBUFFER,0);BlitFramebuffer(0,0,64,64,0,0,64,64,GL_COLOR_BUFFER_BIT,GL_NEAREST);}
  EndConditional();BindFramebuffer(GL_FRAMEBUFFER,0);
  check(out,op==0?"draw":op==1?"clear":"blit",modes[m],positive,positive&&op!=2?255:0,positive?0:255,positive&&op==2?255:0);
 }
 /* End must restore rendering; query reuse must not retain a stale predicate. */
 glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);check(out,"after_end",0,1,255,0,0);
 BeginQuery(GL_SAMPLES_PASSED,query);EndQuery(GL_SAMPLES_PASSED);BeginConditional(query,GL_QUERY_WAIT);
 glBindTexture(GL_TEXTURE_2D,tex);glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,0,0,64,64);
 EndConditional();BindFramebuffer(GL_FRAMEBUFFER,fbo);check(out,"unconditional_copy",0,0,255,0,0);
 BindFramebuffer(GL_FRAMEBUFFER,0);SwapBuffers(dc);
 fprintf(out,"summary\tchecks=%d\tfailures=%d\n",checks,failures);fclose(out);
 DeleteFramebuffers(1,&fbo);glDeleteTextures(1,&tex);DeleteQueries(1,&query);wglMakeCurrent(NULL,NULL);wglDeleteContext(ctx);ReleaseDC(h,dc);DestroyWindow(h);
 return failures?7:0;
}
