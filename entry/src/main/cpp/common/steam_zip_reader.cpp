#include "common/steam_zip_reader.h"
#include "steam_decompress/zlib_vendored/zlib.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

namespace steamzip {
namespace {
[[noreturn]] void Fail(const char* message) { throw std::runtime_error(message); }
uint16_t U16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t U32(const uint8_t* p) { return uint32_t(U16(p)) | uint32_t(U16(p + 2)) << 16; }
uint64_t U64(const uint8_t* p) { return uint64_t(U32(p)) | uint64_t(U32(p + 4)) << 32; }
void Read(int fd, uint64_t at, void* buffer, size_t bytes, uint64_t end) {
    if (at > end || bytes > end - at) Fail("ZIP 数据范围无效");
    auto* p = static_cast<uint8_t*>(buffer);
    while (bytes) {
        const auto n = pread(fd, p, bytes, static_cast<off_t>(at));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) Fail("读取 ZIP 失败，文件可能已损坏");
        p += n; at += n; bytes -= n;
    }
}
std::string Lower(std::string value) {
    for (auto& c : value) if (uint8_t(c) < 128) c = static_cast<char>(std::tolower(uint8_t(c)));
    return value;
}
std::string SafeName(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.empty() || value.size() > 2048 || value.front() == '/') Fail("ZIP 路径无效");
    for (const unsigned char c : value) if (c < 32 || c == ':') Fail("ZIP 含不安全路径");
    if (value.back() == '/') value.pop_back();
    for (size_t from = 0; from <= value.size();) {
        const size_t to = value.find('/', from);
        const auto part = value.substr(from, to == std::string::npos ? to : to - from);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ')
            Fail("ZIP 含不安全路径");
        if (to == std::string::npos) break;
        from = to + 1;
    }
    return value;
}
struct Entry {
    std::string name, raw;
    uint64_t compressed = 0, bytes = 0, offset = 0;
    uint32_t crc = 0;
    uint16_t flags = 0, method = 0;
    bool directory = false;
};
void Mkdir(const std::string& path) {
    if (mkdir(path.c_str(), 0700) == 0) return;
    struct stat st{};
    if (errno != EEXIST || lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) Fail("无法建立解包目录");
}
void Parents(const std::string& destination, const std::string& relative, bool directory) {
    size_t at = 0;
    while ((at = relative.find('/', at)) != std::string::npos) {
        Mkdir(destination + '/' + relative.substr(0, at++));
    }
    if (directory) Mkdir(destination + '/' + relative);
}
struct Fd {
    int value;
    explicit Fd(int fd) : value(fd) {}
    ~Fd() { if (value >= 0) close(value); }
};
void Write(int fd, const uint8_t* data, size_t size) {
    while (size) {
        const auto n = write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) Fail("解包写入失败，请检查存储空间");
        data += n; size -= n;
    }
}
voidpf Alloc(voidpf, uInt n, uInt size) { return calloc(n, size); }
void Free(voidpf, voidpf p) { free(p); }
}

