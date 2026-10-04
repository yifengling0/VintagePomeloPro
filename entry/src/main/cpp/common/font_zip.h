#pragma once

#include <napi/native_api.h>

/** Extract validated ZIP fonts into a fresh staging directory using sanitized
 * UTF-8 basenames (GBK converted where necessary). No live fonts are replaced.
 * Returns { ok, fonts, bad, firstBadExt, error, errorCode, zlibCode }.
 * bad counts non-font extensions only; extraction failures have typed errors.
 */
napi_value ExtractFontZip(napi_env env, napi_callback_info info);

/** The same extraction on a background thread, returning a Promise. */
napi_value ExtractFontZipAsync(napi_env env, napi_callback_info info);
