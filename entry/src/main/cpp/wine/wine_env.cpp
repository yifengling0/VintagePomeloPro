#include "wine/wine_env.h"
#include "wine/env_spec.h"
#include "wine/wine_constants.h"
#include "audio/audio_broker.h"
#include "protocols/audio_ipc_protocol.h"
#include "graphics/graphics_broker.h"
#include "graphics/graphics_profile.h"
#include "compositor/wayland_server.h"
#include "input/controller/controller_runtime.h"

#include <unistd.h>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

#ifndef __aarch64__
namespace {
constexpr const char* X86_BUNDLED_GUEST_GFX_DIR = "/data/storage/el1/bundle/libs/x86_64";
}
#endif

int CreateAudioBootstrapFd(const std::string& runtimeDir) {
    if (!winehua::AudioBroker::GetInstance().EnsureStarted(runtimeDir)) {
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] failed to start for runtimeDir=%{public}s", runtimeDir.c_str());
        return -1;
    }
    int fd = winehua::AudioBroker::GetInstance().CreateBootstrapHandle();
    if (fd < 0) {
        OH_LOG_ERROR(LOG_APP, "[AudioBroker] failed to create bootstrap FD for runtimeDir=%{public}s", runtimeDir.c_str());
        return -1;
    }
    OH_LOG_INFO(LOG_APP, "[AudioBroker] bootstrap ready runtimeDir=%{public}s", runtimeDir.c_str());
    return fd;
}

std::vector<std::string> BuildWineEnv(const std::string& sockDir,
                                      const std::string& sockName,
                                      const std::string& libPath,
                                      const std::string& binDir,
                                      int audioBootstrapFd,
                                      const std::string& homeDir,
                                      const std::string& prefixDir) {
    std::string runtimeLibPath = binDir + ":" + binDir + "/x86_64-unix:" + binDir + "/../lib/x86_64";
    winehua::GraphicsBackendState graphicsState = winehua::GraphicsBroker::GetInstance().GetState();
    std::string guestReceiverLibDir;
    bool useGuestReceiverRuntime = graphicsState.active == winehua::GraphicsBackend::Virgl;

    if (useGuestReceiverRuntime && graphicsState.guestReceiverPresent && !graphicsState.guestReceiverRuntimeDir.empty()) {
#ifdef __aarch64__
        guestReceiverLibDir = graphicsState.guestReceiverRuntimeDir + "/lib";
#else
        const std::string bundledGuestLibDir = X86_BUNDLED_GUEST_GFX_DIR;
        if (access((bundledGuestLibDir + "/libwinehua_guest_EGL.so").c_str(), R_OK) == 0 &&
            access((bundledGuestLibDir + "/libgallium-25.0.1.so").c_str(), R_OK) == 0) {
            guestReceiverLibDir = bundledGuestLibDir;
        } else {
            guestReceiverLibDir = graphicsState.guestReceiverRuntimeDir + "/lib";
        }
#endif
        if (access(guestReceiverLibDir.c_str(), F_OK) == 0) {
            runtimeLibPath = guestReceiverLibDir + ":" + runtimeLibPath;
        }
    }

    const std::string prefix = prefixDir.empty() ? std::string(WINE_PREFIX) : prefixDir;
    std::vector<std::string> env = winehua::BuildWineBaselineLines({binDir, homeDir, prefix});
    env.insert(env.begin(), {
        "XDG_RUNTIME_DIR=" + sockDir,
        "WAYLAND_DISPLAY=" + sockName,
    });
    /* Front-load WHGP so NativeChildProcess truncation cannot drop it. */
    winehua::controller::EnsureBridgeForWineLaunch(prefix);
    winehua::controller::AppendWineGamepadEnv(env);
    env.push_back("WINEDEBUG=-all");
    env.push_back("LANG=zh_CN.UTF-8");
    env.push_back("GST_PLUGIN_PATH=" + binDir + "/x86_64-unix/gstreamer-1.0");
    env.push_back("GST_PLUGIN_SYSTEM_PATH=" + binDir + "/x86_64-unix/gstreamer-1.0");
    winehua::AppendBox64PerfStrings(env);
#ifdef __aarch64__
    env.push_back("LD_LIBRARY_PATH=" + libPath);
    env.push_back("BOX64_LD_LIBRARY_PATH=" + runtimeLibPath);
#else
    env.push_back("LD_LIBRARY_PATH=" + runtimeLibPath);
#endif
    if (audioBootstrapFd >= 0) {
        env.push_back("WINE_OHOS_AUDIO_ENABLE=1");
        env.push_back("WINE_OHOS_AUDIO_BOOTSTRAP_FD=" + std::to_string(audioBootstrapFd));
        env.push_back("WINE_OHOS_AUDIO_PROTOCOL_VERSION=" + std::to_string(WINEHUA_AUDIO_PROTOCOL_VERSION));
    }
    winehua::GraphicsBroker::GetInstance().SetWineRuntimeBinaryDir(binDir);
    // Layer 4: 窗口模式。与 master 同层; 取值只来自 WindowingModeFor。
    // NOTE: 桌面模式下 wine_child 仍可通过 __winehua_desktop__ token 设 DESKTOP_MODE=1 (冗余保险)
    winehua::AppendWindowingModeLines(env, WaylandServer::GetInstance()->IsDesktopMode());
    // ==== Layer 5: 图形状态 ====
    // NOTE: BOX64_EMULATED_LIBS (ARM64) 在 DXVK 路径下会被 AppendD3dBackendEnv 覆盖
    winehua::GraphicsBroker::GetInstance().AppendWineEnv(env);

    OH_LOG_INFO(LOG_APP,
                "[WineEnv] backend=%{public}s guestMode=%{public}s guestLib=%{public}s runtimeLibPath=%{public}s",
                winehua::GraphicsBroker::BackendName(graphicsState.active),
                graphicsState.guestReceiverMode.empty() ? "stock-egl" : graphicsState.guestReceiverMode.c_str(),
                guestReceiverLibDir.empty() ? "(none)" : guestReceiverLibDir.c_str(),
                runtimeLibPath.c_str());
    return env;
}

