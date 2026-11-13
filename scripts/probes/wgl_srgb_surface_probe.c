/* MIT. Real GL 3.0/3.1 rendering and sRGB storage coherence checks.
 * No version overrides; readback is only the test oracle. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#define FUNCS(X) \
 X(PFNGLCREATESHADERPROC,CreateShader) X(PFNGLSHADERSOURCEPROC,ShaderSource) \
 X(PFNGLCOMPILESHADERPROC,CompileShader) X(PFNGLGETSHADERIVPROC,GetShaderiv) \
 X(PFNGLGETSHADERINFOLOGPROC,GetShaderInfoLog) X(PFNGLDELETESHADERPROC,DeleteShader) \
 X(PFNGLCREATEPROGRAMPROC,CreateProgram) X(PFNGLATTACHSHADERPROC,AttachShader) \
 X(PFNGLLINKPROGRAMPROC,LinkProgram) X(PFNGLGETPROGRAMIVPROC,GetProgramiv) \
 X(PFNGLGETPROGRAMINFOLOGPROC,GetProgramInfoLog) X(PFNGLUSEPROGRAMPROC,UseProgram) \
 X(PFNGLDELETEPROGRAMPROC,DeleteProgram) X(PFNGLGETUNIFORMLOCATIONPROC,GetUniformLocation) \
 X(PFNGLUNIFORM4FPROC,Uniform4f) X(PFNGLUNIFORM1IPROC,Uniform1i) \
 X(PFNGLGENVERTEXARRAYSPROC,GenVertexArrays) X(PFNGLBINDVERTEXARRAYPROC,BindVertexArray) \
 X(PFNGLDELETEVERTEXARRAYSPROC,DeleteVertexArrays) X(PFNGLGENFRAMEBUFFERSPROC,GenFramebuffers) \
 X(PFNGLBINDFRAMEBUFFERPROC,BindFramebuffer) X(PFNGLDELETEFRAMEBUFFERSPROC,DeleteFramebuffers) \
 X(PFNGLFRAMEBUFFERTEXTURE2DPROC,FramebufferTexture2D) X(PFNGLFRAMEBUFFERTEXTURELAYERPROC,FramebufferTextureLayer) \
 X(PFNGLCHECKFRAMEBUFFERSTATUSPROC,CheckFramebufferStatus) X(PFNGLBLITFRAMEBUFFERPROC,BlitFramebuffer) \
 X(PFNGLDRAWBUFFERSPROC,DrawBuffers) X(PFNGLTEXIMAGE3DPROC,TexImage3D) \
 X(PFNGLGENERATEMIPMAPPROC,GenerateMipmap) X(PFNGLGENRENDERBUFFERSPROC,GenRenderbuffers) \
 X(PFNGLBINDRENDERBUFFERPROC,BindRenderbuffer) X(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC,RenderbufferStorageMultisample) \
 X(PFNGLFRAMEBUFFERRENDERBUFFERPROC,FramebufferRenderbuffer) X(PFNGLDELETERENDERBUFFERSPROC,DeleteRenderbuffers) \
 X(PFNGLGENBUFFERSPROC,GenBuffers) X(PFNGLBINDBUFFERPROC,BindBuffer) \
 X(PFNGLBUFFERDATAPROC,BufferData) X(PFNGLMAPBUFFERRANGEPROC,MapBufferRange) \
 X(PFNGLUNMAPBUFFERPROC,UnmapBuffer) X(PFNGLDELETEBUFFERSPROC,DeleteBuffers) \
 X(PFNGLCOPYBUFFERSUBDATAPROC,CopyBufferSubData) X(PFNGLGETBUFFERSUBDATAPROC,GetBufferSubData) \
 X(PFNGLDRAWARRAYSINSTANCEDPROC,DrawArraysInstanced) X(PFNGLTRANSFORMFEEDBACKVARYINGSPROC,TransformFeedbackVaryings) \
 X(PFNGLBEGINTRANSFORMFEEDBACKPROC,BeginTransformFeedback) X(PFNGLENDTRANSFORMFEEDBACKPROC,EndTransformFeedback) \
 X(PFNGLBINDBUFFERBASEPROC,BindBufferBase) \
 X(PFNGLGETUNIFORMBLOCKINDEXPROC,GetUniformBlockIndex) X(PFNGLUNIFORMBLOCKBINDINGPROC,UniformBlockBinding) \
 X(PFNGLTEXBUFFERPROC,TexBuffer) X(PFNGLPRIMITIVERESTARTINDEXPROC,PrimitiveRestartIndex)
#define DECL(type,n) static type n;
FUNCS(DECL)
static FILE *out; static int checks, failures, ver;
static GLuint solid, sample, vao; static HDC dc;
typedef HGLRC (WINAPI *create_fn)(HDC,HGLRC,const int *);
static create_fn CreateContext;
static int load(void) {
#define GET(type,n) n=(type)wglGetProcAddress("gl" #n); if(!n) {fprintf(out,"missing\t%s\n",#n);return 0;}
 FUNCS(GET)
 return 1;
}
static void record(const char *name,int pass,GLenum err) {
 ++checks; if(!pass||err)++failures;
 fprintf(out,"check\t%d\t%s\t%d\tglerr=%u\n",ver,name,pass&&!err,err);fflush(out);
}
static int enc(float v) {return (int)lroundf(255*(v<=.0031308f?12.92f*v:1.055f*powf(v,1.f/2.4f)-.055f));}
static void pixel(const char *name,int x,int y,int r,int g,int b) {
 unsigned char p[4]={0}; glReadPixels(x,y,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);
 GLenum e=glGetError(); fprintf(out,"pixel\t%d\t%s\t%u,%u,%u,%u\texpect=%d,%d,%d\n",ver,name,p[0],p[1],p[2],p[3],r,g,b);
 record(name,abs((int)p[0]-r)<=2&&abs((int)p[1]-g)<=2&&abs((int)p[2]-b)<=2,e);
}
static GLuint shader(GLenum type,const char *text) {
 GLuint s=CreateShader(type); GLint ok; char log[2048];
 ShaderSource(s,1,&text,NULL);CompileShader(s);GetShaderiv(s,GL_COMPILE_STATUS,&ok);
 if(!ok){GetShaderInfoLog(s,sizeof(log),NULL,log);fprintf(out,"shaderFail\t%s\n",log);DeleteShader(s);return 0;}return s;
}
static GLuint program(const char *body,int feedback) {
 char vs[2048],fs[2048],log[2048]; GLint ok; GLuint p,v,f;
 snprintf(vs,sizeof(vs),"#version %d\nout vec2 uv;out float captured;void main(){vec2 q[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));uv=(q[gl_VertexID]+1.0)*0.5;captured=float(gl_VertexID)+0.25;gl_Position=vec4(q[gl_VertexID],0,1);}",ver);
 snprintf(fs,sizeof(fs),"#version %d\nin vec2 uv;uniform vec4 color;uniform sampler2D tex;out vec4 c0;out vec4 c1;void main(){c0=%s;c1=c0;}",ver,body);
 v=shader(GL_VERTEX_SHADER,vs);f=shader(GL_FRAGMENT_SHADER,fs);if(!v||!f)return 0;
 p=CreateProgram();AttachShader(p,v);AttachShader(p,f);
 if(feedback){const char *n="captured";TransformFeedbackVaryings(p,1,&n,GL_INTERLEAVED_ATTRIBS);}
 LinkProgram(p);GetProgramiv(p,GL_LINK_STATUS,&ok);DeleteShader(v);DeleteShader(f);
 if(!ok){GetProgramInfoLog(p,sizeof(log),NULL,log);fprintf(out,"linkFail\t%s\n",log);DeleteProgram(p);return 0;}return p;
}
static void draw(float r,float g,float b,float a) {UseProgram(solid);Uniform4f(GetUniformLocation(solid,"color"),r,g,b,a);glDrawArrays(GL_TRIANGLES,0,3);}
static void attach(GLuint fbo,GLenum target,GLuint tex,int level,int layer) {
 BindFramebuffer(GL_FRAMEBUFFER,fbo);
 if(target==GL_TEXTURE_2D_ARRAY||target==GL_TEXTURE_3D)FramebufferTextureLayer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,tex,level,layer);
 else FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,target,tex,level);
 glDrawBuffer(GL_COLOR_ATTACHMENT0);glReadBuffer(GL_COLOR_ATTACHMENT0);
 record("framebuffer-complete",CheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,glGetError());
}
static GLuint texture(GLenum internal,GLenum target,int levels,int layers) {
 GLuint t;int i;glGenTextures(1,&t);glBindTexture(target,t);
 glTexParameteri(target,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(target,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
 glTexParameteri(target,GL_TEXTURE_MAX_LEVEL,levels-1);
 for(i=0;i<levels;i++){
   int w=8>>i;
   if(target==GL_TEXTURE_2D_ARRAY||target==GL_TEXTURE_3D)TexImage3D(target,i,internal,w,w,layers,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
   else if(target==GL_TEXTURE_CUBE_MAP){int face;for(face=0;face<6;face++)glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,i,internal,w,w,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);}
   else glTexImage2D(target,i,internal,w,w,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
 }
 return t;
}
static void color_tests(void) {
 GLuint fbo[2],t[2];int s;GenFramebuffers(2,fbo);
 t[0]=texture(GL_SRGB8_ALPHA8,GL_TEXTURE_2D,3,1);t[1]=texture(GL_RGBA8,GL_TEXTURE_2D,1,1);
 glViewport(0,0,8,8);
 for(s=0;s<2;s++){
   attach(fbo[0],GL_TEXTURE_2D,t[0],0,0);
   if(s)glEnable(GL_FRAMEBUFFER_SRGB);else glDisable(GL_FRAMEBUFFER_SRGB);
   glClearColor(.25,.5,.75,1);glClear(GL_COLOR_BUFFER_BIT);
   pixel(s?"srgb-clear-on":"srgb-clear-off",4,4,s?enc(.25):64,s?enc(.5):128,s?enc(.75):191);
   draw(.25,.5,.75,1);
   pixel(s?"srgb-draw-on":"srgb-draw-off",4,4,s?enc(.25):64,s?enc(.5):128,s?enc(.75):191);
   attach(fbo[1],GL_TEXTURE_2D,t[1],0,0);glBindTexture(GL_TEXTURE_2D,t[0]);
   UseProgram(sample);Uniform1i(GetUniformLocation(sample,"tex"),0);glDrawArrays(GL_TRIANGLES,0,3);
   pixel(s?"srgb-sample-on":"srgb-sample-off",4,4,s?64:13,s?128:55,s?191:133);
 }
 attach(fbo[0],GL_TEXTURE_2D,t[0],0,0);glEnable(GL_FRAMEBUFFER_SRGB);draw(.25,.25,.25,1);
 glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);draw(.5,.5,.5,.5);
 pixel("destination-linearized-blend",4,4,enc(.375),enc(.375),enc(.375));glDisable(GL_BLEND);
 /* Upload into the public storage after its linear shadow has already been used. */
 glDisable(GL_FRAMEBUFFER_SRGB);draw(.8,.8,.8,1);
 {unsigned char data[256];memset(data,64,sizeof(data));glBindTexture(GL_TEXTURE_2D,t[0]);glTexSubImage2D(GL_TEXTURE_2D,0,0,0,8,8,GL_BGRA,GL_UNSIGNED_BYTE,data);}
 glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);draw(.5,.5,.5,.5);
 pixel("upload-after-shadow-blend",4,4,96,96,96);glDisable(GL_BLEND);
 glEnable(GL_FRAMEBUFFER_SRGB);draw(.25,.25,.25,1);
 glEnable(GL_SCISSOR_TEST);glScissor(0,0,4,8);draw(.5,.5,.5,1);glDisable(GL_SCISSOR_TEST);
 pixel("scissor-written",2,4,enc(.5),enc(.5),enc(.5));pixel("scissor-preserved",6,4,enc(.25),enc(.25),enc(.25));
 glColorMask(GL_FALSE,GL_TRUE,GL_FALSE,GL_TRUE);draw(.75,.75,.75,1);glColorMask(1,1,1,1);
 pixel("color-mask-preserved",6,4,enc(.25),enc(.75),enc(.25));
 /* Alternate views with no readback between the dependent draws. */
 glEnable(GL_FRAMEBUFFER_SRGB);draw(.25,.25,.25,1);glDisable(GL_FRAMEBUFFER_SRGB);
 glEnable(GL_BLEND);draw(.5,.5,.5,.5);glDisable(GL_BLEND);
 pixel("dependent-view-switch",4,4,132,132,132);
 /* Mipmap generation consumes canonical contents after an emulated write. */
 draw(.5,.5,.5,1);glBindTexture(GL_TEXTURE_2D,t[0]);GenerateMipmap(GL_TEXTURE_2D);
 attach(fbo[0],GL_TEXTURE_2D,t[0],1,0);glViewport(0,0,4,4);pixel("mipmap-shadow-write",2,2,128,128,128);
 glEnable(GL_FRAMEBUFFER_SRGB);draw(.25,.25,.25,1);pixel("mipmap-typed-draw",2,2,enc(.25),enc(.25),enc(.25));
 glViewport(0,0,8,8);attach(fbo[0],GL_TEXTURE_2D,t[0],0,0);draw(.25,.25,.25,1);
 attach(fbo[1],GL_TEXTURE_2D,t[1],0,0);BindFramebuffer(GL_READ_FRAMEBUFFER,fbo[0]);BindFramebuffer(GL_DRAW_FRAMEBUFFER,fbo[1]);
 BlitFramebuffer(0,0,8,8,0,0,8,8,GL_COLOR_BUFFER_BIT,GL_NEAREST);
 BindFramebuffer(GL_FRAMEBUFFER,fbo[1]);pixel("blit-srgb-to-linear",4,4,64,64,64);
 /* One MRT command with both colorspaces. */
 attach(fbo[0],GL_TEXTURE_2D,t[0],0,0);FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT1,GL_TEXTURE_2D,t[1],0);
 {GLenum bufs[2]={GL_COLOR_ATTACHMENT0,GL_COLOR_ATTACHMENT1};DrawBuffers(2,bufs);}
 draw(.25,.5,.75,1);glReadBuffer(GL_COLOR_ATTACHMENT0);pixel("mixed-mrt-srgb",4,4,enc(.25),enc(.5),enc(.75));
 glReadBuffer(GL_COLOR_ATTACHMENT1);pixel("mixed-mrt-linear",4,4,64,128,191);
 BindFramebuffer(GL_FRAMEBUFFER,0);glBindTexture(GL_TEXTURE_2D,0);DeleteFramebuffers(2,fbo);glDeleteTextures(2,t);
}
static void layers(void) {
 GLenum targets[]={GL_TEXTURE_2D_ARRAY,GL_TEXTURE_CUBE_MAP,GL_TEXTURE_3D};int k,l;GLuint fbo;
 GenFramebuffers(1,&fbo);glViewport(0,0,8,8);
 for(k=0;k<3;k++){
   int n=k==1?6:2;GLuint t=texture(GL_SRGB8_ALPHA8,targets[k],1,2);
   for(l=0;l<n;l++){
     GLenum at=k==1?GL_TEXTURE_CUBE_MAP_POSITIVE_X+l:targets[k];char name[100];
     attach(fbo,at,t,0,l);glEnable(GL_FRAMEBUFFER_SRGB);draw(.25,.25,.25,1);
     glDisable(GL_FRAMEBUFFER_SRGB);draw(.5,.5,.5,1);
     snprintf(name,sizeof(name),"layer-%d-%d-off",k,l);pixel(name,4,4,128,128,128);
   }
   for(l=0;l<n;l++){
     GLenum at=k==1?GL_TEXTURE_CUBE_MAP_POSITIVE_X+l:targets[k];char name[100];
     attach(fbo,at,t,0,l);glEnable(GL_FRAMEBUFFER_SRGB);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);draw(.25,.25,.25,.5);glDisable(GL_BLEND);
     snprintf(name,sizeof(name),"layer-%d-%d-blend",k,l);pixel(name,4,4,enc(.233),enc(.233),enc(.233));
   }
   BindFramebuffer(GL_FRAMEBUFFER,0);glBindTexture(targets[k],0);glDeleteTextures(1,&t);
 }
 DeleteFramebuffers(1,&fbo);
}
static void msaa(void) {
 GLuint fb[2],rb,t;int s;GenFramebuffers(2,fb);GenRenderbuffers(1,&rb);BindRenderbuffer(GL_RENDERBUFFER,rb);
 RenderbufferStorageMultisample(GL_RENDERBUFFER,4,GL_SRGB8_ALPHA8,8,8);
 BindFramebuffer(GL_FRAMEBUFFER,fb[0]);FramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,rb);
 record("msaa-complete",CheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,glGetError());
 t=texture(GL_SRGB8_ALPHA8,GL_TEXTURE_2D,1,1);attach(fb[1],GL_TEXTURE_2D,t,0,0);glViewport(0,0,8,8);
 for(s=0;s<2;s++){
   if(s)glEnable(GL_FRAMEBUFFER_SRGB);else glDisable(GL_FRAMEBUFFER_SRGB);
   BindFramebuffer(GL_FRAMEBUFFER,fb[0]);draw(.25,.5,.75,1);
   BindFramebuffer(GL_READ_FRAMEBUFFER,fb[0]);BindFramebuffer(GL_DRAW_FRAMEBUFFER,fb[1]);
   BlitFramebuffer(0,0,8,8,0,0,8,8,GL_COLOR_BUFFER_BIT,GL_NEAREST);BindFramebuffer(GL_FRAMEBUFFER,fb[1]);
   pixel(s?"msaa-resolve-on":"msaa-resolve-off",4,4,s?enc(.25):64,s?enc(.5):128,s?enc(.75):191);
 }
 BindFramebuffer(GL_FRAMEBUFFER,0);DeleteFramebuffers(2,fb);DeleteRenderbuffers(1,&rb);glDeleteTextures(1,&t);
}
static void buffer_tests(void) {
 GLuint b[2],tf;unsigned values[]={1,7,19,31},actual[4]={0};float captures[3]={0};void *p;
 GenBuffers(2,b);BindBuffer(GL_COPY_READ_BUFFER,b[0]);BufferData(GL_COPY_READ_BUFFER,sizeof(values),values,GL_STATIC_DRAW);
 BindBuffer(GL_COPY_WRITE_BUFFER,b[1]);BufferData(GL_COPY_WRITE_BUFFER,sizeof(values),NULL,GL_STREAM_COPY);
 CopyBufferSubData(GL_COPY_READ_BUFFER,GL_COPY_WRITE_BUFFER,0,0,sizeof(values));
 p=MapBufferRange(GL_COPY_WRITE_BUFFER,0,sizeof(values),GL_MAP_READ_BIT);if(p){memcpy(actual,p,sizeof(actual));UnmapBuffer(GL_COPY_WRITE_BUFFER);}
 record("gl31-copy-buffer-map",p&&!memcmp(values,actual,sizeof(values)),glGetError());
 tf=program("color",1);UseProgram(tf);BindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,b[1]);BufferData(GL_TRANSFORM_FEEDBACK_BUFFER,sizeof(captures),NULL,GL_STREAM_READ);
 BindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,b[1]);glEnable(GL_RASTERIZER_DISCARD);
 BeginTransformFeedback(GL_POINTS);glDrawArrays(GL_POINTS,0,3);EndTransformFeedback();glDisable(GL_RASTERIZER_DISCARD);
 GetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER,0,sizeof(captures),captures);
 record("gl30-transform-feedback",fabsf(captures[0]-.25f)<.001f&&fabsf(captures[1]-1.25f)<.001f&&fabsf(captures[2]-2.25f)<.001f,glGetError());
 BindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,0);UseProgram(0);DeleteProgram(tf);DeleteBuffers(2,b);
 BindFramebuffer(GL_FRAMEBUFFER,0);glDisable(GL_FRAMEBUFFER_SRGB);glViewport(0,0,8,8);UseProgram(solid);
 Uniform4f(GetUniformLocation(solid,"color"),.25,.5,.75,1);DrawArraysInstanced(GL_TRIANGLES,0,3,2);
 pixel("gl31-instanced-draw",4,4,64,128,191);
}
static void gl31_features(void) {
 GLuint ubo,tbo,tex,p,v,f,buffers[2];GLint ok;float colors[4]={.25,.5,.75,1};unsigned short indices[]={0,1,2,65535,0,1,2};
 const char *vs="#version 140\nvoid main(){vec2 q[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));gl_Position=vec4(q[gl_VertexID],0,1);}";
 const char *fs="#version 140\nlayout(std140) uniform Colors{vec4 color;};out vec4 result;void main(){result=color;}";
 v=shader(GL_VERTEX_SHADER,vs);f=shader(GL_FRAGMENT_SHADER,fs);p=CreateProgram();AttachShader(p,v);AttachShader(p,f);LinkProgram(p);GetProgramiv(p,GL_LINK_STATUS,&ok);
 record("glsl140-ubo-link",ok,glGetError());UseProgram(p);GenBuffers(1,&ubo);BindBuffer(GL_UNIFORM_BUFFER,ubo);BufferData(GL_UNIFORM_BUFFER,sizeof(colors),colors,GL_DYNAMIC_DRAW);
 UniformBlockBinding(p,GetUniformBlockIndex(p,"Colors"),0);BindBufferBase(GL_UNIFORM_BUFFER,0,ubo);
 BindFramebuffer(GL_FRAMEBUFFER,0);glDisable(GL_FRAMEBUFFER_SRGB);glViewport(0,0,8,8);glDrawArrays(GL_TRIANGLES,0,3);pixel("glsl140-ubo-draw",4,4,64,128,191);
 UseProgram(0);DeleteProgram(p);DeleteShader(v);DeleteShader(f);BindBufferBase(GL_UNIFORM_BUFFER,0,0);DeleteBuffers(1,&ubo);
 fs="#version 140\nuniform samplerBuffer values;out vec4 result;void main(){result=texelFetch(values,0);}";
 v=shader(GL_VERTEX_SHADER,vs);f=shader(GL_FRAGMENT_SHADER,fs);p=CreateProgram();AttachShader(p,v);AttachShader(p,f);LinkProgram(p);GetProgramiv(p,GL_LINK_STATUS,&ok);record("glsl140-texbuffer-link",ok,glGetError());
 GenBuffers(1,&tbo);BindBuffer(GL_TEXTURE_BUFFER,tbo);BufferData(GL_TEXTURE_BUFFER,sizeof(colors),colors,GL_STATIC_DRAW);glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_BUFFER,tex);TexBuffer(GL_TEXTURE_BUFFER,GL_RGBA32F,tbo);
 UseProgram(p);Uniform1i(GetUniformLocation(p,"values"),0);glDrawArrays(GL_TRIANGLES,0,3);pixel("glsl140-texbuffer-draw",4,4,64,128,191);
 UseProgram(0);DeleteProgram(p);DeleteShader(v);DeleteShader(f);glBindTexture(GL_TEXTURE_BUFFER,0);glDeleteTextures(1,&tex);DeleteBuffers(1,&tbo);
 GenBuffers(1,buffers);BindBuffer(GL_ELEMENT_ARRAY_BUFFER,buffers[0]);BufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(indices),indices,GL_STATIC_DRAW);
 UseProgram(solid);Uniform4f(GetUniformLocation(solid,"color"),.25,.5,.75,1);glEnable(GL_PRIMITIVE_RESTART);PrimitiveRestartIndex(65535);glDrawElements(GL_TRIANGLE_STRIP,7,GL_UNSIGNED_SHORT,NULL);glDisable(GL_PRIMITIVE_RESTART);
 pixel("gl31-primitive-restart",4,4,64,128,191);BindBuffer(GL_ELEMENT_ARRAY_BUFFER,0);DeleteBuffers(1,buffers);
}
static void shared_context(HGLRC ctx,int minor) {
 GLuint t,fbo,otherVao;HGLRC other;int attrs[]={0x2091,3,0x2092,minor,0};
 t=texture(GL_SRGB8_ALPHA8,GL_TEXTURE_2D,1,1);GenFramebuffers(1,&fbo);attach(fbo,GL_TEXTURE_2D,t,0,0);glViewport(0,0,8,8);
 glDisable(GL_FRAMEBUFFER_SRGB);draw(.25,.25,.25,1);glFinish();DeleteFramebuffers(1,&fbo);
 other=CreateContext(dc,ctx,attrs);record("shared-context-create",other!=NULL,glGetError());
 if(other&&wglMakeCurrent(dc,other)){
   GenVertexArrays(1,&otherVao);BindVertexArray(otherVao);GenFramebuffers(1,&fbo);attach(fbo,GL_TEXTURE_2D,t,0,0);glViewport(0,0,8,8);
   glEnable(GL_FRAMEBUFFER_SRGB);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);draw(.5,.5,.5,.5);glDisable(GL_BLEND);
   pixel("shared-context-coherent-blend",4,4,enc(.2755),enc(.2755),enc(.2755));glFinish();
   UseProgram(0);BindFramebuffer(GL_FRAMEBUFFER,0);DeleteFramebuffers(1,&fbo);DeleteVertexArrays(1,&otherVao);
   wglMakeCurrent(NULL,NULL);wglDeleteContext(other);wglMakeCurrent(dc,ctx);
 }
 glDeleteTextures(1,&t);BindVertexArray(vao);
}
typedef BOOL (WINAPI *choose_pf_fn)(HDC,const int *,const FLOAT *,UINT,int *,UINT *);
static void window_srgb(HGLRC ctx,int minor) {
 choose_pf_fn choose=(choose_pf_fn)wglGetProcAddress("wglChoosePixelFormatARB");
 HWND window=CreateWindowA("VPSrgbSurface","sRGB native window",WS_OVERLAPPEDWINDOW|WS_VISIBLE,0,0,160,160,NULL,NULL,GetModuleHandleA(NULL),NULL);
 HDC windowdc=GetDC(window);HGLRC other=NULL;GLuint otherVao;int format=0;UINT count=0;
 int attrs[]={0x2001,1,0x2010,1,0x2011,1,0x2013,0x202B,0x2014,32,0x20A9,1,0};
 int ca[]={0x2091,3,0x2092,minor,0};PIXELFORMATDESCRIPTOR pfd;
 if(!choose||!choose(windowdc,attrs,NULL,1,&format,&count)||!count) {
   fprintf(out,"optional\twinsys-srgb-format-unavailable\n");
   goto end;
 }
 DescribePixelFormat(windowdc,format,sizeof(pfd),&pfd);SetPixelFormat(windowdc,format,&pfd);
 other=CreateContext(windowdc,ctx,ca);record("winsys-srgb-context",other!=NULL,0);
 if(!other||!wglMakeCurrent(windowdc,other))goto end;
 GenVertexArrays(1,&otherVao);BindVertexArray(otherVao);glViewport(0,0,8,8);glDisable(GL_DITHER);
 glDisable(GL_FRAMEBUFFER_SRGB);draw(.25,.5,.75,1);pixel("winsys-srgb-off",4,4,64,128,191);
 glEnable(GL_FRAMEBUFFER_SRGB);draw(.25,.5,.75,1);pixel("winsys-srgb-on",4,4,enc(.25),enc(.5),enc(.75));
 SwapBuffers(windowdc);glDisable(GL_FRAMEBUFFER_SRGB);draw(.25,.5,.75,1);pixel("winsys-srgb-after-swap",4,4,64,128,191);
 UseProgram(0);DeleteVertexArrays(1,&otherVao);wglMakeCurrent(NULL,NULL);
end:
 if(other) wglDeleteContext(other);
 ReleaseDC(window,windowdc);DestroyWindow(window);wglMakeCurrent(dc,ctx);BindVertexArray(vao);
}
static LRESULT CALLBACK wndproc(HWND w,UINT m,WPARAM p,LPARAM l){return DefWindowProcA(w,m,p,l);}
int main(int argc,char **argv) {
 WNDCLASSA wc={0};PIXELFORMATDESCRIPTOR pfd={sizeof(pfd),1,PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER,PFD_TYPE_RGBA,32};
 HWND w;HGLRC boot,ctx;int minor;out=fopen(argc>1?argv[1]:"C:\\vp-srgb-surface.tsv","w");if(!out)return 2;
 wc.lpfnWndProc=wndproc;wc.lpszClassName="VPSrgbSurface";wc.style=CS_OWNDC;wc.hInstance=GetModuleHandleA(NULL);RegisterClassA(&wc);
 w=CreateWindowA(wc.lpszClassName,"GL 3.0 / 3.1 GPU regression",WS_OVERLAPPEDWINDOW|WS_VISIBLE,0,0,160,160,NULL,NULL,wc.hInstance,NULL);
 dc=GetDC(w);SetPixelFormat(dc,ChoosePixelFormat(dc,&pfd),&pfd);boot=wglCreateContext(dc);wglMakeCurrent(dc,boot);CreateContext=(create_fn)wglGetProcAddress("wglCreateContextAttribsARB");
 if(!CreateContext||!load())return 3;
 for(minor=0;minor<=1;minor++){
   int attrs[]={0x2091,3,0x2092,minor,0};ver=130+10*minor;ctx=CreateContext(dc,NULL,attrs);
   record("requested-context",ctx!=NULL,0);if(!ctx||!wglMakeCurrent(dc,ctx))continue;
   fprintf(out,"context\t3.%d\t%s\t%s\t%s\n",minor,glGetString(GL_VERSION),glGetString(GL_SHADING_LANGUAGE_VERSION),glGetString(GL_RENDERER));fflush(out);
   GenVertexArrays(1,&vao);BindVertexArray(vao);solid=program("color",0);sample=program("texture(tex,uv)",0);
   record("glsl-compile-link",solid&&sample,glGetError());glDisable(GL_DITHER);glDisable(GL_DEPTH_TEST);
   if(solid&&sample){color_tests();layers();msaa();buffer_tests();if(minor)gl31_features();shared_context(ctx,minor);window_srgb(ctx,minor);}
   UseProgram(0);DeleteProgram(solid);DeleteProgram(sample);DeleteVertexArrays(1,&vao);
   wglMakeCurrent(NULL,NULL);wglDeleteContext(ctx);wglMakeCurrent(dc,boot);
 }
 fprintf(out,"summary\tchecks=%d\tfailures=%d\n",checks,failures);fclose(out);
 wglMakeCurrent(NULL,NULL);wglDeleteContext(boot);ReleaseDC(w,dc);DestroyWindow(w);return failures?1:0;
}
