#pragma once

#include <napi/native_api.h>

napi_value InitGameController(napi_env env, napi_callback_info info);
napi_value CleanupGameController(napi_env env, napi_callback_info info);
napi_value IsGamepadConnected(napi_env env, napi_callback_info info);
napi_value GetGamepadCount(napi_env env, napi_callback_info info);
napi_value SetGamepadButtonCallback(napi_env env, napi_callback_info info);
napi_value SetGamepadAxisCallback(napi_env env, napi_callback_info info);
napi_value SetGamepadDeviceCallback(napi_env env, napi_callback_info info);
napi_value SetGamepadRumbleCallback(napi_env env, napi_callback_info info);
