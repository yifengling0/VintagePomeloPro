#include "graphics/native_window_gles_target.h"
#include <native_buffer/native_buffer.h>
#include <native_window/graphic_error_code.h>
#include <cassert>
#include <cstdarg>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <unordered_map>
#include <unistd.h>

struct NativeWindow {};
struct NativeWindowBuffer { uint32_t seq; };
namespace {
using namespace winehua;
NativeWindow window;
NativeWindowBuffer buffer{1};
GlesBufferOwner owner{1, 1, 1, 2, 800, 600, NATIVEBUFFER_PIXEL_FMT_RGBA_8888};
EGLContext context = reinterpret_cast<EGLContext>(owner.context);
const char* extensions = "EGL_OHOS_image_native_buffer EGL_KHR_image_base EGL_ANDROID_native_fence_sync EGL_KHR_wait_sync";
const char* missingProc = "";
int requestError = 0, flushError = 0, refs = 0, imports = 0, aborts = 0;
int requestFd = -1, acquireFd = -1, exportedFd = -1;
bool signaled = true, imageFailure = false, fenceFailure = false, exportFailure = false;
bool acquireFailure = false, fboFailure = false, wrongLayout = false;
bool stamped = false, fenceSubmitted = false;
uintptr_t nextHandle = 10;
int timeoutMs = -1;
std::unordered_map<EGLSyncKHR, int> syncFds;

int NewFd() { int fd = open("/dev/null", O_RDONLY); assert(fd >= 0); return fd; }
bool Closed(int fd) { return fcntl(fd, F_GETFD) == -1; }
EGLImageKHR CreateImage(EGLDisplay, EGLContext ctx, EGLenum target, EGLClientBuffer client, const EGLint*)
{
    assert(ctx == EGL_NO_CONTEXT && target == EGL_NATIVE_BUFFER_OHOS && client == &buffer);
    ++imports;
    return imageFailure ? EGL_NO_IMAGE_KHR : reinterpret_cast<EGLImageKHR>(++nextHandle);
}
EGLBoolean DestroyImage(EGLDisplay, EGLImageKHR) { return EGL_TRUE; }
void ImageStorage(GLenum, GLeglImageOES) {}
EGLSyncKHR CreateSync(EGLDisplay, EGLenum type, const EGLint* attributes)
{
    assert(type == EGL_SYNC_NATIVE_FENCE_ANDROID);
    if (attributes && acquireFailure) return EGL_NO_SYNC_KHR;
    if (!attributes && fenceFailure) return EGL_NO_SYNC_KHR;
    EGLSyncKHR sync = reinterpret_cast<EGLSyncKHR>(++nextHandle);
    syncFds[sync] = attributes ? attributes[1] : -1;
    if (!attributes) fenceSubmitted = false;
    return sync;
}
EGLBoolean DestroySync(EGLDisplay, EGLSyncKHR sync)
{
    if (syncFds.at(sync) >= 0) close(syncFds.at(sync));
    syncFds.erase(sync);
    return EGL_TRUE;
}
EGLint WaitSync(EGLDisplay, EGLSyncKHR sync, EGLint) { assert(syncFds.count(sync)); return EGL_TRUE; }
EGLint DupFence(EGLDisplay, EGLSyncKHR)
{
    assert(fenceSubmitted); // glFlush must follow native fence creation.
    if (exportFailure) return -1;
    return exportedFd = NewFd();
}
void ClearScenario()
{
    assert(refs == 0 && syncFds.empty());
    requestError = flushError = imports = aborts = 0;
    requestFd = acquireFd = exportedFd = -1;
    signaled = true;
    imageFailure = fenceFailure = exportFailure = acquireFailure = fboFailure = wrongLayout = false;
    stamped = fenceSubmitted = false;
    buffer.seq = 1;
    timeoutMs = -1;
    context = reinterpret_cast<EGLContext>(owner.context);
}
}

