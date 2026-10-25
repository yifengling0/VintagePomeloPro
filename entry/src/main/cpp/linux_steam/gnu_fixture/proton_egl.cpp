// Experimental GNU Wayland EGL window backend over the existing OHOS presenter.
// Mesa owns rendering; native objects remain in the OHOS receiver. Window frames
// are submitted by resource id. No readback or per-frame wl_shm upload is used.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <wayland-client.h>
#include <wayland-egl-backend.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {
std::recursive_mutex mutex;
void* library;
thread_local EGLint error=EGL_SUCCESS;
struct Page { uint32_t magic,version,surface,reserved; };
Page* page;
std::string pagePath;
struct Display {
    wl_display* native=nullptr;
    wl_shm* shm=nullptr;
    wl_registry* registry=nullptr;
    wl_event_queue* queue=nullptr;
};
Display display;
struct Surface {
    EGLDisplay display;
    EGLConfig config;
    EGLSurface real;
    wl_egl_window* window;
    wl_buffer* blank=nullptr;
    int width=0,height=0;
    unsigned frames=0;
};
std::map<EGLSurface,std::shared_ptr<Surface>> surfaces;
thread_local std::shared_ptr<Surface> draw,readSurface;
thread_local EGLSurface drawHandle=EGL_NO_SURFACE,readHandle=EGL_NO_SURFACE;
template<class T> T real(const char* name) {
    if(!library) {
        const char* root=getenv("STEAMOHOS_CODE_ROOT");
        if(!root||strcmp(root,"/data/storage/el2/base/files/steamohos-native/proton-probe"))return nullptr;
        std::string path=std::string(root)+"/graphics/libEGL-proton.so.1";
        library=dlopen(path.c_str(),RTLD_NOW|RTLD_LOCAL);
    }
    return library?reinterpret_cast<T>(dlsym(library,name)):nullptr;
}
template<class T> T gl(const char* name) {
    auto get=real<PFNEGLGETPROCADDRESSPROC>("eglGetProcAddress");
    return get?reinterpret_cast<T>(get(name)):nullptr;
}
void cleanupPage() { if(page)munmap(page,sizeof(*page));if(!pagePath.empty())unlink(pagePath.c_str()); }
bool initPage() {
    if(page)return true;
    const char* tmp=getenv("TMPDIR");if(!tmp)return false;
    pagePath=std::string(tmp)+"/winehua_present_surface_"+std::to_string(getpid())+".shm";
    int fd=open(pagePath.c_str(),O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return false;
    if(ftruncate(fd,sizeof(Page))){close(fd);unlink(pagePath.c_str());return false;}
    auto p=mmap(nullptr,sizeof(Page),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);close(fd);
    if(p==MAP_FAILED){unlink(pagePath.c_str());return false;}
    page=static_cast<Page*>(p);page->version=1;page->surface=0;page->reserved=0;
    __atomic_store_n(&page->magic,0x57535053u,__ATOMIC_RELEASE);atexit(cleanupPage);return true;
}
void global(void*,wl_registry* registry,uint32_t id,const char* name,uint32_t) {
    if(!strcmp(name,"wl_shm"))display.shm=static_cast<wl_shm*>(wl_registry_bind(registry,id,&wl_shm_interface,1));
}
void removed(void*,wl_registry*,uint32_t) {}
const wl_registry_listener registryListener={global,removed};
bool initDisplay(wl_display* native) {
    if(display.native)return display.native==native;
    if(!native||!initPage())return false;
    display.queue=wl_display_create_queue(native);if(!display.queue)return false;
    auto wrapper=static_cast<wl_display*>(wl_proxy_create_wrapper(native));
    if(!wrapper)return false;
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper),display.queue);
    display.registry=wl_display_get_registry(wrapper);wl_proxy_wrapper_destroy(wrapper);
    if(!display.registry)return false;
    wl_registry_add_listener(display.registry,&registryListener,nullptr);
    if(wl_display_roundtrip_queue(native,display.queue)<0||!display.shm)return false;
    display.native=native;return true;
}
std::shared_ptr<Surface> find(EGLSurface surface) {
    const auto it=surfaces.find(surface);return it==surfaces.end()?nullptr:it->second;
}
EGLSurface underlying(EGLSurface surface) { auto value=find(surface);return value?value->real:surface; }
bool geometry(Surface& s) {
    char path[4096];const char* tmp=getenv("TMPDIR");
    if(!tmp||snprintf(path,sizeof(path),"%s/egl-geometry-XXXXXX",tmp)>=(int)sizeof(path))return false;
    int fd=mkstemp(path);if(fd<0)return false;unlink(path);
    const int64_t bytes=int64_t(s.width)*s.height*4;
    if(bytes<=0||bytes>128*1024*1024||ftruncate(fd,bytes)){close(fd);return false;}
    auto pool=wl_shm_create_pool(display.shm,fd,static_cast<int>(bytes));close(fd);if(!pool)return false;
    auto buffer=wl_shm_pool_create_buffer(pool,0,s.width,s.height,s.width*4,WL_SHM_FORMAT_XRGB8888);wl_shm_pool_destroy(pool);
    if(!buffer)return false;
    wl_surface_attach(s.window->surface,buffer,0,0);wl_surface_damage(s.window->surface,0,0,s.width,s.height);
    wl_surface_commit(s.window->surface);wl_display_flush(display.native);
    if(s.blank)wl_buffer_destroy(s.blank);
    s.blank=buffer;
    s.window->attached_width=s.width;s.window->attached_height=s.height;return true;
}
EGLSurface pbuffer(EGLDisplay d,EGLConfig config,int width,int height) {
    if(width<=0||height<=0||width>16384||height>16384)return EGL_NO_SURFACE;
    auto create=real<PFNEGLCREATEPBUFFERSURFACEPROC>("eglCreatePbufferSurface");
    EGLint size[]={EGL_WIDTH,width,EGL_HEIGHT,height,EGL_NONE};return create?create(d,config,size):EGL_NO_SURFACE;
}
// Serialize the existing per-process present-page ABI only while flushing.
// All GL work outside this small interval remains normally concurrent.
void flushCurrent(bool finish) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    using Flush=void(*)();auto flush=gl<Flush>(finish?"glFinish":"glFlush");if(!flush)return;
    if(draw&&draw->window&&page)__atomic_store_n(&page->surface,wl_proxy_get_id(reinterpret_cast<wl_proxy*>(draw->window->surface)),__ATOMIC_RELEASE);
    flush();if(page)__atomic_store_n(&page->surface,0,__ATOMIC_RELEASE);
}
void flush(){flushCurrent(false);}void finish(){flushCurrent(true);}
// The window's render target is a single-buffer Mesa pbuffer. Wine uses
// GL_BACK/GL_BACK_LEFT for WGL window buffers. Translate only that default
// framebuffer; named FBO attachments and invalid inputs retain GL semantics.
bool defaultFramebuffer(GLenum binding, const std::shared_ptr<Surface>& surface) {
    if(!surface||!surface->window)return false;
    GLint framebuffer=-1;auto get=gl<void(*)(GLenum,GLint*)>("glGetIntegerv");
    if(!get)return false;
    get(binding,&framebuffer);return framebuffer==0;
}
GLenum windowBuffer(GLenum value) {
    return value==GL_BACK||value==GL_BACK_LEFT?GL_FRONT_LEFT:value;
}
void drawBuffers(GLsizei count,const GLenum* buffers) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto call=gl<void(*)(GLsizei,const GLenum*)>("glDrawBuffers");if(!call)return;
    // Wine restores MAX_DRAW_BUFFERS entries, padding with GL_NONE. Keep all
    // entries and count intact; Mesa still validates limits/duplicate targets.
    if(count>0&&count<=32&&buffers&&defaultFramebuffer(GL_DRAW_FRAMEBUFFER_BINDING,draw)) {
        GLenum mapped[32];for(GLsizei i=0;i<count;i++)mapped[i]=windowBuffer(buffers[i]);
        call(count,mapped);
    } else call(count,buffers);
}
void drawBuffer(GLenum buffer) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto call=gl<void(*)(GLenum)>("glDrawBuffer");if(!call)return;
    if(defaultFramebuffer(GL_DRAW_FRAMEBUFFER_BINDING,draw))buffer=windowBuffer(buffer);
    call(buffer);
}
void readBuffer(GLenum buffer) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto call=gl<void(*)(GLenum)>("glReadBuffer");if(!call)return;
    if(defaultFramebuffer(GL_READ_FRAMEBUFFER_BINDING,readSurface))buffer=windowBuffer(buffer);
    call(buffer);
}
}

