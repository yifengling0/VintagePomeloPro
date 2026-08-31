#include <napi/native_api.h>
#include "common/fs_utils.h"
#include "compositor/wayland_server.h"
#include "bridge/plugin_manager.h"
#include "input/input_manager.h"
#include "input/text_input.h"
#include "input/pointer_extras.h"
#include "graphics/egl_renderer.h"
#include "common/fps_counter.h"
#include "bridge/performance_monitor_napi.h"
#include "audio/audio_broker.h"
#include "protocols/audio_ipc_protocol.h"
#include "graphics/graphics_broker.h"
#include "graphics/graphics_profile.h"
#include "wine/wine_constants.h"
#include "wine/wine_env.h"
#include "proc/wine_process.h"
#include "wine/wine_launch.h"
#include "wine/wine_exe.h"
#include "wine/wine_mmap_test.h"
#include "common/font_zip.h"
#include "graphics/host_vulkan_probe.h"
#include "input/game_controller_bridge.h"
#include "input/controller/controller_napi.h"
#include "phone_adapter/phone_adapter.h"
#include "common/app_log.h"

#include <unistd.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>
#include <fcntl.h>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <algorithm>
#include <vector>
#include <dlfcn.h>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

// -- 全局状态 (NAPI 层, 被 wine_process / wine_launch 引用) --
napi_threadsafe_function gStateTsfn = nullptr;
std::string gSockPath;

// -- State 回调 -> ArkTS --
static void CallJsState(napi_env env, napi_value cb, void*, void* data) {
    char* msg = static_cast<char*>(data);
    if (env && cb && msg) {
        napi_value undef, arg;
        napi_get_undefined(env, &undef);
        napi_create_string_utf8(env, msg, NAPI_AUTO_LENGTH, &arg);
        napi_call_function(env, undef, cb, 1, &arg, nullptr);
    }
    free(msg);
}

// -- NAPI: setStateCallback --
static napi_value SetStateCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (gStateTsfn) {
        napi_release_threadsafe_function(gStateTsfn, napi_tsfn_release);
        gStateTsfn = nullptr;
    }

    napi_value name;
    napi_create_string_utf8(env, "WLState", NAPI_AUTO_LENGTH, &name);
    napi_create_threadsafe_function(env, args[0], nullptr, name,
                                     0, 1, nullptr, nullptr, nullptr, CallJsState, &gStateTsfn);

    WaylandServer::GetInstance()->SetStateCallback([](const char* s) {
        if (gStateTsfn) {
            napi_call_threadsafe_function(gStateTsfn, strdup(s), napi_tsfn_blocking);
        }
    });
    return nullptr;
}

// -- NAPI: startServer --
static napi_value StartServer(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    char path[512] = {};
    napi_get_value_string_utf8(env, args[0], path, sizeof(path), nullptr);

    OH_LOG_INFO(LOG_APP, "[NAPI] startServer: %{public}s", path);
    // 确保 socket 父目录存在 (WINEPREFIX=.wine/)
    {
        std::string sockDir = path;
        auto pos = sockDir.find_last_of('/');
        if (pos != std::string::npos) {
            sockDir = sockDir.substr(0, pos);
            mkdir(sockDir.c_str(), 0755);
        }
    }
    gSockPath = path;
    bool ok = WaylandServer::GetInstance()->Start(path);
    OH_LOG_INFO(LOG_APP, "[NAPI] startServer result: %{public}s", ok ? "OK" : "FAIL");
    // 确认 socket 文件存在
    if (ok) {
        struct stat st;
        int sr = stat(path, &st);
        OH_LOG_INFO(LOG_APP, "[NAPI] wayland socket stat=%{public}d (errno=%{public}d)",
                    sr, sr == 0 ? 0 : errno);
    }

    napi_value r;
    napi_get_boolean(env, ok, &r);
    return r;
}

static napi_value BooleanResult(napi_env env, bool value) {
    napi_value result;
    napi_get_boolean(env, value, &result);
    return result;
}

static bool SetHostGraphicsEnv(const char* key, std::string_view value) {
    // std::string_view does not guarantee a trailing NUL. Profiles currently
    // come from literals, but copying here keeps this boundary correct if a
    // generated or sliced profile is introduced later.
    const std::string stableValue(value);
    if (setenv(key, stableValue.c_str(), 1) == 0) return true;
    OH_LOG_ERROR(LOG_APP,
                 "[NAPI] graphics environment apply failed key=%{public}s errno=%{public}d",
                 key, errno);
    return false;
}

static bool ApplyHostGraphicsProfile(const winehua::HostGraphicsProfile& profile) {
    bool applied = true;
    applied &= SetHostGraphicsEnv("WINEHUA_GRAPHICS_PROFILE", profile.name);
    applied &= SetHostGraphicsEnv("VKR_WINEHUA_SHADOW_FROM_HOST", profile.shadowMode);
    applied &= SetHostGraphicsEnv("VKR_WINEHUA_SHADOW_TRACE", profile.shadowSelector);
    applied &= SetHostGraphicsEnv("VKR_WINEHUA_SHADOW_MERGE_RANGES",
                                  profile.mergeShadowRanges ? "1" : "0");
    applied &= SetHostGraphicsEnv("VKR_WINEHUA_GPU_UPLOAD_WAIT",
                                  profile.waitGpuUpload ? "1" : "0");
    applied &= SetHostGraphicsEnv("VKR_WINEHUA_DESCRIPTOR_UPDATE_SERIALIZE",
                                  profile.serializeDescriptorUpdates ? "1" : "0");
    applied &= SetHostGraphicsEnv("VN_WINEHUA_DEFER_SHMEM_UNREF",
                                  profile.deferSharedMemoryUnref ? "1" : "0");
    /* Keep the App-side control plane separate from renderer environment.
     * Phone hosts run in this process, so virgl_child's derived renderer
     * settings must not change the profile observed by a later EnsureStarted. */
    applied &= SetHostGraphicsEnv("WINEHUA_VIRGL_HOST_SHADOW_MODE",
                                  profile.shadowMode);
    applied &= SetHostGraphicsEnv("WINEHUA_VIRGL_HOST_SHADOW_SELECTOR",
                                  profile.shadowSelector);
    applied &= SetHostGraphicsEnv("WINEHUA_VIRGL_HOST_SHADOW_MERGE_RANGES",
                                  profile.mergeShadowRanges ? "1" : "0");
    applied &= SetHostGraphicsEnv("WINEHUA_VIRGL_HOST_GPU_UPLOAD_WAIT",
                                  profile.waitGpuUpload ? "1" : "0");
    applied &= SetHostGraphicsEnv("WINEHUA_VIRGL_HOST_DESCRIPTOR_UPDATE_SERIALIZE",
                                  profile.serializeDescriptorUpdates ? "1" : "0");
    applied &= SetHostGraphicsEnv("WINEHUA_VIRGL_HOST_PERF_SUMMARY",
                                  profile.perfSummary ? "1" : "0");

    if (!applied) return false;

    const char* gpuUpload = profile.gpuUpload == winehua::GpuUploadPolicy::Disabled
        ? "0" : (profile.gpuUpload == winehua::GpuUploadPolicy::Cpu ? "cpu" : "auto");
    OH_LOG_INFO(LOG_APP,
                "[NAPI] graphics profile=%{public}s mode=%{public}s "
                "selector=%{public}s perf_summary=%{public}s "
                "gpu_upload=%{public}s upload_wait=%{public}s "
                "descriptor_serialize=%{public}s defer_shmem_unref=%{public}s",
                profile.name.data(), profile.shadowMode.data(),
                profile.shadowSelector.data(), profile.perfSummary ? "1" : "0",
                gpuUpload, profile.waitGpuUpload ? "1" : "0",
                profile.serializeDescriptorUpdates ? "1" : "0",
                profile.deferSharedMemoryUnref ? "1" : "0");
    return true;
}

