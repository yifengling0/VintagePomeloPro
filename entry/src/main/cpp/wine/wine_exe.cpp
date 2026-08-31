#include "wine/wine_exe.h"

#include "proc/broker.h"
#include "wine/env_profiles.h"
#include "proc/spawner.h"
#include "graphics/graphics_broker.h"
#include "graphics/graphics_profile.h"
#include "compositor/wayland_server.h"
#include "wine/wine_constants.h"
#include "wine/wine_env.h"
#include "proc/wine_process.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <string>
#include <thread>
#include <vector>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

extern napi_threadsafe_function gStateTsfn;

namespace {

struct GuestProgramOptions {
    std::string executablePath;
    std::vector<std::string> argv;
    std::vector<std::string> environment;
    std::string workingDirectory;
    bool automationMode = true;
};

struct HostProgramOptions {
    std::string executablePath;
    std::vector<std::string> argv;
    std::vector<std::string> environment;
    std::string workingDirectory;
    bool automationMode = true;
};

using HostReplayMain = int (*)(int, char**);
static std::atomic<bool> gHostReplayRunning{false};

static bool HasUnsafeProtocolChar(const std::string& value)
{
    return value.find('|') != std::string::npos || value.find('\n') != std::string::npos ||
           value.find('\r') != std::string::npos;
}

static std::string ReadString(napi_env env, napi_value value)
{
    size_t length = 0;
    if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) return {};
    std::vector<char> buffer(length + 1);
    if (napi_get_value_string_utf8(env, value, buffer.data(), buffer.size(), &length) != napi_ok) return {};
    return std::string(buffer.data(), length);
}

static bool GetNamed(napi_env env, napi_value object, const char* name, napi_value* out)
{
    bool has = false;
    if (napi_has_named_property(env, object, name, &has) != napi_ok || !has) return false;
    return napi_get_named_property(env, object, name, out) == napi_ok;
}

static std::string GetString(napi_env env, napi_value object, const char* name,
                             const std::string& fallback = {})
{
    napi_value value;
    napi_valuetype type;
    if (!GetNamed(env, object, name, &value) || napi_typeof(env, value, &type) != napi_ok ||
        type != napi_string)
        return fallback;
    std::string result = ReadString(env, value);
    return result.empty() ? fallback : result;
}

static bool GetBool(napi_env env, napi_value object, const char* name, bool fallback)
{
    napi_value value;
    napi_valuetype type;
    bool result = fallback;
    if (GetNamed(env, object, name, &value) && napi_typeof(env, value, &type) == napi_ok &&
        type == napi_boolean)
        napi_get_value_bool(env, value, &result);
    return result;
}

static void ReadStringArray(napi_env env, napi_value object, const char* name,
                            std::vector<std::string>* out)
{
    napi_value array;
    bool isArray = false;
    uint32_t length = 0;
    if (!GetNamed(env, object, name, &array) || napi_is_array(env, array, &isArray) != napi_ok || !isArray ||
        napi_get_array_length(env, array, &length) != napi_ok)
        return;
    for (uint32_t i = 0; i < length; ++i)
    {
        napi_value item;
        napi_valuetype type;
        if (napi_get_element(env, array, i, &item) == napi_ok &&
            napi_typeof(env, item, &type) == napi_ok && type == napi_string)
            out->push_back(ReadString(env, item));
    }
}

static bool IsValidEnvKey(const std::string& key)
{
    if (key.empty() || !(std::isalpha(static_cast<unsigned char>(key[0])) || key[0] == '_')) return false;
    return std::all_of(key.begin() + 1, key.end(), [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
    });
}

static void ReadEnvironment(napi_env env, napi_value object, std::vector<std::string>* out)
{
    napi_value record;
    napi_valuetype type;
    if (!GetNamed(env, object, "environment", &record) ||
        napi_typeof(env, record, &type) != napi_ok || type != napi_object)
        return;

    napi_value keys;
    uint32_t length = 0;
    if (napi_get_property_names(env, record, &keys) != napi_ok ||
        napi_get_array_length(env, keys, &length) != napi_ok)
        return;
    for (uint32_t i = 0; i < length; ++i)
    {
        napi_value keyValue, value;
        napi_valuetype valueType;
        if (napi_get_element(env, keys, i, &keyValue) != napi_ok) continue;
        std::string key = ReadString(env, keyValue);
        if (!IsValidEnvKey(key) || napi_get_property(env, record, keyValue, &value) != napi_ok ||
            napi_typeof(env, value, &valueType) != napi_ok || valueType != napi_string)
            continue;
        std::string line = key + "=" + ReadString(env, value);
        if (!HasUnsafeProtocolChar(line)) out->push_back(std::move(line));
    }
}

