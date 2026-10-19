#include "native_loader.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

extern "C" [[noreturn]] void SteamOhosEnterNativeLoader(uintptr_t entry, uintptr_t sp);
namespace steamohos {
namespace {
uint64_t Down(uint64_t n, uint64_t a) { return n & ~(a - 1); }
uint64_t Up(uint64_t n, uint64_t a) { return (n + a - 1) & ~(a - 1); }
bool PowerOfTwo(uint64_t n) { return n && !(n & (n - 1)); }
struct File { int fd = -1; ~File() { if (fd >= 0) close(fd); } };
bool Read(int fd, void* buffer, size_t bytes, off_t offset) {
    auto* out = static_cast<char*>(buffer);
    while (bytes) {
        ssize_t n = pread(fd, out, bytes, offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        out += n; bytes -= n; offset += n;
    }
    return true;
}
}
NativeLoaderImage::~NativeLoaderImage() {
    if (image_) munmap(image_, imageBytes_);
    if (stack_) munmap(stack_, stackBytes_);
}
bool NativeLoaderImage::Fail(const char* value, int code) { stage = value; error = code; return false; }
bool NativeLoaderImage::Prepare(const std::vector<std::string>& argv, const std::vector<std::string>& environment) {
    if (image_ || stack_ || argv.empty() || argv.size() > 128 || environment.size() > 256 ||
        argv[0].empty() || argv[0][0] != '/') return Fail("loader_arguments", EINVAL);
    bool nativeMemory = false;
    std::string externalRoot;
    for (const auto& value : environment) {
        if (value == "STEAMOHOS_NATIVE_MEMORY=1") nativeMemory = true;
        const std::string prefix = "STEAMOHOS_EXTERNAL_RUNTIME=";
        if (value.compare(0, prefix.size(), prefix) == 0) externalRoot = value.substr(prefix.size());
    }
    if (nativeMemory && (externalRoot.empty() || externalRoot[0] != '/' ||
        argv[0].compare(0, externalRoot.size() + 1, externalRoot + '/') ||
        argv[0].find("/../") != std::string::npos || argv[0].find("/./") != std::string::npos))
        return Fail("loader_external_root", EINVAL);
    const long nativePage = sysconf(_SC_PAGESIZE);
    if (nativePage != 4096 && nativePage != 16384 && nativePage != 65536) return Fail("loader_page_size", ENOTSUP);
    const uint64_t page = static_cast<uint64_t>(nativePage);
    File file; file.fd = open(argv[0].c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file.fd < 0) return Fail("loader_open", errno);
    struct stat st{}; Elf64_Ehdr eh{};
    if (fstat(file.fd, &st) || !S_ISREG(st.st_mode) || st.st_size < static_cast<off_t>(sizeof(eh)) ||
        !Read(file.fd, &eh, sizeof(eh), 0)) return Fail("loader_header_read", EIO);
#if defined(__aarch64__)
    constexpr uint16_t machine = EM_AARCH64;
    const char platform[] = "aarch64";
#elif defined(__x86_64__)
    constexpr uint16_t machine = EM_X86_64;
    const char platform[] = "x86_64";
#else
#error Unsupported native ISA
#endif
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_ident[EI_VERSION] != EV_CURRENT ||
        eh.e_type != ET_DYN || eh.e_machine != machine || eh.e_ehsize != sizeof(eh) ||
        eh.e_phentsize != sizeof(Elf64_Phdr) || !eh.e_phnum || eh.e_phnum > 64 ||
        eh.e_phoff > static_cast<uint64_t>(st.st_size) ||
        eh.e_phnum * sizeof(Elf64_Phdr) > static_cast<uint64_t>(st.st_size) - eh.e_phoff)
        return Fail("loader_elf_contract", ENOEXEC);
    std::vector<Elf64_Phdr> phdrs(eh.e_phnum);
    if (!Read(file.fd, phdrs.data(), phdrs.size() * sizeof(Elf64_Phdr), eh.e_phoff)) return Fail("loader_phdr_read", EIO);
    uint64_t end = 0, alignment = page;
    bool entryFound = false, headersFound = false, dynamicFound = false;
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    for (const auto& p : phdrs) {
        if (p.p_type == PT_INTERP || p.p_type == PT_TLS || (p.p_type == PT_GNU_STACK && (p.p_flags & PF_X)))
            return Fail("loader_not_self_contained_rtld", ENOEXEC);
        if (p.p_type == PT_DYNAMIC) {
            if (dynamicFound || p.p_filesz > 65536 || p.p_filesz % sizeof(Elf64_Dyn) ||
                p.p_offset > static_cast<uint64_t>(st.st_size) || p.p_filesz > static_cast<uint64_t>(st.st_size)-p.p_offset)
                return Fail("loader_dynamic_contract", ENOEXEC);
            dynamicFound = true;
            std::vector<Elf64_Dyn> dynamic(p.p_filesz / sizeof(Elf64_Dyn));
            if (!Read(file.fd, dynamic.data(), p.p_filesz, p.p_offset)) return Fail("loader_dynamic_read", EIO);
            for (const auto& d : dynamic) {
                if (d.d_tag == DT_NULL) break;
                if (d.d_tag == DT_NEEDED || d.d_tag == DT_TEXTREL) return Fail("loader_external_dependency", ENOEXEC);
            }
        }
        if (p.p_type != PT_LOAD || !p.p_memsz) continue;
        if (p.p_filesz > p.p_memsz || p.p_offset > static_cast<uint64_t>(st.st_size) ||
            p.p_filesz > static_cast<uint64_t>(st.st_size) - p.p_offset || p.p_vaddr > 64 * 1024 * 1024 ||
            p.p_memsz > 64 * 1024 * 1024 - p.p_vaddr || (p.p_flags & (PF_W | PF_X)) == (PF_W | PF_X) ||
            !(p.p_flags & PF_R) || !PowerOfTwo(p.p_align) || p.p_align > 2 * 1024 * 1024 ||
            (p.p_offset % page) != (p.p_vaddr % page) ||
            (p.p_offset % p.p_align) != (p.p_vaddr % p.p_align)) return Fail("loader_segment_contract", ENOEXEC);
        uint64_t lo = Down(p.p_vaddr, page), hi = Up(p.p_vaddr + p.p_memsz, page);
        for (const auto& range : ranges) if (lo < range.second && hi > range.first)
            return Fail("loader_overlapping_pages", ENOEXEC);
        ranges.emplace_back(lo, hi);
        end = std::max(end, hi); alignment = std::max(alignment, p.p_align);
        entryFound |= (p.p_flags & PF_X) && eh.e_entry >= p.p_vaddr && eh.e_entry - p.p_vaddr < p.p_filesz;
        headersFound |= !p.p_offset && !p.p_vaddr && eh.e_phoff + eh.e_phnum * sizeof(Elf64_Phdr) <= p.p_filesz;
        if (p.p_memsz > p.p_filesz && !(p.p_flags & PF_W)) return Fail("loader_readonly_bss", ENOEXEC);
    }
    if (!entryFound || !headersFound || !dynamicFound || !end) return Fail("loader_missing_segments", ENOEXEC);
    void* reservation = mmap(nullptr, end + alignment, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reservation == MAP_FAILED) return Fail("loader_reserve", errno);
    uintptr_t raw = reinterpret_cast<uintptr_t>(reservation), base = Up(raw, alignment);
    if (base > raw) munmap(reservation, base - raw);
    if (base + end < raw + end + alignment) munmap(reinterpret_cast<void*>(base + end), raw + end + alignment - base - end);
    image_ = reinterpret_cast<void*>(base); imageBytes_ = end;
    for (const auto& p : phdrs) {
        if (p.p_type != PT_LOAD || !p.p_memsz) continue;
        uint64_t lo = Down(p.p_vaddr, page);
        uint64_t fileEnd = p.p_filesz ? Up(p.p_vaddr + p.p_filesz, page) : lo;
        const int protection = PROT_READ | ((p.p_flags & PF_W) ? PROT_WRITE : 0) | ((p.p_flags & PF_X) ? PROT_EXEC : 0);
        if (p.p_filesz) {
            void* address = reinterpret_cast<void*>(base + lo);
            if (nativeMemory) {
                // Z: files are readable but cannot supply executable mappings on
                // OHOS. Copy native instructions into private RW pages, then
                // remove write permission before execution. No ISA translation.
                const uint64_t offset = Down(p.p_offset, page);
                const size_t length = static_cast<size_t>(fileEnd - lo);
                if (mmap(address, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                         -1, 0) == MAP_FAILED) return Fail("loader_native_memory_map", errno);
                const size_t available = std::min<uint64_t>(length, static_cast<uint64_t>(st.st_size) - offset);
                if (!Read(file.fd, address, available, offset)) return Fail("loader_native_memory_read", EIO);
                if (protection & PROT_EXEC) __builtin___clear_cache(static_cast<char*>(address), static_cast<char*>(address) + length);
                int policyError = 0;
#if defined(__aarch64__)
                // Wine uses this OHOS per-thread operation before executable
                // permission transitions. Do not infer permission for an ELF
                // range from a successful small anonymous-page probe.
                if ((protection & PROT_EXEC) && prctl(0x6a6974, 0UL, 0UL, 0UL, 0UL)) policyError = errno;
#endif
                if (mprotect(address, length, protection)) {
                    const int protectError = errno;
                    char detail[256];
                    snprintf(detail, sizeof(detail), "STEAMOHOS_NATIVE_PROTECT address=%p length=%zu protection=%d page=%ld policy_errno=%d\n",
                             address, length, protection, nativePage, policyError);
                    diagnostics = detail;
                    return Fail("loader_native_memory_protect", protectError);
                }
            } else if (mmap(address, fileEnd - lo, protection, MAP_PRIVATE | MAP_FIXED, file.fd,
                            Down(p.p_offset, page)) == MAP_FAILED) return Fail("loader_file_mmap", errno);
        }
        if (p.p_memsz > p.p_filesz) {
            uint64_t zero = p.p_vaddr + p.p_filesz, memoryEnd = p.p_vaddr + p.p_memsz;
            // Linux padzero clears the complete tail page, including bytes
            // beyond p_memsz. rtld's early allocator uses that zeroed slack.
            if (zero < fileEnd) memset(reinterpret_cast<void*>(base + zero), 0, fileEnd - zero);
            if (Up(memoryEnd, page) > fileEnd && mmap(reinterpret_cast<void*>(base + fileEnd), Up(memoryEnd, page)-fileEnd,
                PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
                return Fail("loader_bss_mmap", errno);
        }
    }
    constexpr size_t stackDataBytes = 2 * 1024 * 1024;
    stackBytes_ = stackDataBytes + 2 * page;
    stack_ = mmap(nullptr, stackBytes_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack_ == MAP_FAILED) { stack_ = nullptr; return Fail("loader_stack_reserve", errno); }
    if (mprotect(static_cast<char*>(stack_) + page, stackDataBytes, PROT_READ | PROT_WRITE)) return Fail("loader_stack_rw", errno);
    uintptr_t cursor = reinterpret_cast<uintptr_t>(stack_) + page + stackDataBytes;
    size_t used = 0;
    auto copy = [&](const void* bytes, size_t count) -> uintptr_t {
        if (count > 16384 || used + count > 65536) return 0;
        used += count; cursor -= count; memcpy(reinterpret_cast<void*>(cursor), bytes, count); return cursor;
    };
    std::vector<uint64_t> args, env;
    for (const auto& value : argv) { auto p = copy(value.c_str(), value.size()+1); if (!p) return Fail("loader_argument_bytes", E2BIG); args.push_back(p); }
    for (const auto& value : environment) { auto p = copy(value.c_str(), value.size()+1); if (!p) return Fail("loader_environment_bytes", E2BIG); env.push_back(p); }
    File random; random.fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    unsigned char entropy[16];
    if (random.fd < 0 || !Read(random.fd, entropy, sizeof(entropy), 0)) return Fail("loader_random", EIO);
    uintptr_t randomPointer = copy(entropy, sizeof(entropy)), platformPointer = copy(platform, sizeof(platform));
    if (getuid() != geteuid() || getgid() != getegid()) return Fail("loader_privileged_identity", EPERM);
    std::vector<uint64_t> words{args.size()};
    words.insert(words.end(), args.begin(), args.end()); words.push_back(0);
    words.insert(words.end(), env.begin(), env.end()); words.push_back(0);
    auto aux = [&](uint64_t type, uint64_t value) { words.push_back(type); words.push_back(value); };
    aux(AT_PHDR, base + eh.e_phoff); aux(AT_PHENT, sizeof(Elf64_Phdr)); aux(AT_PHNUM, eh.e_phnum);
    aux(AT_PAGESZ, page); aux(AT_BASE, 0); aux(AT_FLAGS, 0); aux(AT_ENTRY, base + eh.e_entry);
    aux(AT_UID, getuid()); aux(AT_EUID, geteuid()); aux(AT_GID, getgid()); aux(AT_EGID, getegid());
    aux(AT_SECURE, 0); aux(AT_RANDOM, randomPointer); aux(AT_EXECFN, args[0]); aux(AT_PLATFORM, platformPointer);
    aux(AT_HWCAP, getauxval(AT_HWCAP)); aux(AT_HWCAP2, getauxval(AT_HWCAP2));
    aux(AT_CLKTCK, sysconf(_SC_CLK_TCK));
    if (getauxval(AT_SYSINFO_EHDR)) aux(AT_SYSINFO_EHDR, getauxval(AT_SYSINFO_EHDR));
    aux(AT_NULL, 0);
    sp_ = Down(cursor - words.size() * sizeof(uint64_t), 16);
    memcpy(reinterpret_cast<void*>(sp_), words.data(), words.size() * sizeof(uint64_t));
    entry_ = base + eh.e_entry; stage = "prepared"; return true;
}
[[noreturn]] void NativeLoaderImage::Enter() const { SteamOhosEnterNativeLoader(entry_, sp_); }
}
