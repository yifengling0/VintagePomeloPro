#pragma once
#include <cstdint>
// Host fixture for the process-launch API. No OHOS binary or SDK stub is run.
enum Ability_NativeChildProcess_ErrCode { NCP_NO_ERROR = 0, NCP_ERR_INVALID_PARAM = 1, NCP_ERR_NOT_SUPPORTED = 2 };
struct NativeChildProcess_Fd { char* fdName; int fd; NativeChildProcess_Fd* next; };
struct NativeChildProcess_Args { char* entryParams; struct { NativeChildProcess_Fd* head; } fdList; };
struct NativeChildProcess_Options { int isolationMode; };
constexpr int NCP_ISOLATION_MODE_NORMAL = 0;
using OH_Ability_OnNativeChildProcessStarted = void(*)(int, void*);
