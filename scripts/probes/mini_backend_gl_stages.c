#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <string.h>

/* Real pixel checks. No version overrides, renderer substitution, or timing claims. */
static FILE *out;
static unsigned failures;
static PFNGLGENFRAMEBUFFERSPROC GenFramebuffers;
static PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
static PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus;
static PFNGLBLITFRAMEBUFFERPROC BlitFramebuffer;
static PFNGLGENBUFFERSPROC GenBuffers;
static PFNGLBINDBUFFERPROC BindBuffer;
static PFNGLBUFFERDATAPROC BufferData;
static PFNGLBUFFERSUBDATAPROC BufferSubData;
static PFNGLMAPBUFFERRANGEPROC MapBufferRange;
static PFNGLFLUSHMAPPEDBUFFERRANGEPROC FlushMappedBufferRange;
static PFNGLUNMAPBUFFERPROC UnmapBuffer;
static PFNGLCREATESHADERPROC CreateShader;
static PFNGLSHADERSOURCEPROC ShaderSource;
static PFNGLCOMPILESHADERPROC CompileShader;
static PFNGLGETSHADERIVPROC GetShaderiv;
static PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog;
static PFNGLCREATEPROGRAMPROC CreateProgram;
static PFNGLATTACHSHADERPROC AttachShader;
static PFNGLLINKPROGRAMPROC LinkProgram;
static PFNGLGETPROGRAMIVPROC GetProgramiv;
static PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog;
static PFNGLUSEPROGRAMPROC UseProgram;
static PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation;
static PFNGLUNIFORM4FPROC Uniform4f;
static PFNGLUNIFORM1IPROC Uniform1i;
static PFNGLUNIFORMMATRIX4FVPROC UniformMatrix4fv;
#define LOAD(n) do { n = (void *)wglGetProcAddress("gl" #n); if (!(n)) { fprintf(out,"MISSING gl%s\n",#n); return 6; } } while (0)

static void check(const char *stage, unsigned r, unsigned g, unsigned b)
{
    unsigned char p[4] = {0};
    glReadPixels(16,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);
    GLenum e=glGetError();
    int ok = p[0]==r && p[1]==g && p[2]==b && e==GL_NO_ERROR;
    fprintf(out,"stage=%s pixel=%u,%u,%u,%u error=%x %s\n",stage,p[0],p[1],p[2],p[3],e,ok?"OK":"FAIL");
    failures += !ok;
    fflush(out);
}

static void triangle(void)
{
    glBegin(GL_TRIANGLES);
    glTexCoord2f(.5f,.5f); glVertex2f(-1,-1);
    glTexCoord2f(.5f,.5f); glVertex2f(3,-1);
    glTexCoord2f(.5f,.5f); glVertex2f(-1,3);
    glEnd();
}

static GLuint shader(GLenum type, const char *source)
{
    GLuint s=CreateShader(type); GLint ok=0; char log[2048]={0};
    ShaderSource(s,1,&source,NULL); CompileShader(s); GetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if (!ok) { GetShaderInfoLog(s,sizeof(log),NULL,log); fprintf(out,"SHADER_FAIL type=%x %s\n",type,log); failures++; }
    return s;
}

