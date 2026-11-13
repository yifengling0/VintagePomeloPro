/* Exercise the production WGL/VirGL path, including all 12 blocks in
 * each graphics stage, ordinary uniforms, range binding and upload changes. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
typedef HGLRC (WINAPI *create_attribs_fn)(HDC,HGLRC,const int *);
#define LOAD(type,name) static type name
LOAD(PFNGLCREATESHADERPROC,CreateShader);
LOAD(PFNGLSHADERSOURCEPROC,ShaderSource);
LOAD(PFNGLCOMPILESHADERPROC,CompileShader);
LOAD(PFNGLGETSHADERIVPROC,GetShaderiv);
LOAD(PFNGLGETSHADERINFOLOGPROC,GetShaderInfoLog);
LOAD(PFNGLCREATEPROGRAMPROC,CreateProgram);
LOAD(PFNGLATTACHSHADERPROC,AttachShader);
LOAD(PFNGLLINKPROGRAMPROC,LinkProgram);
LOAD(PFNGLGETPROGRAMIVPROC,GetProgramiv);
LOAD(PFNGLGETPROGRAMINFOLOGPROC,GetProgramInfoLog);
LOAD(PFNGLUSEPROGRAMPROC,UseProgram);
LOAD(PFNGLDELETEPROGRAMPROC,DeleteProgram);
LOAD(PFNGLDELETESHADERPROC,DeleteShader);
LOAD(PFNGLGENBUFFERSPROC,GenBuffers);
LOAD(PFNGLBINDBUFFERPROC,BindBuffer);
LOAD(PFNGLBUFFERDATAPROC,BufferData);
LOAD(PFNGLBUFFERSUBDATAPROC,BufferSubData);
LOAD(PFNGLBINDBUFFERRANGEPROC,BindBufferRange);
LOAD(PFNGLDELETEBUFFERSPROC,DeleteBuffers);
LOAD(PFNGLGETUNIFORMBLOCKINDEXPROC,GetUniformBlockIndex);
LOAD(PFNGLUNIFORMBLOCKBINDINGPROC,UniformBlockBinding);
LOAD(PFNGLGETUNIFORMLOCATIONPROC,GetUniformLocation);
LOAD(PFNGLUNIFORM4FPROC,Uniform4f);
LOAD(PFNGLUNIFORM1IPROC,Uniform1i);
LOAD(PFNGLGENVERTEXARRAYSPROC,GenVertexArrays);
LOAD(PFNGLBINDVERTEXARRAYPROC,BindVertexArray);
LOAD(PFNGLDELETEVERTEXARRAYSPROC,DeleteVertexArrays);
LOAD(PFNGLBINDATTRIBLOCATIONPROC,BindAttribLocation);
LOAD(PFNGLVERTEXATTRIBPOINTERPROC,VertexAttribPointer);
LOAD(PFNGLENABLEVERTEXATTRIBARRAYPROC,EnableVertexAttribArray);
#define GET(name) name=(void*)wglGetProcAddress("gl" #name); if (!name) return 0
static int load(void) {
 GET(CreateShader); GET(ShaderSource); GET(CompileShader); GET(GetShaderiv);
 GET(GetShaderInfoLog); GET(CreateProgram); GET(AttachShader); GET(LinkProgram);
 GET(GetProgramiv); GET(GetProgramInfoLog); GET(UseProgram); GET(DeleteProgram); GET(DeleteShader);
 GET(GenBuffers); GET(BindBuffer); GET(BufferData); GET(BufferSubData); GET(BindBufferRange);
 GET(DeleteBuffers); GET(GetUniformBlockIndex); GET(UniformBlockBinding); GET(GetUniformLocation);
 GET(Uniform4f); GET(Uniform1i); GET(GenVertexArrays); GET(BindVertexArray); GET(DeleteVertexArrays);
 GET(BindAttribLocation); GET(VertexAttribPointer); GET(EnableVertexAttribArray);
 return 1;
}
static GLuint compile(GLenum type, const char *text, FILE *out) {
 GLuint s=CreateShader(type); GLint ok=0; char log[4096];
 ShaderSource(s,1,&text,NULL); CompileShader(s); GetShaderiv(s,GL_COMPILE_STATUS,&ok);
 if (!ok) {GetShaderInfoLog(s,sizeof(log),NULL,log); fprintf(out,"compileFail\t%u\t%s\n",type,log); DeleteShader(s);return 0;}
 return s;
}
static int pixel(FILE *out,const char *profile,int dynamic,int step, float r,float g,float b) {
 unsigned char p[4]={0}; GLenum error; int ok;
 glClearColor(1,0,1,1); glClear(GL_COLOR_BUFFER_BIT); glDrawArrays(GL_TRIANGLES,0,3);
 glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p); error=glGetError();
 ok=error==0 && abs(p[0]-(int)lroundf(r*255))<=2 && abs(p[1]-(int)lroundf(g*255))<=2 && abs(p[2]-(int)lroundf(b*255))<=2;
 fprintf(out,"draw\t%s\t%d\t%d\t%u,%u,%u,%u\t%.3f,%.3f,%.3f\t%d\t%u\n",profile,dynamic,step,p[0],p[1],p[2],p[3],r,g,b,ok,error);fflush(out);return ok;
}
static int exercise(FILE *out,const char *profile,int es,int dynamic) {
 char vs[16384],fs[16384],line[1024],block[64]; GLuint v=0,f=0,program=0,buffer=0,vao=0,vertexBuffer=0;
 GLint ok=0, alignment=0, vertex_blocks=0,fragment_blocks=0,bindings=0, stride; float *data; int i,pass=0;
 glGetIntegerv(GL_MAX_VERTEX_UNIFORM_BLOCKS,&vertex_blocks);glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_BLOCKS,&fragment_blocks);
 glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS,&bindings);glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT,&alignment);
 fprintf(out,"limits\t%s\tvertex=%d\tfragment=%d\tbindings=%d\talign=%d\n",profile,vertex_blocks,fragment_blocks,bindings,alignment);
 if(vertex_blocks<12||fragment_blocks<12||bindings<24||alignment<=0)return 0;
 strcpy(vs,es?"#version 300 es\nprecision highp float;precision highp int;\n":"#version 120\n#extension GL_ARB_uniform_buffer_object : require\n");
 strcpy(fs,vs);
 for(i=0;i<12;i++) {
   sprintf(line,"layout(std140) uniform VB%d {vec4 v%d%s;};\n",i,i,dynamic&&i==0?"[2]":"");strcat(vs,line);
   sprintf(line,"layout(std140) uniform FB%d {vec4 f%d%s;};\n",i,i,dynamic&&i==0?"[2]":"");strcat(fs,line);
 }
 if(dynamic){strcat(vs,"uniform int pick;\n");strcat(fs,"uniform int pick;\n");}
 strcat(vs,es?"uniform vec4 vertexOrdinary;out vec4 contribution;in vec2 position;":"uniform vec4 vertexOrdinary;varying vec4 contribution;attribute vec2 position;");
 strcat(vs,"void main(){gl_Position=vec4(position,0,1);contribution=vertexOrdinary;");
 strcat(fs,es?"uniform vec4 fragmentOrdinary;in vec4 contribution;out vec4 color;":"uniform vec4 fragmentOrdinary;varying vec4 contribution;\n#define color gl_FragColor\n");
 strcat(fs,"void main(){color=contribution+fragmentOrdinary;");
 for(i=0;i<12;i++) {
   sprintf(line,"contribution+=v%d%s;",i,dynamic&&i==0?"[pick]":"");strcat(vs,line);
   sprintf(line,"color+=f%d%s;",i,dynamic&&i==0?"[pick]":"");strcat(fs,line);
 }
 strcat(vs,"}");strcat(fs,"color.a=1.0;}");
 v=compile(GL_VERTEX_SHADER,vs,out);f=compile(GL_FRAGMENT_SHADER,fs,out);
 if(!v||!f)goto end;
 program=CreateProgram();AttachShader(program,v);AttachShader(program,f);BindAttribLocation(program,0,"position");LinkProgram(program);GetProgramiv(program,GL_LINK_STATUS,&ok);
 if(!ok){GetProgramInfoLog(program,sizeof(line),NULL,line);fprintf(out,"linkFail\t%s\t%s\n",profile,line);goto end;}
 UseProgram(program);Uniform4f(GetUniformLocation(program,"vertexOrdinary"),.05f,.10f,.15f,0);
 Uniform4f(GetUniformLocation(program,"fragmentOrdinary"),0,0,0,0);
 stride=((32+alignment-1)/alignment)*alignment;data=calloc(24,stride);
 for(i=0;i<24;i++){float value=(i%12+1)*.001f;float *p=(float*)((char*)data+i*stride);p[0]=p[1]=p[2]=value;p[4]=p[5]=p[6]=.007f;}
 GenBuffers(1,&buffer);BindBuffer(GL_UNIFORM_BUFFER,buffer);BufferData(GL_UNIFORM_BUFFER,24*stride,data,GL_DYNAMIC_DRAW);free(data);
 for(i=0;i<24;i++) {
   sprintf(block,"%s%d",i<12?"VB":"FB",i%12);
   GLuint index=GetUniformBlockIndex(program,block);
   if(index==GL_INVALID_INDEX){fprintf(out,"missingBlock\t%s\n",block);goto end;}
   UniformBlockBinding(program,index,i);BindBufferRange(GL_UNIFORM_BUFFER,i,buffer,i*stride,32);
 }
 GenVertexArrays(1,&vao);BindVertexArray(vao);
 const float vertices[]={-1,-1,3,-1,-1,3};GenBuffers(1,&vertexBuffer);BindBuffer(GL_ARRAY_BUFFER,vertexBuffer);
 BufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices,GL_STATIC_DRAW);EnableVertexAttribArray(0);VertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,NULL);
 glViewport(0,0,64,64);glDisable(GL_DITHER);glDisable(GL_DEPTH_TEST);glDisable(GL_BLEND);
 if(dynamic)Uniform1i(GetUniformLocation(program,"pick"),0);
 pass+=pixel(out,profile,dynamic,0,.206f,.256f,.306f);
 float update[4]={.20f,.20f,.20f,0};BufferSubData(GL_UNIFORM_BUFFER,11*stride,16,update);
 pass+=pixel(out,profile,dynamic,1,.394f,.444f,.494f);
 BindBufferRange(GL_UNIFORM_BUFFER,11,buffer,0,32);
 pass+=pixel(out,profile,dynamic,2,.195f,.245f,.295f);
 Uniform4f(GetUniformLocation(program,"vertexOrdinary"),.1f,.2f,.3f,0);
 pass+=pixel(out,profile,dynamic,3,.245f,.345f,.445f);
 if(dynamic){Uniform1i(GetUniformLocation(program,"pick"),1);pass+=pixel(out,profile,dynamic,4,.257f,.357f,.457f);}
end:
 UseProgram(0);BindVertexArray(0);BindBuffer(GL_UNIFORM_BUFFER,0);
 if(vao)DeleteVertexArrays(1,&vao);
 if(buffer)DeleteBuffers(1,&buffer);
 if(vertexBuffer)DeleteBuffers(1,&vertexBuffer);
 if(program)DeleteProgram(program);
 if(v)DeleteShader(v);
 if(f)DeleteShader(f);
 while(glGetError()!=0){}
 return pass;
}
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l){return DefWindowProcA(h,m,w,l);}
int main(int argc,char **argv){
 const char *output=argc>1?argv[1]:"C:\\vp-ubo-result.tsv";FILE*out=fopen(output,"w");
 WNDCLASSA wc={0};PIXELFORMATDESCRIPTOR pfd={sizeof(pfd),1,PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER,PFD_TYPE_RGBA,32};
 HWND h;HDC dc;HGLRC bootstrap,ctx;create_attribs_fn create;int i,total=0;
 if(!out)return 2;
 wc.lpfnWndProc=proc;wc.lpszClassName="VPUboProbe";wc.hInstance=GetModuleHandleA(NULL);wc.style=CS_OWNDC;RegisterClassA(&wc);
 h=CreateWindowA(wc.lpszClassName,"UBO hardware probe",WS_OVERLAPPEDWINDOW|WS_VISIBLE,30,30,320,240,NULL,NULL,wc.hInstance,NULL);
 dc=GetDC(h);if(!SetPixelFormat(dc,ChoosePixelFormat(dc,&pfd),&pfd)||(bootstrap=wglCreateContext(dc))==NULL||!wglMakeCurrent(dc,bootstrap))return 3;
 create=(void*)wglGetProcAddress("wglCreateContextAttribsARB");
 for(i=0;i<7;i++){
   int es=i>=4;
   int minor=i?(es?i-4:i-1):0;
   int attrs[]={0x2091,3,0x2092,minor,0x9126,es?4:2,0};char profile[32];
   if(!i)strcpy(profile,"legacy");else sprintf(profile,es?"es3.%d":"desktop3.%d",minor);
   ctx=i?(create?create(dc,NULL,attrs):NULL):wglCreateContext(dc);
   if(!ctx||!wglMakeCurrent(dc,ctx)){fprintf(out,"context\t%s\t0\twinerr=%lu\n",profile,GetLastError());if(ctx)wglDeleteContext(ctx);continue;}
   fprintf(out,"context\t%s\t1\t%s\t%s\n",profile,glGetString(GL_VERSION),glGetString(GL_RENDERER));
   if(load()){total+=exercise(out,profile,es,0);total+=exercise(out,profile,es,1);}
   SwapBuffers(dc);wglMakeCurrent(NULL,NULL);wglDeleteContext(ctx);wglMakeCurrent(dc,bootstrap);fflush(out);
 }
 fprintf(out,"totalPassed\t%d\n",total);fclose(out);wglMakeCurrent(NULL,NULL);wglDeleteContext(bootstrap);ReleaseDC(h,dc);DestroyWindow(h);
 return total>=9?0:4;
}
