/* Private GNU/virpipe -> existing OHOS NativeImage presenter integration test.
 * No pixel readback, X11, or per-frame wl_shm upload. The one-time blank
 * wl_buffer establishes window geometry; all rendered frames stay on GPU.
 * This tests the transport, not Wine, DXVK, or Steam authentication. */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct present_page { uint32_t magic, version, surface_id, reserved; };
static struct present_page *page;
static char page_path[4096];
static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
struct window {
    struct wl_surface *surface;
    struct xdg_surface *xdg;
    struct xdg_toplevel *toplevel;
    struct wl_buffer *placeholder;
    int width, height;
    bool configured, closed;
    EGLSurface gpu;
    unsigned frames;
};
static void cleanup_page(void) {
    if(page) { __atomic_store_n(&page->surface_id,0,__ATOMIC_RELEASE); munmap(page,sizeof(*page)); }
    if(page_path[0]) unlink(page_path);
}
static void ping(void *data,struct xdg_wm_base *base,uint32_t serial) { (void)data;xdg_wm_base_pong(base,serial); }
static const struct xdg_wm_base_listener wm_listener={.ping=ping};
static void global(void *data,struct wl_registry *registry,uint32_t name,const char *iface,uint32_t version) {
    (void)data;
    if(!strcmp(iface,"wl_compositor")) compositor=wl_registry_bind(registry,name,&wl_compositor_interface,version<4?version:4);
    else if(!strcmp(iface,"wl_shm")) shm=wl_registry_bind(registry,name,&wl_shm_interface,1);
    else if(!strcmp(iface,"xdg_wm_base")) {wm=wl_registry_bind(registry,name,&xdg_wm_base_interface,1);xdg_wm_base_add_listener(wm,&wm_listener,NULL);}
}
static void removed(void *data,struct wl_registry *r,uint32_t name) {(void)data;(void)r;(void)name;}
static const struct wl_registry_listener registry_listener={global,removed};
static void configured(void *data,struct xdg_surface *surface,uint32_t serial) {
    struct window *w=data;xdg_surface_ack_configure(surface,serial);w->configured=true;
}
static const struct xdg_surface_listener surface_listener={configured};
static void resized(void *data,struct xdg_toplevel *top,int32_t width,int32_t height,struct wl_array *states) {
    (void)data;(void)top;(void)width;(void)height;(void)states;
}
static void closed(void *data,struct xdg_toplevel *top) { (void)top;((struct window*)data)->closed=true; }
static const struct xdg_toplevel_listener top_listener={.configure=resized,.close=closed};
static bool placeholder(struct window *w,int width,int height) {
    char path[4096];const char *tmp=getenv("TMPDIR");
    if(!tmp||snprintf(path,sizeof(path),"%s/gnu-present-XXXXXX",tmp)>=(int)sizeof(path))return false;
    int fd=mkstemp(path);if(fd<0)return false;unlink(path);
    const int bytes=width*height*4;
    if(ftruncate(fd,bytes)){close(fd);return false;}
    struct wl_shm_pool *pool=wl_shm_create_pool(shm,fd,bytes);
    close(fd);if(!pool)return false;
    struct wl_buffer *buffer=wl_shm_pool_create_buffer(pool,0,width,height,width*4,WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);if(!buffer)return false;
    wl_surface_attach(w->surface,buffer,0,0);wl_surface_damage(w->surface,0,0,width,height);
    xdg_surface_set_window_geometry(w->xdg,0,0,width,height);wl_surface_commit(w->surface);
    if(w->placeholder)wl_buffer_destroy(w->placeholder);
    w->placeholder=buffer;w->width=width;w->height=height;
    return wl_display_roundtrip(display)>=0;
}
static bool create_window(struct window *w,const char *title,int width,int height) {
    w->surface=wl_compositor_create_surface(compositor);if(!w->surface)return false;
    w->xdg=xdg_wm_base_get_xdg_surface(wm,w->surface);if(!w->xdg)return false;
    xdg_surface_add_listener(w->xdg,&surface_listener,w);
    w->toplevel=xdg_surface_get_toplevel(w->xdg);if(!w->toplevel)return false;
    xdg_toplevel_add_listener(w->toplevel,&top_listener,w);xdg_toplevel_set_title(w->toplevel,title);
    xdg_toplevel_set_app_id(w->toplevel,"hmosdeck.gnu.shared-present");wl_surface_commit(w->surface);
    if(wl_display_roundtrip(display)<0||!w->configured)return false;
    return placeholder(w,width,height);
}
static bool receiver_ready(struct window *w) {
    char path[256];uint64_t key=((uint64_t)(uint32_t)getpid()<<32)|wl_proxy_get_id((struct wl_proxy*)w->surface);
    snprintf(path,sizeof(path),"/data/storage/el2/base/cache/winehua_zc_surface_%"PRIu64".ready",key);
    return access(path,F_OK)==0;
}
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    const char *root=getenv("STEAMOHOS_CODE_ROOT"),*tmp=getenv("TMPDIR");
    if(!root||!tmp||strcmp(getenv("WINEHUA_VTEST_PRESENT")?:"","1"))return 2;
    if(snprintf(page_path,sizeof(page_path),"%s/winehua_present_surface_%u.shm",tmp,(uint32_t)getpid())>=(int)sizeof(page_path))return 2;
    int fd=open(page_path,O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return 3;
    atexit(cleanup_page);
    if(ftruncate(fd,sizeof(*page))){close(fd);return 3;}
    page=mmap(NULL,sizeof(*page),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);close(fd);
    if(page==MAP_FAILED){page=NULL;return 3;}
    page->version=1;page->surface_id=0;page->reserved=0;
    __atomic_store_n(&page->magic,0x57535053u,__ATOMIC_RELEASE);
    char library[4096];snprintf(library,sizeof(library),"%s/graphics/libEGL-proton.so.1",root);
    void *egl=dlopen(library,RTLD_NOW|RTLD_LOCAL);if(!egl){fprintf(stderr,"EGL load: %s\n",dlerror());return 4;}
#define EGL_PROC(type,name) type p_##name=(type)dlsym(egl,#name);if(!p_##name){fprintf(stderr,"EGL missing %s\n",#name);return 4;}
    EGL_PROC(PFNEGLGETPROCADDRESSPROC,eglGetProcAddress);
    EGL_PROC(PFNEGLGETPLATFORMDISPLAYPROC,eglGetPlatformDisplay);
    EGL_PROC(PFNEGLINITIALIZEPROC,eglInitialize);EGL_PROC(PFNEGLBINDAPIPROC,eglBindAPI);
    EGL_PROC(PFNEGLCHOOSECONFIGPROC,eglChooseConfig);EGL_PROC(PFNEGLCREATEPBUFFERSURFACEPROC,eglCreatePbufferSurface);
    EGL_PROC(PFNEGLCREATECONTEXTPROC,eglCreateContext);EGL_PROC(PFNEGLMAKECURRENTPROC,eglMakeCurrent);
    EGL_PROC(PFNEGLSWAPBUFFERSPROC,eglSwapBuffers);EGL_PROC(PFNEGLDESTROYSURFACEPROC,eglDestroySurface);
    EGL_PROC(PFNEGLDESTROYCONTEXTPROC,eglDestroyContext);EGL_PROC(PFNEGLTERMINATEPROC,eglTerminate);
    EGL_PROC(PFNEGLGETERRORPROC,eglGetError);
#undef EGL_PROC
#define GL_PROC(type,name) type p_##name=(type)p_eglGetProcAddress(#name);if(!p_##name)return 5;
    typedef const GLubyte*(*GetString)(GLenum);typedef void(*ClearColor)(GLfloat,GLfloat,GLfloat,GLfloat);
    typedef void(*Mask)(GLbitfield);typedef void(*Void)(void);typedef GLenum(*Error)(void);
    typedef void(*Rectangle)(GLint,GLint,GLsizei,GLsizei);
    GL_PROC(GetString,glGetString);GL_PROC(ClearColor,glClearColor);GL_PROC(Mask,glClear);
    GL_PROC(Void,glFlush);GL_PROC(Error,glGetError);GL_PROC(Rectangle,glViewport);
#undef GL_PROC
    EGLDisplay gpu=p_eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,NULL);
    EGLint major,minor;
    if(!p_eglInitialize(gpu,&major,&minor)||!p_eglBindAPI(EGL_OPENGL_API)){printf("EGL init error=0x%x\n",p_eglGetError());return 6;}
    EGLint attributes[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
    EGLConfig config;EGLint count;
    if(!p_eglChooseConfig(gpu,attributes,&config,1,&count)||count!=1)return 7;
    EGLContext context=p_eglCreateContext(gpu,config,EGL_NO_CONTEXT,NULL);if(context==EGL_NO_CONTEXT)return 8;
    display=wl_display_connect(NULL);if(!display)return 9;
    struct wl_registry *registry=wl_display_get_registry(display);wl_registry_add_listener(registry,&registry_listener,NULL);
    if(wl_display_roundtrip(display)<0||!compositor||!shm||!wm)return 9;
    char single_path[4096];snprintf(single_path,sizeof(single_path),"%s/present-single",root);
    unsigned window_count=access(single_path,F_OK)==0?1:2;
    printf("GNU_SHARED_BEGIN pid=%u windows=%u\n",(unsigned)getpid(),window_count);
    struct window windows[2]={0};
    for(unsigned i=0;i<window_count;i++) {
        struct window *w=&windows[i];if(!create_window(w,i?"GNU NativeImage B":"GNU NativeImage A",i?300:640,i?180:400))return 10;
        EGLint size[]={EGL_WIDTH,w->width,EGL_HEIGHT,w->height,EGL_NONE};
        w->gpu=p_eglCreatePbufferSurface(gpu,config,size);if(w->gpu==EGL_NO_SURFACE)return 11;
    }
    if(!p_eglMakeCurrent(gpu,windows[0].gpu,windows[0].gpu,context))return 12;
    const char *renderer=(const char*)p_glGetString(GL_RENDERER);
    printf("GNU_SHARED_RENDERER name=%s version=%s\n",renderer?renderer:"null",p_glGetString(GL_VERSION));
    if(!renderer||!strstr(renderer,"virgl"))return 13;
    for(unsigned frame=0;frame<240;frame++) {
        if(wl_display_roundtrip(display)<0)return 14;
        if(frame==120) {
            if(!p_eglMakeCurrent(gpu,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT))return 15;
            p_eglDestroySurface(gpu,windows[0].gpu);
            if(!placeholder(&windows[0],480,300))return 15;
            EGLint size[]={EGL_WIDTH,480,EGL_HEIGHT,300,EGL_NONE};
            windows[0].gpu=p_eglCreatePbufferSurface(gpu,config,size);if(windows[0].gpu==EGL_NO_SURFACE)return 15;
            puts("GNU_SHARED_RESIZE width=480 height=300");
        }
        for(unsigned i=0;i<window_count;i++) {
            struct window *w=&windows[i];if(w->closed)return 16;
            if(!p_eglMakeCurrent(gpu,w->gpu,w->gpu,context))return 16;
            p_glViewport(0,0,w->width,w->height);
            p_glClearColor(i?0.9f:0.1f,i?0.3f:0.65f,0.2f+(frame%60)/100.f,1);p_glClear(GL_COLOR_BUFFER_BIT);
            if(p_glGetError()!=GL_NO_ERROR)return 17;
            __atomic_store_n(&page->surface_id,wl_proxy_get_id((struct wl_proxy*)w->surface),__ATOMIC_RELEASE);
            p_glFlush();EGLBoolean swapped=p_eglSwapBuffers(gpu,w->gpu);
            __atomic_store_n(&page->surface_id,0,__ATOMIC_RELEASE);
            if(!swapped||p_glGetError()!=GL_NO_ERROR)return 18;
            ++w->frames;
        }
        if(frame==60||frame==180)printf("GNU_SHARED_PROGRESS frame=%u ready_a=%d ready_b=%d\n",frame,receiver_ready(&windows[0]),window_count==2&&receiver_ready(&windows[1]));
        struct timespec delay={0,16000000};nanosleep(&delay,NULL);
    }
    const bool ready=receiver_ready(&windows[0])&&(window_count==1||receiver_ready(&windows[1]));
    printf("GNU_SHARED_RESULT frames_a=%u frames_b=%u receiver_ready=%d pixel_readbacks=0 per_frame_shm_uploads=0 geometry_buffers=3\n",windows[0].frames,windows[1].frames,ready);
    p_eglMakeCurrent(gpu,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    for(unsigned i=0;i<window_count;i++) {
        struct window *w=&windows[i];p_eglDestroySurface(gpu,w->gpu);wl_buffer_destroy(w->placeholder);
        xdg_toplevel_destroy(w->toplevel);xdg_surface_destroy(w->xdg);wl_surface_destroy(w->surface);
    }
    wl_display_roundtrip(display);wl_registry_destroy(registry);xdg_wm_base_destroy(wm);wl_shm_destroy(shm);wl_compositor_destroy(compositor);wl_display_disconnect(display);
    p_eglDestroyContext(gpu,context);p_eglTerminate(gpu);dlclose(egl);
    if(!ready)return 19;
    puts("GNU_SHARED_PRESENT_OK");return 0;
}