extern "C" {
EGLDisplay eglGetCurrentDisplay() { return reinterpret_cast<EGLDisplay>(owner.display); }
EGLContext eglGetCurrentContext() { return context; }
const char* eglQueryString(EGLDisplay, EGLint) { return extensions; }
EGLint eglGetError() { return EGL_BAD_ALLOC; }
const GLubyte* glGetString(GLenum) { return reinterpret_cast<const GLubyte*>("GL_OES_EGL_image"); }
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char* name)
{
    if (!strcmp(name, missingProc)) return nullptr;
#define ENTRY(symbol, implementation) if (!strcmp(name, symbol)) return reinterpret_cast<__eglMustCastToProperFunctionPointerType>(implementation)
    ENTRY("eglCreateImageKHR", CreateImage);
    ENTRY("eglDestroyImageKHR", DestroyImage);
    ENTRY("glEGLImageTargetRenderbufferStorageOES", ImageStorage);
    ENTRY("eglCreateSyncKHR", CreateSync);
    ENTRY("eglDestroySyncKHR", DestroySync);
    ENTRY("eglWaitSyncKHR", WaitSync);
    ENTRY("eglDupNativeFenceFDANDROID", DupFence);
#undef ENTRY
    assert(false); return nullptr;
}
void glGenRenderbuffers(GLsizei, GLuint* out) { *out = ++nextHandle; }
void glGenFramebuffers(GLsizei, GLuint* out) { *out = ++nextHandle; }
void glBindRenderbuffer(GLenum, GLuint) {}
void glBindFramebuffer(GLenum, GLuint) {}
void glFramebufferRenderbuffer(GLenum, GLenum, GLenum, GLuint) {}
GLenum glCheckFramebufferStatus(GLenum) { return fboFailure ? GL_FRAMEBUFFER_UNSUPPORTED : GL_FRAMEBUFFER_COMPLETE; }
GLenum glGetError() { return GL_NO_ERROR; }
void glDeleteFramebuffers(GLsizei, const GLuint*) {}
void glDeleteRenderbuffers(GLsizei, const GLuint*) {}
GLsync glFenceSync(GLenum, GLbitfield) { return reinterpret_cast<GLsync>(++nextHandle); }
void glDeleteSync(GLsync) {}
void glFlush() { fenceSubmitted = true; }
GLenum glClientWaitSync(GLsync, GLbitfield flags, GLuint64 timeout)
{
    assert(flags == 0 && timeout == 0); // Never block under presenter/manager lock.
    return signaled ? GL_ALREADY_SIGNALED : GL_TIMEOUT_EXPIRED;
}
int32_t OH_NativeWindow_NativeWindowHandleOpt(OHNativeWindow*, int code, ...)
{
    va_list args; va_start(args, code);
    if (code == SET_TIMEOUT) timeoutMs = va_arg(args, int32_t);
    if (code == SET_UI_TIMESTAMP) { assert(va_arg(args, uint64_t) > 0); stamped = true; }
    va_end(args); return 0;
}
int32_t OH_NativeWindow_NativeWindowRequestBuffer(OHNativeWindow*, OHNativeWindowBuffer** out, int* fd)
{
    assert(stamped); stamped = false;
    *out = requestError ? nullptr : &buffer;
    *fd = requestFd; acquireFd = requestFd; requestFd = -1;
    return requestError;
}
int32_t OH_NativeWindow_NativeWindowFlushBuffer(OHNativeWindow*, OHNativeWindowBuffer*, int fd, Region)
{
    assert(fd >= 0 && !Closed(fd)); close(fd);
    return flushError;
}
int32_t OH_NativeWindow_NativeWindowAbortBuffer(OHNativeWindow*, OHNativeWindowBuffer*) { ++aborts; return 0; }
int32_t OH_NativeWindow_NativeObjectReference(void*) { ++refs; return 0; }
int32_t OH_NativeWindow_NativeObjectUnreference(void*) { assert(refs > 0); --refs; return 0; }
int32_t OH_NativeBuffer_FromNativeWindowBuffer(OHNativeWindowBuffer* in, OH_NativeBuffer** out)
{
    *out = reinterpret_cast<OH_NativeBuffer*>(in); return 0;
}
uint32_t OH_NativeBuffer_GetSeqNum(OH_NativeBuffer* native) { return reinterpret_cast<NativeWindowBuffer*>(native)->seq; }
void OH_NativeBuffer_GetConfig(OH_NativeBuffer*, OH_NativeBuffer_Config* config)
{
    *config = {};
    config->width = wrongLayout ? 42 : owner.width;
    config->height = owner.height; config->format = owner.format;
}
}