static napi_value SetHostGraphicsExperimentForLab(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {};
    if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok ||
        argc < 2) {
        return BooleanResult(env, false);
    }
    char experimentId[96] = {};
    char backendName[64] = {};
    if (napi_get_value_string_utf8(env, args[0], experimentId,
                                   sizeof(experimentId), nullptr) != napi_ok ||
        napi_get_value_string_utf8(env, args[1], backendName,
                                   sizeof(backendName), nullptr) != napi_ok) {
        return BooleanResult(env, false);
    }

    winehua::ProductGraphicsPolicy experiment;
    const winehua::D3dBackendKind backend =
        winehua::ParseD3dBackend(backendName);
    if (!winehua::ResolveLabGraphicsExperiment(
            experimentId, backend, &experiment)) {
        OH_LOG_ERROR(LOG_APP,
                     "[NAPI] invalid graphics LAB experiment=%{public}s "
                     "backend=%{public}s",
                     experimentId, backendName);
        return BooleanResult(env, false);
    }
    return BooleanResult(env, ApplyHostGraphicsProfile(experiment.host));
}

static napi_value SetHostGraphicsBackend(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    char backendName[64] = {};
    if (argc >= 1)
        napi_get_value_string_utf8(env, args[0], backendName, sizeof(backendName), nullptr);

    const winehua::D3dBackendKind backend = winehua::ParseD3dBackend(backendName);
    winehua::ProductGraphicsPolicy policy;
    if (!winehua::ResolveProductGraphicsPolicy(backend, &policy)) {
        OH_LOG_ERROR(LOG_APP, "[NAPI] unknown graphics backend=%{public}s", backendName);
        return BooleanResult(env, false);
    }
    OH_LOG_INFO(LOG_APP,
                "[NAPI] graphics backend=%{public}s route=%{public}s",
                backendName, policy.route.data());
    return BooleanResult(env, ApplyHostGraphicsProfile(policy.host));
}

static napi_value NullResult(napi_env env) {
    napi_value result;
    napi_get_null(env, &result);
    return result;
}

static napi_value ResolveGuestGraphicsEnvironmentForLab(
    napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {};
    if (napi_get_cb_info(env, info, &argc, args, nullptr, nullptr) != napi_ok ||
        argc < 2) {
        return NullResult(env);
    }

    char profileName[96] = {};
    char backendName[64] = {};
    if (napi_get_value_string_utf8(env, args[0], profileName,
                                   sizeof(profileName), nullptr) != napi_ok ||
        napi_get_value_string_utf8(env, args[1], backendName,
                                   sizeof(backendName), nullptr) != napi_ok) {
        return NullResult(env);
    }

    std::vector<std::string> environment;
    const winehua::D3dBackendKind backend = winehua::ParseD3dBackend(backendName);
    if (!winehua::BuildLabGuestGraphicsEnvironment(
            profileName, backend, &environment)) {
        OH_LOG_ERROR(LOG_APP,
                     "[NAPI] Guest LAB environment resolve failed "
                     "profile=%{public}s backend=%{public}s",
                     profileName, backendName);
        return NullResult(env);
    }

    napi_value result;
    if (napi_create_array_with_length(env, environment.size(), &result) != napi_ok)
        return NullResult(env);
    for (size_t index = 0; index < environment.size(); ++index) {
        napi_value line;
        if (napi_create_string_utf8(env, environment[index].c_str(),
                                    NAPI_AUTO_LENGTH, &line) != napi_ok ||
            napi_set_element(env, result, static_cast<uint32_t>(index), line) != napi_ok) {
            return NullResult(env);
        }
    }
    return result;
}

static napi_value LaunchClient(napi_env env, napi_callback_info info) {
    size_t argc = 9;
    napi_value args[9] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    auto* p = new LaunchParams();

    char buf[2048] = {};
    napi_get_value_string_utf8(env, args[0], buf, sizeof(buf), nullptr);
    p->exePath = buf;
    napi_get_value_string_utf8(env, args[2], buf, sizeof(buf), nullptr);
    p->sockPath = buf;
    napi_get_value_string_utf8(env, args[3], buf, sizeof(buf), nullptr);
    p->libPath = buf;
    if (argc >= 5) {
        napi_get_value_string_utf8(env, args[4], buf, sizeof(buf), nullptr);
        p->homeDir = buf;
    }
    if (argc >= 6) napi_get_value_bool(env, args[5], &p->automationMode);
    p->prefixDir = WINE_PREFIX;
    if (argc >= 7) {
        char prefixMode[32] = {};
        napi_get_value_string_utf8(env, args[6], prefixMode, sizeof(prefixMode), nullptr);
        if (!strcmp(prefixMode, "clean")) p->prefixDir = WINE_SMOKE_PREFIX;
    }
    if (argc >= 8) {
        char d3dBackend[64] = {};
        napi_get_value_string_utf8(env, args[7], d3dBackend, sizeof(d3dBackend), nullptr);
        const winehua::D3dBackendKind backend =
            winehua::ParseD3dBackend(d3dBackend);
        if (backend == winehua::D3dBackendKind::WineD3d ||
            winehua::IsDxvkBackend(backend)) {
            p->d3dBackend = d3dBackend;
        } else {
            OH_LOG_ERROR(LOG_APP,
                         "[Launch] rejected unsupported d3d backend=%{public}s",
                         d3dBackend);
            delete p;
            napi_value result;
            napi_create_int32(env, -EINVAL, &result);
            return result;
        }
    }
    if (argc >= 9) {
        char compatEnv[2048] = {};
        napi_status compatStatus =
            napi_get_value_string_utf8(env, args[8], compatEnv, sizeof(compatEnv), nullptr);
        if (compatStatus != napi_ok) {
            OH_LOG_WARN(LOG_APP, "[Launch] compatEnvStr arg is not a string, ignored");
        } else {
            p->compatEnvStr = compatEnv;
        }
    }
    // 向后兼容: 旧调用未传 homeDir 时使用默认路径
    if (p->homeDir.empty()) {
        p->homeDir = "/storage/Users/currentUser/Download";
    }

    OH_LOG_INFO(LOG_APP,
                "[Launch] exe=%{public}s sock=%{public}s lib=%{public}s home=%{public}s prefix=%{public}s automation=%{public}s (async)",
                p->exePath.c_str(), p->sockPath.c_str(), p->libPath.c_str(), p->homeDir.c_str(),
                p->prefixDir.c_str(), p->automationMode ? "true" : "false");
    OH_LOG_INFO(LOG_APP, "[Launch] desktop D3D backend=%{public}s compat=%{public}s",
                p->d3dBackend.c_str(), p->compatEnvStr.empty() ? "baseline" : "preset");

    // 保证可执行
    if (access(p->exePath.c_str(), X_OK) != 0) chmod(p->exePath.c_str(), 0755);

    // 提取 sockDir, sockName, winehuaBin
    auto pos = p->sockPath.find_last_of('/');
    p->sockDir = (pos == std::string::npos) ? "/tmp" : p->sockPath.substr(0, pos);
    p->sockName = (pos == std::string::npos) ? p->sockPath : p->sockPath.substr(pos + 1);
    pos = p->exePath.find_last_of('/');
    p->winehuaBin = (pos != std::string::npos) ? p->exePath.substr(0, pos) : p->exePath;

    signal(SIGCHLD, sigchld_handler);

    // 启动后台线程: wineserver -> wineboot --init
    std::thread(LaunchThreadFunc, p).detach();

    OH_LOG_INFO(LOG_APP, "[Launch] background thread started, returning to JS");

    napi_value r;
    napi_create_int32(env, 0, &r);
    return r;
}

