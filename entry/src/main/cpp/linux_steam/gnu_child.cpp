#include "native_exec.h"
#include "xwayland_probe.h"
#include <AbilityKit/native_child_process.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <fstream>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
// Match Wine's OHOS anonymous executable-memory policy in this isolated
// supervisor only. A successful small W^X probe is not a policy guarantee
// for later ELF ranges. This never changes the ArkUI parent or creates RWX pages.
std::string PrepareExternalNativeMemory() {
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) return "{\"pageErrno\":" + std::to_string(EINVAL) + '}';
    auto probe = [page]() {
        void* memory = mmap(nullptr, static_cast<size_t>(page), PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED) return errno;
        const int error = mprotect(memory, static_cast<size_t>(page), PROT_READ | PROT_EXEC) ? errno : 0;
        munmap(memory, static_cast<size_t>(page));
        return error;
    };
    const int before = probe();
    int policyError = 0;
    if (prctl(0x6a6974, 0UL, 0UL, 0UL, 0UL)) policyError = errno;
    const int after = probe();
    return "{\"beforeProtectErrno\":" + std::to_string(before) +
        ",\"policyRequested\":true" +
        ",\"policyErrno\":" + std::to_string(policyError) +
        ",\"afterProtectErrno\":" + std::to_string(after) + '}';
}
std::string ObserveMapping(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return "{\"openErrno\":" + std::to_string(errno) + '}';
    struct stat st{}; fstat(fd, &st);
    long inodeFlags = 0;
    int flagsError = ioctl(fd, FS_IOC_GETFLAGS, &inodeFlags) ? errno : 0;
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void* view = mmap(nullptr, page, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    int hostError = view == MAP_FAILED ? errno : 0;
    if (view != MAP_FAILED) munmap(view, page);
    view = reinterpret_cast<void*>(syscall(SYS_mmap, nullptr, page, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0));
    int directError = view == MAP_FAILED ? errno : 0;
    if (view != MAP_FAILED) munmap(view, page);
    view = reinterpret_cast<void*>(syscall(SYS_mmap, nullptr, page, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_DENYWRITE, fd, 0));
    int denyWriteError = view == MAP_FAILED ? errno : 0;
    if (view != MAP_FAILED) munmap(view, page);
    Elf64_Ehdr header{};
    size_t fullImageBytes = 0;
    int fullImageError = 0;
    if (pread(fd, &header, sizeof(header), 0) == sizeof(header) && header.e_phnum <= 64 &&
        header.e_phentsize == sizeof(Elf64_Phdr)) {
        std::vector<Elf64_Phdr> phdrs(header.e_phnum);
        if (pread(fd, phdrs.data(), phdrs.size()*sizeof(Elf64_Phdr), header.e_phoff) ==
            static_cast<ssize_t>(phdrs.size()*sizeof(Elf64_Phdr))) {
            for (const auto& ph : phdrs) if (ph.p_type == PT_LOAD && ph.p_vaddr + ph.p_memsz > fullImageBytes)
                fullImageBytes = ph.p_vaddr + ph.p_memsz;
        }
    }
    if (fullImageBytes && fullImageBytes <= 64 * 1024 * 1024) {
        view = reinterpret_cast<void*>(syscall(SYS_mmap, nullptr, fullImageBytes, PROT_READ | PROT_EXEC,
                                               MAP_PRIVATE | MAP_DENYWRITE, fd, 0));
        fullImageError = view == MAP_FAILED ? errno : 0;
        if (view != MAP_FAILED) munmap(view, fullImageBytes);
    }
    close(fd);
    return "{\"bytes\":" + std::to_string(st.st_size) + ",\"mode\":" + std::to_string(st.st_mode & 0777) +
        ",\"inodeFlagsErrno\":" + std::to_string(flagsError) + ",\"inodeFlags\":" + std::to_string(inodeFlags) +
        ",\"hostMmapErrno\":" + std::to_string(hostError) + ",\"directMmapErrno\":" + std::to_string(directError) +
        ",\"denyWriteMmapErrno\":" + std::to_string(denyWriteError) +
        ",\"fullImageBytes\":" + std::to_string(fullImageBytes) +
        ",\"fullImageMmapErrno\":" + std::to_string(fullImageError) + '}';
}
// Diagnostic for native external-code loading and future CEF JIT. This runs
// only a fixed two-instruction local function, never external input or a CPU interpreter.
std::string ObserveNativeMemory() {
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void* view = mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (view == MAP_FAILED) return "{\"mapErrno\":" + std::to_string(errno) + '}';
#if defined(__aarch64__)
    const unsigned char program[]{0x40,0x05,0x80,0x52,0xc0,0x03,0x5f,0xd6}; // mov w0,42; ret
#elif defined(__x86_64__)
    const unsigned char program[]{0xb8,0x2a,0,0,0,0xc3}; // mov eax,42; ret
#endif
    memcpy(view, program, sizeof(program));
    __builtin___clear_cache(static_cast<char*>(view), static_cast<char*>(view) + sizeof(program));
    const int protectError = mprotect(view, page, PROT_READ | PROT_EXEC) ? errno : 0;
    int childExit = -1, childSignal = 0;
    if (!protectError) {
        pid_t child = fork();
        if (!child) _exit(reinterpret_cast<int(*)()>(view)() == 42 ? 0 : 1);
        if (child > 0) {
            int status = 0;
            for (unsigned n=0; n<100; ++n) {
                pid_t result = waitpid(child, &status, WNOHANG);
                if (result == child) {
                    if (WIFEXITED(status)) childExit=WEXITSTATUS(status);
                    if (WIFSIGNALED(status)) childSignal=WTERMSIG(status);
                    break;
                }
                if (result < 0 && errno != EINTR) break;
                usleep(5000);
            }
            if (childExit < 0 && !childSignal) { kill(child, SIGKILL); while (waitpid(child,&status,0)<0 && errno==EINTR) {} childSignal=SIGKILL; }
        }
    }
    munmap(view, page);
    return "{\"protectErrno\":" + std::to_string(protectError) + ",\"exitCode\":" + std::to_string(childExit) +
        ",\"signal\":" + std::to_string(childSignal) + '}';
}
// The native-lib installer uses mode 0644. Test a byte-identical copy in this
// app's private diagnostic directory with the ordinary owner execute bit.
// This changes no installed library, system policy, UID or provisioning.
struct PrivateLoader {
    std::string directory, path;
    int error = 0;
    bool bytesVerified = false;
    ~PrivateLoader() { if (!path.empty()) unlink(path.c_str()); if (!directory.empty()) rmdir(directory.c_str()); }
    bool Prepare(const std::string& source, const std::string& root = "/data/storage/el2/base/files/steamohos-native", bool executableMode = true) {
        const std::string templatePath = root + "/exec-XXXXXX";
        std::vector<char> pattern(templatePath.begin(), templatePath.end()); pattern.push_back(0);
        char* created = mkdtemp(pattern.data());
        if (!created) { error = errno; return false; }
        directory = created; path = directory + "/ld-linux-aarch64.so.1";
        int input = open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        int output = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (input < 0 || output < 0) error = errno;
        char bytes[4096], verify[4096];
        off_t offset = 0;
        while (!error) {
            ssize_t n = read(input, bytes, sizeof(bytes));
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) { error = errno; break; }
            if (!n) break;
            ssize_t sent = 0;
            while (sent < n) {
                ssize_t written = write(output, bytes + sent, static_cast<size_t>(n - sent));
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) { error = written < 0 ? errno : EIO; break; }
                sent += written;
            }
            if (error) break;
            if (pread(output, verify, static_cast<size_t>(n), offset) != n || memcmp(bytes, verify, static_cast<size_t>(n))) {
                error = EIO; break;
            }
            offset += n;
        }
        if (!error && fsync(output)) error = errno;
        if (!error && executableMode && fchmod(output, 0500)) error = errno;
        if (input >= 0) close(input);
        if (output >= 0) close(output);
        bytesVerified = !error && offset > 0;
        return bytesVerified;
    }
};
}