static std::string EnvKey(const std::string& line)
{
    size_t separator = line.find('=');
    return separator == std::string::npos ? line : line.substr(0, separator);
}

static std::string FindEnvValue(const std::vector<std::string>& env,
                                const char* key)
{
    const std::string prefix = std::string(key) + "=";
    for (auto it = env.rbegin(); it != env.rend(); ++it) {
        if (it->rfind(prefix, 0) == 0) return it->substr(prefix.size());
    }
    return {};
}

static void UpsertEnv(std::vector<std::string>* env, std::string line)
{
    const std::string key = EnvKey(line);
    env->erase(std::remove_if(env->begin(), env->end(), [&](const std::string& existing) {
        return EnvKey(existing) == key;
    }), env->end());
    env->push_back(std::move(line));
}

static std::string PrefixForMode(const std::string& mode)
{
    return mode == "clean" ? WINE_SMOKE_PREFIX : WINE_PREFIX;
}

static std::string NativePathToWindows(const std::string& path, const std::string& prefix)
{
    const std::string driveRoot = prefix + "/drive_c/";
    if (path.rfind(driveRoot, 0) != 0) return path;
    std::string result = "C:\\" + path.substr(driveRoot.size());
    std::replace(result.begin(), result.end(), '/', '\\');
    return result;
}

} // namespace