extern "C" EGLAPI EGLint EGLAPIENTRY eglGetError(void) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if(error!=EGL_SUCCESS){auto result=error;error=EGL_SUCCESS;return result;}
    auto f=real<PFNEGLGETERRORPROC>("eglGetError");return f?f():EGL_NOT_INITIALIZED;
}
extern "C" EGLAPI const char* EGLAPIENTRY eglQueryString(EGLDisplay d,EGLint name) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto f=real<PFNEGLQUERYSTRINGPROC>("eglQueryString");return f?f(d,name):nullptr;
}
extern "C" EGLAPI EGLDisplay EGLAPIENTRY eglGetPlatformDisplay(EGLenum platform,void* native,const EGLAttrib* attributes) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto f=real<PFNEGLGETPLATFORMDISPLAYPROC>("eglGetPlatformDisplay");if(!f)return EGL_NO_DISPLAY;
    if(platform==EGL_PLATFORM_WAYLAND_KHR) {
        if((attributes&&attributes[0]!=EGL_NONE)||!initDisplay(static_cast<wl_display*>(native))){error=EGL_BAD_PARAMETER;return EGL_NO_DISPLAY;}
        return f(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,nullptr);
    }
    return f(platform,native,attributes);
}
extern "C" EGLAPI EGLBoolean EGLAPIENTRY eglGetConfigAttrib(EGLDisplay d,EGLConfig c,EGLint attr,EGLint* value) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto f=real<PFNEGLGETCONFIGATTRIBPROC>("eglGetConfigAttrib");if(!f)return EGL_FALSE;
    EGLBoolean result=f(d,c,attr,value);
    if(result&&value&&display.native&&attr==EGL_SURFACE_TYPE&&(*value&EGL_PBUFFER_BIT))*value|=EGL_WINDOW_BIT;
    return result;
}
extern "C" EGLAPI EGLSurface EGLAPIENTRY eglCreateWindowSurface(EGLDisplay d,EGLConfig config,EGLNativeWindowType window,const EGLint* attributes) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto native=reinterpret_cast<wl_egl_window*>(window);
    if(!display.native||!native||native->version!=WL_EGL_WINDOW_VERSION||!native->surface||surfaces.size()>=128){error=EGL_BAD_NATIVE_WINDOW;return EGL_NO_SURFACE;}
    if(attributes)for(unsigned i=0;i<64;i+=2) {
        if(attributes[i]==EGL_NONE)break;
        if(attributes[i]!=EGL_PRESENT_OPAQUE_EXT || (attributes[i+1]!=EGL_TRUE&&attributes[i+1]!=EGL_FALSE) || i==62){error=EGL_BAD_ATTRIBUTE;return EGL_NO_SURFACE;}
    }
    auto realSurface=pbuffer(d,config,native->width,native->height);if(realSurface==EGL_NO_SURFACE)return realSurface;
    auto s=std::make_shared<Surface>();s->display=d;s->config=config;s->real=realSurface;s->window=native;s->width=native->width;s->height=native->height;
    if(!geometry(*s)){real<PFNEGLDESTROYSURFACEPROC>("eglDestroySurface")(d,realSurface);error=EGL_BAD_ALLOC;return EGL_NO_SURFACE;}
    // An opaque handle owned by this backend remains stable across resizes.
    auto handle=reinterpret_cast<EGLSurface>(s.get());surfaces.emplace(handle,s);
    fprintf(stderr,"GNU_WINE_NATIVE_EGL create pid=%d surface=%u size=%dx%d\n",getpid(),wl_proxy_get_id(reinterpret_cast<wl_proxy*>(native->surface)),s->width,s->height);
    return handle;
}
extern "C" EGLAPI EGLBoolean EGLAPIENTRY eglMakeCurrent(EGLDisplay d,EGLSurface a,EGLSurface b,EGLContext c) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto f=real<PFNEGLMAKECURRENTPROC>("eglMakeCurrent");if(!f)return EGL_FALSE;
    auto da=find(a),db=find(b);auto result=f(d,da?da->real:a,db?db->real:b,c);
    if(result){draw=da;readSurface=db;drawHandle=a;readHandle=b;}return result;
}
extern "C" EGLAPI EGLSurface EGLAPIENTRY eglGetCurrentSurface(EGLint which) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if(which==EGL_DRAW&&draw)return drawHandle;
    if(which==EGL_READ&&readSurface)return readHandle;
    auto f=real<PFNEGLGETCURRENTSURFACEPROC>("eglGetCurrentSurface");return f?f(which):EGL_NO_SURFACE;
}
extern "C" EGLAPI EGLBoolean EGLAPIENTRY eglQuerySurface(EGLDisplay d,EGLSurface s,EGLint attribute,EGLint* value) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto f=real<PFNEGLQUERYSURFACEPROC>("eglQuerySurface");return f?f(d,underlying(s),attribute,value):EGL_FALSE;
}
extern "C" EGLAPI EGLBoolean EGLAPIENTRY eglDestroySurface(EGLDisplay d,EGLSurface handle) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto f=real<PFNEGLDESTROYSURFACEPROC>("eglDestroySurface");if(!f)return EGL_FALSE;
    auto s=find(handle);auto result=f(d,s?s->real:handle);
    if(result&&s){if(s->blank)wl_buffer_destroy(s->blank);s->blank=nullptr;s->window=nullptr;surfaces.erase(handle);}
    return result;
}
extern "C" EGLAPI EGLBoolean EGLAPIENTRY eglSwapBuffers(EGLDisplay d,EGLSurface handle) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto swap=real<PFNEGLSWAPBUFFERSPROC>("eglSwapBuffers");if(!swap)return EGL_FALSE;
    auto s=find(handle);if(!s)return swap(d,handle);
    if(!s->window||draw!=s){error=EGL_BAD_SURFACE;return EGL_FALSE;}
    if(s->width!=s->window->width||s->height!=s->window->height) {
        auto replacement=pbuffer(d,s->config,s->window->width,s->window->height);if(replacement==EGL_NO_SURFACE)return EGL_FALSE;
        auto context=real<PFNEGLGETCURRENTCONTEXTPROC>("eglGetCurrentContext")();
        auto bind=real<PFNEGLMAKECURRENTPROC>("eglMakeCurrent");
        if(!bind(d,replacement,readSurface==s?replacement:underlying(readHandle),context)){real<PFNEGLDESTROYSURFACEPROC>("eglDestroySurface")(d,replacement);return EGL_FALSE;}
        real<PFNEGLDESTROYSURFACEPROC>("eglDestroySurface")(d,s->real);s->real=replacement;s->width=s->window->width;s->height=s->window->height;
        if(!geometry(*s)){error=EGL_BAD_ALLOC;return EGL_FALSE;}
        return EGL_TRUE; // Next frame renders at the new size, as in OHOS Wine.
    }
    __atomic_store_n(&page->surface,wl_proxy_get_id(reinterpret_cast<wl_proxy*>(s->window->surface)),__ATOMIC_RELEASE);
    gl<void(*)()>("glFlush")();auto result=swap(d,s->real);
    __atomic_store_n(&page->surface,0,__ATOMIC_RELEASE);
    // Real window EGL commits the surface at every swap. Wine queues frame
    // callbacks on that commit; without it the next swap waits indefinitely.
    // No wl_buffer attach/upload is needed: the GPU resource is already queued.
    if(result){wl_surface_commit(s->window->surface);wl_display_flush(display.native);}
    if(result&&++s->frames%120==0)fprintf(stderr,"GNU_WINE_NATIVE_EGL frames=%u surface=%u pixel_readbacks=0\n",s->frames,wl_proxy_get_id(reinterpret_cast<wl_proxy*>(s->window->surface)));
    return result;
}
extern "C" EGLAPI __eglMustCastToProperFunctionPointerType EGLAPIENTRY eglGetProcAddress(const char* name) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if(!name)return nullptr;
#define PROC(n) if(!strcmp(name,#n))return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(&n)
    PROC(eglGetError);PROC(eglQueryString);PROC(eglGetPlatformDisplay);PROC(eglGetConfigAttrib);
    PROC(eglCreateWindowSurface);PROC(eglMakeCurrent);PROC(eglGetCurrentSurface);PROC(eglQuerySurface);PROC(eglDestroySurface);PROC(eglSwapBuffers);
#undef PROC
    if(!strcmp(name,"glFlush"))return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(&flush);
    if(!strcmp(name,"glFinish"))return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(&finish);
    if(!strcmp(name,"glDrawBuffers")||!strcmp(name,"glDrawBuffersARB"))return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(&drawBuffers);
    if(!strcmp(name,"glDrawBuffer"))return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(&drawBuffer);
    if(!strcmp(name,"glReadBuffer"))return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(&readBuffer);
    auto get=real<PFNEGLGETPROCADDRESSPROC>("eglGetProcAddress");return get?get(name):nullptr;
}
