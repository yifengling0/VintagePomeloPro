// Run with EGL_PLATFORM=surfaceless on Mesa or with a native EGL display.
#include <EGL/egl.h>
#include <cassert>
#include <climits>
#include <cstdio>
#include <vector>

#include "compositor/frame/shm_frame_source.h"
#include "graphics/shm_texture_upload.h"

int main() {
    constexpr int width = 3120, height = 2080, rowLength = width + 4;
    const EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    assert(eglInitialize(display, nullptr, nullptr) && eglBindAPI(EGL_OPENGL_ES_API));
    const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                 EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLConfig config;
    EGLint count;
    assert(eglChooseConfig(display, attributes, &config, 1, &count) && count == 1);
    const EGLint surfaceAttributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const auto surface = eglCreatePbufferSurface(display, config, surfaceAttributes);
    const auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
    assert(eglMakeCurrent(display, surface, surface, context));
    GLuint textures[2], framebuffer;
    glGenTextures(2, textures);
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[0], 0);

    std::vector<uint8_t> pixels(static_cast<size_t>(rowLength) * height * 4);
    std::vector<uint8_t> readback(static_cast<size_t>(width) * height * 4);
    PresentedFrame frame;
    frame.w = width;
    frame.h = height;
    frame.pixels = pixels.data();
    frame.pixelSource = PresentedFrame::PixelSource::Toplevel;
    frame.serial = 1;
    winehua::ShmTextureUpload texture;
    const auto paint = [&](int x, int y, int w, int h, uint8_t value) {
        for (int row = y; row < y + h; ++row)
            std::fill_n(pixels.data() + (static_cast<size_t>(row) * rowLength + x) * 4, w * 4, value);
    };
    const auto uploadAndCheck = [&](uint64_t calls, uint64_t bytes) {
        winehua::TextureUploadStats stats;
        assert(texture.Upload(7, frame, rowLength, stats) == GL_NO_ERROR);
        assert(stats.calls == calls && stats.bytes == bytes);
        GLint unpack = -1;
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack);
        assert(unpack == 0 && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, readback.data());
        assert(glGetError() == GL_NO_ERROR);
        for (int row = 0; row < height; ++row)
            assert(std::equal(readback.data() + static_cast<size_t>(row) * width * 4,
                              readback.data() + static_cast<size_t>(row + 1) * width * 4,
                              pixels.data() + static_cast<size_t>(row) * rowLength * 4));
    };
    const uint64_t fullBytes = static_cast<uint64_t>(width) * height * 4;
    uploadAndCheck(1, fullBytes);

    ShmCommitInfo info;
    info.bufW = info.contentW = width;
    info.bufH = info.contentH = height;
    info.stride = rowLength * 4;
    const auto layout = GetShmDamageLayout(info, {}, true, true);
    assert(layout.Matches(layout));
    PixelDamage surfaceDamage, bufferDamage;
    surfaceDamage.AddRequest(-2, 12, 10, 8);
    surfaceDamage.AddRequest(INT_MIN, INT_MIN, INT_MAX, INT_MAX);
    bufferDamage.AddRequest(INT_MAX - 4, INT_MAX - 4, INT_MAX, INT_MAX);
    bufferDamage.AddRequest(12, 12, 8, 8);
    paint(0, 12, 8, 8, 57);
    paint(12, 12, 8, 8, 61);
    frame.baseSerial = frame.serial++;
    frame.damage = MapShmDamage(layout, surfaceDamage, bufferDamage);
    uploadAndCheck(2, 512);

    for (int blocks : {32, 33}) {
        bufferDamage.Clear();
        for (int i = 0; i < blocks; ++i) {
            const int x = 100 + i % 8 * 12;
            const int y = 100 + i / 8 * 12;
            bufferDamage.AddRequest(x, y, 8, 8);
            paint(x, y, 8, 8, static_cast<uint8_t>(blocks));
        }
        frame.baseSerial = frame.serial++;
        frame.damage = MapShmDamage(layout, {}, bufferDamage);
        uploadAndCheck(blocks == 32 ? 32 : 1, blocks == 32 ? 8192 : 92 * 56 * 4);
    }

    // Skip an intermediate version whose damage is outside the latest rect.
    paint(300, 600, 8, 8, 101);
    ++frame.serial;
    paint(700, 800, 6, 8, 102);
    frame.baseSerial = frame.serial++;
    frame.damage.Clear();
    frame.damage.AddRequest(700, 800, 6, 8);
    uploadAndCheck(1, fullBytes);

    // An upload error invalidates the texture baseline before the next retry.
    paint(700, 800, 6, 8, 103);
    frame.baseSerial = frame.serial++;
    glBindTexture(GL_TEXTURE_2D, textures[1]);
    winehua::TextureUploadStats rejected;
    assert(texture.Upload(7, frame, rowLength, rejected) == GL_INVALID_OPERATION);
    assert(texture.serial == 0);
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    uploadAndCheck(1, fullBytes);

    DirectViewportState viewport;
    viewport.scale = 2;
    assert(!GetShmDamageLayout(info, viewport, true, true).valid);
    viewport.scale = 1;
    viewport.destinationW = width / 2;
    viewport.destinationH = height / 2;
    const auto downsampled = GetShmDamageLayout(info, viewport, true, true);
    assert(downsampled.valid && !layout.Matches(downsampled));
    bufferDamage.Clear();
    bufferDamage.AddRequest(10, 12, 4, 6);
    const auto mapped = MapShmDamage(downsampled, {}, bufferDamage).Bounds();
    assert(mapped.left == 5 && mapped.top == 6 && mapped.Width() == 2 && mapped.Height() == 3);
    std::printf("EGL %s: %llu bytes full, 512 bytes two blocks, 8192 bytes 32 blocks, "
                "20608 bytes 33 clustered blocks. Pixel readback and fallback PASS.\n",
                glGetString(GL_RENDERER), static_cast<unsigned long long>(fullBytes));
    eglTerminate(display);
}