pid_t SpawnViaBroker(const std::string& entryParams,
                     const std::vector<std::string>& environment)
{
    const char* brokerPath = getenv("PROCESSBROKER");
    if (!brokerPath || !brokerPath[0]) brokerPath = WINE_BROKER_SOCKET;
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    if (strlen(brokerPath) >= sizeof(address.sun_path))
        return -1;
    strcpy(address.sun_path, brokerPath);

    int brokerFd = -1;
    int connectError = 0;
    for (int attempt = 0; attempt < 10; attempt++) {
        brokerFd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (brokerFd >= 0 &&
            connect(brokerFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
            break;
        connectError = errno;
        if (brokerFd >= 0) close(brokerFd);
        brokerFd = -1;
        usleep(50000);
    }
    if (brokerFd < 0) {
        OH_LOG_ERROR(LOG_APP, "[Program] broker connect failed: %{public}s", strerror(connectError));
        return -1;
    }

    /* The broker protocol has one authoritative environment channel:
     * |__env=KEY=VALUE segments embedded in entryParams. */
    const std::string requestParams = entryParams + SerializeEnvToEntryParams(environment);
    static constexpr char header[] = "SPAWN\n";
    std::string requestTail = requestParams + "\n";
    iovec iov[2] = {
        {const_cast<char*>(header), sizeof(header) - 1},
        {const_cast<char*>(requestTail.data()), requestTail.size()},
    };
    msghdr message = {};
    message.msg_iov = iov;
    message.msg_iovlen = 2;
    if (sendmsg(brokerFd, &message, MSG_NOSIGNAL) < 0)
    {
        close(brokerFd);
        return -1;
    }

    int32_t response[2] = {-1, -1};
    ssize_t received = recv(brokerFd, response, sizeof(response), MSG_WAITALL);
    close(brokerFd);
    if (received != sizeof(response) || response[1] != 0 || response[0] <= 0) return -1;
    return response[0];
}

namespace {

static napi_value MakeProcessObject(napi_env env, const WineProcessEntry* entry, bool found)
{
    napi_value object;
    napi_create_object(env, &object);

    auto setBool = [&](const char* name, bool value) {
        napi_value item; napi_get_boolean(env, value, &item); napi_set_named_property(env, object, name, item);
    };
    auto setInt = [&](const char* name, int32_t value) {
        napi_value item; napi_create_int32(env, value, &item); napi_set_named_property(env, object, name, item);
    };
    auto setDouble = [&](const char* name, double value) {
        napi_value item; napi_create_double(env, value, &item); napi_set_named_property(env, object, name, item);
    };
    auto setString = [&](const char* name, const std::string& value) {
        napi_value item; napi_create_string_utf8(env, value.c_str(), NAPI_AUTO_LENGTH, &item);
        napi_set_named_property(env, object, name, item);
    };

    setBool("found", found);
    if (!found || !entry)
    {
        setInt("pid", -1);
        setString("status", "unknown");
        setString("exitCodeSource", "unknown");
        napi_value nullValue; napi_get_null(env, &nullValue);
        napi_set_named_property(env, object, "exitCode", nullValue);
        return object;
    }

    setInt("pid", entry->pid);
    setString("status", entry->running ? "running" : "exited");
    setDouble("startTimestamp", static_cast<double>(entry->startTimestampMs));
    setDouble("endTimestamp", static_cast<double>(entry->endTimestampMs));
    setString("exitCodeSource", entry->exitCodeSource);
    if (entry->exitCode >= 0) setInt("exitCode", entry->exitCode);
    else
    {
        napi_value nullValue; napi_get_null(env, &nullValue);
        napi_set_named_property(env, object, "exitCode", nullValue);
    }
    return object;
}

static int SpawnWineProgramImpl(const ProgramOptions& options)
{
    if (options.windowsExePath.empty() || HasUnsafeProtocolChar(options.windowsExePath)) return -1;
    for (const std::string& arg : options.argv) if (HasUnsafeProtocolChar(arg)) return -1;

    const std::string binDir = WINE_RUNTIME_BIN;
    const std::string prefixDir = PrefixForMode(options.prefixMode);
    const std::string homeDir = options.automationMode ? WINE_AUTOMATION_HOME
        : (gBrokerHomeDir.empty() ? "/storage/Users/currentUser/Download" : gBrokerHomeDir);
    const std::string sockDir = prefixDir;
    const std::string sockName = "wine-wayland";
    const std::string libPath = binDir + ":" + binDir + "/x86_64-unix";
    const std::string exePath = NativePathToWindows(options.windowsExePath, prefixDir);

    winehua::GraphicsBroker::GetInstance().SetWineRuntimeBinaryDir(binDir);
    winehua::GraphicsBroker::GetInstance().SetRequestedBackend(winehua::GraphicsBackend::Virgl);
    if (!winehua::GraphicsBroker::GetInstance().EnsureStarted(prefixDir)) return -1;
    const winehua::D3dBackendKind backend =
        winehua::ParseD3dBackend(options.d3dBackend);
    const bool publishVulkanSurface =
        options.presentToSurface && winehua::UsesVenusPresent(backend);
    winehua::GraphicsBroker::GetInstance().SetVulkanPresentMode(
        publishVulkanSurface);

    winehua::SessionEnvPolicy policy;
    policy.sockDir = sockDir;
    policy.sockName = sockName;
    policy.libPath = libPath;
    policy.binDir = binDir;
    policy.homeDir = homeDir;
    policy.prefixDir = prefixDir;
    policy.d3dBackend = options.d3dBackend;
    /* Product DXVK capabilities must apply to every managed program, not
     * only the desktop shell. This covers managed smoke and game windows
     * without relying on an A/B environment override. */
    policy.applyStableOverlay = winehua::IsDxvkBackend(backend);
    policy.desktopShellFlag = WaylandServer::GetInstance()->IsDesktopMode();
    policy.extraEnv = options.environment;
    policy.extraEnv.push_back("WINEHUA_D3D_BACKEND=" + options.d3dBackend);
    if (IsVkd3dSmokeDemo(options.windowsExePath))
        AppendVkd3dDemoPresentEnv(policy.extraEnv, options.d3dBackend, binDir);
    policy.extraEnv.push_back(std::string("WINEHUA_AUTOMATION=") +
                              (options.automationMode ? "1" : "0"));
    if (options.d3dBackend.rfind("dxvk_", 0) == 0)
        OH_LOG_INFO(LOG_APP, "[WineProgram] managed D3D backend=%{public}s",
                    options.d3dBackend.c_str());
    policy.extraEnv.push_back("WINEHUA_WINE_UNIX_ARCH=x86_64");
    policy.extraEnv.push_back("WINEHUA_HOST_ARCH=" + std::string(
#ifdef __aarch64__
        "aarch64"
#else
        "x86_64"
#endif
    ));
    if (!options.workingDirectory.empty())
        policy.extraEnv.push_back("WINEHUA_WORKING_DIRECTORY=" + options.workingDirectory);
    std::vector<std::string> envStrs = winehua::BuildSessionEnv(policy);

    winehua::SpawnRequest req{winehua::SpawnKind::WineExe};
    req.argv.push_back(exePath);
    req.argv.insert(req.argv.end(), options.argv.begin(), options.argv.end());
    req.env = std::move(envStrs);

    const pid_t pid = winehua::Spawner::Spawn(req);
    if (pid <= 0) return -1;
    AddProcess(pid, options.windowsExePath, -1);
    OH_LOG_INFO(LOG_APP,
                "[WineProgram] pid=%{public}d exe=%{public}s prefix=%{public}s d3d=%{public}s route=%{public}s output=%{public}s automation=%{public}s",
                pid, exePath.c_str(), prefixDir.c_str(), options.d3dBackend.c_str(),
                winehua::UsesVenusPresent(backend) ?
                    winehua::kProductVulkanRoute.data() :
                    winehua::kProductVirglRoute.data(),
                options.presentToSurface ? "surface" : "offscreen",
                options.automationMode ? "true" : "false");
    if (gStateTsfn)
    {
        char state[64];
        snprintf(state, sizeof(state), "%d:wine-running", pid);
        napi_call_threadsafe_function(gStateTsfn, strdup(state), napi_tsfn_blocking);
    }
    return pid;
}

static pid_t SpawnGuestProgram(const GuestProgramOptions& options)
{
    const std::string guestRoot = std::string(WINE_RUNTIME_BIN) + "/guest_vulkan";
    if (options.executablePath.rfind(guestRoot + "/", 0) != 0 ||
        HasUnsafeProtocolChar(options.executablePath))
        return -1;
    for (const std::string& arg : options.argv) if (HasUnsafeProtocolChar(arg)) return -1;

    const std::string binDir = WINE_RUNTIME_BIN;
    const std::string guestLib = guestRoot + "/lib";
    const std::string gfxLib = binDir + "/guest_gfx/lib";
    const std::string unixLib = binDir + "/x86_64-unix";
    const std::string libraryPath = guestLib + ":" + gfxLib + ":" + binDir + ":" + unixLib;
    const std::string icd = guestRoot + "/share/vulkan/icd.d/venus_icd.x86_64.json";

    std::vector<std::string> envStrs = BuildWineEnv(
        WINE_PREFIX, "wine-wayland", libraryPath, binDir, -1,
        WINE_AUTOMATION_HOME, WINE_PREFIX);
#ifdef __aarch64__
    // The NCP and box64.so are native AArch64.  Guest x86_64 directories must
    // only enter Box64's emulated lookup path; putting them in LD_LIBRARY_PATH
    // can make the native dynamic linker inspect wrong-architecture objects.
    envStrs.erase(std::remove_if(envStrs.begin(), envStrs.end(), [](const std::string& line) {
        return EnvKey(line) == "LD_LIBRARY_PATH";
    }), envStrs.end());
#else
    UpsertEnv(&envStrs, "LD_LIBRARY_PATH=" + libraryPath);
#endif
#ifdef __aarch64__
    UpsertEnv(&envStrs, "BOX64_LD_LIBRARY_PATH=" + libraryPath);
    UpsertEnv(&envStrs, "BOX64_EMULATED_LIBS=" + Box64EmulatedLibs());
    // Library loading has its own smoke assertions.  Function-call tracing is
    // prohibitively noisy when a disconnected vtest socket is polled and can
    // otherwise grow the shared stderr log by gigabytes before the watchdog.
    UpsertEnv(&envStrs, "BOX64_LOG=1");
    UpsertEnv(&envStrs, "BOX64_NOBANNER=1");
#endif
    UpsertEnv(&envStrs, "VK_DRIVER_FILES=" + icd);
    UpsertEnv(&envStrs, "VK_ICD_FILENAMES=" + icd);
    UpsertEnv(&envStrs, "VN_DEBUG=vtest,result");
    // OHOS Host Vulkan memory uses an explicit SHM shadow when the driver
    // cannot export dma-buf/opaque-fd memory. GPU fence and query feedback
    // writes only the Host mapping, so query the real Host objects instead
    // of polling stale Guest feedback slots.
    UpsertEnv(&envStrs, "VN_PERF=no_fence_feedback,no_query_feedback");
    UpsertEnv(&envStrs, "WINEHUA_HOST_ARCH=" + std::string(
#ifdef __aarch64__
        "aarch64"
#else
        "x86_64"
#endif
    ));
    UpsertEnv(&envStrs, std::string("WINEHUA_AUTOMATION=") +
              (options.automationMode ? "1" : "0"));
    if (!options.workingDirectory.empty())
        UpsertEnv(&envStrs, "WINEHUA_WORKING_DIRECTORY=" + options.workingDirectory);
    for (const std::string& line : options.environment) UpsertEnv(&envStrs, line);

    winehua::SpawnRequest req{winehua::SpawnKind::GuestElf};
    req.argv.push_back(options.executablePath);
    req.argv.insert(req.argv.end(), options.argv.begin(), options.argv.end());
    req.env = std::move(envStrs);

    const pid_t pid = winehua::Spawner::Spawn(req);
    if (pid <= 0) return -1;
    AddProcess(pid, options.executablePath, -1);
    OH_LOG_INFO(LOG_APP, "[GuestProgram] pid=%{public}d elf=%{public}s icd=%{public}s",
                pid, options.executablePath.c_str(), icd.c_str());
    return pid;
}

static bool ResolveManagedHostExecutable(const std::string& requested,
                                         std::string* resolved)
{
    const std::string managedRoot = std::string(WINE_RUNTIME_BIN) + "/host_vulkan";
    char rootPath[PATH_MAX] = {};
    char executablePath[PATH_MAX] = {};
    struct stat info = {};

    if (!realpath(managedRoot.c_str(), rootPath) ||
        !realpath(requested.c_str(), executablePath))
        return false;
    const std::string rootPrefix = std::string(rootPath) + "/";
    if (std::string(executablePath).rfind(rootPrefix, 0) != 0 ||
        stat(executablePath, &info) != 0 || !S_ISREG(info.st_mode))
        return false;
    *resolved = executablePath;
    return true;
}

static pid_t SpawnHostProgram(const HostProgramOptions& options)
{
    std::string executablePath;
    if (options.executablePath.empty() || HasUnsafeProtocolChar(options.executablePath) ||
        !ResolveManagedHostExecutable(options.executablePath, &executablePath))
        return -1;
    for (const std::string& arg : options.argv)
        if (HasUnsafeProtocolChar(arg)) return -1;

    std::vector<std::string> envStrs = options.environment;
    UpsertEnv(&envStrs, "HOME=" + std::string(WINE_AUTOMATION_HOME));
    UpsertEnv(&envStrs, "TMPDIR=" + std::string(WINE_TMPDIR));
    UpsertEnv(&envStrs, "WINEHUA_HOST_ARCH=" + std::string(
#ifdef __aarch64__
        "aarch64"
#else
        "x86_64"
#endif
    ));
    UpsertEnv(&envStrs, std::string("WINEHUA_AUTOMATION=") +
              (options.automationMode ? "1" : "0"));
    if (!options.workingDirectory.empty())
        UpsertEnv(&envStrs, "WINEHUA_WORKING_DIRECTORY=" + options.workingDirectory);

    winehua::SpawnRequest req{winehua::SpawnKind::HostElf};
    req.argv.push_back(executablePath);
    req.argv.insert(req.argv.end(), options.argv.begin(), options.argv.end());
    req.env = std::move(envStrs);

    const pid_t pid = winehua::Spawner::Spawn(req);
    if (pid <= 0) return -1;
    AddProcess(pid, executablePath, -1);
    OH_LOG_INFO(LOG_APP, "[HostProgram] pid=%{public}d elf=%{public}s",
                pid, executablePath.c_str());
    return pid;
}

} // namespace

// 公开入口 (wine_launch.cpp 自动拉起 explorer 复用): 转发到匿名
// namespace 内的实现, 后者依赖 PrefixForMode/SpawnViaBroker 等内部函数。
int SpawnWineProgram(const ProgramOptions& options)
{
    return SpawnWineProgramImpl(options);
}

napi_value RunWineProgram(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return MakeProcessObject(env, nullptr, false);

    napi_valuetype type;
    if (napi_typeof(env, args[0], &type) != napi_ok || type != napi_object)
        return MakeProcessObject(env, nullptr, false);

    ProgramOptions options;
    options.windowsExePath = GetString(env, args[0], "windowsExePath");
    options.workingDirectory = GetString(env, args[0], "workingDirectory");
    options.prefixMode = GetString(env, args[0], "prefixMode", "reuse");
    options.d3dBackend = GetString(env, args[0], "d3dBackend", "dxvk_legacy");
    options.presentToSurface = GetBool(env, args[0], "presentToSurface", true);
    options.automationMode = GetBool(env, args[0], "automationMode", false);
    ReadStringArray(env, args[0], "argv", &options.argv);
    ReadEnvironment(env, args[0], &options.environment);
    const winehua::D3dBackendKind requestedBackend =
        winehua::ParseD3dBackend(options.d3dBackend);
    if (requestedBackend == winehua::D3dBackendKind::Unknown ||
        requestedBackend == winehua::D3dBackendKind::Vkd3dLimited500k) {
        OH_LOG_ERROR(LOG_APP,
                     "[WineProgram] rejected unsupported d3d backend=%{public}s",
                     options.d3dBackend.c_str());
        return MakeProcessObject(env, nullptr, false);
    }
    OH_LOG_INFO(LOG_APP,
                "[WineProgram] parsed options exe=%{public}s argc=%{public}zu env=%{public}zu",
                options.windowsExePath.c_str(), options.argv.size(), options.environment.size());

    const pid_t pid = SpawnWineProgram(options);
    WineProcessEntry entry;
    return pid > 0 && QueryProcessSnapshot(pid, &entry)
        ? MakeProcessObject(env, &entry, true)
        : MakeProcessObject(env, nullptr, false);
}

napi_value RunGuestProgram(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return MakeProcessObject(env, nullptr, false);

    napi_valuetype type;
    if (napi_typeof(env, args[0], &type) != napi_ok || type != napi_object)
        return MakeProcessObject(env, nullptr, false);

    GuestProgramOptions options;
    options.executablePath = GetString(env, args[0], "executablePath");
    options.workingDirectory = GetString(env, args[0], "workingDirectory");
    options.automationMode = GetBool(env, args[0], "automationMode", true);
    ReadStringArray(env, args[0], "argv", &options.argv);
    ReadEnvironment(env, args[0], &options.environment);

    const pid_t pid = SpawnGuestProgram(options);
    WineProcessEntry entry;
    return pid > 0 && QueryProcessSnapshot(pid, &entry)
        ? MakeProcessObject(env, &entry, true)
        : MakeProcessObject(env, nullptr, false);
}

napi_value RunHostProgram(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return MakeProcessObject(env, nullptr, false);

    napi_valuetype type;
    if (napi_typeof(env, args[0], &type) != napi_ok || type != napi_object)
        return MakeProcessObject(env, nullptr, false);

    HostProgramOptions options;
    options.executablePath = GetString(env, args[0], "executablePath");
    options.workingDirectory = GetString(env, args[0], "workingDirectory");
    options.automationMode = GetBool(env, args[0], "automationMode", true);
    ReadStringArray(env, args[0], "argv", &options.argv);
    ReadEnvironment(env, args[0], &options.environment);

    const pid_t pid = SpawnHostProgram(options);
    WineProcessEntry entry;
    return pid > 0 && QueryProcessSnapshot(pid, &entry)
        ? MakeProcessObject(env, &entry, true)
        : MakeProcessObject(env, nullptr, false);
}

napi_value RunHostReplay(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    bool started = false;
    napi_valuetype type;
    if (argc >= 1 && napi_typeof(env, args[0], &type) == napi_ok && type == napi_object)
    {
        HostProgramOptions options;
        options.executablePath = GetString(env, args[0], "executablePath");
        ReadStringArray(env, args[0], "argv", &options.argv);
        std::string managedPath;
        if (ResolveManagedHostExecutable(options.executablePath, &managedPath) &&
            !gHostReplayRunning.exchange(true, std::memory_order_acq_rel))
        {
            void *module = dlopen("libwinehua_host_heaven_replay.so", RTLD_NOW | RTLD_LOCAL);
            HostReplayMain replayMain = module ? reinterpret_cast<HostReplayMain>(
                dlsym(module, "winehua_host_replay_main")) : nullptr;
            if (!replayMain)
            {
                const char *loadError = dlerror();
                OH_LOG_ERROR(LOG_APP, "[HostReplay] signed module unavailable: %{public}s",
                             loadError ? loadError : "unknown");
                gHostReplayRunning.store(false, std::memory_order_release);
            }
            else
            {
                std::thread([managedPath = std::move(managedPath),
                             replayArgs = std::move(options.argv), replayMain]() mutable {
                    std::vector<char*> argv;
                    argv.reserve(replayArgs.size() + 2);
                    argv.push_back(const_cast<char*>(managedPath.c_str()));
                    for (std::string& argument : replayArgs)
                        argv.push_back(const_cast<char*>(argument.c_str()));
                    argv.push_back(nullptr);
                    OH_LOG_INFO(LOG_APP, "[HostReplay] main-process worker started argc=%{public}zu",
                                argv.size() - 1);
                    const int result = replayMain(static_cast<int>(argv.size() - 1), argv.data());
                    OH_LOG_INFO(LOG_APP, "[HostReplay] main-process worker finished rc=%{public}d",
                                result);
                    gHostReplayRunning.store(false, std::memory_order_release);
                }).detach();
                started = true;
            }
        }
    }

    napi_value result;
    napi_get_boolean(env, started, &result);
    return result;
}

napi_value IsHostReplayRunning(napi_env env, napi_callback_info)
{
    napi_value result;
    napi_get_boolean(env, gHostReplayRunning.load(std::memory_order_acquire), &result);
    return result;
}

napi_value QueryWineProcess(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {};
    int32_t pid = -1;
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc >= 1) napi_get_value_int32(env, args[0], &pid);
    WineProcessEntry entry;
    return pid > 0 && QueryProcessSnapshot(pid, &entry)
        ? MakeProcessObject(env, &entry, true)
        : MakeProcessObject(env, nullptr, false);
}

napi_value TerminateWineProcess(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {};
    int32_t pid = -1;
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc >= 1) napi_get_value_int32(env, args[0], &pid);
    bool ok = pid > 0 && kill(pid, SIGKILL) == 0;
    if (ok) RemoveProcess(pid, -1, "unknown");
    napi_value result;
    napi_get_boolean(env, ok, &result);
    return result;
}