// -- NAPI: checkWinePrefix -- 检测 .wine 是否已完整初始化 --
static napi_value CheckWinePrefix(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string prefix = WINE_PREFIX;
    if (argc >= 1) {
        char mode[32] = {};
        napi_get_value_string_utf8(env, args[0], mode, sizeof(mode), nullptr);
        if (!strcmp(mode, "clean")) prefix = WINE_SMOKE_PREFIX;
    }
    const std::string initMarker = prefix + "/.winehua-init-in-progress";
    bool ok = IsWinePrefixInitialized(prefix)
        && access(initMarker.c_str(), F_OK) != 0;
    OH_LOG_INFO(LOG_APP, "[Wine] checkWinePrefix prefix=%{public}s initialized=%{public}s",
                prefix.c_str(), ok ? "yes" : "no");
    napi_value r;
    napi_get_boolean(env, ok, &r);
    return r;
}

// -- NAPI: resetWinePrefix -- 一键清空受管 prefix 目录
static bool RmDir(const char* path) {
    DIR* d = opendir(path);
    if (!d) return errno == ENOENT;
    bool ok = true;
    dirent* e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        std::string full = std::string(path) + "/" + e->d_name;
        struct stat st;
        if (lstat(full.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
                if (!RmDir(full.c_str())) ok = false;
            } else if (unlink(full.c_str()) != 0) {
                ok = false;
                OH_LOG_ERROR(LOG_APP, "[NAPI] unlink %{public}s failed: %{public}s",
                             full.c_str(), strerror(errno));
            }
        } else {
            ok = false;
            OH_LOG_ERROR(LOG_APP, "[NAPI] lstat %{public}s failed: %{public}s",
                         full.c_str(), strerror(errno));
        }
    }
    closedir(d);
    if (rmdir(path) != 0 && errno != ENOENT) {
        ok = false;
        OH_LOG_ERROR(LOG_APP, "[NAPI] rmdir %{public}s failed: %{public}s",
                     path, strerror(errno));
    }
    return ok;
}

static napi_value ResetWinePrefix(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    const char* prefix = WINE_PREFIX;
    if (argc >= 1) {
        char mode[32] = {};
        napi_get_value_string_utf8(env, args[0], mode, sizeof(mode), nullptr);
        if (!strcmp(mode, "clean")) prefix = WINE_SMOKE_PREFIX;
    }
    OH_LOG_INFO(LOG_APP, "[NAPI] resetWinePrefix called prefix=%{public}s", prefix);
    KillAllProcesses();
    bool ok = RmDir(prefix);
    if (mkdir(prefix, 0755) != 0 && errno != EEXIST) {
        ok = false;
        OH_LOG_ERROR(LOG_APP, "[NAPI] mkdir %{public}s failed: %{public}s",
                     prefix, strerror(errno));
    }
    OH_LOG_INFO(LOG_APP, "[NAPI] resetWinePrefix: %{public}s %{public}s",
                prefix, ok ? "cleared and recreated" : "reset failed");
    napi_value result;
    napi_get_boolean(env, ok, &result);
    return result;
}

static napi_value RunHostVulkanProbe(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint64_t surfaceId = 0;
    bool lossless = false;
    char runId[128] = {};
    if (argc < 2 ||
        napi_get_value_bigint_uint64(env, args[0], &surfaceId, &lossless) != napi_ok || !lossless ||
        napi_get_value_string_utf8(env, args[1], runId, sizeof(runId), nullptr) != napi_ok) {
        napi_value result;
        napi_get_boolean(env, false, &result);
        return result;
    }
    bool started = StartHostVulkanProbe(surfaceId, runId);
    OH_LOG_INFO(LOG_APP, "[HostVulkan] start surface=%{public}llu run=%{public}s result=%{public}s",
                static_cast<unsigned long long>(surfaceId), runId, started ? "true" : "false");
    napi_value result;
    napi_get_boolean(env, started, &result);
    return result;
}

static napi_value StopHostVulkanProbeNapi(napi_env env, napi_callback_info) {
    StopHostVulkanProbe();
    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

// 返回 host Vulkan 物理设备 GPU 名称 (如 "Mali-G920"), 供 ArkTS 按 GPU 能力
// 控制 DXVK2.6 选项 (马良 920 以下不支持)。失败返回空串。
static napi_value GetHostGpuNameNapi(napi_env env, napi_callback_info) {
    const std::string name = ProbeGpuDeviceName();
    napi_value result;
    napi_create_string_utf8(env, name.c_str(), NAPI_AUTO_LENGTH, &result);
    OH_LOG_INFO(LOG_APP, "[HostVulkan] gpuName=%{public}s", name.c_str());
    return result;
}


// -- NAPI: stopClient — 杀掉所有 Wine 进程 --
static napi_value StopClient(napi_env, napi_callback_info) {
    KillAllProcesses();
    // 会话终结统一收口 (与桌面退出同路径): 杀进程后进程级一次性状态全部
    // 复位, 下次引擎启动从冷启动基线开始 (ResetSessionState 含 firstFrame/
    // move grab/输入状态; StopAll 走 WaylandServer::Stop 全量重建, 无需处理)
    WaylandServer::GetInstance()->ResetSessionState();
    winehua::GraphicsBroker::GetInstance().Stop();
    return nullptr;
}

// -- NAPI: stopAll — 杀掉所有 Wine 进程 + 停 Wayland server --
static napi_value StopAll(napi_env, napi_callback_info) {
    KillAllProcesses();
    winehua::GraphicsBroker::GetInstance().Stop();
    WaylandServer::GetInstance()->Stop();
    return nullptr;
}

// -- Toplevel 回调 -> ArkTS --
static napi_threadsafe_function gToplevelTsfn = nullptr;

struct ToplevelEvent {
    uint32_t id;
    std::string event;
    std::string data;
};

static void CallJsToplevel(napi_env env, napi_value cb, void*, void* raw) {
    auto* ev = static_cast<ToplevelEvent*>(raw);
    if (env && cb && ev) {
        OH_LOG_INFO(LOG_APP, "[MW-TSCB] calling JS toplevel cb: id=%{public}u event=%{public}s data=%{public}s",
                    ev->id, ev->event.c_str(), ev->data.c_str());
        napi_value undef, args[3];
        napi_get_undefined(env, &undef);
        napi_create_uint32(env, ev->id, &args[0]);
        napi_create_string_utf8(env, ev->event.c_str(), NAPI_AUTO_LENGTH, &args[1]);
        napi_create_string_utf8(env, ev->data.c_str(), NAPI_AUTO_LENGTH, &args[2]);
        napi_call_function(env, undef, cb, 3, args, nullptr);
    }
    delete ev;
}

// -- NAPI: setToplevelCallback --
static napi_value SetToplevelCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (gToplevelTsfn) {
        napi_release_threadsafe_function(gToplevelTsfn, napi_tsfn_release);
        gToplevelTsfn = nullptr;
    }

    napi_value name;
    napi_create_string_utf8(env, "WLToplevel", NAPI_AUTO_LENGTH, &name);
    napi_create_threadsafe_function(env, args[0], nullptr, name,
                                     0, 1, nullptr, nullptr, nullptr, CallJsToplevel, &gToplevelTsfn);

    WaylandServer::GetInstance()->SetToplevelCallback([](uint32_t id, const char* event, const char* data) {
        if (gToplevelTsfn) {
            OH_LOG_INFO(LOG_APP, "[MW-TSCB] enqueue toplevel cb: id=%{public}u event=%{public}s", id, event);
            auto* ev = new ToplevelEvent{id, event ? event : "", data ? data : "{}"};
            napi_call_threadsafe_function(gToplevelTsfn, ev, napi_tsfn_blocking);
        } else {
            OH_LOG_WARN(LOG_APP, "[MW-TSCB] toplevel cb dropped (tsfn not ready): id=%{public}u event=%{public}s",
                        id, event);
        }
    });

    return nullptr;
}