void UpsertEnvLine(std::vector<std::string>& env, const std::string& line)
{
    const size_t sep = line.find('=');
    if (sep == std::string::npos || sep == 0) return;
    const std::string key = line.substr(0, sep);
    // 清理所有同 key 的旧条目, 然后追加新值 — 避免预填充 vector 上
    // push_back 路径 (如 AppendProductDxvkEnv 覆盖 WEAKBARRIER) 产生重复 key。
    // (与上游 bb617a4 收敛语义一致)
    env.erase(std::remove_if(env.begin(), env.end(), [&](const std::string& existing) {
        return existing.compare(0, key.size(), key) == 0 &&
               existing.size() > key.size() && existing[key.size()] == '=';
    }), env.end());
    env.push_back(line);
}

static bool EnvHasKey(const std::vector<std::string>& env, const char* key)
{
    if (!key || !key[0]) return false;
    const size_t length = std::strlen(key);
    for (const std::string& line : env) {
        if (line.size() > length && line.compare(0, length, key) == 0 &&
            line[length] == '=')
            return true;
    }
    return false;
}

void AppendD3dBackendEnv(std::vector<std::string>& env,
                         const std::string& d3dBackend,
                         const std::string& binDir)
{
    const winehua::D3dBackendKind backend = winehua::ParseD3dBackend(d3dBackend);
    if (backend == winehua::D3dBackendKind::WineD3d) {
        const std::vector<std::string> managed = {
            "WINEHUA_D3D_BACKEND=wined3d",
            /* Compatibility mode must be deterministic even when a game
             * directory contains a copied DXVK DLL. Wine's built-in D3D
             * modules render through WineD3D -> OpenGL -> VirGL. */
            "WINEDLLOVERRIDES=d3d8=b;d3d9=b;d3d10core=b;d3d10=b;d3d10_1=b;d3d11=b;dxgi=b",
        };
        for (const std::string& line : managed) UpsertEnvLine(env, line);
        return;
    }
    if (!winehua::IsDxvkBackend(backend)) return;

    winehua::DxvkRuntimeProfile dxvkRuntime;
    if (!winehua::ResolveDxvkRuntimeProfile(backend, &dxvkRuntime)) return;
    const std::string runtimeProfile(dxvkRuntime.directory);
    const std::string overlayRoot = std::string(WINE_RUNTIME_ROOT) +
        "/dxvk/" + runtimeProfile;
    const std::string overlay64 = overlayRoot + "/x64";
    const std::string overlay86 = overlayRoot + "/x86";
    const std::string guestVulkanRoot = binDir + "/guest_vulkan";
    const std::string guestVulkanLib = guestVulkanRoot + "/lib";
    const std::string guestVulkanIcd = guestVulkanRoot +
        "/share/vulkan/icd.d/venus_icd.x86_64.json";
    const std::string box64LibraryPath = guestVulkanLib + ":" +
        binDir + "/guest_gfx/lib:" + binDir + ":" +
        binDir + "/x86_64-unix:" + std::string(WINE_RUNTIME_ROOT) + "/lib/x86_64";
    const std::string wineDllPath = overlay64 + ":" + overlay86 + ":" +
        binDir + "/x86_64-windows:" + binDir + "/i386-windows:" + binDir;

    const std::vector<std::string> managed = {
        "WINEHUA_D3D_BACKEND=" + d3dBackend,
        "WINEHUA_DXVK_ROOT=" + overlayRoot,
        "WINEHUA_DXVK_PROFILE=" + runtimeProfile,
        "WINEHUA_DXVK_VERSION=" + std::string(dxvkRuntime.version),
        "WINEHUA_VULKAN_RUNTIME=1",
        "WINEHUA_VULKAN_LOADER_ARCH=x86_64",
        "WINEHUA_VENUS_ICD_ARCH=x86_64",
#ifdef __aarch64__
        "USE_LIBBOX64=1",
#endif
#ifdef __aarch64__
        "BOX64_LD_LIBRARY_PATH=" + box64LibraryPath,
        "BOX64_EMULATED_LIBS=" + Box64EmulatedLibs(),
#endif
        "VK_DRIVER_FILES=" + guestVulkanIcd,
        "VK_ICD_FILENAMES=" + guestVulkanIcd,
        "VN_DEBUG=vtest",
        /* 与 master 方针一致: DXVK 只接管 D3D11。DX9/10/10.1 使用 Wine 内建
         * WineD3D → OpenGL → VirGL, 该路径在 Venus/Maleoon 栈上对老游戏更
         * 成熟稳定; 全 D3D 走 DXVK(Venus) 会破坏原本 VirGL 驱动的游戏。 */
        "WINEDLLOVERRIDES=d3d11=n;dxgi=n",
        "WINEDLLPATH=" + wineDllPath,
        "WINEDLLDIR0=" + overlay64,
        "WINEDLLDIR1=" + overlay86,
        /* Keep the Wine PE directories contiguous after the DXVK overlays.
         * ntdll stops scanning at the first missing WINEDLLDIR index. */
        "WINEDLLDIR2=" + binDir + "/x86_64-windows",
        "WINEDLLDIR3=" + binDir + "/i386-windows",
        "WINEDLLDIR4=" + binDir,
    };
    for (const std::string& line : managed) UpsertEnvLine(env, line);

    /* Product DXVK sessions also expose VKD3D-Proton for D3D12. Keep the
     * existing 3-arg signature: there is no separate render-mode button.
     *
     * WINEDLLDIR0 stays the DXVK overlay (d3d11/dxgi). d3d12.dll is loaded
     * from WINEDLLPATH. Do not copy it into DIR0 (stray d3d12 next to d3d11
     * made cube's first present white) and do not set
     * VN_WINEHUA_DIRECT_FENCE_WAIT on the shared session — that flag
     * deadlocks Venus when cube and gears share one ring (cube 0.4 FPS,
     * gears white/stuck). GPU_UPLOAD=0 stays vkd3d-prefixed only. */
    const std::string vkd3dRoot = std::string(WINE_RUNTIME_ROOT) +
        "/vkd3d/limited-500k";
    const std::string vkd3d64 = vkd3dRoot + "/x64";
    if (access((vkd3d64 + "/d3d12.dll").c_str(), R_OK) == 0) {
        const std::string overlayD3d12 = overlay64 + "/d3d12.dll";
        if (access(overlayD3d12.c_str(), F_OK) == 0 &&
            unlink(overlayD3d12.c_str()) == 0) {
            OH_LOG_INFO(LOG_APP,
                "[D3D] removed staged d3d12.dll from DXVK overlay %{public}s",
                overlay64.c_str());
        }
        const std::string wineDllPathWithVkd3d = vkd3d64 + ":" + wineDllPath;
        const std::vector<std::string> vkd3dOverlay = {
            "WINEHUA_VKD3D_ROOT=" + vkd3dRoot,
            "WINEHUA_VKD3D_PROFILE=limited-500k",
            "WINEHUA_VKD3D_VERSION=2.6",
            "VKD3D_WINEHUA_GPU_UPLOAD=0",
            "WINEDLLOVERRIDES=d3d12=n;d3d11=n;dxgi=n",
            "WINEDLLPATH=" + wineDllPathWithVkd3d,
        };
        for (const std::string& line : vkd3dOverlay) UpsertEnvLine(env, line);
    }

    if (dxvkRuntime.relaxedFeatureCompatibility) {
        UpsertEnvLine(env, "WINEHUA_DXVK_RELAXED_FEATURES=1");
        /* Prefer the native RGBA8 SNORM render-target path. On devices such
         * as Maleoon where sampling is supported but color attachment usage
         * is not, DXVK may substitute its qualified RGBA16F backing image. */
        UpsertEnvLine(env, "DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT=auto");
    }
    if (dxvkRuntime.commandQueryReset)
        UpsertEnvLine(env, "DXVK_WINEHUA_COMMAND_QUERY_RESET=1");
}