int main()
{
    using namespace winehua;
    {
        NativeWindowGlesTarget target;
        const char* valid = extensions; extensions = "EGL_KHR_image_base";
        assert(!target.Configure(owner, &window) && !target.Ready());
        extensions = valid;
        missingProc = "eglWaitSyncKHR";
        assert(!target.Configure(owner, &window)); missingProc = "";
        assert(target.Configure(owner, &window));
        auto different = owner; ++different.generation;
        assert(!target.Configure(different, &window));
        requestFd = NewFd();
        assert(target.BeginFrame(0, 1) == GlesBeginResult::Ready);
        assert(Closed(acquireFd) && timeoutMs == 100 && refs == 1);
        assert(target.Publish() && Closed(exportedFd));
        assert(target.BeginFrame(24, 2) == GlesBeginResult::Ready && timeoutMs == 0);
        assert(imports == 1); // Cache hit within one owner/sequence.
        assert(target.Publish());
        signaled = false; assert(!target.Reset() && refs == 1);
        signaled = true;
        context = EGL_NO_CONTEXT; assert(!target.Reset() && refs == 1);
        context = reinterpret_cast<EGLContext>(owner.context);
        assert(target.Reset() && refs == 0 && !target.Ready());
    }
    ClearScenario();
    for (int error : {static_cast<int>(NATIVE_ERROR_NO_BUFFER),
                     static_cast<int>(NATIVE_ERROR_BUFFER_QUEUE_FULL), -1}) {
        NativeWindowGlesTarget target; assert(target.Configure(owner, &window));
        requestError = error; requestFd = NewFd();
        assert(target.BeginFrame(24, 1) == (error == -1 ? GlesBeginResult::Failed : GlesBeginResult::Deferred));
        assert(Closed(acquireFd) && imports == 0 && refs == 0);
        assert(target.Reset()); ClearScenario();
    }
    for (int mode = 0; mode < 4; ++mode) {
        NativeWindowGlesTarget target; assert(target.Configure(owner, &window));
        imageFailure = mode == 0; fboFailure = mode == 1;
        acquireFailure = mode == 2; wrongLayout = mode == 3;
        requestFd = NewFd();
        assert(target.BeginFrame(24, 1) == GlesBeginResult::Failed);
        assert(Closed(acquireFd) && aborts == 1);
        assert(target.Reset() && refs == 0); ClearScenario();
    }
    for (int mode = 0; mode < 3; ++mode) {
        NativeWindowGlesTarget target; assert(target.Configure(owner, &window));
        assert(target.BeginFrame(24, 1) == GlesBeginResult::Ready);
        fenceFailure = mode == 0; exportFailure = mode == 1; flushError = mode == 2 ? -1 : 0;
        assert(!target.Publish());
        signaled = false;
        assert(!target.AbortFrame() && !target.Reset() && refs == 1 && aborts == 0);
        if (mode == 2) assert(Closed(exportedFd));
        signaled = true;
        assert(target.Reset() && aborts == 1 && refs == 0); ClearScenario();
    }
    {
        NativeWindowGlesTarget target; assert(target.Configure(owner, &window));
        for (buffer.seq = 1; buffer.seq <= 64; ++buffer.seq) {
            assert(target.BeginFrame(24, buffer.seq) == GlesBeginResult::Ready);
            assert(target.Publish());
        }
        assert(target.ImportedSlots() == 64 && refs == 64);
        assert(target.BeginFrame(24, 65) == GlesBeginResult::Failed);
        assert(!strcmp(target.Reason(), "import-limit"));
        assert(target.Reset() && refs == 0);
    }
    assert(syncFds.empty());
    std::cout << "native_window_gles_target PASS (capabilities, cache, fd ownership, queue retry, nonblocking retirement, failures)\n";
}
