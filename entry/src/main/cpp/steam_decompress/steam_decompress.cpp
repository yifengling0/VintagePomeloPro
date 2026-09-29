// Steam depot chunk 解压 (W6): LZMA (VZip) + zstd (VSZa) + zlib(gzip/deflate 兜底不用, CM 层已在 ArkTS)。
// NAPI 入口: decompressChunk(bytes, uncompressedSize, format) → ArrayBuffer
//   format: "lzma" = 5B props + LZMA 流 (VZip 剥壳后); "zstd" = VSZa 剥壳后的 zstd 帧。
// 由宿主 ArkTS 负责剥 VZip/VSZa 壳 (魔数识别 + 偏移), native 只做纯解码。
#include "napi/native_api.h"
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include "LzmaDec.h"
#include "zstd/zstd.h"
}

static void *SzAlloc(ISzAllocPtr p, size_t size) {
    (void)p;
    return malloc(size);
}

static void SzFree(ISzAllocPtr p, void *address) {
    (void)p;
    free(address);
}

static napi_value DecompressLzma(napi_env env, const uint8_t *data, size_t dataLen, size_t expectedOut) {
    if (dataLen < 5) {
        napi_throw_error(env, nullptr, "LZMA input too short (props)");
        return nullptr;
    }
    const uint8_t *props = data;
    const uint8_t *stream = data + 5;
    size_t streamLen = dataLen - 5;

    size_t outCap = expectedOut > 0 ? expectedOut : (64u << 20);
    uint8_t *out = static_cast<uint8_t *>(malloc(outCap));
    if (out == nullptr) {
        napi_throw_error(env, nullptr, "out of memory");
        return nullptr;
    }
    SizeT outLen = outCap;
    SizeT inLen = streamLen;
    ELzmaStatus status;
    ISzAlloc alloc = { SzAlloc, SzFree };
    // VZip 的 LZMA 流带 end-marker: 用 FINISH_ANY + 大缓冲 (与 SteamKit2 VZipUtil 一致)
    SRes res = LzmaDecode(out, &outLen, stream, &inLen, props, 5, LZMA_FINISH_ANY, &status, &alloc);
    if (res != SZ_OK) {
        free(out);
        std::string msg = "LzmaDecode failed res=" + std::to_string(res) + " status=" + std::to_string(status);
        napi_throw_error(env, nullptr, msg.c_str());
        return nullptr;
    }
    napi_value buffer;
    void *outData = nullptr;
    if (napi_create_buffer_copy(env, static_cast<size_t>(outLen), out, &outData, &buffer) != napi_ok) {
        free(out);
        napi_throw_error(env, nullptr, "create buffer failed");
        return nullptr;
    }
    free(out);
    return buffer;
}

static napi_value DecompressZstd(napi_env env, const uint8_t *data, size_t dataLen, size_t expectedOut) {
    size_t outCap = expectedOut > 0 ? expectedOut : ZSTD_getFrameContentSize(data, dataLen);
    if (outCap == ZSTD_CONTENTSIZE_ERROR || outCap == ZSTD_CONTENTSIZE_UNKNOWN) {
        outCap = 64u << 20;
    }
    uint8_t *out = static_cast<uint8_t *>(malloc(outCap));
    if (out == nullptr) {
        napi_throw_error(env, nullptr, "out of memory");
        return nullptr;
    }
    size_t written = ZSTD_decompress(out, outCap, data, dataLen);
    if (ZSTD_isError(written)) {
        free(out);
        std::string msg = "ZSTD_decompress failed: " + std::string(ZSTD_getErrorName(written));
        napi_throw_error(env, nullptr, msg.c_str());
        return nullptr;
    }
    napi_value buffer;
    void *outData = nullptr;
    if (napi_create_buffer_copy(env, written, out, &outData, &buffer) != napi_ok) {
        free(out);
        napi_throw_error(env, nullptr, "create buffer failed");
        return nullptr;
    }
    free(out);
    return buffer;
}

static napi_value DecompressChunk(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc < 2) {
        napi_throw_error(env, nullptr, "decompressChunk(bytes, expectedSize, format?)");
        return nullptr;
    }
    size_t dataLen = 0;
    void *dataPtr = nullptr;
    bool isBuffer = false;
    napi_is_arraybuffer(env, argv[0], &isBuffer);
    if (isBuffer) {
        napi_get_arraybuffer_info(env, argv[0], &dataPtr, &dataLen);
    } else {
        napi_get_typedarray_info(env, argv[0], nullptr, &dataLen, &dataPtr, nullptr, nullptr);
    }
    double expectedOut = 0;
    napi_get_value_double(env, argv[1], &expectedOut);
    std::string format = "lzma";
    if (argc >= 3) {
        char formatBuf[16] = {0};
        size_t formatLen = 0;
        napi_get_value_string_utf8(env, argv[2], formatBuf, sizeof(formatBuf), &formatLen);
        format = formatBuf;
    }

    const uint8_t *bytes = static_cast<const uint8_t *>(dataPtr);
    if (format == "zstd") {
        return DecompressZstd(env, bytes, dataLen, static_cast<size_t>(expectedOut));
    }
    return DecompressLzma(env, bytes, dataLen, static_cast<size_t>(expectedOut));
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"decompressChunk", nullptr, DecompressChunk, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

static napi_module steamDecompressModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "steamdecompress",
    .nm_priv = nullptr,
    .reserved = {nullptr},
};

extern "C" __attribute__((constructor)) void RegisterSteamDecompressModule(void) {
    napi_module_register(&steamDecompressModule);
}