// -- NAPI: getCurrentToplevelId -- (WineWindow.aboutToAppear 同步读取, 无竞态)
static napi_value GetCurrentToplevelId(napi_env env, napi_callback_info info) {
    uint32_t id = PluginManager::GetInstance()->DequeuePendingToplevel();
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] getCurrentToplevelId = %{public}u", id);
    napi_value r;
    napi_create_uint32(env, id, &r);
    return r;
}

// -- NAPI: setPendingToplevel -- (WineWindowAbility 在 loadContent 前调用)
static napi_value SetPendingToplevel(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    napi_get_value_uint32(env, args[0], &id);
    PluginManager::GetInstance()->SetPendingToplevel(id);
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] setPendingToplevel id=%{public}u", id);
    return nullptr;
}

// -- NAPI: destroyToplevel -- (ArkTS 关闭子窗口后调用)
static napi_value DestroyToplevel(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    napi_get_value_uint32(env, args[0], &id);
    PluginManager::GetInstance()->DestroyToplevel(id);
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] destroyToplevel id=%{public}u", id);
    return nullptr;
}

// -- NAPI: sendToplevelClose -- (通知 Wine 关闭窗口)
static napi_value SendToplevelClose(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    napi_get_value_uint32(env, args[0], &id);
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] sendToplevelClose id=%{public}u", id);
    WaylandServer::GetInstance()->SendToplevelClose(id);
    return nullptr;
}

// -- NAPI: createRenderer -- (XComponentController.onSurfaceCreated 调用)
static napi_value CreateRenderer(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) {
        OH_LOG_ERROR(LOG_APP, "[MW-NAPI] createRenderer: need 2 args (toplevelId, surfaceId)");
        return nullptr;
    }
    uint32_t tid = 0;
    napi_get_value_uint32(env, args[0], &tid);
    int64_t surfaceId = 0;
    bool lossless = true;
    napi_status s = napi_get_value_bigint_int64(env, args[1], &surfaceId, &lossless);
    if (s != napi_ok) {
        OH_LOG_ERROR(LOG_APP, "[MW-NAPI] createRenderer: BIGINT parse failed status=%{public}d", s);
        return nullptr;
    }
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] createRenderer tl=%{public}u surfaceId=%{public}ld", tid, surfaceId);
    PluginManager::GetInstance()->CreateRenderer(tid, surfaceId);
    return nullptr;
}

// -- NAPI: resizeRenderer -- (XComponentController.onSurfaceChanged 调用)
static napi_value ResizeRenderer(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) {
        OH_LOG_ERROR(LOG_APP, "[MW-NAPI] resizeRenderer: need 3 args (toplevelId, w, h)");
        return nullptr;
    }
    uint32_t tid = 0;
    napi_get_value_uint32(env, args[0], &tid);
    int32_t w = 0, h = 0;
    napi_get_value_int32(env, args[1], &w);
    napi_get_value_int32(env, args[2], &h);
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] resizeRenderer tl=%{public}u %{public}dx%{public}d", tid, w, h);
    PluginManager::GetInstance()->ResizeRenderer(tid, w, h);
    return nullptr;
}

// -- NAPI: refreshRenderer -- (页面返回后保留 NativeWindow 的安全重绘)
static napi_value RefreshRenderer(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) {
        OH_LOG_ERROR(LOG_APP, "[MW-NAPI] refreshRenderer: need toplevelId");
        return nullptr;
    }
    uint32_t tid = 0;
    napi_get_value_uint32(env, args[0], &tid);
    PluginManager::GetInstance()->RefreshRenderer(tid);
    return nullptr;
}

// -- NAPI: destroyRenderer -- (XComponentController.onSurfaceDestroyed 调用)
static napi_value DestroyRenderer(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t tid = 0;
    if (argc >= 1) {
        napi_get_value_uint32(env, args[0], &tid);
    }
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] destroyRenderer tl=%{public}u", tid);
    PluginManager::GetInstance()->DestroyToplevel(tid);
    return nullptr;
}

// -- NAPI: setOutputSize --
static napi_value SetOutputSize(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return nullptr;
    int32_t w, h;
    napi_get_value_int32(env, args[0], &w);
    napi_get_value_int32(env, args[1], &h);
    WaylandServer::GetInstance()->SetOutputSize(w, h);
    return nullptr;
}

// 已无效: C++ 坐标换算不使用 display scale (letterbox 由 renderer viewport 推导,
// globalDisplayScale_ 只写不读已删除)。保留导出仅为兼容 ArkTS 侧调用, 收到直接忽略。
static napi_value SetDisplayScale(napi_env env, napi_callback_info info) {
    (void)env;
    (void)info;
    return nullptr;
}

static napi_value SetDesktopMode(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc >= 1) {
        bool on;
        napi_get_value_bool(env, args[0], &on);
        WaylandServer::GetInstance()->SetDesktopMode(on);
        OH_LOG_INFO(LOG_APP, "[MW-NAPI] setDesktopMode = %{public}s", on ? "true" : "false");
    }
    return nullptr;
}

static napi_value SetPhoneMode(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc >= 1) {
        bool on;
        napi_get_value_bool(env, args[0], &on);
        PhoneAdapter_SetPhoneMode(on);
        OH_LOG_INFO(LOG_APP, "[MW-NAPI] setPhoneMode = %{public}s", on ? "true" : "false");
    }
    return nullptr;
}

// -- NAPI: Wayland text-input 桥 (宿主输入法 -> Wine) --
static napi_value WineTextInputPreedit(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    char text[4096] = {};
    if (argc >= 1 && args[0] != nullptr) {
        size_t len = 0;
        napi_get_value_string_utf8(env, args[0], text, sizeof(text), &len);
    }
    // 预上屏光标放在 UTF-8 末尾 (字节偏移), 避免把 JS UTF-16 下标误当字节数。
    int32_t end = static_cast<int32_t>(strlen(text));
    bool delivered = TextInputManager::GetInstance()->SendPreedit(text, 0, end);
    napi_value result;
    napi_get_boolean(env, delivered, &result);
    return result;
}

