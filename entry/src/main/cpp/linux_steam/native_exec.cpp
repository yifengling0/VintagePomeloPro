#include "native_exec.h"
#include "native_loader.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

namespace steamohos {
namespace {
uint64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct ChildError { int setup, exec; };
// SIGSYS can occur after GNU rtld takes ownership of TP. This diagnostic
// handler uses only native syscall instructions, never either libc or TLS.
__attribute__((no_stack_protector)) long RawCall(long number, long a, long b, long c) {
#if defined(__aarch64__)
    register long x8 asm("x8") = number;
    register long x0 asm("x0") = a;
    register long x1 asm("x1") = b;
    register long x2 asm("x2") = c;
    asm volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
#elif defined(__x86_64__)
    long result;
    asm volatile("syscall" : "=a"(result) : "a"(number), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory", "cc");
    return result;
#endif
}
__attribute__((no_stack_protector)) void SigSys(int, siginfo_t* info, void*) {
    static const char prefix[] = "STEAMOHOS_SIGSYS syscall=";
    RawCall(SYS_write, 2, reinterpret_cast<long>(prefix), sizeof(prefix)-1);
    char number[24]; int used = 0;
    unsigned value = info ? static_cast<unsigned>(info->si_syscall) : 0;
    do { number[used++] = static_cast<char>('0' + value % 10); value /= 10; } while (value && used < 22);
    for (int i = 0; i < used / 2; ++i) { char t = number[i]; number[i] = number[used-i-1]; number[used-i-1] = t; }
    number[used++] = '\n';
    RawCall(SYS_write, 2, reinterpret_cast<long>(number), used);
    RawCall(SYS_exit_group, 159, 0, 0);
    for (;;) {}
}
bool Pipe(int (&fds)[2]) {
    if (pipe2(fds, O_CLOEXEC)) return false;
    // NCPs are not required to arrive with all three standard streams open.
    for (int& fd : fds) {
        if (fd >= 3) continue;
        int high = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        if (high < 0) return false;
        close(fd); fd = high;
    }
    return true;
}
void Drain(int fd, std::string& out, size_t limit, bool& truncated,int mirror=-1) {
    char bytes[1024];
    // Bounded even if the child continuously writes without exiting.
    for (int i = 0; i < 32; ++i) {
        ssize_t n = read(fd, bytes, sizeof(bytes));
        if (n <= 0) break;
        size_t take = std::min(static_cast<size_t>(n), limit - out.size());
        out.append(bytes, take);
        if(mirror>=0) {
            // Keep receiving live diagnostics after the result's small prefix
            // fills. Rotate this private regular file at 8 MiB; never grow it
            // without bound or retain only an unrelated startup prefix.
            constexpr off_t mirrorLimit = 8 * 1024 * 1024;
            off_t position = lseek(mirror,0,SEEK_CUR);
            if(position<0)continue;
            if(position+n>mirrorLimit && (ftruncate(mirror,0) || lseek(mirror,0,SEEK_SET)<0))continue;
            size_t offset=0;
            while(offset<static_cast<size_t>(n)) {
                ssize_t written=write(mirror,bytes+offset,static_cast<size_t>(n)-offset);
                if(written<0&&errno==EINTR)continue;
                if(written<=0)break;
                offset+=static_cast<size_t>(written);
            }
        }
        truncated |= take != static_cast<size_t>(n);
    }
}
std::string FaultRegisters(int tid, int signal) {
    // Read addresses only, never stack bytes, credentials or application data.
    uint64_t registers[34]{};
    iovec view{registers, sizeof(registers)};
    std::ostringstream text;
    text << "STEAMOHOS_NATIVE_FAULT tid=" << tid << " signal=" << signal;
    siginfo_t information{};
    if (!ptrace(PTRACE_GETSIGINFO, tid, nullptr, &information))
        text << " code=" << information.si_code << " address=0x" << std::hex << reinterpret_cast<uintptr_t>(information.si_addr);
    if (ptrace(PTRACE_GETREGSET, tid, reinterpret_cast<void*>(NT_PRSTATUS), &view) < 0)
        text << " registers_errno=" << errno;
    else {
        uintptr_t frame = 0, stack = 0, pc = 0;
#if defined(__aarch64__)
        text << " pc=0x" << std::hex << registers[32] << " lr=0x" << registers[30]
             << " sp=0x" << registers[31];
        frame = registers[29]; stack = registers[31];
        pc = registers[32];
#elif defined(__x86_64__)
        text << " pc=0x" << std::hex << registers[16] << " sp=0x" << registers[19];
        frame = registers[4]; stack = registers[19];
        pc = registers[16];
#endif
        // Anonymous external ELF has no pathname in /proc/maps. Identify its
        // nearby ELF base using only the public ELF signature, never dump code.
        const uintptr_t page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
        for (uintptr_t offset = 0; page && offset < 1024 * 1024 && offset <= pc; offset += page) {
            const uintptr_t address = (pc & ~(page - 1)) - offset;
            errno = 0;
            const unsigned long word = static_cast<unsigned long>(ptrace(PTRACE_PEEKDATA, tid,
                reinterpret_cast<void*>(address), nullptr));
            if (!errno && (word & 0xffffffffffffUL) == 0x0102464c457fUL) {
                text << " elfBase=0x" << std::hex << address << " elfOffset=0x" << pc - address;
                break;
            }
        }
        // Read only frame-chain return addresses. No stack dump or arguments.
        for (unsigned depth = 0; depth < 16 && frame >= stack && frame - stack < 8 * 1024 * 1024 &&
             frame % sizeof(uintptr_t) == 0; ++depth) {
            errno = 0;
            uintptr_t previous = static_cast<uintptr_t>(ptrace(PTRACE_PEEKDATA, tid,
                reinterpret_cast<void*>(frame), nullptr));
            if (errno) break;
            uintptr_t address = static_cast<uintptr_t>(ptrace(PTRACE_PEEKDATA, tid,
                reinterpret_cast<void*>(frame + sizeof(uintptr_t)), nullptr));
            if (errno) break;
            text << "\nframe=" << std::dec << depth << " pc=0x" << std::hex << address;
            if (previous <= frame) break;
            frame = previous;
        }
    }
    text << '\n';
    return text.str();
}
}
static NativeExecResult RunNativeProcess(const std::vector<std::string>& argv,
                               const std::vector<std::string>& environment, uint32_t timeoutMs,
                               const NativeLoaderImage* loader, const NativeExecOptions& options) {
    NativeExecResult r;
    if (argv.empty() || argv[0].empty() || argv[0][0] != '/' || (!options.persistentSession && timeoutMs < 10) || timeoutMs > 14420000) {
        r.stage = "invalid_arguments"; r.setupError = EINVAL; return r;
    }
    if (options.passFds.size() > 16) { r.stage = "invalid_pass_fds"; r.setupError = EINVAL; return r; }
    for (size_t i = 0; i < options.passFds.size(); ++i) {
        const int fd = options.passFds[i];
        if (fd < 3 || fcntl(fd, F_GETFD) < 0 ||
            std::find(options.passFds.begin(), options.passFds.begin() + i, fd) != options.passFds.begin() + i) {
            r.stage = "invalid_pass_fds"; r.setupError = EINVAL; return r;
        }
    }
    std::vector<char*> args, env;
    for (const auto& value : argv) args.push_back(const_cast<char*>(value.c_str()));
    for (const auto& value : environment) env.push_back(const_cast<char*>(value.c_str()));
    args.push_back(nullptr); env.push_back(nullptr);
    int output[2] = {-1, -1}, errors[2] = {-1, -1};
    auto closePipes = [&]() { for (int fd : {output[0], output[1], errors[0], errors[1]}) if (fd >= 0) close(fd); };
    if (!Pipe(output) || !Pipe(errors)) {
        r.stage = "pipe"; r.setupError = errno; closePipes(); return r;
    }
    // Do all allocations and descriptor discovery before fork. Its child only
    // calls async-signal-safe functions and prctl before exec/_exit.
    std::vector<int> descriptors;
    DIR* directory = opendir("/proc/self/fd");
    if (!directory) { r.stage = "descriptor_inventory"; r.setupError = errno; closePipes(); return r; }
    while (auto* item = readdir(directory)) {
        char* end = nullptr;
        long fd = strtol(item->d_name, &end, 10);
        if (end != item->d_name && !*end && fd > 2 && fd != dirfd(directory) && fd != errors[1] &&
            std::find(options.passFds.begin(), options.passFds.end(), fd) == options.passFds.end())
            descriptors.push_back(static_cast<int>(fd));
        if (descriptors.size() > 4096) break;
    }
    closedir(directory);
    if (descriptors.size() > 4096) { r.stage = "descriptor_limit"; r.setupError = EMFILE; closePipes(); return r; }
    if (fcntl(output[0], F_SETFL, O_NONBLOCK) || fcntl(errors[0], F_SETFL, O_NONBLOCK)) {
        r.stage = "nonblocking_pipe"; r.setupError = errno; closePipes(); return r;
    }
    using ForkFn = pid_t (*)();
    ForkFn forkProcess = fork;
    if (options.libcForkOnly) {
        // The phone supervisor was already forked by Wine's adapter. Replaying
        // inherited ArkUI/driver callbacks can close newly reused descriptor
        // numbers (Xwayland displayfd/listener). _Fork still resets libc's child
        // state; use it only in this dedicated, demonstrably single-thread process.
        unsigned threads=0;
        DIR* tasks=opendir("/proc/self/task");
        if(tasks) { while(auto* task=readdir(tasks))if(task->d_name[0]!='.')++threads;closedir(tasks); }
        forkProcess=reinterpret_cast<ForkFn>(dlsym(RTLD_DEFAULT,"_Fork"));
        if(threads!=1||!forkProcess) {
            r.stage="fork_policy";r.setupError=ENOTSUP;closePipes();return r;
        }
    }
    const pid_t owner = getpid();
    void* const hostTp = __builtin_thread_pointer();
    const uint64_t start = Now();
    const pid_t pid = forkProcess();
    if (pid < 0) { r.stage = "fork"; r.setupError = errno; closePipes(); return r; }
    if (!pid) {
        ChildError error{};
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != owner) error.setup = errno ? errno : ESRCH;
        if (!error.setup && (dup2(output[1], STDOUT_FILENO) < 0 || dup2(output[1], STDERR_FILENO) < 0))
            error.setup = errno;
        for (int fd : descriptors) close(fd);
        if (!error.setup && !options.workingDirectory.empty() && chdir(options.workingDirectory.c_str()))
            error.setup = errno;
        for (int fd : options.passFds) {
            int flags = fcntl(fd, F_GETFD);
            if (flags < 0 || fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) < 0) error.setup = errno;
        }
        if (!error.setup && (options.traceSyscalls || options.traceSignals)) {
            // Debug our own child only. No attach to another app or process.
            if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) < 0) error.setup = errno;
            if (!error.setup && raise(SIGSTOP)) error.setup = errno;
        }
        if (!error.setup && loader) {
            // No inherited OHOS fault handler may run with GNU-owned TLS.
            struct sigaction action{}; action.sa_handler = SIG_DFL;
            sigemptyset(&action.sa_mask);
            for (int signal = 1; signal < NSIG; ++signal) {
                if (signal == SIGKILL || signal == SIGSTOP) continue;
                if (sigaction(signal, &action, nullptr) && errno != EINVAL) { error.setup = errno; break; }
            }
            sigset_t mask; sigemptyset(&mask);
            stack_t alternate{}; alternate.ss_flags = SS_DISABLE;
            if (!error.setup && (sigprocmask(SIG_SETMASK, &mask, nullptr) || sigaltstack(&alternate, nullptr)))
                error.setup = errno;
            struct sigaction diagnostic{};
            diagnostic.sa_sigaction = SigSys; diagnostic.sa_flags = SA_SIGINFO;
            sigemptyset(&diagnostic.sa_mask);
            if (!error.setup && sigaction(SIGSYS, &diagnostic, nullptr)) error.setup = errno;
            if (!error.setup) {
                close(errors[1]);
                // One-way native branch. GNU rtld initializes its own TP; no
                // host function, callback or unwinding is allowed afterwards.
                loader->Enter();
            }
        }
        if (!error.setup) { execve(args[0], args.data(), env.data()); error.exec = errno; }
        const char* bytes = reinterpret_cast<const char*>(&error);
        size_t sent = 0;
        while (sent < sizeof(error)) {
            ssize_t n = write(errors[1], bytes + sent, sizeof(error) - sent);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            sent += static_cast<size_t>(n);
        }
        _exit(error.setup ? 126 : 127);
    }
    close(output[1]); output[1] = -1;
    close(errors[1]); errors[1] = -1;
    if (options.parentTask) options.parentTask(options.context, pid);
    int status = 0, leaderStatus = 0;
    uint64_t deadline = options.persistentSession && timeoutMs == 0 ? UINT64_MAX : start + timeoutMs;
    bool killed = false;
    bool tracingStarted = false;
    struct TraceEntry { unsigned long number = 0, pc = 0; } trace[32];
    size_t traceCount = 0;
    std::vector<int> tracees;
    if (options.traceSignals) tracees.push_back(pid);
    uint64_t nextParentPoll = 0;
    while (!r.reaped || !tracees.empty()) {
        if (options.parentPoll && Now() >= nextParentPoll) {
            nextParentPoll = Now() + 5000;
            options.parentPoll(options.context, pid, false);
        }
        Drain(output[0], r.output, std::min<uint32_t>(262144, std::max<uint32_t>(1024, options.outputLimit)), r.outputTruncated,options.diagnosticFd);
        pid_t waited = 0;
        if (options.traceSignals) {
            // Never waitpid(-1): Xwayland and other owned children have their
            // own supervisors. Only collect this traced thread group's events.
            for (size_t i = 0; i < tracees.size(); ++i) {
                waited = waitpid(tracees[i], &status, WNOHANG | __WALL);
                if (waited < 0 && errno == ECHILD && tracees[i] != pid) {
                    tracees.erase(tracees.begin() + i); waited = 0; break;
                }
                if (waited != 0) break;
            }
        } else waited = waitpid(pid, &status, WNOHANG);
        if (waited > 0) {
            if ((options.traceSyscalls || options.traceSignals) && WIFSTOPPED(status)) {
                int signal = WSTOPSIG(status);
                const unsigned event = static_cast<unsigned>(status) >> 16;
                if (!tracingStarted) {
                    const auto flags = PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL | PTRACE_O_TRACEEXEC |
                        (options.traceSignals ? PTRACE_O_TRACECLONE : 0) |
                        (options.traceForks ? PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK : 0);
                    if (ptrace(PTRACE_SETOPTIONS, waited, nullptr, reinterpret_cast<void*>(flags)) < 0) {
                        r.waitError = errno; kill(pid, SIGKILL); killed = true;
                    }
                    tracingStarted = true;
                } else if (options.traceSyscalls && signal == (SIGTRAP | 0x80)) {
                    // Ask the kernel for entry/exit classification: clone/exec
                    // events can otherwise desynchronise a userspace toggle.
                    struct SyscallInfo {
                        uint8_t operation, padding[3]; uint32_t architecture;
                        uint64_t pc, sp, number, arguments[6];
                    } information{};
                    if (ptrace(PTRACE_GET_SYSCALL_INFO,waited,
                               reinterpret_cast<void*>(sizeof(information)),&information)>=0 && information.operation==1)
                        trace[traceCount++ % 32]={static_cast<unsigned long>(information.number),static_cast<unsigned long>(information.pc)};
                }
                if (options.traceSignals && signal == SIGTRAP && (event == PTRACE_EVENT_CLONE ||
                    (options.traceForks && (event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_VFORK)))) {
                    unsigned long tid = 0;
                    if (ptrace(PTRACE_GETEVENTMSG, waited, nullptr, &tid) < 0 || !tid || tracees.size() >= 512) {
                        r.waitError = errno ? errno : EOVERFLOW; kill(pid, SIGKILL); killed = true;
                    } else if (std::find(tracees.begin(), tracees.end(), static_cast<int>(tid)) == tracees.end())
                        tracees.push_back(static_cast<int>(tid));
                }
                if (options.traceSignals && (signal == SIGSEGV || signal == SIGBUS || signal == SIGILL ||
                    signal == SIGABRT || signal == SIGSYS) && r.faultDiagnostics.size() < 4096)
                    r.faultDiagnostics += FaultRegisters(waited, signal);
                const int deliver = signal == (SIGTRAP | 0x80) || signal == SIGSTOP ||
                    (signal == SIGTRAP && event != 0) ? 0 : signal;
                const auto request = options.traceSyscalls ? PTRACE_SYSCALL : PTRACE_CONT;
                if (ptrace(request, waited, nullptr, reinterpret_cast<void*>(static_cast<intptr_t>(deliver))) < 0 && errno != ESRCH)
                    r.waitError = errno;
                // Check the deadline even under a continuous stream of stops.
                if (Now() >= deadline && !killed) {
                    r.timedOut = true; kill(pid, SIGKILL); killed = true; deadline = Now() + 1000;
                }
                continue;
            }
            if (waited == pid) { r.reaped = true; leaderStatus = status; }
            if (options.traceSignals) {
                tracees.erase(std::remove(tracees.begin(), tracees.end(), waited), tracees.end());
                continue;
            }
            break;
        }
        if (waited < 0 && errno != EINTR) { r.waitError = errno; break; }
        uint64_t now = Now();
        if(options.cancellationFd>=0 && !killed) {
            char byte;ssize_t received=recv(options.cancellationFd,&byte,1,MSG_PEEK|MSG_DONTWAIT);
            if(received>=0 || (errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)) {
                r.cancelled=true;
                if(kill(pid,SIGKILL)&&errno!=ESRCH){r.waitError=errno;break;}
                killed=true;deadline=now+1000;
            }
        }
        if (now >= deadline) {
            if (killed) { r.waitError = ETIMEDOUT; break; }
            r.timedOut = true;
            if (kill(pid, SIGKILL) && errno != ESRCH) { r.waitError = errno; break; }
            killed = true; deadline = now + 1000;
        }
        pollfd fd{output[0], POLLIN, 0};
        // HUP can remain asserted after exec failure; do not spin on it.
        if (options.traceSyscalls || options.traceSignals) {
            if (poll(&fd, 1, 0) <= 0) usleep(100);
        } else if (poll(&fd, 1, 10) > 0 && (fd.revents & POLLHUP)) poll(nullptr, 0, 10);
    }
    Drain(output[0], r.output, std::min<uint32_t>(262144, std::max<uint32_t>(1024, options.outputLimit)), r.outputTruncated,options.diagnosticFd);
    ChildError childError{};
    ssize_t got = read(errors[0], &childError, sizeof(childError));
    if (got == static_cast<ssize_t>(sizeof(childError))) {
        r.setupError = childError.setup; r.execError = childError.exec;
    } else if (got > 0) r.setupError = EPROTO;
    if (r.reaped) {
        if (WIFEXITED(leaderStatus)) r.exitCode = WEXITSTATUS(leaderStatus);
        else if (WIFSIGNALED(leaderStatus)) r.signal = WTERMSIG(leaderStatus);
    }
    if (options.traceSyscalls) {
        r.output += "\nSTEAMOHOS_SYSCALL_TRACE count=" + std::to_string(traceCount) + "\n";
        for (size_t n = traceCount > 32 ? traceCount - 32 : 0; n < traceCount; ++n) {
            const auto& item = trace[n % 32];
            std::ostringstream line; line << "syscall=" << item.number << " pc=0x" << std::hex << item.pc << '\n';
            r.output += line.str();
        }
    }
    r.stage = !r.reaped ? "reap_unverified" : r.setupError ? "child_setup" : r.execError ? "execve" :
              r.cancelled ? "cancelled" : r.timedOut ? "timeout" : r.signal ? "signal" : r.exitCode ? "program_exit" : "completed";
    r.elapsedMs = Now() - start;
    r.hostTpUnchanged = __builtin_thread_pointer() == hostTp;
    closePipes();
    if (options.parentPoll) options.parentPoll(options.context, pid, true);
    return r;
}
NativeExecResult RunNativeExec(const std::vector<std::string>& argv,
                               const std::vector<std::string>& environment, uint32_t timeoutMs, const NativeExecOptions& options) {
    return RunNativeProcess(argv, environment, timeoutMs, nullptr, options);
}
NativeExecResult RunNativeLoader(const std::vector<std::string>& argv,
                                 const std::vector<std::string>& environment, uint32_t timeoutMs, const NativeExecOptions& options) {
    NativeLoaderImage loader;
    if (!loader.Prepare(argv, environment)) {
        NativeExecResult result; result.stage = loader.stage; result.setupError = loader.error;
        result.faultDiagnostics = loader.diagnostics; return result;
    }
    return RunNativeProcess(argv, environment, timeoutMs, &loader, options);
}
std::string JsonString(const std::string& value) {
    std::ostringstream s; s << '"';
    const char hex[] = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') s << '\\' << static_cast<char>(c);
        else if (c < 32 || c >= 127) s << "\\u00" << hex[c >> 4] << hex[c & 15];
        else s << static_cast<char>(c);
    }
    s << '"'; return s.str();
}
std::string NativeExecJson(const NativeExecResult& r) {
    std::ostringstream s;
    s << "{\"stage\":" << JsonString(r.stage) << ",\"setupErrno\":" << r.setupError
      << ",\"execErrno\":" << r.execError << ",\"waitErrno\":" << r.waitError
      << ",\"exitCode\":" << r.exitCode << ",\"signal\":" << r.signal
      << ",\"timedOut\":" << (r.timedOut ? "true" : "false")
      << ",\"cancelled\":" << (r.cancelled ? "true" : "false")
      << ",\"childReaped\":" << (r.reaped ? "true" : "false")
      << ",\"hostTpUnchanged\":" << (r.hostTpUnchanged ? "true" : "false")
      << ",\"outputTruncated\":" << (r.outputTruncated ? "true" : "false")
      << ",\"elapsedMs\":" << r.elapsedMs << ",\"faultDiagnostics\":" << JsonString(r.faultDiagnostics)
      << ",\"output\":" << JsonString(r.output) << '}';
    return s.str();
}
}
