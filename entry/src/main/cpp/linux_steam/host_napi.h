#pragma once
#include <napi/native_api.h>

namespace steamohos {
napi_value ProbeHost(napi_env env, napi_callback_info info);
napi_value InjectProbeKeys(napi_env env, napi_callback_info info);
napi_value IsLinuxSteamProcess(napi_env env, napi_callback_info info);
napi_value StartGameLaunchBroker(napi_env env, napi_callback_info info);
napi_value StopGameLaunchBroker(napi_env env, napi_callback_info info);
napi_value DrainGameLaunchBroker(napi_env env, napi_callback_info info);
napi_value ReplyGameLaunchBroker(napi_env env, napi_callback_info info);
napi_value PollGameLaunchSession(napi_env env, napi_callback_info info);
napi_value ProbeGnuSteam(napi_env env, napi_callback_info info);
napi_value ProbeGnuXwayland(napi_env env, napi_callback_info info);
napi_value ProbeGnuWayland(napi_env env, napi_callback_info info);
napi_value ProbeGnu(napi_env env, napi_callback_info info);
}
