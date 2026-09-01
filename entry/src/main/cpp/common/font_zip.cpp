#include "common/font_zip.h"

#include <zlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iconv.h>
#include <set>
#include <string>
#include <vector>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "FontZip"
#include <hilog/log.h>

namespace {

constexpr uint32_t kEocdSig = 0x06054b50;
constexpr uint32_t kCentralSig = 0x02014b50;
constexpr uint32_t kLocalSig = 0x04034b50;
// 单个条目压缩数据 / 解压后上限, 防 zip bomb 与超大 TTC。
// 收紧到 128MB/64MB: 正常字体单文件通常 < 64MB, 一次性 inflate 分配
// 过大内存会在低内存设备上 OOM 闪退 (native 崩溃无法被 ArkTS catch)。
constexpr size_t kMaxEntryBytes = 128u * 1024u * 1024u;
constexpr size_t kMaxUncompBytes = 64u * 1024u * 1024u;

struct FontZipResult {
    bool ok = false;
    int fonts = 0;
    int bad = 0;
    std::string firstBadExt;
    std::string error;
};

uint16_t Rd16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t Rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool ReadAt(int fd, uint64_t offset, void* buf, size_t size) {
    uint8_t* dst = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < size) {
        const ssize_t n = pread(fd, dst + done, size - done, static_cast<off_t>(offset + done));
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

/** 取 basename 的小写扩展名; 目录 (以 / 结尾) 单独标记。 */
void ExtractExt(const std::vector<uint8_t>& name, std::string* ext, bool* isDir) {
    ext->clear();
    *isDir = !name.empty() && (name.back() == '/' || name.back() == '\\');
    size_t baseStart = 0;
    for (size_t i = 0; i < name.size(); i++) {
        if (name[i] == '/' || name[i] == '\\') baseStart = i + 1;
    }
    size_t dot = name.size();
    for (size_t i = baseStart; i < name.size(); i++) {
        if (name[i] == '.') dot = i;
    }
    for (size_t i = dot + 1; i < name.size(); i++) {
        uint8_t c = name[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<uint8_t>(c - 'A' + 'a');
        ext->push_back(static_cast<char>(c));
    }
}

bool IsFontExt(const std::string& ext) {
    return ext == "ttf" || ext == "otf" || ext == "ttc" || ext == "otc" ||
           ext == "fon" || ext == "fnt";
}

bool MagicOk(const uint8_t h[4], const std::string& ext) {
    if (ext == "fnt") return true; // 位图字体没有统一魔数
    if (ext == "fon") return h[0] == 'M' && h[1] == 'Z';
    if (h[0] == 0x00 && h[1] == 0x01 && h[2] == 0x00 && h[3] == 0x00) return true; // TTF
    if (h[0] == 'O' && h[1] == 'T' && h[2] == 'T' && h[3] == 'O') return true;     // OTTO
    if (h[0] == 't' && h[1] == 'r' && h[2] == 'u' && h[3] == 'e') return true;     // true
    if (h[0] == 't' && h[1] == 't' && h[2] == 'c' && h[3] == 'f') return true;     // ttcf
    if (h[0] == 't' && h[1] == 'y' && h[2] == 'p' && h[3] == '1') return true;     // typ1
    return false;
}

/** 严格校验一段字节是否为合法 UTF-8 (拒绝过短/过长/代理区/越界编码)。 */
bool IsValidUtf8(const uint8_t* p, size_t n) {
    size_t i = 0;
    while (i < n) {
        const uint8_t c = p[i];
        if (c < 0x80) {
            i += 1;
        } else if (c >= 0xC2 && c <= 0xDF) {
            if (i + 1 >= n || (p[i + 1] & 0xC0) != 0x80) return false;
            i += 2;
        } else if (c == 0xE0) {
            if (i + 2 >= n || p[i + 1] < 0xA0 || p[i + 1] > 0xBF ||
                (p[i + 2] & 0xC0) != 0x80) return false;
            i += 3;
        } else if (c >= 0xE1 && c <= 0xEC) {
            if (i + 2 >= n || (p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80) return false;
            i += 3;
        } else if (c == 0xED) {
            if (i + 2 >= n || p[i + 1] < 0x80 || p[i + 1] > 0x9F ||
                (p[i + 2] & 0xC0) != 0x80) return false;
            i += 3;
        } else if (c >= 0xEE && c <= 0xEF) {
            if (i + 2 >= n || (p[i + 1] & 0xC0) != 0x80 || (p[i + 2] & 0xC0) != 0x80) return false;
            i += 3;
        } else if (c == 0xF0) {
            if (i + 3 >= n || p[i + 1] < 0x90 || p[i + 1] > 0xBF ||
                (p[i + 2] & 0xC0) != 0x80 || (p[i + 3] & 0xC0) != 0x80) return false;
            i += 4;
        } else if (c >= 0xF1 && c <= 0xF3) {
            if (i + 3 >= n || (p[i + 1] & 0xC0) != 0x80 ||
                (p[i + 2] & 0xC0) != 0x80 || (p[i + 3] & 0xC0) != 0x80) return false;
            i += 4;
        } else if (c == 0xF4) {
            if (i + 3 >= n || p[i + 1] < 0x80 || p[i + 1] > 0x8F ||
                (p[i + 2] & 0xC0) != 0x80 || (p[i + 3] & 0xC0) != 0x80) return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

/** GBK/GB2312 -> UTF-8; 失败返回 false (musl iconv 支持 GBK)。 */
bool GbkToUtf8(const std::string& in, std::string* out) {
    iconv_t cd = iconv_open("UTF-8", "GBK");
    if (cd == reinterpret_cast<iconv_t>(-1)) return false;
    std::string buf(in.size() * 3 + 16, '\0');
    char* inPtr = const_cast<char*>(in.data());
    size_t inLeft = in.size();
    char* outPtr = buf.data();
    size_t outLeft = buf.size();
    const size_t ret = iconv(cd, &inPtr, &inLeft, &outPtr, &outLeft);
    iconv_close(cd);
    if (ret == static_cast<size_t>(-1)) return false;
    buf.resize(buf.size() - outLeft);
    *out = buf;
    return true;
}

/**
 * 取条目 basename 并转成可落盘的 UTF-8 文件名:
 * 依据 ZIP 的 UTF-8 标志位决定解码方式 (0x800=UTF-8, 否则按 GBK/CP437 处理),
 * 过滤路径分隔与非法字符。
 * 返回空串表示无法解码, 由调用方回退到 font-NNN.ext。
 */
std::string ResolveEntryBasename(const std::vector<uint8_t>& rawName, bool utf8Flag) {
    size_t base = 0;
    for (size_t i = 0; i < rawName.size(); i++) {
        if (rawName[i] == '/' || rawName[i] == '\\') base = i + 1;
    }
    if (base >= rawName.size()) return "";
    const uint8_t* start = rawName.data() + base;
    const size_t len = rawName.size() - base;
    if (len == 0 || len > 255) return "";

    std::string decoded;
    if (utf8Flag) {
        if (!IsValidUtf8(start, len)) return "";
        decoded.assign(reinterpret_cast<const char*>(start), len);
    } else {
        const std::string raw(reinterpret_cast<const char*>(start), len);
        // Windows 压缩包未置 UTF-8 标志时文件名多为 GBK; ASCII 也兼容 GBK。
        if (!GbkToUtf8(raw, &decoded) && !IsValidUtf8(start, len)) return "";
        if (decoded.empty()) decoded.assign(reinterpret_cast<const char*>(start), len);
    }

    std::string safe;
    for (size_t i = 0; i < decoded.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(decoded[i]);
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            safe.push_back('_');
        } else {
            safe.push_back(static_cast<char>(c));
        }
    }
    // 去掉首尾空格与点号, 避免 Windows 侧不可见/隐藏文件。
    size_t begin = safe.find_first_not_of(" .");
    if (begin == std::string::npos) return "";
    size_t end = safe.find_last_not_of(" .");
    safe = safe.substr(begin, end - begin + 1);
    if (safe.size() > 255) safe = safe.substr(0, 255);
    return safe;
}

/** 避免同名条目 (不同子目录) 冲突: 重名时在扩展名前追加 -2/-3。 */
std::string UniqueBasename(const std::string& base, std::set<std::string>* used) {
    if (!used->count(base)) {
        used->insert(base);
        return base;
    }
    const size_t dot = base.find_last_of('.');
    const std::string stem = dot == std::string::npos ? base : base.substr(0, dot);
    const std::string suffix = dot == std::string::npos ? "" : base.substr(dot);
    for (int i = 2; ; i++) {
        const std::string candidate = stem + "-" + std::to_string(i) + suffix;
        if (!used->count(candidate)) {
            used->insert(candidate);
            return candidate;
        }
    }
}

bool InflateRaw(const uint8_t* src, size_t srcSize, uint32_t expectSize,
                std::vector<uint8_t>* out) {
    if (expectSize > kMaxUncompBytes) return false;
    out->resize(expectSize);
    z_stream strm{};
    strm.next_in = const_cast<Bytef*>(src);
    strm.avail_in = static_cast<uInt>(srcSize);
    strm.next_out = out->data();
    strm.avail_out = static_cast<uInt>(expectSize);
    if (inflateInit2(&strm, -15) != Z_OK) return false;
    const int ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    return ret == Z_STREAM_END && strm.total_out == expectSize;
}

bool MkdirP(const std::string& path) {
    std::string acc;
    for (size_t i = 0; i < path.size(); i++) {
        acc.push_back(path[i]);
        if (path[i] == '/' && acc.size() > 1) {
            if (mkdir(acc.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    }
    if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) return false;
    return true;
}

std::string Pad3(int value) {
    std::string s = std::to_string(value);
    while (s.size() < 3) s = "0" + s;
    return s;
}

/** 提取单个条目到 outPath; 魔数在解压之后校验 (压缩条目的首字节是 deflate 流)。 */
bool ExtractEntry(int fd, uint32_t localOffset, uint16_t method, uint32_t compSize,
                  uint32_t uncompSize, const std::string& ext, const std::string& outPath) {
    if (compSize > kMaxEntryBytes || uncompSize > kMaxUncompBytes) return false;
    uint8_t lh[30];
    if (!ReadAt(fd, localOffset, lh, sizeof(lh)) || Rd32(lh) != kLocalSig) return false;
    const uint64_t dataOffset = static_cast<uint64_t>(localOffset) + 30 +
                                Rd16(lh + 26) + Rd16(lh + 28);
    std::vector<uint8_t> raw(compSize);
    if (!ReadAt(fd, dataOffset, raw.data(), raw.size())) return false;

    std::vector<uint8_t> data;
    if (method == 0) {
        data.swap(raw);
        if (data.size() != uncompSize) return false;
    } else if (method == 8) {
        if (!InflateRaw(raw.data(), raw.size(), uncompSize, &data)) return false;
    } else {
        return false; // 不支持的压缩方式
    }
    if (data.size() < 4 || !MagicOk(data.data(), ext)) return false;

    const int outFd = open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (outFd < 0) return false;
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = write(outFd, data.data() + done, data.size() - done);
        if (n <= 0) {
            close(outFd);
            return false;
        }
        done += static_cast<size_t>(n);
    }
    close(outFd);
    return true;
}

FontZipResult ExtractFontZipImpl(const std::string& zipPath, const std::string& outDir) {
    FontZipResult r;
    const int fd = open(zipPath.c_str(), O_RDONLY);
    if (fd < 0) {
        r.error = "无法打开压缩包";
        return r;
    }
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size < 22) {
        r.error = "压缩包无效";
        close(fd);
        return r;
    }

    uint64_t eocdOffset = static_cast<uint64_t>(st.st_size) - 22;
    uint8_t tail[22];
    if (!ReadAt(fd, eocdOffset, tail, sizeof(tail)) || Rd32(tail) != kEocdSig) {
        // 注释字段可能不为空, 从末尾向前扫描 EOCD 签名。
        const uint64_t scanStart = eocdOffset > 65535 ? eocdOffset - 65535 : 0;
        bool found = false;
        for (uint64_t off = eocdOffset; ; off--) {
            uint8_t sig[4];
            if (!ReadAt(fd, off, sig, sizeof(sig))) break;
            if (Rd32(sig) == kEocdSig) {
                eocdOffset = off;
                found = true;
                break;
            }
            if (off == scanStart) break;
        }
        if (!found) {
            r.error = "不是有效的 ZIP 压缩包";
            close(fd);
            return r;
        }
        if (!ReadAt(fd, eocdOffset, tail, sizeof(tail))) {
            r.error = "ZIP 结尾损坏";
            close(fd);
            return r;
        }
    }

    const uint16_t totalEntries = Rd16(tail + 10);
    const uint32_t cdOffset = Rd32(tail + 16);
    if (totalEntries == 0xFFFF || cdOffset == 0xFFFFFFFF) {
        r.error = "暂不支持 ZIP64 超大压缩包";
        close(fd);
        return r;
    }
    if (!MkdirP(outDir)) {
        r.error = "无法创建解压目录";
        close(fd);
        return r;
    }

    uint64_t cur = cdOffset;
    int fontIndex = 0;
    std::set<std::string> usedNames;
    for (int i = 0; i < totalEntries; i++) {
        uint8_t cd[46];
        if (!ReadAt(fd, cur, cd, sizeof(cd)) || Rd32(cd) != kCentralSig) {
            r.error = "ZIP 目录损坏";
            break;
        }
        const uint16_t nameLen = Rd16(cd + 28);
        const uint16_t extraLen = Rd16(cd + 30);
        const uint16_t commentLen = Rd16(cd + 32);
        const uint32_t localOffset = Rd32(cd + 42);
        const uint16_t method = Rd16(cd + 10);
        const uint16_t flags = Rd16(cd + 8);
        const uint32_t compSize = Rd32(cd + 20);
        const uint32_t uncompSize = Rd32(cd + 24);

        std::vector<uint8_t> name(nameLen);
        if (nameLen > 0 && !ReadAt(fd, cur + 46, name.data(), nameLen)) {
            r.error = "ZIP 文件名读取失败";
            break;
        }
        cur += 46u + nameLen + extraLen + commentLen;

        std::string ext;
        bool isDir = false;
        ExtractExt(name, &ext, &isDir);
        if (isDir) continue; // 目录不算非法内容
        if (!IsFontExt(ext)) {
            if (r.bad == 0) r.firstBadExt = ext.empty() ? "(无扩展名)" : ext;
            r.bad++;
            continue;
        }

        fontIndex++;
        std::string basename = ResolveEntryBasename(name, (flags & 0x800) != 0);
        if (basename.empty()) {
            basename = "font-" + Pad3(fontIndex) + "." + ext;
        }
        basename = UniqueBasename(basename, &usedNames);
        const std::string outPath = outDir + "/" + basename;
        if (!ExtractEntry(fd, localOffset, method, compSize, uncompSize, ext, outPath)) {
            r.bad++;
            continue;
        }
        r.fonts++;
        OH_LOG_INFO(LOG_APP, "[FontZip] extracted %{public}s", basename.c_str());
    }
    close(fd);
    r.ok = true;
    return r;
}

std::string ReadNapiString(napi_env env, napi_value value) {
    size_t len = 0;
    napi_get_value_string_utf8(env, value, nullptr, 0, &len);
    std::string s(len, '\0');
    napi_get_value_string_utf8(env, value, s.data(), len + 1, &len);
    s.resize(len);
    return s;
}

} // namespace

napi_value MakeFontZipResultObject(napi_env env, const FontZipResult& r) {
    napi_value result;
    napi_create_object(env, &result);
    napi_value val;
    napi_get_boolean(env, r.ok, &val);
    napi_set_named_property(env, result, "ok", val);
    napi_create_int32(env, r.fonts, &val);
    napi_set_named_property(env, result, "fonts", val);
    napi_create_int32(env, r.bad, &val);
    napi_set_named_property(env, result, "bad", val);
    napi_create_string_utf8(env, r.firstBadExt.c_str(), NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, result, "firstBadExt", val);
    napi_create_string_utf8(env, r.error.c_str(), NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, result, "error", val);
    return result;
}

/**
 * 异步解压：ExtractFontZipImpl 在 napi async work 的后台线程执行，
 * 主线程不会被大字体包阻塞，避免 AppFreeze/ANR 杀进程。
 */
struct FontZipWorkContext {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::string zipPath;
    std::string outDir;
    FontZipResult result;
};

static void FontZipExecute(napi_env env, void* data) {
    auto* ctx = static_cast<FontZipWorkContext*>(data);
    ctx->result = ExtractFontZipImpl(ctx->zipPath, ctx->outDir);
}

static void FontZipComplete(napi_env env, napi_status status, void* data) {
    auto* ctx = static_cast<FontZipWorkContext*>(data);
    napi_value result = MakeFontZipResultObject(env, ctx->result);
    napi_resolve_deferred(env, ctx->deferred, result);
    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

napi_value ExtractFontZipAsync(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return nullptr;

    auto* ctx = new FontZipWorkContext();
    ctx->zipPath = ReadNapiString(env, args[0]);
    ctx->outDir = ReadNapiString(env, args[1]);

    napi_value promise;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value resourceName;
    napi_create_string_utf8(env, "ExtractFontZip", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, nullptr, resourceName,
        FontZipExecute, FontZipComplete, ctx, &ctx->work);
    napi_queue_async_work(env, ctx->work);
    return promise;
}

napi_value ExtractFontZip(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return nullptr;

    const std::string zipPath = ReadNapiString(env, args[0]);
    const std::string outDir = ReadNapiString(env, args[1]);
    const FontZipResult r = ExtractFontZipImpl(zipPath, outDir);
    return MakeFontZipResultObject(env, r);
}