bool IsVkd3dSmokeDemo(const std::string& exePath)
{
    std::string lower = exePath;
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c == '/') c = '\\';
    }
    const size_t slash = lower.find_last_of("\\/");
    const std::string name = slash == std::string::npos ? lower : lower.substr(slash + 1);
    if (name != "gears.exe" && name != "triangle.exe") return false;
    return lower.find("smoke") != std::string::npos || lower == name;
}

void AppendVkd3dDemoPresentEnv(std::vector<std::string>& env,
                               const std::string& d3dBackend,
                               const std::string& binDir)
{
    const winehua::D3dBackendKind selectedBackend =
        winehua::ParseD3dBackend(d3dBackend);
    winehua::DxvkRuntimeProfile dxvkRuntime;
    if (!winehua::ResolveDxvkRuntimeProfile(
            winehua::D3dBackendKind::Vkd3dLimited500k, &dxvkRuntime))
        return;
    if (selectedBackend != winehua::D3dBackendKind::DxvkModern26) {
        OH_LOG_WARN(LOG_APP,
                    "[D3D] vkd3d_limited_500k requires DXVK %{public}s DXGI "
                    "(selected=%{public}s); using %{public}s",
                    dxvkRuntime.version.data(), d3dBackend.c_str(),
                    dxvkRuntime.directory.data());
    }

    const std::string runtimeProfile(dxvkRuntime.directory);
    const std::string vkd3dRoot = std::string(WINE_RUNTIME_ROOT) +
        "/vkd3d/limited-500k";
    const std::string vkd3d64 = vkd3dRoot + "/x64";
    if (access((vkd3d64 + "/d3d12.dll").c_str(), R_OK) != 0) return;

    // Reuse the qualified modern DXVK/Venus transport setup, then put VKD3D
    // first for d3d12. This also makes VKD3D smoke deterministic when the
    // user's normal session backend is WineD3D or DXVK 1.10.
    AppendD3dBackendEnv(env, "dxvk_modern_2_6", binDir);
    std::vector<std::string> productGuestEnvironment;
    if (!winehua::BuildProductGuestGraphicsEnvironment(
            winehua::D3dBackendKind::Vkd3dLimited500k,
            &productGuestEnvironment)) {
        OH_LOG_ERROR(LOG_APP,
                     "[D3D] failed to resolve VKD3D product graphics route");
        return;
    }
    const std::string dxvk64 = std::string(WINE_RUNTIME_ROOT) + "/dxvk/" +
        runtimeProfile + "/x64";
    const std::string dxvk86 = std::string(WINE_RUNTIME_ROOT) + "/dxvk/" +
        runtimeProfile + "/x86";

    const std::string wineDllPath = vkd3d64 + ":" + dxvk64 + ":" + dxvk86 + ":" +
        binDir + "/x86_64-windows:" + binDir + "/i386-windows:" + binDir;
    /* Master 13236b5 vkd3d_limited_500k (qualified gears/triangle). Do not
     * copy b2437a3 explorer-inject (PERSISTENT_MAP_SYNC=0 / FORCE_COHERENT=0 /
     * GPU_UPLOAD=0): that presents a black rotating window. Cube stays on the
     * DXVK DIR0 session; these overrides apply only to this process. */
    env.erase(std::remove_if(env.begin(), env.end(), [](const std::string& existing) {
        return existing.rfind("VKD3D_WINEHUA_GPU_UPLOAD=", 0) == 0;
    }), env.end());
    const std::vector<std::string> demo = {
        "WINEHUA_D3D_BACKEND=vkd3d_limited_500k",
        "WINEHUA_VKD3D_ROOT=" + vkd3dRoot,
        "WINEHUA_VKD3D_PROFILE=limited-500k",
        "WINEHUA_VKD3D_VERSION=2.6",
        "WINEHUA_DXVK_ROOT=" + std::string(WINE_RUNTIME_ROOT) + "/dxvk/" + runtimeProfile,
        "WINEHUA_DXVK_PROFILE=" + runtimeProfile,
        "WINEHUA_DXVK_VERSION=" + std::string(dxvkRuntime.version),
        "WINEDLLDIR0=" + vkd3d64,
        "WINEDLLDIR1=" + dxvk64,
        "WINEDLLDIR2=" + dxvk86,
        "WINEDLLDIR3=" + binDir + "/x86_64-windows",
        "WINEDLLDIR4=" + binDir + "/i386-windows",
        "WINEDLLDIR5=" + binDir,
        "WINEDLLPATH=" + wineDllPath,
        "WINEDLLOVERRIDES=d3d12=n;d3d11=n;dxgi=n",
        "VKR_WINEHUA_SHADOW_FROM_HOST=precise",
    };
    for (const std::string& line : demo) UpsertEnvLine(env, line);
    for (const std::string& line : productGuestEnvironment)
        UpsertEnvLine(env, line);
    if (!EnvHasKey(env, "VN_WINEHUA_PERSISTENT_MAP_SYNC"))
        UpsertEnvLine(env, "VN_WINEHUA_PERSISTENT_MAP_SYNC=1");
    if (!EnvHasKey(env, "VKD3D_WINEHUA_FORCE_COHERENT_MAP_SYNC"))
        UpsertEnvLine(env, "VKD3D_WINEHUA_FORCE_COHERENT_MAP_SYNC=1");
    OH_LOG_INFO(LOG_APP,
        "[D3D] vkd3d demo present DIR0=%{public}s route=product-vulkan "
        "persistent=managed coherent=managed dxgi=%{public}s selected=%{public}s",
        vkd3d64.c_str(), dxvkRuntime.version.data(), d3dBackend.c_str());
}

