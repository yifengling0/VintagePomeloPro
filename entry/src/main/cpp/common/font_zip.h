#pragma once

#include <napi/native_api.h>

/**
 * 从 ZIP 中提取字体文件并直接以 ASCII 安全名 (font-NNN.ext) 写入 outDir。
 * 原始条目名只用于判断扩展名/魔数, 不参与落盘命名, 因此中文/GBK 文件名
 * 不会在解压或复制阶段引发编码问题。
 *
 * 返回 { ok, fonts, bad, firstBadExt, error }。
 */
napi_value ExtractFontZip(napi_env env, napi_callback_info info);

/**
 * 异步版：解压在后台线程执行，返回 Promise<FontZipExtractResult>，
 * 避免大字体包在 UI 线程同步解压导致 AppFreeze/ANR。
 */
napi_value ExtractFontZipAsync(napi_env env, napi_callback_info info);
