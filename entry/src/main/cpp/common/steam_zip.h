#pragma once
#include <napi/native_api.h>
napi_value ExtractSteamZipAsync(napi_env env, napi_callback_info info);
napi_value GetSteamZipProgress(napi_env env, napi_callback_info info);