static napi_value WineTextInputCommit(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    char text[4096] = {};
    if (argc >= 1 && args[0] != nullptr) {
        size_t len = 0;
        napi_get_value_string_utf8(env, args[0], text, sizeof(text), &len);
    }
    bool delivered = TextInputManager::GetInstance()->SendCommit(text);
    napi_value result;
    napi_get_boolean(env, delivered, &result);
    return result;
}

static napi_value WineTextInputEnabled(napi_env env, napi_callback_info) {
    napi_value result;
    napi_get_boolean(env, TextInputManager::GetInstance()->IsEnabled(), &result);
    return result;
}

static napi_value WineTextInputSetArmed(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool armed = false;
    if (argc >= 1 && args[0] != nullptr) napi_get_value_bool(env, args[0], &armed);
    TextInputManager::GetInstance()->SetArmed(armed);
    return nullptr;
}

// -- NAPI: setImeCallback (激活/失活回调, 对齐上游 54d188d) --
// Wine 文本框聚焦 (enable + 非零光标矩形) → active=1 回调 ArkTS 弹系统软键盘;
// 失焦/leave → active=0 收起。
// 跨线程安全: 用 threadsafe function (与 gStateTsfn/gToplevelTsfn 同模式),
// Wayland 线程触发回调经 TSFN 投递到 JS 线程执行 — 直接 napi_call_function
// 跨线程会崩溃。
struct ImeEvent {
    int active;
    int x, y, w, h;
};
static napi_threadsafe_function gImeTsfn = nullptr;

static void CallJsIme(napi_env env, napi_value cb, void*, void* data) {
    ImeEvent* ev = static_cast<ImeEvent*>(data);
    if (env && cb && ev) {
        napi_value undef, argv[5];
        napi_get_undefined(env, &undef);
        napi_create_int32(env, ev->active, &argv[0]);
        napi_create_int32(env, ev->x, &argv[1]);
        napi_create_int32(env, ev->y, &argv[2]);
        napi_create_int32(env, ev->w, &argv[3]);
        napi_create_int32(env, ev->h, &argv[4]);
        napi_call_function(env, undef, cb, 5, argv, nullptr);
    }
    delete ev;
}

static napi_value SetImeCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (gImeTsfn) {
        napi_release_threadsafe_function(gImeTsfn, napi_tsfn_release);
        gImeTsfn = nullptr;
    }

    napi_value name;
    napi_create_string_utf8(env, "WL_Ime", NAPI_AUTO_LENGTH, &name);
    napi_create_threadsafe_function(env, args[0], nullptr, name,
                                    0, 1, nullptr, nullptr, nullptr, CallJsIme, &gImeTsfn);

    TextInputManager::GetInstance()->SetActivateCallback(
        [](bool active, int x, int y, int w, int h) {
            if (gImeTsfn) {
                auto* ev = new ImeEvent{active ? 1 : 0, x, y, w, h};
                napi_call_threadsafe_function(gImeTsfn, ev, napi_tsfn_blocking);
            } else {
                OH_LOG_WARN(LOG_APP, "[WL_NAPI] ime cb dropped (tsfn not ready) active=%{public}d", active);
            }
        });
    return nullptr;
}

// -- NAPI: imeBackspace (软键盘退格 → Wine KEY_BACKSPACE 键盘注入;
//    Wine 的 delete_surrounding_text 是空实现, 退格走现有 key 注入链路) --
static napi_value ImeBackspace(napi_env env, napi_callback_info info) {
    (void)env;
    (void)info;
    constexpr uint32_t KEY_BACKSPACE = 14;
    auto* im = InputManager::GetInstance();
    im->InjectKeyboardKey(KEY_BACKSPACE, WL_KEYBOARD_KEY_STATE_PRESSED);
    im->InjectKeyboardKey(KEY_BACKSPACE, WL_KEYBOARD_KEY_STATE_RELEASED);
    return nullptr;
}

// -- NAPI: 应用日志文件 sink --
static napi_value InitAppLog(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    char path[1024] = {};
    if (argc >= 1 && args[0] != nullptr) {
        size_t len = 0;
        napi_get_value_string_utf8(env, args[0], path, sizeof(path), &len);
    }
    WineHuaLogInit(path);
    return nullptr;
}

static napi_value ClearNativeLog(napi_env env, napi_callback_info info) {
    WineHuaLogClear();
    return nullptr;
}

static napi_value GetDesktopRootId(napi_env env, napi_callback_info) {
    uint32_t id = WaylandServer::GetInstance()->GetDesktopRootToplevelId();
    napi_value r;
    napi_create_uint32(env, id, &r);
    return r;
}

static napi_value GetDisplayFps(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    if (argc >= 1) napi_get_value_uint32(env, args[0], &id);
    const double fps = static_cast<double>(DisplayFpsRegistry::Instance().Get(id));
    napi_value result;
    napi_create_double(env, fps, &result);
    return result;
}

// -- NAPI: takeWindowMask -- (ARGB 异型窗口剪影掩码, ArkTS 轮询拉取)
static napi_value TakeWindowMask(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    uint32_t id = 0;
    if (argc >= 1) {
        napi_get_value_uint32(env, args[0], &id);
    }
    int w = 0, h = 0;
    std::vector<uint8_t> bits;
    if (!WaylandServer::GetInstance()->TakeWindowMask(id, w, h, bits)) {
        return nullptr;
    }
    napi_value result, wv, hv, buf;
    napi_create_object(env, &result);
    napi_create_int32(env, w, &wv);
    napi_create_int32(env, h, &hv);
    void* data = nullptr;
    napi_create_arraybuffer(env, bits.size(), &data, &buf);
    if (data && !bits.empty()) {
        memcpy(data, bits.data(), bits.size());
    }
    napi_value wKey, hKey, bufKey;
    napi_create_string_utf8(env, "w", 1, &wKey);
    napi_create_string_utf8(env, "h", 1, &hKey);
    napi_create_string_utf8(env, "buffer", 6, &bufKey);
    napi_set_property(env, result, wKey, wv);
    napi_set_property(env, result, hKey, hv);
    napi_set_property(env, result, bufKey, buf);
    return result;
}

// -- Input forwarding NAPI (unified InputManager path) --
static napi_value SendPointerEvent(napi_env env, napi_callback_info info) {
    size_t argc = 8;
    napi_value args[8];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 5) return nullptr;
    uint32_t tl; int32_t action; double px, py; int32_t button;
    napi_get_value_uint32(env, args[0], &tl);
    napi_get_value_int32(env, args[1], &action);
    napi_get_value_double(env, args[2], &px);
    napi_get_value_double(env, args[3], &py);
    napi_get_value_int32(env, args[4], &button);
    // 可选: MouseEvent.rawDeltaX/Y (API15+, 仅 Move 传; 缺省 0 = 无 raw 数据,
    // InputManager 回退绝对差分); args[7] fromMouse: onMouse 物理鼠标通道
    // 传 true (触屏 onTouch 路径不传) — 相对模式 PRESS 是否跳过 enter 重定位
    double rawDx = 0, rawDy = 0;
    if (argc >= 7) {
        napi_get_value_double(env, args[5], &rawDx);
        napi_get_value_double(env, args[6], &rawDy);
    }
    bool fromMouse = false;
    if (argc >= 8) {
        napi_get_value_bool(env, args[7], &fromMouse);
    }
    // 跳过 MOVE (=3, 高频), 只记录 button/enter/leave。
    // 曾误写 action!=1: 跳过的是 PRESS (日志只见 Release 不见 Press),
    // 且 MOVE 全量刷屏 — ArkTS MouseAction: Press=1 Release=2 Move=3
    // (与 input_manager.cpp ACT_* 注释一致)
    if (action != 3) {
        OH_LOG_INFO(LOG_APP, "[PIPE] ptr tl=%{public}u a=%{public}d btn=0x%{public}x "
                    "px=(%{public}.0f,%{public}.0f) raw=(%{public}.1f,%{public}.1f) fromMouse=%{public}d",
                    tl, action, button, px, py, rawDx, rawDy, fromMouse ? 1 : 0);
    }
    InputManager::GetInstance()->SendPointerEvent(tl, action, px, py, button, rawDx, rawDy, fromMouse);
    return nullptr;
}

