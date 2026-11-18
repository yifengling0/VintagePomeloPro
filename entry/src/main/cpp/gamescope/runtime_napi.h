// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <napi/native_api.h>
namespace winehua::gamescope {
napi_value StartDiagnosticClient(napi_env env, napi_callback_info info);
napi_value PrepareRuntime(napi_env env, napi_callback_info info);
napi_value ProbeRuntime(napi_env env, napi_callback_info info);
napi_value ProbeHarmonyFrameGenerationNapi(napi_env env, napi_callback_info info);
napi_value StartRuntimeSession(napi_env env, napi_callback_info info);
napi_value StopRuntimeSession(napi_env env, napi_callback_info info);
napi_value GetRuntimeSessionStatus(napi_env env, napi_callback_info info);
napi_value SetRuntimeOutputState(napi_env env, napi_callback_info info);
}