void Extract(int fd, const std::string& destination,
             const std::function<void(uint64_t, uint64_t)>& progress) {
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size < 22) Fail("ZIP 文件无效");
    const uint64_t size = st.st_size;
    std::vector<uint8_t> tail(static_cast<size_t>(std::min<uint64_t>(size, 65557)));
    Read(fd, size - tail.size(), tail.data(), tail.size(), size);
    size_t eocd = tail.size();
    for (size_t i = tail.size() - 22;; --i) {
        if (U32(&tail[i]) == 0x06054b50 && i + 22 + U16(&tail[i + 20]) == tail.size()) { eocd = i; break; }
        if (!i) break;
    }
    if (eocd == tail.size()) Fail("未找到 ZIP 目录");
    const auto* e = tail.data() + eocd;
    if (U16(e + 4) || U16(e + 6) || U16(e + 8) != U16(e + 10)) Fail("不支持分卷 ZIP");
    uint64_t count = U16(e + 10), centralBytes = U32(e + 12), central = U32(e + 16);
    const uint64_t eocdAt = size - tail.size() + eocd;
    uint64_t centralLimit = eocdAt;
    if (count == 0xffff || centralBytes == 0xffffffff || central == 0xffffffff) {
        if (eocdAt < 20) Fail("ZIP64 目录无效");
        uint8_t locator[20], record[56];
        Read(fd, eocdAt - 20, locator, sizeof(locator), size);
        if (U32(locator) != 0x07064b50 || U32(locator + 4) || U32(locator + 16) != 1) Fail("ZIP64 分卷或目录无效");
        centralLimit = U64(locator + 8);
        Read(fd, centralLimit, record, sizeof(record), eocdAt - 20);
        if (U32(record) != 0x06064b50 || U64(record + 4) < 44 || U32(record + 16) || U32(record + 20) ||
            U64(record + 24) != U64(record + 32)) Fail("ZIP64 目录无效");
        count = U64(record + 32); centralBytes = U64(record + 40); central = U64(record + 48);
    }
    if (!count || count > 500000 || central > centralLimit || centralBytes > centralLimit - central)
        Fail("ZIP 目录大小无效");
    const uint64_t end = central + centralBytes;
    uint64_t cursor = central, total = 0;
    std::vector<Entry> entries;
    std::set<std::string> names;
    std::string prefix;
    unsigned clients = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint8_t h[46];
        Read(fd, cursor, h, sizeof(h), end);
        if (U32(h) != 0x02014b50) Fail("ZIP 中央目录损坏");
        Entry item;
        item.flags = U16(h + 8); item.method = U16(h + 10); item.crc = U32(h + 16);
        item.compressed = U32(h + 20); item.bytes = U32(h + 24); item.offset = U32(h + 42);
        const auto length = U16(h + 28), extraLength = U16(h + 30), commentLength = U16(h + 32);
        const uint32_t mode = U32(h + 38) >> 16;
        if ((mode & S_IFMT) && !S_ISREG(mode) && !S_ISDIR(mode)) Fail("ZIP 不允许链接或特殊文件");
        if ((item.flags & 1) || (item.method != 0 && item.method != 8) || U16(h + 34)) Fail("ZIP 使用了不支持的加密、压缩或分卷格式");
        std::vector<uint8_t> body(size_t(length) + extraLength + commentLength);
        Read(fd, cursor + 46, body.data(), body.size(), end);
        item.raw.assign(reinterpret_cast<char*>(body.data()), length);
        item.directory = !item.raw.empty() && (item.raw.back() == '/' || item.raw.back() == '\\');
        item.name = SafeName(item.raw);
        if (!names.insert(Lower(item.name)).second) Fail("ZIP 包含重复文件路径");
        bool zip64 = false;
        for (size_t at = length; at + 4 <= size_t(length) + extraLength;) {
            const auto tag = U16(body.data() + at), n = U16(body.data() + at + 2);
            at += 4;
            if (at + n > size_t(length) + extraLength) Fail("ZIP 扩展记录损坏");
            if (tag == 1) {
                size_t pos = at;
                auto next = [&]() { if (pos + 8 > at + n) Fail("ZIP64 文件大小缺失"); const auto v = U64(body.data() + pos); pos += 8; return v; };
                if (item.bytes == 0xffffffff) item.bytes = next();
                if (item.compressed == 0xffffffff) item.compressed = next();
                if (item.offset == 0xffffffff) item.offset = next();
                zip64 = true;
            }
            at += n;
        }
        if (!zip64 && (item.bytes == 0xffffffff || item.compressed == 0xffffffff || item.offset == 0xffffffff)) Fail("缺少 ZIP64 文件信息");
        if (item.directory && (item.bytes || item.crc || item.compressed > 64)) Fail("ZIP 目录含无效数据");
        if (item.bytes > (uint64_t(512) << 30) - total) Fail("ZIP 解包大小超限");
        total += item.bytes;
        if (!item.directory && Lower(item.name) == "steam.exe") { clients++; prefix.clear(); }
        else if (!item.directory && Lower(item.name) == "steam/steam.exe") { clients++; prefix = item.name.substr(0, 6); }
        entries.push_back(std::move(item));
        cursor += 46 + body.size();
    }
    if (cursor != end || clients != 1) Fail("请选择根目录或 Steam 文件夹中含唯一 steam.exe 的完整包");
    for (const char* required : {"steam.exe", "steamclient.dll", "steamclient64.dll", "steamui.dll", "steam.cfg"})
        if (!names.count(Lower(prefix) + required)) Fail("Steam 包缺少客户端必要文件");
    if (names.count(Lower(prefix) + "bin/vgui2_s.dll") &&
        !names.count(Lower(prefix) + "steam/games/platformmenu.vdf"))
        Fail("Steam 包缺少 steam/games/PlatformMenu.vdf 客户端模块配置，请使用修复后的完整包");
    for (const auto& item : entries) {
        if (!prefix.empty() && Lower(item.name) != "steam" && Lower(item.name).rfind("steam/", 0) != 0 &&
            Lower(item.name) != "readme.txt" && Lower(item.name) != "使用说明.txt") Fail("ZIP 含 Steam 文件夹之外的文件");
        uint8_t local[30];
        Read(fd, item.offset, local, sizeof(local), central);
        if (U32(local) != 0x04034b50 || U16(local + 6) != item.flags || U16(local + 8) != item.method || U16(local + 26) != item.raw.size())
            Fail("ZIP 本地文件头不一致");
        std::string localName(item.raw.size(), '\0');
        Read(fd, item.offset + 30, localName.data(), localName.size(), central);
        if (localName != item.raw) Fail("ZIP 文件名校验失败");
        const uint64_t dataAt = item.offset + 30 + U16(local + 26) + U16(local + 28);
        if (dataAt > central || item.compressed > central - dataAt) Fail("ZIP 文件数据越界");
    }
    const auto parent = destination.substr(0, destination.find_last_of('/'));
    struct statvfs space{};
    if (statvfs(parent.c_str(), &space) != 0) Fail("无法检查存储空间");
    if (total + (64ull << 20) > uint64_t(space.f_bavail) * space.f_frsize) Fail("空间不足，无法导入 Steam");
    if (mkdir(destination.c_str(), 0700) != 0) Fail("无法创建独立 Steam 暂存目录");
    uint64_t done = 0;
    progress(done, total);
    for (const auto& item : entries) {
        if (!prefix.empty() && Lower(item.name).rfind("steam/", 0) != 0) continue;
        std::string name = item.name.substr(prefix.size());
        if (name.empty()) continue;
        // Canonicalize only Steam's structural names, never a game's filenames.
        const auto slash = name.find('/');
        const auto head = Lower(name.substr(0, slash));
        if (head == "steam.exe" || head == "steam.cfg" || head == "steamapps" ||
            head == "config" || head == "userdata" || head == "appcache" || head == "depotcache")
            name.replace(0, slash == std::string::npos ? name.size() : slash, head);
        Parents(destination, name, item.directory);
        if (item.directory) continue;
        Fd output(open((destination + '/' + name).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600));
        if (output.value < 0) Fail("不能创建 ZIP 输出文件");
        uint8_t local[30]; Read(fd, item.offset, local, sizeof(local), central);
        uint64_t at = item.offset + 30 + U16(local + 26) + U16(local + 28), remaining = item.compressed, written = 0;
        uLong crc = crc32(0, nullptr, 0);
        std::array<uint8_t, 65536> in{}, out{};
        z_stream stream{}; stream.zalloc = Alloc; stream.zfree = Free;
        struct Inflater { z_stream* p; ~Inflater() { if (p) inflateEnd(p); } } guard{nullptr};
        int result = Z_OK;
        if (item.method == 8) {
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) Fail("无法初始化 ZIP 解包器");
            guard.p = &stream;
        }
        do {
            const auto bytes = static_cast<size_t>(std::min<uint64_t>(remaining, in.size()));
            Read(fd, at, in.data(), bytes, central); at += bytes; remaining -= bytes;
            stream.next_in = in.data(); stream.avail_in = bytes;
            do {
                stream.next_out = out.data(); stream.avail_out = out.size();
                size_t produced = bytes;
                const uint8_t* data = in.data();
                if (item.method == 8) {
                    result = inflate(&stream, Z_NO_FLUSH);
                    if (result != Z_OK && result != Z_STREAM_END)
                        throw std::runtime_error("ZIP 解压失败: " + item.name + " (zlib=" + std::to_string(result) + ")");
                    produced = out.size() - stream.avail_out; data = out.data();
                }
                if (produced > item.bytes - written) Fail("ZIP 文件大小校验失败");
                Write(output.value, data, produced); crc = crc32(crc, data, produced);
                written += produced; done += produced; progress(done, total);
                if (item.method == 0) break;
                if (result == Z_STREAM_END && (stream.avail_in || remaining)) Fail("ZIP 压缩数据含多余字节");
            // Refill compressed input before asking zlib for more output.
            // Calling inflate with no input at a full-buffer boundary produces
            // Z_BUF_ERROR for a valid stream that still has bytes in the ZIP.
            } while (result != Z_STREAM_END && (stream.avail_in || (stream.avail_out == 0 && remaining == 0)));
        } while (remaining && result != Z_STREAM_END);
        if (written != item.bytes || crc != item.crc || (item.method == 8 && result != Z_STREAM_END)) Fail("ZIP CRC 或长度校验失败");
        if (fsync(output.value) != 0) Fail("无法保存 Steam 文件");
    }
    progress(total, total);
}
}