// -- NAPI: registerHostWindow -- (ets 各 Ability 注册主窗口 id, 供
// OH_WindowManager_LockCursor 锁定光标用 — 仅获焦窗口能锁, 逐个尝试)
static napi_value RegisterHostWindow(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;
    int32_t windowId = 0;
    napi_get_value_int32(env, args[0], &windowId);
    PointerExtras::RegisterHostWindow(windowId);
    return nullptr;
}

// -- NAPI: setPointerLockCallback -- (锁定状态 → ets 隐藏/恢复系统光标)
// gPointerLockTsfn 在主线程注册/替换、wl 线程回调读取 (20260822 review:
// 裸指针跨线程读写 + 先 release 后 create 是 data race — 窗口重建 (launcher
// resume 再次 init) 恰逢相对模式切换时, wl 线程可能读到已关闭/被交换的
// 句柄 → UAF 或锁状态通知丢失)。修复: atomic 化; 替换时先原子摘除旧句柄
// 再 release(释放后 wl 线程不可能再读到旧值); 读到 closing 中旧句柄时
// napi_call_threadsafe_function 返回错误 — 忽略, 新句柄接管后续事件。
static std::atomic<napi_threadsafe_function> gPointerLockTsfn{nullptr};
static void CallJsPointerLock(napi_env env, napi_value cb, void*, void* data) {
    if (env && cb) {
        // data 打包: bit0 = locked, 高位 = 触发相对模式的约束 surface 的 toplevelId
        // (解锁时该值为 0)。送到 ets 供"桌面 shell 自身藏光标 vs 游戏真相对模式"
        // 的门禁区分。
        uintptr_t packed = reinterpret_cast<uintptr_t>(data);
        const bool locked = (packed & 1u) != 0;
        const uint32_t toplevelId = static_cast<uint32_t>(packed >> 1);
        napi_value undef, argLocked, argTl;
        napi_get_undefined(env, &undef);
        napi_get_boolean(env, locked, &argLocked);
        napi_create_uint32(env, toplevelId, &argTl);
        napi_value args[2] = {argLocked, argTl};
        napi_call_function(env, undef, cb, 2, args, nullptr);
    }
}
static napi_value SetPointerLockCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;
    if (napi_threadsafe_function old = gPointerLockTsfn.exchange(nullptr)) {
        napi_release_threadsafe_function(old, napi_tsfn_release);
    }
    napi_value name;
    napi_create_string_utf8(env, "WLPointerLock", NAPI_AUTO_LENGTH, &name);
    napi_threadsafe_function newTsfn = nullptr;
    if (napi_create_threadsafe_function(env, args[0], nullptr, name,
                                         0, 1, nullptr, nullptr, nullptr, CallJsPointerLock,
                                         &newTsfn) != napi_ok) {
        OH_LOG_ERROR(LOG_APP, "[NAPI] setPointerLockCallback: create tsfn failed");
        return nullptr;
    }
    gPointerLockTsfn.store(newTsfn);
    PointerExtras::GetInstance()->SetPointerLockCallback([](bool locked, uint32_t toplevelId) {
        if (napi_threadsafe_function tsfn = gPointerLockTsfn.load()) {
            // bit0 = locked, 高位 = toplevelId (见 CallJsPointerLock 解包)
            uintptr_t packed = static_cast<uintptr_t>(locked ? 1u : 0u) |
                               (static_cast<uintptr_t>(toplevelId) << 1);
            if (napi_call_threadsafe_function(tsfn, reinterpret_cast<void*>(packed),
                    napi_tsfn_blocking) != napi_ok) {
                // tsfn 正在关闭 (窗口重建竞态): 忽略, 新句柄接管后续事件
            }
        }
    });
    return nullptr;
}

static napi_value SendKeyEvent(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) return nullptr;
    uint32_t tl; int32_t evdevCode; bool pressed;
    napi_get_value_uint32(env, args[0], &tl);
    napi_get_value_int32(env, args[1], &evdevCode);
    napi_get_value_bool(env, args[2], &pressed);
    OH_LOG_INFO(LOG_APP, "[PIPE] key tl=%{public}u evdev=%{public}d down=%{public}s",
                tl, evdevCode, pressed ? "true" : "false");
    InputManager::GetInstance()->SendKeyEvent(tl, evdevCode, pressed);
    return nullptr;
}

static napi_value SendScrollEvent(napi_env env, napi_callback_info info) {
    size_t argc = 6;
    napi_value args[6];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 6) return nullptr;
    uint32_t tl; int32_t axis; double value; int32_t scrollStep; double px; double py;
    napi_get_value_uint32(env, args[0], &tl);
    napi_get_value_int32(env, args[1], &axis);
    napi_get_value_double(env, args[2], &value);
    napi_get_value_int32(env, args[3], &scrollStep);
    napi_get_value_double(env, args[4], &px);
    napi_get_value_double(env, args[5], &py);
    OH_LOG_INFO(LOG_APP, "[PIPE] scroll tl=%{public}u axis=%{public}s val=%{public}.1f step=%{public}d px=(%{public}.0f,%{public}.0f)",
                tl, axis == 0 ? "VERT" : "HORIZ", value, scrollStep, px, py);
    InputManager::GetInstance()->SendScrollEvent(tl, axis, value, scrollStep, px, py);
    return nullptr;
}

static napi_value NotifyToplevelResize(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) return nullptr;
    uint32_t tl; int32_t w, h;
    napi_get_value_uint32(env, args[0], &tl);
    napi_get_value_int32(env, args[1], &w);
    napi_get_value_int32(env, args[2], &h);
    OH_LOG_INFO(LOG_APP, "[NAPI] notifyToplevelResize tl=%{public}u %{public}dx%{public}d",
                tl, w, h);
    WaylandServer::GetInstance()->NotifyToplevelResize(tl, w, h);
    return nullptr;
}

// Desktop 模式: 将 toplevel 提到 Z-order 最顶层
static napi_value RaiseToplevel(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;
    uint32_t tl;
    napi_get_value_uint32(env, args[0], &tl);
    // 用户显式操作 (任务栏/窗口点击) 路径: 已 fullscreen 的目标会重新取
    // 全屏优先级号, 支撑两个全屏窗口间的主动切换
    WaylandServer::GetInstance()->RaiseToplevel(tl, true);
    return nullptr;
}

