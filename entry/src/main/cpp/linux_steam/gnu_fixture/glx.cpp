// Verify actual Mesa rendering (softpipe or shared virpipe) and X11 presentation.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glx.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <link.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <cstdlib>
#include <cstdarg>
#include <fcntl.h>
namespace {
FILE* capabilityLog=nullptr;
void Report(const char* format,...) {
    va_list args;va_start(args,format);va_list copy;va_copy(copy,args);
    vfprintf(stderr,format,args);va_end(args);
    if(capabilityLog){vfprintf(capabilityLog,format,copy);fflush(capabilityLog);}
    va_end(copy);
}
struct Segment { uintptr_t begin, end, base; char name[256]; };
Segment segments[256];
unsigned segmentCount;
int Collect(struct dl_phdr_info* info, size_t, void*) {
    for (unsigned i=0;i<info->dlpi_phnum && segmentCount<256;++i) {
        const auto& ph=info->dlpi_phdr[i];
        if(ph.p_type!=PT_LOAD)continue;
        auto& segment=segments[segmentCount++];
        segment.begin=info->dlpi_addr+ph.p_vaddr;segment.end=segment.begin+ph.p_memsz;segment.base=info->dlpi_addr;
        const char* name=info->dlpi_name && *info->dlpi_name ? info->dlpi_name : "steamohos_gnu_glx";
        size_t n=strnlen(name,sizeof(segment.name)-1);memcpy(segment.name,name,n);segment.name[n]=0;
    }
    return 0;
}
void Text(const char* text) { size_t n=0;while(text[n])++n;(void)write(STDERR_FILENO,text,n); }
void Hex(uintptr_t value) {
    char out[2+sizeof(value)*2];out[0]='0';out[1]='x';
    for(unsigned i=0;i<sizeof(value)*2;++i)out[2+i]="0123456789abcdef"[(value>>((sizeof(value)*2-1-i)*4))&15];
    (void)write(STDERR_FILENO,out,sizeof(out));
}
void Location(const char* label,uintptr_t address) {
    Text(label);Hex(address);
    for(unsigned i=0;i<segmentCount;++i)if(address>=segments[i].begin && address<segments[i].end) {
        Text(" image=");Text(segments[i].name);Text(" offset=");Hex(address-segments[i].base);break;
    }
    Text("\n");
}
void Fault(int, siginfo_t*, void* context) {
#if defined(__aarch64__)
    const auto* state=static_cast<const ucontext_t*>(context);
    Location("STEAMOHOS_GLX_FAULT pc=",state->uc_mcontext.pc);
    Location("STEAMOHOS_GLX_FAULT lr=",state->uc_mcontext.regs[30]);
#else
    (void)context;
    Location("STEAMOHOS_GLX_FAULT architecture=",0);
#endif
    // SA_RESETHAND preserves the actual fatal signal when the instruction retries.
}
int ContextError(Display*,XErrorEvent*) { return 0; }

void AngleCapabilities(Display* xdisplay) {
    const char* root=getenv("STEAMOHOS_CODE_ROOT");char path[4096];
    if(!root||snprintf(path,sizeof(path),"%s/steamrtarm64/libEGL.so",root)>=(int)sizeof(path))return;
    void* library=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if(!library){Report("STEAMOHOS_ANGLE load=failed\n");return;}
#define EGL_FN(type,name) auto name=reinterpret_cast<type>(dlsym(library,#name))
    EGL_FN(PFNEGLGETDISPLAYPROC,eglGetDisplay); EGL_FN(PFNEGLINITIALIZEPROC,eglInitialize);
    EGL_FN(PFNEGLGETERRORPROC,eglGetError); EGL_FN(PFNEGLGETPROCADDRESSPROC,eglGetProcAddress);
    EGL_FN(PFNEGLCHOOSECONFIGPROC,eglChooseConfig); EGL_FN(PFNEGLCREATEPBUFFERSURFACEPROC,eglCreatePbufferSurface);
    EGL_FN(PFNEGLCREATECONTEXTPROC,eglCreateContext); EGL_FN(PFNEGLMAKECURRENTPROC,eglMakeCurrent);
    EGL_FN(PFNEGLDESTROYSURFACEPROC,eglDestroySurface); EGL_FN(PFNEGLDESTROYCONTEXTPROC,eglDestroyContext);
    EGL_FN(PFNEGLTERMINATEPROC,eglTerminate);
#undef EGL_FN
    if(!eglGetDisplay||!eglInitialize||!eglGetError||!eglGetProcAddress||!eglChooseConfig||!eglCreatePbufferSurface||
        !eglCreateContext||!eglMakeCurrent||!eglDestroySurface||!eglDestroyContext||!eglTerminate)return;
    EGLDisplay display=eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(xdisplay));
    EGLint major=0,minor=0;
    if(!eglInitialize(display,&major,&minor)){
        Report("STEAMOHOS_ANGLE initialize=failed error=0x%x\n",eglGetError());return;
    }
    const EGLint attributes[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    EGLConfig config{};EGLint count=0;
    if(!eglChooseConfig(display,attributes,&config,1,&count)||count<1){
        Report("STEAMOHOS_ANGLE config=failed error=0x%x\n",eglGetError());eglTerminate(display);return;
    }
    const EGLint size[]={EGL_WIDTH,32,EGL_HEIGHT,32,EGL_NONE};
    const EGLint version[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    EGLSurface surface=eglCreatePbufferSurface(display,config,size);
    EGLContext context=eglCreateContext(display,config,EGL_NO_CONTEXT,version);
    if(surface!=EGL_NO_SURFACE&&context!=EGL_NO_CONTEXT&&eglMakeCurrent(display,surface,surface,context)){
        auto getString=reinterpret_cast<const GLubyte*(*)(GLenum)>(eglGetProcAddress("glGetString"));
        auto clearColor=reinterpret_cast<void(*)(GLfloat,GLfloat,GLfloat,GLfloat)>(eglGetProcAddress("glClearColor"));
        auto clear=reinterpret_cast<void(*)(GLbitfield)>(eglGetProcAddress("glClear"));
        auto pixels=reinterpret_cast<void(*)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*)>(eglGetProcAddress("glReadPixels"));
        auto finish=reinterpret_cast<void(*)()>(eglGetProcAddress("glFinish"));
        if(getString&&clearColor&&clear&&pixels&&finish){
            clearColor(.25f,.5f,.75f,1);clear(GL_COLOR_BUFFER_BIT);finish();unsigned char rgba[4]{};
            pixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
            bool valid=abs(int(rgba[0])-64)<=2&&abs(int(rgba[1])-128)<=2&&abs(int(rgba[2])-191)<=2;
            Report("STEAMOHOS_ANGLE initialized=%d.%d renderer=%s version=%s pixelVerified=%d\n",
                major,minor,getString(GL_RENDERER),getString(GL_VERSION),valid);
        }
        eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    } else Report("STEAMOHOS_ANGLE makeCurrent=failed error=0x%x\n",eglGetError());
    if(context!=EGL_NO_CONTEXT)eglDestroyContext(display,context);
    if(surface!=EGL_NO_SURFACE)eglDestroySurface(display,surface);
    eglTerminate(display);
    // ANGLE registers Xlib hooks; keep its library loaded until process exit.
}

bool CompressedTextures() {
    const char* vertex="#version 120\nvoid main(){gl_Position=gl_Vertex;}";
    const char* fragment="#version 120\nuniform sampler2D tex;void main(){gl_FragColor=texture2D(tex,vec2(0.5));}";
    GLuint vs=glCreateShader(GL_VERTEX_SHADER),fs=glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(vs,1,&vertex,nullptr);glShaderSource(fs,1,&fragment,nullptr);
    glCompileShader(vs);glCompileShader(fs);
    GLuint program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);
    GLint linked=0;glGetProgramiv(program,GL_LINK_STATUS,&linked);
    bool verified=linked==GL_TRUE;
    glUseProgram(program);glUniform1i(glGetUniformLocation(program,"tex"),0);
    const GLenum formats[]={GL_COMPRESSED_RED_RGTC1,GL_COMPRESSED_SIGNED_RED_RGTC1,
        GL_COMPRESSED_RG_RGTC2,GL_COMPRESSED_SIGNED_RG_RGTC2};
    for(unsigned i=0;i<4;++i) {
        GLuint texture=0;glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        unsigned char blocks[16]={64,64,0,0,0,0,0,0,32,32,0,0,0,0,0,0};
        while(glGetError()!=GL_NO_ERROR){}
        glCompressedTexImage2D(GL_TEXTURE_2D,0,formats[i],4,4,0,i<2?8:16,blocks);
        glViewport(0,0,32,32);
        glBegin(GL_TRIANGLES);glVertex2f(-1,-1);glVertex2f(3,-1);glVertex2f(-1,3);glEnd();
        glFinish();unsigned char pixel[4]{};glReadPixels(16,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        GLenum error=glGetError();int red=i%2?129:64,green=i<2?0:(i%2?64:32);
        bool valid=linked&&error==GL_NO_ERROR&&abs(int(pixel[0])-red)<=2&&abs(int(pixel[1])-green)<=2&&pixel[2]<=2;
        Report("STEAMOHOS_GLX_RGTC format=%u pixel=%u,%u,%u error=%u verified=%d\n",formats[i],pixel[0],pixel[1],pixel[2],error,valid);
        verified&=valid;glDeleteTextures(1,&texture);
    }
    glUseProgram(0);glDeleteProgram(program);glDeleteShader(vs);glDeleteShader(fs);
    return verified;
}
void FeatureCapabilities() {
    Dl_info image{};
    if(dladdr(reinterpret_cast<void*>(glClear),&image))
        Report("STEAMOHOS_GLX_LIBRARY image=%s\n",image.dli_fname);
    const char* extensions=reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    const char* names[]={"GL_ARB_color_buffer_float","GL_ARB_depth_buffer_float","GL_ARB_half_float_vertex","GL_ARB_map_buffer_range","GL_ARB_shader_texture_lod","GL_ARB_texture_float","GL_ARB_texture_rg","GL_ARB_texture_compression_rgtc","GL_EXT_draw_buffers2","GL_ARB_framebuffer_object","GL_EXT_framebuffer_sRGB","GL_EXT_packed_float","GL_EXT_texture_array","GL_EXT_texture_integer","GL_EXT_texture_shared_exponent","GL_EXT_transform_feedback","GL_NV_conditional_render","GL_ARB_draw_instanced","GL_ARB_texture_buffer_object","GL_ARB_uniform_buffer_object","GL_EXT_texture_snorm","GL_NV_primitive_restart","GL_NV_texture_rectangle","GL_ARB_depth_clamp","GL_ARB_draw_elements_base_vertex","GL_ARB_fragment_coord_conventions","GL_EXT_provoking_vertex","GL_ARB_seamless_cube_map","GL_ARB_sync","GL_ARB_texture_multisample","GL_EXT_vertex_array_bgra","GL_ARB_blend_func_extended","GL_ARB_explicit_attrib_location","GL_ARB_instanced_arrays","GL_ARB_shader_bit_encoding","GL_ARB_texture_rgb10_a2ui","GL_ARB_timer_query","GL_ARB_vertex_type_2_10_10_10_rev","GL_EXT_texture_swizzle"};
    for(const char* name:names)
        if(!extensions||!strstr(extensions,name))Report("STEAMOHOS_GLX_MISSING extension=%s\n",name);
    Report("STEAMOHOS_GLX_GLSL version=%s\n",glGetString(GL_SHADING_LANGUAGE_VERSION));
}

void ModernCapabilities(Display* display) {
    const int screen=DefaultScreen(display);
    const char* extensions=glXQueryExtensionsString(display,screen);
    const bool arb=extensions&&strstr(extensions,"GLX_ARB_create_context");
    int count=0;
    const int attributes[]={GLX_RENDER_TYPE,GLX_RGBA_BIT,GLX_X_RENDERABLE,True,GLX_DOUBLEBUFFER,True,None};
    GLXFBConfig* chosen=glXChooseFBConfig(display,screen,attributes,&count);
    int allCount=0;GLXFBConfig* all=glXGetFBConfigs(display,screen,&allCount);
    int visuals32=0,chosen32=0;
    for(int i=0;i<allCount;++i) {
        XVisualInfo* info=glXGetVisualFromFBConfig(display,all[i]);
        if(info){
            if(info->depth==32)++visuals32;
            int a=0,z=0,stencil=0,db=0;
            glXGetFBConfigAttrib(display,all[i],GLX_ALPHA_SIZE,&a);
            glXGetFBConfigAttrib(display,all[i],GLX_DEPTH_SIZE,&z);
            glXGetFBConfigAttrib(display,all[i],GLX_STENCIL_SIZE,&stencil);
            glXGetFBConfigAttrib(display,all[i],GLX_DOUBLEBUFFER,&db);
            Report("STEAMOHOS_GLX_CONFIG visualDepth=%d alpha=%d depth=%d stencil=%d double=%d\n",info->depth,a,z,stencil,db);
            XFree(info);
        }
    }
    for(int i=0;i<count&&i<8;++i) {
        XVisualInfo* info=glXGetVisualFromFBConfig(display,chosen[i]);
        if(info){if(info->depth==32)++chosen32;XFree(info);}
    }
    Report("STEAMOHOS_GLX_CAPS arbContext=%d allConfigs=%d visuals32=%d chosenDouble=%d chosen32=%d\n",
        arb,allCount,visuals32,count,chosen32);
    auto create=reinterpret_cast<PFNGLXCREATECONTEXTATTRIBSARBPROC>(
        glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXCreateContextAttribsARB")));
    struct Request { int major,minor,profile; const char* name; };
    const Request requests[]={
        {3,3,GLX_CONTEXT_CORE_PROFILE_BIT_ARB,"core33"},
        {3,1,0,"gl31"}, {3,0,0,"gl30"}, {2,1,0,"gl21"},
        {3,1,GLX_CONTEXT_ES_PROFILE_BIT_EXT,"es31"},
        {3,0,GLX_CONTEXT_ES_PROFILE_BIT_EXT,"es30"},
        {2,0,GLX_CONTEXT_ES_PROFILE_BIT_EXT,"es20"}};
    if(arb&&create&&count>0) for(const auto& request:requests) {
        XSync(display,False);auto oldHandler=XSetErrorHandler(ContextError);
        const int attributes[]={GLX_CONTEXT_MAJOR_VERSION_ARB,request.major,
            GLX_CONTEXT_MINOR_VERSION_ARB,request.minor,
            request.profile?GLX_CONTEXT_PROFILE_MASK_ARB:0,request.profile,None};
        GLXContext context=create(display,chosen[0],nullptr,True,attributes);
        XSync(display,False);XSetErrorHandler(oldHandler);
        if(context) {
            XVisualInfo* info=glXGetVisualFromFBConfig(display,chosen[0]);
            if(info) {
                XSetWindowAttributes values{};
                values.colormap=XCreateColormap(display,RootWindow(display,screen),info->visual,AllocNone);
                Window window=XCreateWindow(display,RootWindow(display,screen),0,0,32,32,0,info->depth,
                    InputOutput,info->visual,CWColormap,&values);
                if(glXMakeCurrent(display,window,context)) {
                    glClearColor(0.25f,0.5f,0.75f,1);glClear(GL_COLOR_BUFFER_BIT);glFinish();
                    unsigned char pixel[4]{};glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                    const bool verified=glGetError()==GL_NO_ERROR&&pixel[0]>=62&&pixel[0]<=66&&
                        pixel[1]>=126&&pixel[1]<=130&&pixel[2]>=189&&pixel[2]<=193;
                    Report("STEAMOHOS_GLX_CONTEXT request=%s version=%s renderer=%s pixelVerified=%d\n",
                        request.name,glGetString(GL_VERSION),glGetString(GL_RENDERER),verified);
                    glXMakeCurrent(display,None,nullptr);
                } else Report("STEAMOHOS_GLX_CONTEXT request=%s makeCurrent=failed\n",request.name);
                XDestroyWindow(display,window);XFreeColormap(display,values.colormap);XFree(info);
            }
            glXDestroyContext(display,context);
        } else Report("STEAMOHOS_GLX_CONTEXT request=%s create=failed\n",request.name);
    }
    if(all)XFree(all);
    if(chosen)XFree(chosen);
}
}
int main() {
    const char* temporary=getenv("STEAMOHOS_TMP_ROOT");char report[4096];
    if(temporary&&temporary[0]=='/'&&snprintf(report,sizeof(report),"%s/glx-capabilities.txt",temporary)<(int)sizeof(report)) {
        int fd=open(report,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd>=0)capabilityLog=fdopen(fd,"w");
    }
    dl_iterate_phdr(Collect,nullptr);
    struct sigaction action{};action.sa_sigaction=Fault;action.sa_flags=SA_SIGINFO|SA_RESETHAND;
    sigemptyset(&action.sa_mask);sigaction(SIGSEGV,&action,nullptr);
    Report("STEAMOHOS_GLX_STAGE open_display\n");
    Display* display=XOpenDisplay(nullptr);if(!display)return 10;
    int attributes[]={GLX_RGBA,GLX_DOUBLEBUFFER,GLX_RED_SIZE,8,GLX_GREEN_SIZE,8,GLX_BLUE_SIZE,8,None};
    XVisualInfo* visual=glXChooseVisual(display,DefaultScreen(display),attributes);if(!visual)return 11;
    Report("STEAMOHOS_GLX_STAGE create_context depth=%d\n",visual->depth);
    GLXContext context=glXCreateContext(display,visual,nullptr,True);if(!context)return 12;
    XSetWindowAttributes values{};values.border_pixel=0;values.colormap=XCreateColormap(display,RootWindow(display,visual->screen),visual->visual,AllocNone);
    Window window=XCreateWindow(display,RootWindow(display,visual->screen),0,0,320,180,0,visual->depth,InputOutput,visual->visual,CWColormap|CWBorderPixel,&values);
    XStoreName(display,window,"Steam ARM64 native Mesa OpenGL verification");XMapWindow(display,window);XSync(display,False);
    Report("STEAMOHOS_GLX_STAGE make_current\n");
    if(!glXMakeCurrent(display,window,context))return 13;
    const char* renderer=(const char*)glGetString(GL_RENDERER);const char* version=(const char*)glGetString(GL_VERSION);
    if(!renderer||!version)return 14;
    Report("STEAMOHOS_GLX_STAGE render renderer=%s version=%s\n",renderer,version);
    FeatureCapabilities();
    if(!CompressedTextures())return 18;
    int frames=0;
    for(int frame=0;frame<18;++frame){
        const int channel=frame%3;
        glViewport(0,0,320,180);glClearColor(channel==0,channel==1,channel==2,1);glClear(GL_COLOR_BUFFER_BIT);glFinish();
        unsigned char pixel[4]{};glReadPixels(160,90,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        if(glGetError()!=GL_NO_ERROR)return 15;
        for(int c=0;c<3;++c)if(c==channel ? pixel[c]<250 : pixel[c]>5)return 15;
        glXSwapBuffers(display,window);XSync(display,False);
        XImage* image=XGetImage(display,window,160,90,1,1,AllPlanes,ZPixmap);if(!image)return 16;
        unsigned long actual=XGetPixel(image,0,0);
        const unsigned long masks[]={visual->red_mask,visual->green_mask,visual->blue_mask};
        bool matches=true;for(int c=0;c<3;++c)if(((actual&masks[c])!=0)!=(c==channel))matches=false;
        XDestroyImage(image);if(!matches)return 17;
        ++frames;usleep(16000);
    }
    printf("STEAMOHOS_GNU_GLX_V1 status=native_glx_verified framesReadBack=%d renderer=%s version=%s\n",frames,renderer,version);
    glXMakeCurrent(display,None,nullptr);glXDestroyContext(display,context);XDestroyWindow(display,window);XFreeColormap(display,values.colormap);XFree(visual);
    ModernCapabilities(display);AngleCapabilities(display);XCloseDisplay(display);if(capabilityLog)fclose(capabilityLog);return 0;
}