extern "C" __attribute__((visibility("default"))) void SteamOhosGnuChildMain(NativeChildProcess_Args args) {
    int resultFd = -1,audioFd=-1;
    for (auto* node = args.fdList.head; node; node = node->next) {
        if (node->fdName && !strcmp(node->fdName, "gnu_result")) resultFd = node->fd;
        if (node->fdName && !strcmp(node->fdName, "hmosdeck_audio")) audioFd = node->fd;
    }
    if (resultFd < 0) return;
    Dl_info info{};
    const bool signedLibraryLocated = dladdr(reinterpret_cast<void*>(&SteamOhosGnuChildMain), &info) &&
        info.dli_fname && info.dli_fname[0] == '/' && strrchr(info.dli_fname, '/');
    std::string externalBase, release;
    std::ifstream externalRootFile("/data/storage/el2/base/files/steamohos-native/external-root.path");
    std::getline(externalRootFile, externalBase);
    std::ifstream activeFile("/data/storage/el2/base/files/steamohos-native/external-active.path");
    std::getline(activeFile, release);
    const std::string releasePrefix = externalBase + "/releases/";
    const bool external = !externalBase.empty() && externalBase[0] == '/' &&
        release.compare(0, releasePrefix.size(), releasePrefix) == 0 &&
        release.size() == releasePrefix.size() + 64 &&
        release.substr(releasePrefix.size()).find_first_not_of("0123456789abcdef") == std::string::npos;
    const std::string memoryPolicy = external ? PrepareExternalNativeMemory() : "null";
    std::string json;
    // The phone's existing fork dispatcher can dlopen this signed library by
    // basename. External releases have their own verified absolute directory;
    // they do not depend on dladdr returning an absolute signed-library name.
    if (!external && !signedLibraryLocated) {
        json = "{\"schemaVersion\":1,\"status\":\"native_library_path_unavailable\",\"gnuExecutionVerified\":false}";
    } else {
        const std::string library(signedLibraryLocated ? info.dli_fname : "");
        const std::string directory = external ? release + "/runtime" : library.substr(0, library.rfind('/')) + "/gnu-native";
        const std::string loader = directory + "/ld-linux-aarch64.so.1";
        // Preserve the GNU executable's real name in the signed native-code
        // directory. Writable app data cannot supply executable mappings.
        const std::string fixture = directory + "/steamohos_gnu_fixture";
        struct stat loaderStat{};
        const bool loaderExists = stat(loader.c_str(), &loaderStat) == 0;
        const std::string mappingChecks = "{\"loader\":" + ObserveMapping(loader) +
            ",\"libc\":" + ObserveMapping(directory + "/libc.so.6") + ",\"fixture\":" + ObserveMapping(fixture) + '}';
        std::string parameterText=args.entryParams?args.entryParams:"";
        const bool forkBackend=parameterText.size()>=7&&parameterText.compare(parameterText.size()-7,7,"\nFORK=1")==0;
        if(forkBackend)parameterText.resize(parameterText.size()-7);
        const char* parameters = parameterText.c_str();
        const bool wayland = parameters && !strncmp(parameters, "wayland:", 8);
        if (parameters && !strncmp(parameters, "xwayland:", 9)) {
            json = steamohos::ProbeXwayland(directory, parameters + 9, false, external ? externalBase : "");
        } else if (parameters && !strncmp(parameters, "steam:", 6)) {
            if (!external) json = "{\"schemaVersion\":1,\"status\":\"external_runtime_missing\",\"gnuExecutionVerified\":false,\"steamClientStarted\":false}";
            else {
                std::string paths(parameters + 6),audioSocket,gamepadSocket;
                const auto audio=paths.rfind("\nAUDIO=");
                if(audio!=std::string::npos){audioSocket=paths.substr(audio+7);paths.resize(audio);}
                const auto gamepad=paths.rfind("\nGAMEPAD=");
                if(gamepad!=std::string::npos){gamepadSocket=paths.substr(gamepad+9);paths.resize(gamepad);}
                const auto separator = paths.find('\n');
                const std::string waylandSocket = paths.substr(0, separator);
                const auto second = separator == std::string::npos ? separator : paths.find('\n', separator + 1);
                const std::string vtestSocket = separator == std::string::npos ? "" : paths.substr(separator + 1, second == std::string::npos ? second : second - separator - 1);
                const std::string launch = second == std::string::npos ? "" : paths.substr(second + 1);
                json = steamohos::ProbeXwayland(directory, waylandSocket, true, externalBase, vtestSocket, launch, audioFd, audioSocket, gamepadSocket, forkBackend);
            }
        } else if (wayland) {
            const std::string socketPath(parameters + 8);
            if (socketPath.empty() || socketPath[0] != '/' || socketPath.size() >= 108) {
                json = "{\"schemaVersion\":1,\"status\":\"invalid_wayland_socket\",\"gnuExecutionVerified\":false}";
            } else {
                std::vector<std::string> environment{"LANG=C", "LC_ALL=C", "PATH=/system/bin",
                    "GLIBC_TUNABLES=glibc.pthread.rseq=0", "STEAMOHOS_WAYLAND_SOCKET=" + socketPath};
                if (external) {
                    environment.emplace_back("STEAMOHOS_NATIVE_MEMORY=1");
                    environment.emplace_back("STEAMOHOS_EXTERNAL_RUNTIME=" + directory);
                }
                const std::vector<std::string> argv{loader, "--inhibit-cache", "--library-path", directory,
                    directory + "/steamohos_gnu_wayland"};
                const auto result = steamohos::RunNativeLoader(argv, environment, 7000);
                const bool verified = result.reaped && result.hostTpUnchanged && result.exitCode == 0 &&
                    !result.setupError && !result.execError && result.output.find(
                    "\"status\":\"gnu_wayland_shm_verified\"") != std::string::npos;
                json = "{\"schemaVersion\":1,\"executionModel\":\"native_aarch64\","
                    "\"selectedBackend\":\"file_backed_gnu_rtld\",\"status\":" +
                    steamohos::JsonString(verified ? "gnu_wayland_shm_verified" : "gnu_wayland_failed") +
                    ",\"gnuExecutionVerified\":" + (verified ? "true" : "false") +
                    ",\"gnuWaylandVerified\":" + (verified ? "true" : "false") +
                    ",\"steamClientStarted\":false,\"normal\":" + steamohos::NativeExecJson(result) + '}';
            }
        } else {
        // rseq is optional in glibc and the tablet sandbox rejects syscall 293.
        std::vector<std::string> environment{"LANG=C", "LC_ALL=C", "PATH=/system/bin",
            "GLIBC_TUNABLES=glibc.pthread.rseq=0"};
        if (external) {
            environment.emplace_back("STEAMOHOS_NATIVE_MEMORY=1");
            environment.emplace_back("STEAMOHOS_EXTERNAL_RUNTIME=" + directory);
        }
        std::vector<std::string> prefix{loader, "--inhibit-cache", "--library-path", directory, fixture};
        auto normalArgs = prefix; normalArgs.emplace_back("normal");
        const auto packaged = steamohos::RunNativeExec(normalArgs, environment, 5000);
        auto normal = packaged;
        bool mappedRtld = false;
        PrivateLoader privateLoader;
        if (packaged.execError == EACCES && loaderExists && !(loaderStat.st_mode & 0111) && privateLoader.Prepare(loader)) {
            prefix[0] = privateLoader.path;
            normalArgs = prefix; normalArgs.emplace_back("normal");
            normal = steamohos::RunNativeExec(normalArgs, environment, 5000);
        }
        const auto privateEntry = normal;
        const std::string nativeMemory = ObserveNativeMemory();
        const std::string privateMapping = privateLoader.bytesVerified ? ObserveMapping(privateLoader.path) : "null";
        PrivateLoader externalLoader;
        std::string externalRoot;
        std::ifstream externalSettings("/data/storage/el2/base/files/steamohos-native/external-root.path");
        std::getline(externalSettings, externalRoot);
        std::string externalMapping = "null";
        if (!externalRoot.empty() && externalRoot[0] == '/' && externalRoot.size() < 3500 &&
            externalRoot.find("/../") == std::string::npos &&
            externalRoot.size() >= 16 && externalRoot.compare(externalRoot.size()-16, 16, "/SteamLinuxARM64") == 0 &&
            externalLoader.Prepare(loader, externalRoot, false)) externalMapping = ObserveMapping(externalLoader.path);
        if (normal.execError == EACCES) {
            // Use the installed, signed, byte-identical rtld's file-backed RX
            // pages. This does not turn writable data pages into instructions.
            prefix[0] = loader;
            normalArgs = prefix; normalArgs.emplace_back("normal");
            normal = steamohos::RunNativeLoader(normalArgs, environment, 5000);
            mappedRtld = true;
        }
        bool verified = normal.reaped && normal.hostTpUnchanged && normal.exitCode == 0 && !normal.execError && !normal.setupError &&
            normal.output.find("STEAMOHOS_GNU_NATIVE_V1 main tls pthread errno dlvsym destructors PASS\n") != std::string::npos;
        json = "{\"schemaVersion\":1,\"executionModel\":\"native_aarch64\",\"selectedBackend\":" +
            steamohos::JsonString(mappedRtld ? (external ? "native_memory_gnu_rtld" : "file_backed_gnu_rtld") : "kernel_execve") + ",\"status\":" +
            steamohos::JsonString(verified ? "gnu_main_verified" : "gnu_exec_failed") +
            ",\"gnuExecutionVerified\":" + (verified ? "true" : "false") +
            ",\"steamClientStarted\":false,\"rseqDisabled\":true,\"loaderExists\":" + (loaderExists ? "true" : "false") +
            ",\"loaderMode\":" + std::to_string(loaderExists ? (loaderStat.st_mode & 0777) : 0) +
            ",\"loaderReadable\":" + (access(loader.c_str(), R_OK) == 0 ? "true" : "false") +
            ",\"loaderExecutable\":" + (access(loader.c_str(), X_OK) == 0 ? "true" : "false") +
            ",\"fixtureReadable\":" + (access(fixture.c_str(), R_OK) == 0 ? "true" : "false") +
            ",\"mappingChecks\":" + mappingChecks +
            ",\"nativeMemoryExecution\":" + nativeMemory +
            ",\"privateLoaderMapping\":" + privateMapping +
            ",\"externalLoaderRoot\":" + steamohos::JsonString(externalRoot) +
            ",\"externalLoaderStageErrno\":" + std::to_string(externalLoader.error) +
            ",\"externalLoaderBytesVerified\":" + (externalLoader.bytesVerified ? "true" : "false") +
            ",\"externalLoaderMapping\":" + externalMapping +
            ",\"privateLoaderStageErrno\":" + std::to_string(privateLoader.error) +
            ",\"privateLoaderBytesVerified\":" + (privateLoader.bytesVerified ? "true" : "false") +
            ",\"privateLoaderExecutable\":" + (access(privateLoader.path.c_str(), X_OK) == 0 ? "true" : "false") +
            ",\"packagedEntry\":" + steamohos::NativeExecJson(packaged) +
            ",\"privateEntry\":" + steamohos::NativeExecJson(privateEntry) +
            ",\"normal\":" + steamohos::NativeExecJson(normal);
        // Do not repeat a denied execution, missing runtime or loader failure.
        if (verified) {
            auto failureArgs = prefix; failureArgs.emplace_back("exit37");
            auto timeoutArgs = prefix; timeoutArgs.emplace_back("hang");
            const auto failure = mappedRtld ? steamohos::RunNativeLoader(failureArgs, environment, 1000) :
                steamohos::RunNativeExec(failureArgs, environment, 1000);
            const auto timeout = mappedRtld ? steamohos::RunNativeLoader(timeoutArgs, environment, 300) :
                steamohos::RunNativeExec(timeoutArgs, environment, 300);
            json += ",\"exitControl\":" + steamohos::NativeExecJson(failure) +
                ",\"timeoutControl\":" + steamohos::NativeExecJson(timeout);
        }
        json += '}';
        }
    }
    if (!json.empty() && json.back() == '}') {
        json.pop_back();
        json += ",\"executableMemoryPolicy\":" + memoryPolicy + '}';
    }
    size_t sent = 0;
    while (sent < json.size()) {
        ssize_t n = send(resultFd, json.data() + sent, json.size() - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
    close(resultFd);
    if(audioFd>=0)close(audioFd);
}
