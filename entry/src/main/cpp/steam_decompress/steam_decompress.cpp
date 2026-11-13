// Steam depot chunk 解压 (W6): LZMA (VZip) + zstd (VSZa) + zlib(gzip/deflate 兜底不用, CM 层已在 ArkTS)。
// NAPI 入口: decompressChunk(bytes, uncompressedSize, format) → ArrayBuffer
//   format: "lzma" = 5B props + LZMA 流 (VZip 剥壳后); "zstd" = VSZa 剥壳后的 zstd 帧。
// 由宿主 ArkTS 负责剥 VZip/VSZa 壳 (魔数识别 + 偏移), native 只做纯解码。
#include "napi/native_api.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include "zlib.h" /* zlib_vendored (Z_SOLO, 静态内置) */

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
    if (napi_create_arraybuffer(env, static_cast<size_t>(outLen), &outData, &buffer) != napi_ok) {
        free(out);
        napi_throw_error(env, nullptr, "create buffer failed");
        return nullptr;
    }
    memcpy(outData, out, outLen);
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
    if (napi_create_arraybuffer(env, written, &outData, &buffer) != napi_ok) {
        free(out);
        napi_throw_error(env, nullptr, "create buffer failed");
        return nullptr;
    }
    memcpy(outData, out, written);
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
    if (napi_get_value_double(env, argv[1], &expectedOut) != napi_ok ||
        expectedOut <= 0 || expectedOut > static_cast<double>(128u << 20) ||
        expectedOut != static_cast<double>(static_cast<size_t>(expectedOut))) {
        napi_throw_error(env, nullptr, "Invalid Steam chunk original size");
        return nullptr;
    }
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

static uint16_t Read16(const uint8_t *data) {
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

static uint32_t Read32(const uint8_t *data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

// Steam CDN manifests are single-entry ZIP archives. Parse the central
// directory so data-descriptor archives work, and never extract a filename.

// 内置 zlib 以 Z_SOLO 编译: inflateInit2 在该模式下不安装默认分配器,
// zalloc/zfree 为空时直接返回 Z_STREAM_ERROR(-2) (2026-10-02 真机现场)。
// 这里提供静态分配器包装 calloc/free。
static void *ZlibSoloCalloc(voidpf opaque, uInt items, uInt size) {
    (void)opaque;
    if (items == 0 || size == 0) return nullptr;
    return calloc(items, size);
}

static void ZlibSoloFree(voidpf opaque, voidpf address) {
    (void)opaque;
    free(address);
}

static napi_value UnzipManifest(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc != 1) {
        napi_throw_error(env, nullptr, "unzipManifest expects one ZIP buffer");
        return nullptr;
    }
    void *raw = nullptr;
    size_t size = 0;
    napi_typedarray_type type;
    size_t offset = 0;
    napi_value arrayBuffer;
    if (napi_get_typedarray_info(env, argv[0], &type, &size, &raw, &arrayBuffer, &offset) != napi_ok ||
        type != napi_uint8_array || size < 52 || size > (64u << 20)) {
        napi_throw_error(env, nullptr, "Invalid Steam manifest ZIP size or type");
        return nullptr;
    }
    const auto *zip = static_cast<const uint8_t *>(raw);
    size_t eocd = size;
    const size_t minOffset = size > 65557 ? size - 65557 : 0;
    for (size_t i = size - 22; i >= minOffset; --i) {
        if (Read32(zip + i) == 0x06054b50) { eocd = i; break; }
        if (i == 0) break;
    }
    if (eocd == size || Read16(zip + eocd + 10) != 1) {
        napi_throw_error(env, nullptr, "Steam manifest ZIP must contain exactly one entry");
        return nullptr;
    }
    const size_t central = Read32(zip + eocd + 16);
    if (central > size || size - central < 46 || Read32(zip + central) != 0x02014b50) {
        napi_throw_error(env, nullptr, "Invalid Steam manifest ZIP directory");
        return nullptr;
    }
    const uint16_t method = Read16(zip + central + 10);
    const size_t compressed = Read32(zip + central + 20);
    const size_t expanded = Read32(zip + central + 24);
    const size_t local = Read32(zip + central + 42);
    if (expanded == 0 || expanded > (128u << 20) || compressed > size || local > size ||
        size - local < 30 || Read32(zip + local) != 0x04034b50) {
        napi_throw_error(env, nullptr, "Invalid Steam manifest ZIP entry");
        return nullptr;
    }
    const size_t dataOffset = local + 30u + Read16(zip + local + 26) + Read16(zip + local + 28);
    if (dataOffset > size || compressed > size - dataOffset) {
        napi_throw_error(env, nullptr, "Truncated Steam manifest ZIP entry");
        return nullptr;
    }
    napi_value output;
    void *destination = nullptr;
    if (napi_create_arraybuffer(env, expanded, &destination, &output) != napi_ok) return nullptr;
    if (method == 0) {
        if (compressed != expanded) {
            napi_throw_error(env, nullptr, "Invalid stored Steam manifest size");
            return nullptr;
        }
        memcpy(destination, zip + dataOffset, expanded);
    } else if (method == 8) {
        z_stream stream = {};
        stream.next_in = const_cast<Bytef *>(zip + dataOffset);
        stream.avail_in = static_cast<uInt>(compressed);
        stream.next_out = static_cast<Bytef *>(destination);
        stream.avail_out = static_cast<uInt>(expanded);
        stream.zalloc = ZlibSoloCalloc;
        stream.zfree = ZlibSoloFree;
        // 2026-10-02: 真机 695630 现场卡在此处; inflateInit2 与输入无关, 返回码
        // 必须带出来才能区分 Z_MEM_ERROR / Z_VERSION_ERROR(ABI) / Z_STREAM_ERROR。
        char detail[128];
        const int initResult = inflateInit2(&stream, -MAX_WBITS);
        if (initResult != Z_OK) {
            snprintf(detail, sizeof(detail),
                     "Steam manifest inflater init failed (ret=%d zipSize=%zu comp=%zu expand=%zu)",
                     initResult, size, compressed, expanded);
            napi_throw_error(env, nullptr, detail);
            return nullptr;
        }
        const int result = inflate(&stream, Z_FINISH);
        inflateEnd(&stream);
        if (result != Z_STREAM_END || stream.total_out != expanded) {
            snprintf(detail, sizeof(detail),
                     "Steam manifest ZIP decompression failed (ret=%d out=%lu want=%zu in=%lu)",
                     result, static_cast<unsigned long>(stream.total_out), expanded,
                     static_cast<unsigned long>(stream.total_in));
            napi_throw_error(env, nullptr, detail);
            return nullptr;
        }
    } else {
        napi_throw_error(env, nullptr, "Unsupported Steam manifest ZIP compression");
        return nullptr;
    }
    if (crc32(0, static_cast<Bytef *>(destination), static_cast<uInt>(expanded)) != Read32(zip + central + 16)) {
        napi_throw_error(env, nullptr, "Steam manifest ZIP checksum mismatch");
        return nullptr;
    }
    return output;
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"decompressChunk", nullptr, DecompressChunk, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"unzipManifest", nullptr, UnzipManifest, nullptr, nullptr, nullptr, napi_default, nullptr},
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