static napi_value MakeLaunchResult(napi_env env, int32_t pid,
                                   const std::string& sessionId, bool reused)
{
    napi_value result, pidValue, sessionValue, reusedValue;
    napi_create_object(env, &result);
    napi_create_int32(env, pid, &pidValue);
    napi_create_string_utf8(env, sessionId.c_str(), NAPI_AUTO_LENGTH, &sessionValue);
    napi_get_boolean(env, reused, &reusedValue);
    napi_set_named_property(env, result, "pid", pidValue);
    napi_set_named_property(env, result, "sessionId", sessionValue);
    napi_set_named_property(env, result, "reused", reusedValue);
    return result;
}

napi_value RunWineExe(napi_env env, napi_callback_info info)
{
    size_t argc = 9;
    napi_value args[9] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 4) return MakeLaunchResult(env, -1, "", false);

    char binDir[512] = {}, sockPath[512] = {}, libPath[2048] = {}, wineExe[1024] = {},
         homePath[1024] = {}, workingDirectoryPath[1024] = {}, d3dBackend[64] = "dxvk_legacy";
    napi_get_value_string_utf8(env, args[0], binDir, sizeof(binDir), nullptr);
    napi_get_value_string_utf8(env, args[1], sockPath, sizeof(sockPath), nullptr);
    napi_get_value_string_utf8(env, args[2], libPath, sizeof(libPath), nullptr);
    napi_get_value_string_utf8(env, args[3], wineExe, sizeof(wineExe), nullptr);
    if (argc >= 5) {
        napi_get_value_string_utf8(env, args[4], homePath, sizeof(homePath), nullptr);
    }
    std::vector<std::string> launchArguments;
    bool argumentArray = false;
    if (argc >= 6) napi_is_array(env, args[5], &argumentArray);
    if (argumentArray) {
        uint32_t length = 0;
        napi_get_array_length(env, args[5], &length);
        for (uint32_t index = 0; index < length; index++) {
            napi_value item;
            napi_get_element(env, args[5], index, &item);
            size_t size = 0;
            napi_get_value_string_utf8(env, item, nullptr, 0, &size);
            std::string value(size + 1, '\0');
            napi_get_value_string_utf8(env, item, value.data(), value.size(), &size);
            value.resize(size);
            launchArguments.push_back(value);
        }
    }
    if (argc >= 7) {
        napi_get_value_string_utf8(env, args[6], workingDirectoryPath,
                                   sizeof(workingDirectoryPath), nullptr);
    }
    if (argc >= 8) {
        char requestedBackend[64] = {};
        napi_get_value_string_utf8(env, args[7], requestedBackend,
                                   sizeof(requestedBackend), nullptr);
        const winehua::D3dBackendKind backend =
            winehua::ParseD3dBackend(requestedBackend);
        if (backend == winehua::D3dBackendKind::WineD3d ||
            winehua::IsDxvkBackend(backend)) {
            strncpy(d3dBackend, requestedBackend, sizeof(d3dBackend) - 1);
        } else {
            OH_LOG_ERROR(LOG_APP,
                         "[Wine] rejected unsupported d3d backend=%{public}s",
                         requestedBackend);
            return MakeLaunchResult(env, -1, "", false);
        }
    }
    std::vector<std::string> envOverrides;
    bool envArray = false;
    if (argc >= 9) napi_is_array(env, args[8], &envArray);
    if (envArray) {
        uint32_t length = 0;
        napi_get_array_length(env, args[8], &length);
        for (uint32_t index = 0; index < length; index++) {
            napi_value item;
            napi_get_element(env, args[8], index, &item);
            size_t size = 0;
            napi_get_value_string_utf8(env, item, nullptr, 0, &size);
            std::string value(size + 1, '\0');
            napi_get_value_string_utf8(env, item, value.data(), value.size(), &size);
            value.resize(size);
            if (!value.empty()) envOverrides.push_back(value);
        }
    }

    std::string homeDir(homePath);
    if (homeDir.empty()) homeDir = gBrokerHomeDir;
    if (homeDir.empty()) homeDir = "/storage/Users/currentUser/Download";

    std::string exePath(wineExe);
    {
        std::string lower = exePath;
        for (auto& c : lower) c = tolower(c);
        if (lower.find("/drive_c/") != std::string::npos) {
            auto slash = exePath.find_last_of('/');
            if (slash != std::string::npos) exePath = exePath.substr(slash + 1);
        }
    }

    OH_LOG_INFO(LOG_APP, "[Wine] runWineExe bin=%{public}s exe=%{public}s (final=%{public}s) home=%{public}s",
                binDir, wineExe, exePath.c_str(), homeDir.c_str());

    std::string sockStr(sockPath);
    auto pos = sockStr.find_last_of('/');
    std::string sockDir = (pos == std::string::npos) ? "/tmp" : sockStr.substr(0, pos);
    std::string sockName = (pos == std::string::npos) ? sockStr : sockStr.substr(pos + 1);

    winehua::SessionEnvPolicy policy;
    policy.sockDir = sockDir;
    policy.sockName = sockName;
    policy.libPath = libPath;
    policy.binDir = binDir;
    policy.homeDir = homeDir;
    policy.d3dBackend = d3dBackend;
    // Fusion games need the same managed DXVK/Venus/Box64 product capability
    // as the desktop shell. The final presenter policy remains per-surface.
    const bool desktopMode = WaylandServer::GetInstance()->IsDesktopMode();
    policy.applyStableOverlay = winehua::IsDxvkBackend(
        winehua::ParseD3dBackend(d3dBackend));
    policy.desktopShellFlag = desktopMode;
    if (workingDirectoryPath[0])
        policy.extraEnv.push_back("WINEHUA_WORKING_DIRECTORY=" +
                                  std::string(workingDirectoryPath));
    for (const std::string& overrideLine : envOverrides)
    {
        if (overrideLine.rfind("BOX64_DYNAREC_VOLATILE_METADATA=", 0) == 0) {
            OH_LOG_WARN(LOG_APP,
                        "[Wine] ignoring protected BOX64_DYNAREC_VOLATILE_METADATA override");
            continue;
        }
        policy.extraEnv.push_back(overrideLine);
    }
    if (IsVkd3dSmokeDemo(exePath) || IsVkd3dSmokeDemo(wineExe))
        AppendVkd3dDemoPresentEnv(policy.extraEnv, d3dBackend, binDir);
    std::vector<std::string> wineEnv = winehua::BuildSessionEnv(policy);
    OH_LOG_INFO(LOG_APP,
                "[Wine] product D3D backend=%{public}s cwd=%{public}s desktopOverlay=%{public}s",
                d3dBackend,
                workingDirectoryPath[0] ? workingDirectoryPath : "(derived)",
                desktopMode ? "yes" : "no");

    {
        winehua::SpawnRequest req{winehua::SpawnKind::WineExe};
        req.binDir = binDir;
        req.argv.push_back(exePath);
        req.argv.insert(req.argv.end(), launchArguments.begin(), launchArguments.end());
        req.env = std::move(wineEnv);
        OH_LOG_INFO(LOG_APP, "[Wine] runWineExe via broker: %{public}s", exePath.c_str());

        pid_t pid = -1;
        for (int launchAttempt = 0; launchAttempt < 2 && pid <= 0; launchAttempt++) {
            if (launchAttempt > 0) usleep(1000000);
            pid = winehua::Spawner::Spawn(req);
            if (pid <= 0)
                OH_LOG_WARN(LOG_APP, "[Wine] broker spawn failed (attempt %{public}d)",
                            launchAttempt + 1);
        }
        if (pid <= 0) {
            OH_LOG_ERROR(LOG_APP, "[Wine] broker spawn failed");
            if (gStateTsfn) napi_call_threadsafe_function(gStateTsfn, strdup("-1:wine-failed"), napi_tsfn_blocking);
            return MakeLaunchResult(env, -1, "", false);
        }

        const std::string sessionId = "wine-" + std::to_string(pid);
        AddProcess(pid, wineExe, -1, sessionId);
        OH_LOG_INFO(LOG_APP, "[Wine] wine pid=%{public}d exe=%{public}s (via broker)", pid, wineExe);
        if (gStateTsfn) {
            char msg[64];
            snprintf(msg, sizeof(msg), "%d:wine-running", pid);
            napi_call_threadsafe_function(gStateTsfn, strdup(msg), napi_tsfn_blocking);
        }
        return MakeLaunchResult(env, pid, sessionId, false);
    }
}

napi_value RunWineExeLegacy(napi_env env, napi_callback_info info)
{
    napi_value result = RunWineExe(env, info);
    napi_value pid;
    if (napi_get_named_property(env, result, "pid", &pid) != napi_ok)
        napi_create_int32(env, -1, &pid);
    return pid;
}
