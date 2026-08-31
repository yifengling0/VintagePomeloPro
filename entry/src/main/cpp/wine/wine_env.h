#ifndef WINE_ENV_H
#define WINE_ENV_H

/**
 * wine_env.h — Wine 环境变量设置
 */

#include <cstdlib>
#include <string>
#include <vector>

#include "wine/wine_constants.h"
#include "wine/wine_env_baseline.h"

/**
 * BOX64_EMULATED_LIBS 完整列表 (ARM64 真机)。
 *
 * 除图形/输入栈外, gnutls (schannel TLS) 与 gstreamer (winegstreamer)
 * 链的 guest 库都是 x86_64 ELF, 必须由 box64 模拟执行。若不列出,
 * box64 会按 native (arm64 dlopen) 加载, 对 x86_64 目标报
 * "Error initializing native lib...: No such file" — IE/网络走 schannel
 * 时 gnutls 加载失败, HTML 渲染则依赖 Wine Gecko (独立组件)。
 * 与运行时 wine/bin/x86_64-unix 下实际部署的 .so 保持一致。
 */
static inline std::string Box64EmulatedLibs()
{
    return "libvulkan.so:libvulkan.so.1:"
           "libEGL.so:libEGL.so.1:libGLESv2.so:libGLESv2.so.2:"
           "libGLESv1_CM.so:libGLESv1_CM.so.1:libGL.so:libGL.so.1:"
           "libwayland-client.so:libwayland-client.so.0:libwayland-server.so:"
           "libwayland-server.so.0:libwayland-egl.so:libwayland-egl.so.1:"
           "libdrm.so:libdrm.so.2:libffi.so:libffi.so.8:"
           // gnutls 链 (schannel TLS)
           "libgnutls.so:libgnutls.so.30:"
           "libnettle.so:libnettle.so.8:"
           "libhogweed.so:libhogweed.so.6:"
           "libgmp.so:libgmp.so.10:"
           "libtasn1.so:libtasn1.so.6:"
           "libunistring.so:libunistring.so.5:"
           // glib 链
           "libglib-2.0.so:libglib-2.0.so.0:"
           "libgobject-2.0.so:libgobject-2.0.so.0:"
           "libgio-2.0.so:libgio-2.0.so.0:"
           "libgmodule-2.0.so:libgmodule-2.0.so.0:"
           // gstreamer 链 (winegstreamer)
           "libgstreamer-1.0.so:libgstreamer-1.0.so.0:"
           "libgstbase-1.0.so:libgstbase-1.0.so.0:"
           "libgstvideo-1.0.so:libgstvideo-1.0.so.0:"
           "libgstaudio-1.0.so:libgstaudio-1.0.so.0:"
           "libgsttag-1.0.so:libgsttag-1.0.so.0:"
           "libgstpbutils-1.0.so:libgstpbutils-1.0.so.0:"
           "libgstallocators-1.0.so:libgstallocators-1.0.so.0:"
           "libgstapp-1.0.so:libgstapp-1.0.so.0:"
           "libgstcontroller-1.0.so:libgstcontroller-1.0.so.0:"
           "libgstfft-1.0.so:libgstfft-1.0.so.0:"
           "libgstnet-1.0.so:libgstnet-1.0.so.0:"
           "libgstriff-1.0.so:libgstriff-1.0.so.0:"
           "libgstrtp-1.0.so:libgstrtp-1.0.so.0:"
           "libgstrtsp-1.0.so:libgstrtsp-1.0.so.0:"
           "libgstsdp-1.0.so:libgstsdp-1.0.so.0:"
           // libgstcodecparsers: gst-plugins-bad videoparsersbad (h264parse 等)
           // 的依赖库; 不在列表时 box64 无法加载插件, GStreamer 报
           // "Failed to load plugin libgstvideoparsersbad.so"。
           "libgstcodecparsers-1.0.so:libgstcodecparsers-1.0.so.0:"
           // libgstmpegts: libgstmpegtsdemux (MPEG-TS) 的依赖库。
           "libgstmpegts-1.0.so:libgstmpegts-1.0.so.0:"
           // libxml2/libz: gst-plugins-bad 类插件 (libgstadaptivedemux2 等) 依赖。
           // 不在列表时 box64 重定位其版本化符号 (LIBXML2_2.9.0) 失败,
           // 插件 dlopen 失败, GStreamer 报 "Failed to load plugin"。
           "libxml2.so:libxml2.so.2:"
           "libz.so:libz.so.1";
}

using winehua::SetBox64PerfEnv;
using winehua::AppendBox64PerfStrings;

// -- Wine 环境变量构建 --
std::vector<std::string> BuildWineEnv(const std::string& sockDir,
                                      const std::string& sockName,
                                      const std::string& libPath,
                                      const std::string& binDir,
                                      int audioBootstrapFd,
                                      const std::string& homeDir,
                                      const std::string& prefixDir = WINE_PREFIX,
                                      const std::string& wineLang = "zh_CN");

// Add the managed product D3D backend overlay to a process environment. The
// caller selects the product backend once per Wine session; the default is
// dxvk_legacy, while wined3d remains an explicit compatibility fallback.
void AppendD3dBackendEnv(std::vector<std::string>& env,
                         const std::string& d3dBackend,
                         const std::string& binDir);
// WineHua common control-plane overload. The product resolver keeps the final
// runtime choice authoritative and uses the explicit DXVK field for contract
// compatibility, including the VKD3D companion path.
void AppendD3dBackendEnv(std::vector<std::string>& env,
                         const std::string& d3dBackend,
                         const std::string& dxvkBackend,
                         const std::string& binDir);

// C:\smoke gears/triangle only. Cube keeps the DXVK DIR0 overlay; these
// demos need vkd3d-first WINEDLLDIR plus the qualified D3D12 present path
// or Venus presents an empty (black) window.
bool IsVkd3dSmokeDemo(const std::string& exePath);
void AppendVkd3dDemoPresentEnv(std::vector<std::string>& env,
                               const std::string& d3dBackend,
                               const std::string& binDir);

// Add the stable production DXVK policy shared by Explorer descendants and
// applications launched directly from an Old Pomelo app card. Diagnostics
// may provide an explicit LAB profile; an empty value selects the product
// Vulkan route without consulting the LAB registry.
void AppendProductDxvkEnv(std::vector<std::string>& env,
                          const std::string& d3dBackend,
                          const std::string& graphicsExperiment = "");

// 覆盖式追加: 清理同 key 旧条目后追加新值 (graphics_broker 等跨文件使用)
void UpsertEnvLine(std::vector<std::string>& env, const std::string& line);

// -- Audio bootstrap --
int CreateAudioBootstrapFd(const std::string& runtimeDir);

// -- entryParams 序列化 (实现收口到 EnvSpec) --
std::string SerializeEnvToEntryParams(const std::vector<std::string>& env);

// -- Graphics 辅助 --
void LogGraphicsBackendStateForLaunch(const char* tag);

#endif // WINE_ENV_H
