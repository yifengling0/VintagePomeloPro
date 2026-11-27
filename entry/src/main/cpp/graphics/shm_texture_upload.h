#pragma once

#include <GLES3/gl3.h>
#include "common/texture_upload_stats.h"
#include "compositor/frame/presented_frame.h"

namespace winehua {

// Lives with one GL texture in the render thread. Failed uploads invalidate
// the serial even when some rectangles reached the driver.
struct ShmTextureUpload {
    int width = 0, height = 0;
    uint32_t toplevel = 0;
    PresentedFrame::PixelSource source = PresentedFrame::PixelSource::Untracked;
    uint64_t serial = 0;
    bool opaque = false;

    GLenum Upload(uint32_t id, const PresentedFrame& frame, int rowLength,
                  TextureUploadStats& stats) {
        if (!frame.pixels || frame.w <= 0 || frame.h <= 0 || rowLength < frame.w) {
            serial = 0;
            return GL_INVALID_VALUE;
        }
        const bool allocate = width != frame.w || height != frame.h;
        const PixelRect limits{0, 0, frame.w, frame.h};
        bool partial = !allocate && id == toplevel && source == frame.pixelSource &&
                       frame.opaque == opaque && frame.baseSerial != 0 &&
                       serial == frame.baseSerial && !frame.damage.Empty();
        if (partial) {
            uint64_t area = 0;
            for (const auto& rect : frame.damage) {
                if (rect.left < 0 || rect.top < 0 || rect.right > limits.right || rect.bottom > limits.bottom) {
                    partial = false;
                    break;
                }
                area += static_cast<uint64_t>(rect.Width()) * rect.Height();
            }
            const uint64_t totalArea = static_cast<uint64_t>(frame.w) * frame.h;
            partial = partial && area < totalArea - totalArea / 4;
        }

        // This context uses the default unpack row length outside this call.
        const bool setRowLength = partial || rowLength != frame.w;
        if (setRowLength) glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
        if (partial) {
            for (const auto& rect : frame.damage) {
                const auto offset = (static_cast<size_t>(rect.top) * rowLength + rect.left) * 4;
                glTexSubImage2D(GL_TEXTURE_2D, 0, rect.left, rect.top, rect.Width(), rect.Height(),
                                GL_RGBA, GL_UNSIGNED_BYTE, frame.pixels + offset);
                stats.Issued(rect.Width(), rect.Height(), 0);
            }
        } else {
            if (allocate) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frame.w, frame.h, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, frame.pixels);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.w, frame.h,
                                GL_RGBA, GL_UNSIGNED_BYTE, frame.pixels);
            }
            stats.Issued(frame.w, frame.h, 0);
        }
        if (setRowLength) glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        const GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            serial = 0;
            return error;
        }
        width = frame.w;
        height = frame.h;
        toplevel = id;
        source = frame.pixelSource;
        serial = frame.serial;
        opaque = frame.opaque;
        return GL_NO_ERROR;
    }
};

} // namespace winehua