// Desktop 模式: 接收物理像素坐标 (px, py), 通过 viewport 映射为 Wine 逻辑坐标后查找
// resize 后 surface 和逻辑尺寸比例变化, 由 renderer viewport 保证映射正确
static napi_value FindToplevelAt(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return nullptr;
    int32_t px, py;  // 物理像素坐标
    napi_get_value_int32(env, args[0], &px);
    napi_get_value_int32(env, args[1], &py);

    auto* ws = WaylandServer::GetInstance();
    uint32_t rootId = ws->GetDesktopRootToplevelId();
    wl_fixed_t wx, wy;
    InputManager::GetInstance()->CoordTransform(px, py, rootId > 0 ? rootId : 1, &wx, &wy);
    int32_t lx = wl_fixed_to_int(wx);
    int32_t ly = wl_fixed_to_int(wy);

    uint32_t id = ws->FindToplevelAt(lx, ly);
    napi_value result;
    napi_create_uint32(env, id, &result);
    return result;
}

static napi_value SetToplevelVisible(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return nullptr;
    uint32_t tl; bool visible;
    napi_get_value_uint32(env, args[0], &tl);
    napi_get_value_bool(env, args[1], &visible);
    InputManager::GetInstance()->SetToplevelVisible(tl, visible);
    // 同步暂停/恢复渲染: 窗口不可见 (后台/HIDDEN) 时暂停 GPU 渲染, 避免
    // surface 不可呈现时 vsync/eglSwapBuffers 阻塞渲染线程 (卡顿根因之一);
    // 恢复可见时立即重渲染并重新武装 vsync。
    PluginManager::GetInstance()->SetRendererPaused(tl, !visible);
    if (visible) {
        // 6A: 兼容别名 NotifyWindowRestored 已删, 直调语义方法 (同实现同值)
        WaylandServer::GetInstance()->SetToplevelRestored(tl);
        PluginManager::GetInstance()->RefreshRenderer(tl);
    }
    return nullptr;
}

// -- NAPI: getProcessList — 返回运行中进程列表 --
static napi_value GetProcessList(napi_env env, napi_callback_info info) {
    auto snapshot = GetProcessListSnapshot();
    snapshot.erase(std::remove_if(snapshot.begin(), snapshot.end(),
        [](const WineProcessEntry& entry) { return !entry.running; }), snapshot.end());

    napi_value arr;
    napi_create_array_with_length(env, snapshot.size(), &arr);

    for (size_t i = 0; i < snapshot.size(); i++) {
        const auto& entry = snapshot[i];
        napi_value obj;
        napi_create_object(env, &obj);

        napi_value pidVal, nameVal, pathVal, stateVal, sessionVal;
        napi_create_int32(env, entry.pid, &pidVal);
        napi_create_string_utf8(env, entry.exeBasename.c_str(), NAPI_AUTO_LENGTH, &nameVal);
        napi_create_string_utf8(env, entry.exeFullPath.c_str(), NAPI_AUTO_LENGTH, &pathVal);
        napi_create_string_utf8(env, entry.running ? "running" : "exited",
                                NAPI_AUTO_LENGTH, &stateVal);
        napi_create_string_utf8(env, entry.sessionId.c_str(), NAPI_AUTO_LENGTH, &sessionVal);

        napi_property_descriptor props[] = {
            {"pid",   nullptr, nullptr, nullptr, nullptr, pidVal,   napi_default, nullptr},
            {"name",  nullptr, nullptr, nullptr, nullptr, nameVal,  napi_default, nullptr},
            {"path",  nullptr, nullptr, nullptr, nullptr, pathVal,  napi_default, nullptr},
            {"state", nullptr, nullptr, nullptr, nullptr, stateVal, napi_default, nullptr},
            {"sessionId", nullptr, nullptr, nullptr, nullptr, sessionVal, napi_default, nullptr},
        };
        napi_define_properties(env, obj, sizeof(props)/sizeof(props[0]), props);
        napi_set_element(env, arr, i, obj);
    }

    OH_LOG_INFO(LOG_APP, "[NAPI] getProcessList returned %{public}zu processes", snapshot.size());
    return arr;
}

// -- NAPI: killProcess — 杀掉指定进程 --
static napi_value KillProcess(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;

    int32_t pid = 0;
    napi_get_value_int32(env, args[0], &pid);
    OH_LOG_INFO(LOG_APP, "[NAPI] killProcess pid=%{public}d", pid);

    WineProcessEntry entry{};
    const bool known = pid > 0 && QueryProcessSnapshot(pid, &entry);
    KillProcessTree(pid);
    RemoveProcess(pid);
    if (known && gStateTsfn) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%d:exited", pid);
        napi_call_threadsafe_function(gStateTsfn, strdup(msg), napi_tsfn_blocking);
        if (entry.toplevelId > 0) {
            // 立即联动: 让 ArkTS 关闭该进程的窗口, 不等 Wayland 断连回调
            WaylandServer::GetInstance()->OnToplevelDestroyed(entry.toplevelId);
            WaylandServer::GetInstance()->PostToplevelEvent(entry.toplevelId, ToplevelEventType::Destroyed);
        }
    }

    napi_value r;
    napi_get_boolean(env, true, &r);
    return r;
}

static std::string GetStringArgument(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return "";
    size_t length = 0;
    napi_get_value_string_utf8(env, args[0], nullptr, 0, &length);
    std::string value(length + 1, '\0');
    napi_get_value_string_utf8(env, args[0], value.data(), value.size(), &length);
    value.resize(length);
    return value;
}

static napi_value GetWineSession(napi_env env, napi_callback_info info) {
    const std::string sessionId = GetStringArgument(env, info);
    WineProcessEntry entry{};
    if (!GetProcessBySessionId(sessionId, &entry)) {
        napi_value result;
        napi_get_null(env, &result);
        return result;
    }
    napi_value result, pidValue, sessionValue, pathValue, stateValue, toplevelValue;
    napi_create_object(env, &result);
    napi_create_int32(env, entry.pid, &pidValue);
    napi_create_string_utf8(env, entry.sessionId.c_str(), NAPI_AUTO_LENGTH, &sessionValue);
    napi_create_string_utf8(env, entry.exeFullPath.c_str(), NAPI_AUTO_LENGTH, &pathValue);
    napi_create_string_utf8(env, entry.running ? "running" : "exited", NAPI_AUTO_LENGTH,
                            &stateValue);
    napi_create_uint32(env, entry.toplevelId, &toplevelValue);
    napi_set_named_property(env, result, "pid", pidValue);
    napi_set_named_property(env, result, "sessionId", sessionValue);
    napi_set_named_property(env, result, "path", pathValue);
    napi_set_named_property(env, result, "state", stateValue);
    napi_set_named_property(env, result, "toplevelId", toplevelValue);
    return result;
}

static napi_value StopWineSession(napi_env env, napi_callback_info info) {
    const std::string sessionId = GetStringArgument(env, info);
    WineProcessEntry entry{};
    const bool found = GetProcessBySessionId(sessionId, &entry);
    if (found) {
        const uint32_t toplevelId = entry.toplevelId;
        KillProcessTree(entry.pid);
        RemoveProcess(entry.pid, -1, "session-stop");
        if (gStateTsfn) {
            char msg[64];
            snprintf(msg, sizeof(msg), "%d:exited", entry.pid);
            napi_call_threadsafe_function(gStateTsfn, strdup(msg), napi_tsfn_blocking);
        }
        if (toplevelId > 0) {
            // 立即通知 ArkTS 销毁窗口, 保证"关闭运行中的程序"与 Wine
            // 进程/窗口状态联动, 不依赖 Wayland 断连的异步时序。
            WaylandServer::GetInstance()->OnToplevelDestroyed(toplevelId);
            WaylandServer::GetInstance()->PostToplevelEvent(toplevelId, ToplevelEventType::Destroyed);
        }
    }
    napi_value result;
    napi_get_boolean(env, found, &result);
    return result;
}