int main(void)
{
    char path[MAX_PATH]; GetModuleFileNameA(NULL,path,sizeof(path));
    char *sep=strrchr(path,'\\'); if (!sep) return 1;
    strcpy(sep+1,"gl-stages.txt"); out=fopen(path,"wb"); if (!out) return 2;
    HWND w=CreateWindowA("STATIC","GL stages",WS_OVERLAPPEDWINDOW|WS_VISIBLE,0,0,320,320,NULL,NULL,GetModuleHandleA(NULL),NULL);
    HDC dc=GetDC(w);
    PIXELFORMATDESCRIPTOR p={sizeof(p),1,PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER,PFD_TYPE_RGBA,32};
    int pf=ChoosePixelFormat(dc,&p); fprintf(out,"pixelFormat=%d\n",pf); fflush(out);
    if (!pf || !SetPixelFormat(dc,pf,&p)) return 3;
    HGLRC rc=wglCreateContext(dc); if (!rc || !wglMakeCurrent(dc,rc)) return 4;
    fprintf(out,"renderer=%s\nversion=%s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
    LOAD(GenFramebuffers); LOAD(BindFramebuffer); LOAD(FramebufferTexture2D); LOAD(CheckFramebufferStatus); LOAD(BlitFramebuffer);
    LOAD(GenBuffers); LOAD(BindBuffer); LOAD(BufferData); LOAD(BufferSubData); LOAD(MapBufferRange); LOAD(FlushMappedBufferRange); LOAD(UnmapBuffer);
    LOAD(CreateShader); LOAD(ShaderSource); LOAD(CompileShader); LOAD(GetShaderiv); LOAD(GetShaderInfoLog);
    LOAD(CreateProgram); LOAD(AttachShader); LOAD(LinkProgram); LOAD(GetProgramiv); LOAD(GetProgramInfoLog); LOAD(UseProgram);
    LOAD(GetUniformLocation); LOAD(Uniform4f); LOAD(Uniform1i); LOAD(UniformMatrix4fv);
    GLuint rt, fbo; glGenTextures(1,&rt); glBindTexture(GL_TEXTURE_2D,rt);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,32,32,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    GenFramebuffers(1,&fbo); BindFramebuffer(GL_FRAMEBUFFER,fbo);
    FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,rt,0);
    GLenum status=CheckFramebufferStatus(GL_FRAMEBUFFER);
    fprintf(out,"fboStatus=%x\n",status); if (status!=GL_FRAMEBUFFER_COMPLETE) return 5;
    glViewport(0,0,32,32); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
    glClearColor(1,0,0,1); glClear(GL_COLOR_BUFFER_BIT); check("fbo-clear",255,0,0);
    glColor4f(0,1,0,1); triangle(); check("immediate",0,255,0);

    const GLfloat vertices[6]={-1,-1,3,-1,-1,3};
    const GLfloat outside[6]={-9,-9,-8,-9,-9,-8};
    GLuint vb,ib; GenBuffers(1,&vb); BindBuffer(GL_ARRAY_BUFFER,vb);
    BufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices,GL_DYNAMIC_DRAW);
    glEnableClientState(GL_VERTEX_ARRAY); glVertexPointer(2,GL_FLOAT,0,NULL);
    glColor4f(0,0,1,1); glDrawArrays(GL_TRIANGLES,0,3); check("vbo-data",0,0,255);
    GenBuffers(1,&ib); BindBuffer(GL_ELEMENT_ARRAY_BUFFER,ib);
    const GLushort indices[3]={0,1,2}; BufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(indices),indices,GL_STATIC_DRAW);
    glColor4f(1,0,0,1); glDrawElements(GL_TRIANGLES,3,GL_UNSIGNED_SHORT,NULL); check("ibo-data",255,0,0);
    BufferData(GL_ARRAY_BUFFER,128,NULL,GL_DYNAMIC_DRAW);
    BufferSubData(GL_ARRAY_BUFFER,48,sizeof(vertices),vertices); glVertexPointer(2,GL_FLOAT,0,(void *)48);
    glColor4f(0,1,0,1); glDrawArrays(GL_TRIANGLES,0,3); check("vbo-subdata-offset",0,255,0);
    void *map=MapBufferRange(GL_ARRAY_BUFFER,48,sizeof(vertices),GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_RANGE_BIT);
    if (map) memcpy(map,vertices,sizeof(vertices));
    if (map) UnmapBuffer(GL_ARRAY_BUFFER); else failures++;
    glColor4f(0,0,1,1); glDrawArrays(GL_TRIANGLES,0,3); check("vbo-map-offset",0,0,255);
    /* Explicit flush and a second slice must preserve the existing first slice. */
    map=MapBufferRange(GL_ARRAY_BUFFER,80,sizeof(outside),GL_MAP_WRITE_BIT|GL_MAP_FLUSH_EXPLICIT_BIT|GL_MAP_UNSYNCHRONIZED_BIT);
    if (map) {memcpy(map,outside,sizeof(outside)); FlushMappedBufferRange(GL_ARRAY_BUFFER,0,sizeof(outside)); UnmapBuffer(GL_ARRAY_BUFFER);} else failures++;
    glColor4f(1,0,0,1); glDrawArrays(GL_TRIANGLES,0,3); check("vbo-explicit-neighbor-preserved",255,0,0);
    glDisableClientState(GL_VERTEX_ARRAY); BindBuffer(GL_ARRAY_BUFFER,0); BindBuffer(GL_ELEMENT_ARRAY_BUFFER,0);

    GLuint sample; glGenTextures(1,&sample); glBindTexture(GL_TEXTURE_2D,sample);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    unsigned char pixels[4*4*4]; for(unsigned i=0;i<sizeof(pixels);i+=4){pixels[i]=0;pixels[i+1]=255;pixels[i+2]=0;pixels[i+3]=255;}
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,4,4,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    glEnable(GL_TEXTURE_2D); glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_REPLACE);
    triangle(); check("texture-image",0,255,0);
    for(unsigned i=0;i<sizeof(pixels);i+=4){pixels[i+1]=0;pixels[i+2]=255;}
    glTexSubImage2D(GL_TEXTURE_2D,0,1,1,2,2,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    triangle(); check("texture-subimage",0,0,255);
    unsigned char snapshot[64]={0};
    glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,snapshot);
    fprintf(out,"subimage-texels=");
    for(unsigned i=0;i<16;i++)fprintf(out,"%s%u,%u,%u",i?";":"",snapshot[4*i],snapshot[4*i+1],snapshot[4*i+2]);
    fprintf(out," error=%x\n",glGetError());fflush(out);
    triangle(); check("texture-subimage-after-get",0,0,255);
    GLfloat tc[4]={0};glGetFloatv(GL_CURRENT_TEXTURE_COORDS,tc);
    fprintf(out,"current-texcoord=%g,%g,%g,%g\n",tc[0],tc[1],tc[2],tc[3]);
    const char *sample_vs="#version 120\nvoid main(){gl_Position=gl_Vertex;gl_TexCoord[0]=gl_MultiTexCoord0;}";
    const char *sample_fs="#version 120\nuniform sampler2D tex;void main(){gl_FragColor=texture2D(tex,vec2(0.5,0.5));}";
    GLuint sample_prog=CreateProgram();AttachShader(sample_prog,shader(GL_VERTEX_SHADER,sample_vs));AttachShader(sample_prog,shader(GL_FRAGMENT_SHADER,sample_fs));LinkProgram(sample_prog);
    GLint sample_ok=0;GetProgramiv(sample_prog,GL_LINK_STATUS,&sample_ok);if(!sample_ok)failures++;
    UseProgram(sample_prog);Uniform1i(GetUniformLocation(sample_prog,"tex"),0);
    triangle();check("texture-subimage-constant-glsl-coord",0,0,255);
    UseProgram(0);glBindTexture(GL_TEXTURE_2D,0);glBindTexture(GL_TEXTURE_2D,sample);
    triangle();check("texture-subimage-rebind",0,0,255);
    GLuint pbo; GenBuffers(1,&pbo); BindBuffer(GL_PIXEL_UNPACK_BUFFER,pbo);
    for(unsigned i=0;i<sizeof(pixels);i+=4){pixels[i]=255;pixels[i+2]=0;}
    BufferData(GL_PIXEL_UNPACK_BUFFER,sizeof(pixels),pixels,GL_STREAM_DRAW);
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,4,4,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    BindBuffer(GL_PIXEL_UNPACK_BUFFER,0); triangle(); check("texture-pbo",255,0,0);

    const char *vs="#version 120\nuniform mat4 transform; void main(){gl_Position=transform*gl_Vertex;gl_TexCoord[0]=gl_MultiTexCoord0;}";
    const char *fs="#version 120\nuniform vec4 tint; uniform sampler2D tex; void main(){gl_FragColor=texture2D(tex,gl_TexCoord[0].xy)*tint;}";
    GLuint prog=CreateProgram(); AttachShader(prog,shader(GL_VERTEX_SHADER,vs)); AttachShader(prog,shader(GL_FRAGMENT_SHADER,fs)); LinkProgram(prog);
    GLint ok=0; GetProgramiv(prog,GL_LINK_STATUS,&ok); if (!ok){char log[2048]={0};GetProgramInfoLog(prog,sizeof(log),NULL,log);fprintf(out,"LINK_FAIL %s\n",log);failures++;}
    UseProgram(prog); const GLfloat identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    UniformMatrix4fv(GetUniformLocation(prog,"transform"),1,GL_FALSE,identity);
    Uniform1i(GetUniformLocation(prog,"tex"),0); GLint tint=GetUniformLocation(prog,"tint");
    Uniform4f(tint,1,1,1,1); triangle(); check("glsl-uniform-texture",255,0,0);
    Uniform4f(tint,0,1,0,1); triangle(); check("glsl-uniform-update",0,0,0);
    Uniform4f(tint,1,1,1,1); BindBuffer(GL_ARRAY_BUFFER,vb); glEnableClientState(GL_VERTEX_ARRAY); glVertexPointer(2,GL_FLOAT,0,(void *)48);
    glDrawArrays(GL_TRIANGLES,0,3); check("glsl-vbo-matrix",255,0,0);
    glDisableClientState(GL_VERTEX_ARRAY); BindBuffer(GL_ARRAY_BUFFER,0); UseProgram(0); glDisable(GL_TEXTURE_2D);
    /* Window framebuffer and FBO->default blit, independently from FBO readback. */
    BindFramebuffer(GL_FRAMEBUFFER,0); glViewport(0,0,32,32); glClearColor(0,0,1,1); glClear(GL_COLOR_BUFFER_BIT); check("default-clear",0,0,255);
    BindFramebuffer(GL_READ_FRAMEBUFFER,fbo); BindFramebuffer(GL_DRAW_FRAMEBUFFER,0);
    BlitFramebuffer(0,0,32,32,0,0,32,32,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    BindFramebuffer(GL_FRAMEBUFFER,0); check("fbo-default-blit",255,0,0);
    glViewport(0,0,256,256); glClearColor(0,1,0,1); glClear(GL_COLOR_BUFFER_BIT);
    fprintf(out,"swapResult=%u\n",SwapBuffers(dc));
    fprintf(out,"STAGES_DONE failures=%u\n",failures); fflush(out);
    Sleep(8000);
    wglMakeCurrent(NULL,NULL); wglDeleteContext(rc); DestroyWindow(w); fclose(out);
    return failures?10:0;
}
