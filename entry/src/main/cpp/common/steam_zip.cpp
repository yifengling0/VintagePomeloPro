#include "common/steam_zip.h"
#include "common/steam_zip_reader.h"
#include <atomic>
#include <exception>
#include <memory>
#include <string>
#include <unistd.h>

namespace {
std::atomic<bool> busy{false};
std::atomic<uint64_t> done{0}, total{0};
struct Work {
    int fd = -1;
    std::string destination, error;
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    ~Work() { if (fd >= 0) close(fd); }
};
void Reject(napi_env env, napi_deferred deferred, const std::string& message) {
    napi_value text, error;
    napi_create_string_utf8(env, message.c_str(), NAPI_AUTO_LENGTH, &text);
    napi_create_error(env, nullptr, text, &error);
    napi_reject_deferred(env, deferred, error);
}
void Execute(napi_env, void* pointer) {
    auto* work = static_cast<Work*>(pointer);
    try { steamzip::Extract(work->fd, work->destination, [](uint64_t n, uint64_t size) { done = n; total = size; }); }
    catch (const std::exception& error) { work->error = error.what(); }
    catch (...) { work->error = "Steam 解包失败"; }
}
void Complete(napi_env env, napi_status status, void* pointer) {
    std::unique_ptr<Work> work(static_cast<Work*>(pointer));
    busy = false;
    if (status != napi_ok && work->error.empty()) work->error = "Steam 解包被中断";
    if (!work->error.empty()) Reject(env, work->deferred, work->error);
    else { napi_value value; napi_get_undefined(env, &value); napi_resolve_deferred(env, work->deferred, value); }
    napi_delete_async_work(env, work->work);
}
}
napi_value ExtractSteamZipAsync(napi_env env, napi_callback_info info) {
    auto work = std::make_unique<Work>();
    napi_value promise;
    napi_create_promise(env, &work->deferred, &promise);
    napi_value args[2]{}; size_t argc = 2;
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t fd = -1; size_t length = 0;
    if (argc != 2 || napi_get_value_int32(env, args[0], &fd) != napi_ok ||
        napi_get_value_string_utf8(env, args[1], nullptr, 0, &length) != napi_ok || !length || length > 2048) {
        Reject(env, work->deferred, "Steam 解包参数无效"); return promise;
    }
    work->destination.resize(length + 1);
    napi_get_value_string_utf8(env, args[1], work->destination.data(), length + 1, &length);
    work->destination.resize(length);
    work->fd = dup(fd);
    if (work->fd < 0) { Reject(env, work->deferred, "无法读取所选 ZIP"); return promise; }
    if (busy.exchange(true)) { Reject(env, work->deferred, "另一个 Steam 导入正在进行"); return promise; }
    done = 0; total = 0;
    napi_value name; napi_create_string_utf8(env, "SteamZip", NAPI_AUTO_LENGTH, &name);
    if (napi_create_async_work(env, nullptr, name, Execute, Complete, work.get(), &work->work) != napi_ok ||
        napi_queue_async_work(env, work->work) != napi_ok) {
        if (work->work) napi_delete_async_work(env, work->work);
        busy = false; Reject(env, work->deferred, "无法启动 Steam 解包任务");
    } else work.release();
    return promise;
}
napi_value GetSteamZipProgress(napi_env env, napi_callback_info) {
    napi_value result, n, size;
    napi_create_object(env, &result);
    napi_create_double(env, static_cast<double>(done.load()), &n);
    napi_create_double(env, static_cast<double>(total.load()), &size);
    napi_set_named_property(env, result, "done", n);
    napi_set_named_property(env, result, "total", size);
    return result;
}