void AppendProductDxvkEnv(std::vector<std::string>& env,
                          const std::string& d3dBackend,
                          const std::string& graphicsExperiment)
{
    const winehua::D3dBackendKind backend = winehua::ParseD3dBackend(d3dBackend);
    if (!winehua::IsDxvkBackend(backend)) return;

    winehua::DxvkRuntimeProfile dxvkRuntime;
    if (!winehua::ResolveDxvkRuntimeProfile(backend, &dxvkRuntime)) return;

    std::vector<std::string> policyEnvironment;
    const bool resolved = graphicsExperiment.empty()
        ? winehua::BuildProductGuestGraphicsEnvironment(
              backend, &policyEnvironment)
        : winehua::BuildLabGuestGraphicsEnvironment(
              graphicsExperiment, backend, &policyEnvironment);
    if (!resolved) {
        OH_LOG_ERROR(LOG_APP,
                     "[D3D] rejected unresolved Guest graphics policy=%{public}s "
                     "backend=%{public}s",
                     graphicsExperiment.empty() ? "(product)" :
                         graphicsExperiment.c_str(),
                     d3dBackend.c_str());
        return;
    }
    const std::vector<std::string> managed = {
        // Retain actionable DXVK failures without filling a user's game
        // directory with the informational startup stream.
        "DXVK_LOG_LEVEL=warn",
        "DXVK_LOG_PATH=C:\\windows\\temp",
    };
    for (const std::string& line : managed) UpsertEnvLine(env, line);
    if (dxvkRuntime.batchMappedFlush) {
        /* This capability is qualified by command-list ownership and
         * continuous Heaven gates. Per-range statistics remain LAB-only. */
        UpsertEnvLine(env, "DXVK_WINEHUA_BATCH_MAPPED_FLUSH=1");
    }
    for (const std::string& line : policyEnvironment) UpsertEnvLine(env, line);
#ifdef __aarch64__
    // BOX64 变量仅在 ARM64 生效; x86_64 主机设置它会破坏 broker entryParams。
    UpsertEnvLine(env, "BOX64_DYNAREC_WEAKBARRIER=0");
#endif
}

std::string SerializeEnvToEntryParams(const std::vector<std::string>& env) {
    return winehua::EnvSpec::fromLines(env).serializeEntryParams();
}

void LogGraphicsBackendStateForLaunch(const char* tag) {
    winehua::GraphicsBackendState state = winehua::GraphicsBroker::GetInstance().GetState();
    OH_LOG_INFO(LOG_APP,
                "[%{public}s] graphics requested=%{public}s active=%{public}s runtimeReady=%{public}s "
                "guestReceiver=%{public}s(%{public}s) virglSocketReady=%{public}s virglLibraryPresent=%{public}s",
                tag,
                winehua::GraphicsBroker::BackendName(state.requested),
                winehua::GraphicsBroker::BackendName(state.active),
                state.runtimeReady ? "true" : "false",
                state.guestReceiverPresent ? "true" : "false",
                state.guestReceiverMode.empty() ? "stock-egl" : state.guestReceiverMode.c_str(),
                state.virglSocketReady ? "true" : "false",
                state.virglLibraryPresent ? "true" : "false");
    if (!state.lastError.empty())
        OH_LOG_WARN(LOG_APP, "[%{public}s] graphics note: %{public}s", tag, state.lastError.c_str());
}
