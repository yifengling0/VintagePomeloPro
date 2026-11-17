// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <cstdint>
#include <cstring>

namespace gamescope::ohos {
constexpr uint32_t SessionMagic = 0x56475332;
enum class SessionMode : uint32_t { QualifyLoader = 0, Compositor = 1 };
struct SessionConfig {
    uint32_t magic = SessionMagic, version = 1, size = sizeof(SessionConfig);
    SessionMode mode = SessionMode::QualifyLoader;
    uint64_t surfaceId = 0;
    uint32_t renderWidth = 800, renderHeight = 600;
    uint32_t outputWidth = 800, outputHeight = 600;
    uint32_t filter = 0, fpsLimit = 0, sharpness = 2;
    char runtimeDirectory[1024]{};
    char logPath[1024]{};
};
enum class SessionStage : uint32_t {
    Failed = 0, Loaded = 1, Returned = 2, OutputReady = 3, DisplayReady = 4, Presented = 5
};
enum class CommandKind : uint32_t { Stop = 1, OutputState = 2 };
struct SessionCommand {
    uint32_t magic = SessionMagic, version = 1, size = sizeof(SessionCommand);
    CommandKind kind = CommandKind::Stop;
    uint32_t width = 0, height = 0, visible = 1;
};
struct SessionEvent {
    uint32_t magic = SessionMagic, version = 1, size = sizeof(SessionEvent);
    SessionStage stage = SessionStage::Failed;
    int32_t error = 0, pid = -1;
    char detail[160]{};
};
inline bool SafeSessionPath(const char* path, size_t capacity) {
    const char* end = static_cast<const char*>(memchr(path, 0, capacity));
    if (!end || path[0] != '/' || end == path+1) return false;
    for (const char* p = path; p < end; ++p) if (*p == '|' || *p == '\n' || *p == '\r') return false;
    return true;
}
inline bool ValidateSessionConfig(const SessionConfig& config) {
    if (config.magic != SessionMagic || config.version != 1 || config.size != sizeof(config) ||
        (config.mode != SessionMode::QualifyLoader && config.mode != SessionMode::Compositor)) return false;
    if (config.mode == SessionMode::QualifyLoader) return true;
    return config.surfaceId && config.renderWidth >= 64 && config.renderWidth <= 8192 &&
        config.renderHeight >= 64 && config.renderHeight <= 8192 &&
        config.outputWidth >= 64 && config.outputWidth <= 8192 &&
        config.outputHeight >= 64 && config.outputHeight <= 8192 && config.filter <= 3 &&
        (config.fpsLimit == 0 || config.fpsLimit == 30 || config.fpsLimit == 60) && config.sharpness <= 20 &&
        SafeSessionPath(config.runtimeDirectory, sizeof(config.runtimeDirectory)) &&
        SafeSessionPath(config.logPath, sizeof(config.logPath));
}
} // namespace gamescope::ohos