static napi_value ActivateWineSession(napi_env env, napi_callback_info info) {
    const std::string sessionId = GetStringArgument(env, info);
    WineProcessEntry entry{};
    const bool found = GetProcessBySessionId(sessionId, &entry) && entry.toplevelId > 0;
    if (found) WaylandServer::GetInstance()->RaiseToplevel(entry.toplevelId);
    napi_value result;
    napi_get_boolean(env, found, &result);
    return result;
}

// -- 模块注册 --
EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    OH_LOG_INFO(LOG_APP, "[MW-NAPI]  Init called, env=%{public}p", env);

    napi_property_descriptor desc[] = {
        {"startServer",    nullptr, StartServer,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setHostGraphicsExperimentForLab", nullptr, SetHostGraphicsExperimentForLab, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setHostGraphicsBackend", nullptr, SetHostGraphicsBackend, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"resolveGuestGraphicsEnvironmentForLab", nullptr, ResolveGuestGraphicsEnvironmentForLab, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"launchClient",   nullptr, LaunchClient,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopClient",     nullptr, StopClient,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopAll",        nullptr, StopAll,        nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setStateCallback", nullptr, SetStateCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setToplevelCallback", nullptr, SetToplevelCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getCurrentToplevelId", nullptr, GetCurrentToplevelId, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setPendingToplevel", nullptr, SetPendingToplevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroyToplevel", nullptr, DestroyToplevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendToplevelClose", nullptr, SendToplevelClose, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runWineExe",     nullptr, RunWineExe,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runWineExeLegacy", nullptr, RunWineExeLegacy, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getWineSession", nullptr, GetWineSession, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopWineSession", nullptr, StopWineSession, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"activateWineSession", nullptr, ActivateWineSession, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runWineProgram", nullptr, RunWineProgram, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"extractFontZip", nullptr, ExtractFontZip, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"extractFontZipAsync", nullptr, ExtractFontZipAsync, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runGuestProgram", nullptr, RunGuestProgram, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runHostProgram", nullptr, RunHostProgram, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runHostReplay", nullptr, RunHostReplay, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"isHostReplayRunning", nullptr, IsHostReplayRunning, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"queryWineProcess", nullptr, QueryWineProcess, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"terminateWineProcess", nullptr, TerminateWineProcess, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"checkWinePrefix",nullptr, CheckWinePrefix,nullptr, nullptr, nullptr, napi_default, nullptr},
        {"resetWinePrefix",nullptr, ResetWinePrefix,nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runHostVulkanProbe", nullptr, RunHostVulkanProbe, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopHostVulkanProbe", nullptr, StopHostVulkanProbeNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getHostGpuName", nullptr, GetHostGpuNameNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getDisplayFps", nullptr, GetDisplayFps, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"readPerformanceCounters", nullptr, ReadPerformanceCounters, nullptr, nullptr, nullptr, napi_default, nullptr},
        // surfaceId 驱动的渲染器管理 (XComponentController 回调)
        {"createRenderer",  nullptr, CreateRenderer,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"resizeRenderer",  nullptr, ResizeRenderer,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"refreshRenderer", nullptr, RefreshRenderer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroyRenderer", nullptr, DestroyRenderer, nullptr, nullptr, nullptr, napi_default, nullptr},
#ifdef DEBUG_MMAP_TEST
        {"runMmapTests",  nullptr, RunMmapTests,  nullptr, nullptr, nullptr, napi_default, nullptr},
#endif
        {"setOutputSize",   nullptr, SetOutputSize,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDisplayScale",  nullptr, SetDisplayScale,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDesktopMode",   nullptr, SetDesktopMode,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setPhoneMode",     nullptr, SetPhoneMode,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"wineTextInputPreedit",    nullptr, WineTextInputPreedit,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"wineTextInputCommit",     nullptr, WineTextInputCommit,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"wineTextInputEnabled",    nullptr, WineTextInputEnabled,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"wineTextInputSetArmed",   nullptr, WineTextInputSetArmed,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setImeCallback",           nullptr, SetImeCallback,           nullptr, nullptr, nullptr, napi_default, nullptr},
        {"imeBackspace",             nullptr, ImeBackspace,             nullptr, nullptr, nullptr, napi_default, nullptr},
        {"initAppLog",       nullptr, InitAppLog,       nullptr, nullptr, nullptr, napi_default, nullptr},
        {"clearNativeLog",   nullptr, ClearNativeLog,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getDesktopRootId", nullptr, GetDesktopRootId, nullptr, nullptr, nullptr, napi_default, nullptr},
        // ArkTS input forwarding (unified InputManager path)
        {"sendPointerEvent", nullptr, SendPointerEvent, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendKeyEvent",     nullptr, SendKeyEvent,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendScrollEvent",   nullptr, SendScrollEvent,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"registerHostWindow", nullptr, RegisterHostWindow, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setPointerLockCallback", nullptr, SetPointerLockCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"notifyToplevelResize",nullptr,NotifyToplevelResize,nullptr, nullptr, nullptr, napi_default, nullptr},
        {"takeWindowMask", nullptr, TakeWindowMask, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"findToplevelAt",   nullptr, FindToplevelAt,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"raiseToplevel",    nullptr, RaiseToplevel,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setToplevelVisible", nullptr, SetToplevelVisible, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getProcessList",   nullptr, GetProcessList,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"killProcess",     nullptr, KillProcess,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"initGameController", nullptr, InitGameController, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cleanupGameController", nullptr, CleanupGameController, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"isGamepadConnected", nullptr, IsGamepadConnected, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getGamepadCount", nullptr, GetGamepadCount, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setGamepadButtonCallback", nullptr, SetGamepadButtonCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setGamepadAxisCallback", nullptr, SetGamepadAxisCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setGamepadDeviceCallback", nullptr, SetGamepadDeviceCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setGamepadRumbleCallback", nullptr, SetGamepadRumbleCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerSetEnabled", nullptr, ControllerSetEnabled, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerSetButton", nullptr, ControllerSetButton, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerSetAxis", nullptr, ControllerSetAxis, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerSetHat", nullptr, ControllerSetHat, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerResetSource", nullptr, ControllerResetSource, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerGetState", nullptr, ControllerGetState, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerGetStateText", nullptr, ControllerGetStateText, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerStartBridge", nullptr, ControllerStartBridge, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerStopBridge", nullptr, ControllerStopBridge, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerGetSocketPath", nullptr, ControllerGetSocketPath, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerSetOutputMode", nullptr, ControllerSetOutputMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"controllerGetOutputMode", nullptr, ControllerGetOutputMode, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);

    // surfaceId 架构: 不再使用 libraryname='entry', XComponent 通过
    // 自定义 Controller 回调拿到 surfaceId, 由 createRenderer/renderer 管理。
    // 不再需要保存 gEnv/gExports, 不再依赖 XComponent exports 对象。
    OH_LOG_INFO(LOG_APP, "[MW-NAPI] Init complete OK");
    return exports;
}
EXTERN_C_END

static napi_module demoModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "entry",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterEntryModule() {
    napi_module_register(&demoModule);
}
