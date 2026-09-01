#include "bridge/performance_monitor_napi.h"
#include "common/performance_monitor.h"
#include <hidebug/hidebug.h>
#include <cmath>
#include <memory>
#include <new>

namespace {
struct Work {
    napi_async_work handle = nullptr;
    napi_deferred deferred = nullptr;
    bool appCpu = false;
    bool systemCpu = false;
    std::string json;
};
void Execute(napi_env, void* data)
{
    auto* work = static_cast<Work*>(data);
    auto snapshot = performance_monitor::ReadSnapshot(work->appCpu, work->systemCpu);
    if (work->systemCpu && !snapshot.systemReadable) {
        // Public API uses IPC; keep it on this worker. Zero can mean failure,
        // so don't misrepresent an ambiguous result as an idle system.
        const double ratio = OH_HiDebug_GetSystemCpuUsage();
        if (std::isfinite(ratio) && ratio > 0 && ratio <= 1) snapshot.systemPercent = ratio * 100;
    }
    work->json = performance_monitor::ToJson(snapshot);
}
void Complete(napi_env env, napi_status status, void* data)
{
    std::unique_ptr<Work> work(static_cast<Work*>(data));
    napi_value value = nullptr;
    if (status == napi_ok &&
        napi_create_string_utf8(env, work->json.c_str(), work->json.size(), &value) == napi_ok) {
        napi_resolve_deferred(env, work->deferred, value);
    } else {
        napi_get_undefined(env, &value);
        napi_reject_deferred(env, work->deferred, value);
    }
    napi_delete_async_work(env, work->handle);
}
}
napi_value ReadPerformanceCounters(napi_env env, napi_callback_info info)
{
    std::unique_ptr<Work> work(new (std::nothrow) Work);
    if (!work) {
        napi_throw_error(env, nullptr, "Performance sampler allocation failed");
        return nullptr;
    }
    size_t argc = 2;
    napi_value argv[2] = {};
    if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc != 2 ||
        napi_get_value_bool(env, argv[0], &work->appCpu) != napi_ok ||
        napi_get_value_bool(env, argv[1], &work->systemCpu) != napi_ok) {
        napi_throw_type_error(env, nullptr, "Expected appCpu and systemCpu booleans");
        return nullptr;
    }
    napi_value promise = nullptr, name = nullptr;
    if (napi_create_string_utf8(env, "PerformanceCounters", NAPI_AUTO_LENGTH, &name) != napi_ok ||
        napi_create_promise(env, &work->deferred, &promise) != napi_ok) return nullptr;
    if (napi_create_async_work(env, nullptr, name, Execute, Complete, work.get(), &work->handle) != napi_ok ||
        napi_queue_async_work(env, work->handle) != napi_ok) {
        napi_value error = nullptr;
        napi_get_undefined(env, &error);
        napi_reject_deferred(env, work->deferred, error);
        if (work->handle) napi_delete_async_work(env, work->handle);
        return promise;
    }
    work.release();
    return promise;
}
