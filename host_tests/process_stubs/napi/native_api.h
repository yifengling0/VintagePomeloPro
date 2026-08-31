#pragma once
using napi_threadsafe_function = void*;
enum napi_threadsafe_function_call_mode { napi_tsfn_blocking };
inline int napi_call_threadsafe_function(napi_threadsafe_function, void*,
                                        napi_threadsafe_function_call_mode) { return 0; }
