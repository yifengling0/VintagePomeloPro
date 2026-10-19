#include "xwayland_probe.h"
#include "native_exec.h"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
#include <signal.h>
#include <algorithm>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace steamohos {
namespace {
uint64_t Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::string ReadHelperLog(const std::string& path) {
    int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return "{\"readErrno\":"+std::to_string(errno)+"}";
    struct stat status{};
    if(fstat(fd,&status)) {int error=errno;close(fd);return "{\"readErrno\":"+std::to_string(error)+"}";}
    if(!S_ISREG(status.st_mode)) {close(fd);return "{\"readErrno\":"+std::to_string(EINVAL)+"}";}
    constexpr size_t limit=32768;
    off_t offset=status.st_size>static_cast<off_t>(limit)?status.st_size-limit:0;
    char head[16384];ssize_t headCount=offset>0?pread(fd,head,sizeof(head),0):0;
    char bytes[limit];ssize_t count=pread(fd,bytes,sizeof(bytes),offset);int error=count<0?errno:0;close(fd);
    return "{\"readErrno\":"+std::to_string(error)+",\"mtimeSeconds\":"+std::to_string(status.st_mtime)+
        ",\"head\":"+JsonString(headCount>0?std::string(head,static_cast<size_t>(headCount)):"")+
        ",\"fromOffset\":"+std::to_string(offset)+",\"output\":"+
        JsonString(count>0?std::string(bytes,static_cast<size_t>(count)):"")+"}";
}

// This file stays in the app's private directory. It lets HDC collect bounded
// connection/CEF evidence while the external Z: session is still running.
// Raw client logs may contain account information and are not public telemetry.
bool WritePrivateSnapshot(const std::string& path, const std::string& text) {
    if (text.size() > 1024 * 1024) { errno = EFBIG; return false; }
    const std::string temporary = path + ".tmp";
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0) return false;
    struct stat status{};
    bool valid = !fstat(fd, &status) && S_ISREG(status.st_mode) && status.st_uid == geteuid();
    if (valid) valid = !fchmod(fd, 0600) && !ftruncate(fd, 0);
    size_t offset = 0;
    while (valid && offset < text.size()) {
        ssize_t count = write(fd, text.data() + offset, text.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { valid = false; break; }
        offset += static_cast<size_t>(count);
    }
    if (close(fd)) valid = false;
    if (valid && !rename(temporary.c_str(), path.c_str())) return true;
    unlink(temporary.c_str());
    return false;
}
std::string LayoutFailure;
bool EnsureDirectory(const std::string& path) {
    LayoutFailure = "directory:" + path;
    struct stat st{};
    if (!lstat(path.c_str(), &st)) { if (S_ISDIR(st.st_mode)) return true; errno = ENOTDIR; return false; }
    return errno == ENOENT && !mkdir(path.c_str(), 0700);
}
void CleanupSocketMetadata(const std::string& sessionDirectory) {
    const std::string path=sessionDirectory+"/socket-metadata";
    int fd=open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return;
    DIR* entries=fdopendir(fd);
    if(!entries){close(fd);return;}
    while(auto* entry=readdir(entries)) {
        const std::string name=entry->d_name;
        if(name.empty()||name.size()>10||name.find_first_not_of("0123456789")!=std::string::npos)continue;
        struct stat status{};
        if(!fstatat(dirfd(entries),name.c_str(),&status,AT_SYMLINK_NOFOLLOW)&&
            S_ISSOCK(status.st_mode)&&status.st_uid==geteuid())
            unlinkat(dirfd(entries),name.c_str(),0);
    }
    closedir(entries);rmdir(path.c_str());
}
struct Session {
    int listener = -1, readyRead = -1, readyWrite = -1;
    std::string directory, socket;
    bool ready = false;
    int readyError = 0;
    NativeExecResult client, ipcPreflight, xlibPreflight, glxPreflight, loaderPreflight, connectionIdentityPreflight;
    NativeExecOptions clientOptions;
    std::vector<std::string> argv, environment;
    std::string nativeDirectory;
    std::string clientLogDirectory;
    bool steamClient = false;
    uint32_t clientTimeout = 15000;
    bool traceSyscalls = false;
    bool traceSignals = false;
    bool graphicsLoaderDiagnostic = false;
    bool graphicsSelfTest = false;
    bool connectionIdentityOnly = false;
    bool steamClientReadOnlyProbe = false;
    bool steamClientGameBridge = false;
    bool steamClientRpcProbe = false;
    bool steamClientSourceProbe = false;
    bool socketMetadata = false;
    bool desktopSession = false;
    bool desktopBounds = false;
    bool desktopFullscreen = false;
    bool desktopInputProbe = false;
    bool wineWindowHost = false;
    std::string desktopGeometry = "1280x800";
    bool softwareGL = false;
    bool sharedVirpipe = false;
    ~Session() {
        for (int fd : {listener, readyRead, readyWrite}) if (fd >= 0) close(fd);
        if (!socket.empty()) unlink(socket.c_str());
        if(!directory.empty())unlink((directory+"/asound.conf").c_str());
        // This generated session owns the namespace, including endpoints left
        // by GNU children using _exit or killed by the bounded diagnostic.
        if(!directory.empty())CleanupSocketMetadata(directory);
        // Xwayland removes its generated XKM; only our private session files live here.
        if (!directory.empty()) rmdir(directory.c_str());
    }
    static void SnapshotClient(void* data, int childPid, bool finished) {
        auto& session = *static_cast<Session*>(data);
        std::string json = std::string(R"({"schemaVersion":1,"childPid":)") + std::to_string(childPid) +
            R"(,"finished":)" + (finished ? "true" : "false") +
            R"(,"monotonicMs":)" + std::to_string(Now());
        for (const auto& log : std::vector<std::pair<std::string, std::string>>{
            {"connectionLog", "connection_log.txt"}, {"consoleLog", "console_log.txt"},
            {"webhelperLog", "steamwebhelper.log"}, {"cefLog", "cef_log.txt"},
            {"webhelperDetailLog", "webhelper.txt"}, {"cloudLog", "cloud_log.txt"}}) {
            json += ',' + JsonString(log.first) + ':' + ReadHelperLog(session.clientLogDirectory + '/' + log.second);
        }
        json += '}';
        WritePrivateSnapshot("/data/storage/el2/base/files/steamohos-native/client-live.private.json", json);
    }
    static void RunClient(void* data, int serverPid) {
        auto& session = *static_cast<Session*>(data);
        // The private server has -noreset for preflights. Once Steam exits,
        // stop it so a crash returns its real result without waiting 15 min.
        struct StopPrivateServer {
            int pid;
            bool enabled;
            ~StopPrivateServer() { if (enabled && pid > 0) kill(pid, SIGTERM); }
        } stopServer{serverPid, session.steamClient};
        close(session.readyWrite); session.readyWrite = -1;
        std::string number;
        const uint64_t deadline = Now() + 2500;
        while (Now() < deadline) {
            pollfd fd{session.readyRead, POLLIN, 0};
            int code = poll(&fd, 1, 50);
            if (code < 0 && errno == EINTR) continue;
            if (code < 0) { session.readyError = errno; break; }
            if (!code) continue;
            char bytes[16];
            ssize_t n = read(session.readyRead, bytes, sizeof(bytes));
            if (n > 0) {
                number.append(bytes, static_cast<size_t>(n));
                if (number.find('\n') != std::string::npos) break;
                if (number.size() > 16) { session.readyError = EPROTO; break; }
            } else if (!n) { session.readyError = EPIPE; break; }
            else if (errno != EAGAIN && errno != EINTR) { session.readyError = errno; break; }
        }
        session.ready = number == "57\n";
        if (!session.ready) { if (!session.readyError) session.readyError = ETIMEDOUT; return; }
        if (session.steamClient) {
            const auto prefix = std::vector<std::string>{session.nativeDirectory + "/ld-linux-aarch64.so.1",
                "--inhibit-cache", "--library-path", session.nativeDirectory};
            if(session.connectionIdentityOnly) {
                auto identityArgs=prefix;identityArgs.push_back(session.nativeDirectory+"/steamohos_gnu_connection_identity");
                session.connectionIdentityPreflight=RunNativeLoader(identityArgs,session.environment,5000,session.clientOptions);
                return;
            }
            auto ipcArgs = prefix; ipcArgs.push_back(session.nativeDirectory + "/steamohos_gnu_sysvipc");
            session.ipcPreflight = RunNativeLoader(ipcArgs, session.environment, 1800, session.clientOptions);
            if (session.ipcPreflight.exitCode != 0 || session.ipcPreflight.output.find("PASS SysV") == std::string::npos) return;
            if(session.graphicsLoaderDiagnostic) {
                auto loaderArgs=prefix;
                loaderArgs[3]+=":"+session.nativeDirectory+"/steamrtarm64";
                loaderArgs.push_back(session.nativeDirectory+"/steamohos_gnu_graphics_loader");
                session.loaderPreflight=RunNativeLoader(loaderArgs,session.environment,3000,session.clientOptions);
            }
            // Visible color/readback fixtures are diagnostics, not part of normal startup.
            if (session.graphicsSelfTest || session.graphicsLoaderDiagnostic) {
                auto xlibArgs = prefix; xlibArgs.push_back(session.nativeDirectory + "/steamohos_gnu_xlib");
                session.xlibPreflight = RunNativeLoader(xlibArgs, session.environment, 4500, session.clientOptions);
                if (session.xlibPreflight.exitCode != 0 || session.xlibPreflight.output.find("xlib_display_verified") == std::string::npos) return;
                if (session.softwareGL) {
                    auto glxArgs=prefix;glxArgs.push_back(session.nativeDirectory+"/steamohos_gnu_glx");
                    session.glxPreflight=RunNativeLoader(glxArgs,session.environment,5000,session.clientOptions);
                    if(session.glxPreflight.exitCode!=0 || session.glxPreflight.output.find("native_glx_verified")==std::string::npos)return;
                    if(session.sharedVirpipe && session.glxPreflight.output.find("renderer=virgl")==std::string::npos)return;
                }
            }
        }
        if (session.steamClient) session.environment.emplace_back("STEAMOHOS_EXEC_PATH=" + session.argv[4]);
        session.clientOptions.traceSyscalls = session.traceSyscalls;
        session.clientOptions.traceSignals = session.traceSignals;
        const int live= session.steamClient?open("/data/storage/el2/base/files/steamohos-native/client-native.private.log",O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600):-1;
        session.clientOptions.diagnosticFd=live;
        session.clientOptions.parentPoll = session.steamClient ? SnapshotClient : nullptr;
        session.clientOptions.context = &session;
        session.client = RunNativeLoader(session.argv, session.environment, session.clientTimeout, session.clientOptions);
        if(live>=0)close(live);
        session.clientOptions.diagnosticFd=-1;
    }
};
bool ReadClientSettings(const std::string& path, Session& session) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT;
    struct stat st{};
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > 4096) { close(fd); errno = EINVAL; return false; }
    char bytes[4097];
    ssize_t count = read(fd, bytes, 4096); close(fd);
    if (count < 0) return false;
    std::istringstream stream(std::string(bytes, static_cast<size_t>(count)));
    std::string line;
    const std::vector<std::string> allowed{"-cef-disable-gpu", "-cef-disable-gpu-compositing",
        "-cef-verbose-logging", "-cef-disable-breakpad", "-cef-disable-occlusion", "-cef-disable-sandbox", "-gamepadui"};
    unsigned arguments = 0;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.compare(0, 4, "arg=") == 0) {
            const std::string value = line.substr(4);
            if (++arguments > 8 || std::find(allowed.begin(), allowed.end(), value) == allowed.end()) { errno = EINVAL; return false; }
            session.argv.push_back(value);
        } else if (line.compare(0,12,"launchAppId=")==0) {
            const std::string appId=line.substr(12);
            if(appId.empty()||appId.size()>10||appId[0]=='0'||
               appId.find_first_not_of("0123456789")!=std::string::npos||
               std::strtoull(appId.c_str(),nullptr,10)>4294967295ULL||
               std::find(session.argv.begin(),session.argv.end(),"-applaunch")!=session.argv.end()) {
                errno=EINVAL;return false;
            }
            // Explicit research launch through the original Linux Steam client,
            // including its pre-launch Cloud sync and normal compatibility tool.
            session.argv.emplace_back("-applaunch");session.argv.push_back(appId);
        } else if (line == "mesaDriver=virpipe") {
            session.sharedVirpipe = true;
            session.softwareGL = true;
        } else if (line == "softwareOpenGL=1") {
            session.softwareGL = true;
        } else if (line == "graphicsSelfTest=1") {
            session.graphicsSelfTest = true;
        } else if (line == "graphicsLoaderDiagnostic=1") {
            session.graphicsLoaderDiagnostic = true;
        } else if (line == "steamClientBridge=experimental") {
            session.steamClientGameBridge = true;
        } else if (line == "steamClientProbe=read-only-rpc") {
            session.steamClientReadOnlyProbe = true;
            session.steamClientRpcProbe = true;
        } else if (line == "steamClientProbe=source-identity") {
            session.steamClientReadOnlyProbe = true;
            session.steamClientSourceProbe = true;
        } else if (line == "steamClientProbe=read-only") {
            session.steamClientReadOnlyProbe = true;
        } else if (line == "connectionIdentityOnly=1") {
            session.connectionIdentityOnly = true;
        } else if (line == "socketMetadata=1") {
            session.socketMetadata = true;
        } else if (line == "wineWindowHost=1") {
            session.wineWindowHost = true;
        } else if (line == "desktopFullscreen=1") {
            session.desktopFullscreen = true;
        } else if (line == "desktopSession=bounds") {
            session.desktopSession = true;
            session.desktopBounds = true;
        } else if (line == "desktopSession=observe") {
            session.desktopSession = true;
        } else if (line == "desktopSession=input-probe") {
            session.desktopSession = true;
            session.desktopInputProbe = true;
        } else if (line.compare(0, 16, "desktopGeometry=") == 0) {
            const std::string value=line.substr(16);
            const size_t separator=value.find('x');
            if(separator==std::string::npos || separator<3 || separator>4 ||
                value.size()-separator-1<3 || value.size()-separator-1>4 ||
                value.substr(0,separator).find_first_not_of("0123456789")!=std::string::npos ||
                value.substr(separator+1).find_first_not_of("0123456789")!=std::string::npos) {errno=EINVAL;return false;}
            const auto width=strtoul(value.substr(0,separator).c_str(),nullptr,10);
            const auto height=strtoul(value.substr(separator+1).c_str(),nullptr,10);
            if(width<640 || width>4096 || height<360 || height>4096 || width*height>8388608) {errno=EINVAL;return false;}
            session.desktopGeometry=value;
        } else if (line == "diagnosticSyscalls=1") {
            session.traceSyscalls = true;
        } else if (line == "diagnosticSignals=1") {
            session.traceSignals = true;
        } else if (line.compare(0, 10, "timeoutMs=") == 0) {
            const std::string value = line.substr(10);
            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos || value.size() > 8) { errno = EINVAL; return false; }
            const auto timeout = strtoul(value.c_str(), nullptr, 10);
            if (timeout < 1000 || timeout > 14400000) { errno = EINVAL; return false; }
            session.clientTimeout = static_cast<uint32_t>(timeout);
        } else { errno = EINVAL; return false; }
    }
    return true;
}
}
std::string ProbeXwayland(const std::string& directory, const std::string& socketPath, bool steamClient, const std::string& externalRoot, const std::string& vtestSocket, const std::string& launch, int audioFd, const std::string& audioSocket, const std::string& gamepadSocket, bool forkBackend) {
    if (socketPath.empty() || socketPath[0] != '/' || socketPath.size() >= sizeof(sockaddr_un::sun_path))
        return "{\"status\":\"invalid_wayland_socket\",\"gnuExecutionVerified\":false}";
    const std::string base = "/data/storage/el2/base/files/steamohos-native";
    const std::string xkb = (externalRoot.empty() ? base : externalRoot) + "/xkb-data";
    const std::string xlocale = (externalRoot.empty() ? base : externalRoot) + "/x11-locale";
    if (access((xkb + "/rules/evdev").c_str(), R_OK))
        return "{\"status\":\"xkb_data_missing\",\"gnuExecutionVerified\":false}";
    Session session;
    session.nativeDirectory = directory; session.steamClient = steamClient;
    session.clientLogDirectory = (externalRoot.empty() ? base : externalRoot) + "/client/.steam/steam/logs";
    if(steamClient)session.clientOptions.outputLimit=131072;
    char pattern[] = "/data/storage/el2/base/files/steamohos-native/x11-XXXXXX";
    char* created = mkdtemp(pattern);
    if (!created) return "{\"status\":\"session_directory_failed\",\"gnuExecutionVerified\":false}";
    session.directory = created; session.socket = session.directory + "/x11";
    session.listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    if (session.listener < 0 || session.socket.size() >= sizeof(address.sun_path))
        return "{\"status\":\"listen_socket_failed\",\"gnuExecutionVerified\":false}";
    strcpy(address.sun_path, session.socket.c_str());
    if (bind(session.listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(session.listener, 4))
        return "{\"status\":\"listen_failed\",\"gnuExecutionVerified\":false}";
    int ready[2];
    if (pipe2(ready, O_CLOEXEC | O_NONBLOCK))
        return "{\"status\":\"ready_pipe_failed\",\"gnuExecutionVerified\":false}";
    session.readyRead = ready[0]; session.readyWrite = ready[1];
    session.environment = {"LANG=C", "LC_ALL=C", "PATH=/system/bin", "GLIBC_TUNABLES=glibc.pthread.rseq=0",
        "HOME=" + session.directory, "TMPDIR=" + session.directory, "XDG_RUNTIME_DIR=" + session.directory,
        "XKB_CONFIG_ROOT=" + xkb, "STEAMOHOS_XKBCOMP_LIBRARY=" + directory + "/libsteamohos_xkbcomp.so",
        "XLOCALEDIR=" + xlocale,
        "WAYLAND_DISPLAY=" + socketPath, "STEAMOHOS_X11_SOCKET=" + session.socket};
    const std::string loader = directory + "/ld-linux-aarch64.so.1";
    session.argv = {loader, "--inhibit-cache", "--library-path", directory, directory + "/steamohos_gnu_x11"};
    std::vector<std::string> serverArgs{loader, "--inhibit-cache", "--library-path", directory,
        directory + "/Xwayland", ":57", "-xkbdir", xkb, "-listenfd", std::to_string(session.listener), "-displayfd",
        std::to_string(session.readyWrite), "-shm", "-geometry", "640x360", "-nolisten", "tcp",
        "-ac", "-extension", "MIT-SHM", steamClient ? "-noreset" : "-terminate", "-verbose", "0"};
    if (!externalRoot.empty()) {
        session.environment.emplace_back("STEAMOHOS_NATIVE_MEMORY=1");
        session.environment.emplace_back("STEAMOHOS_EXTERNAL_RUNTIME=" + directory);
    }
    const auto serverEnvironment = session.environment;
    if (steamClient) {
        const std::string home = (externalRoot.empty() ? base : externalRoot) + "/client";
        if (mkdir(home.c_str(), 0700) && errno != EEXIST)
            return "{\"status\":\"steam_home_failed\",\"gnuExecutionVerified\":false}";
        const std::string ipc = base + "/client-ipc";
        if (mkdir(ipc.c_str(), 0700) && errno != EEXIST)
            return "{\"status\":\"steam_ipc_root_failed\",\"gnuExecutionVerified\":false}";
        const std::string steamRoot = home + "/.steam/steam";
        if (!EnsureDirectory(home + "/.steam") || !EnsureDirectory(steamRoot))
            return "{\"status\":\"steam_layout_failed\",\"layoutStage\":" + JsonString(LayoutFailure) +
                ",\"layoutErrno\":" + std::to_string(errno) + ",\"gnuExecutionVerified\":false}";
        session.environment.emplace_back("DISPLAY=:57");
        session.environment.emplace_back("LD_PRELOAD=" + directory + "/libsteamohos_posix.so");
        session.environment.emplace_back("STEAMOHOS_IPC_ROOT=" + ipc);
        session.environment.emplace_back("STEAMOHOS_STEAM_ROOT=" + steamRoot);
        session.environment.emplace_back("STEAMOHOS_CODE_ROOT=" + directory);
        // This diagnostic session launches the existing host prefix, not GNU Wine.
        session.environment.emplace_back("STEAMOHOS_WINE_PREFIX=" + base.substr(0, base.rfind('/')) + "/.wine");
        session.environment.emplace_back("STEAMOHOS_LIBRARY_ROOT=" + externalRoot + "/library");
        session.environment.emplace_back("STEAMOHOS_Z_ROOT=" + externalRoot.substr(0, externalRoot.rfind('/')));
        if (!launch.empty()) {
            const auto separator = launch.find('\n');
            if (separator == std::string::npos || launch.size() - separator - 1 != 64)
                return "{\"status\":\"invalid_launch_endpoint\",\"gnuExecutionVerified\":false}";
            session.environment.emplace_back("STEAMOHOS_LAUNCH_SOCKET=" + launch.substr(0, separator));
            session.environment.emplace_back("STEAMOHOS_LAUNCH_NONCE=" + launch.substr(separator + 1));
        }
        // Bootstrap is a capability passed by the host; no global socket or GNU
        // dlopen of OHOS libraries. Children share it but send their own control FD.
        if(audioFd>=0 && !access((directory+"/libasound_module_pcm_hmosdeck.so").c_str(),R_OK)) {
            const char config[]="pcm_type.hmosdeck { lib \"libasound_module_pcm_hmosdeck.so\" }\n"
                "pcm.hmosdeck { type hmosdeck hint { show on description \"HmosDeck OHOS audio\" } }\n"
                "pcm.!default { type plug slave.pcm \"hmosdeck\" hint { show on description \"HmosDeck OHOS audio\" } }\n";
            const std::string configPath=session.directory+"/asound.conf";
            int configFd=open(configPath.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);
            if(configFd<0)return "{\"status\":\"audio_config_failed\",\"gnuExecutionVerified\":false}";
            ssize_t written=write(configFd,config,sizeof(config)-1);close(configFd);
            if(written!=sizeof(config)-1)return "{\"status\":\"audio_config_failed\",\"gnuExecutionVerified\":false}";
            session.clientOptions.passFds.push_back(audioFd);
            session.environment.emplace_back("STEAMOHOS_AUDIO_BOOTSTRAP_FD="+std::to_string(audioFd));
            session.environment.emplace_back("ALSA_CONFIG_PATH="+configPath);
            session.environment.emplace_back("ALSA_PLUGIN_DIR="+directory);
            session.environment.emplace_back("SDL_AUDIO_DRIVER=alsa");
            struct stat endpoint{};
            if(audioSocket.compare(0,base.size()+5,base+"/aud-")==0&&audioSocket.size()<sizeof(sockaddr_un::sun_path)&&
               audioSocket.find_first_of("\r\n")==std::string::npos&&!lstat(audioSocket.c_str(),&endpoint)&&
               S_ISSOCK(endpoint.st_mode)&&endpoint.st_uid==geteuid())
                session.environment.emplace_back("STEAMOHOS_AUDIO_BOOTSTRAP_SOCKET="+audioSocket);
        }
        struct stat gamepadEndpoint{};
        if(!gamepadSocket.empty()&&gamepadSocket[0]=='/'&&gamepadSocket.size()<sizeof(sockaddr_un::sun_path)&&
           gamepadSocket.find_first_of("\r\n")==std::string::npos&&!lstat(gamepadSocket.c_str(),&gamepadEndpoint)&&
           S_ISSOCK(gamepadEndpoint.st_mode)&&gamepadEndpoint.st_uid==geteuid()&&
           !access((directory+"/libsteamohos_gamepad.so").c_str(),R_OK)) {
            session.environment.emplace_back("STEAMOHOS_GAMEPAD_SOCKET="+gamepadSocket);
            for(auto& value:session.environment)if(value.compare(0,11,"LD_PRELOAD=")==0)value+=":"+directory+"/libsteamohos_gamepad.so";
        }
        session.environment.emplace_back("STEAM_RUNTIME=0");
        session.environment.emplace_back("SSL_CERT_FILE=" + steamRoot + "/steamohos/ca-certificates.crt");
        for (auto& value : session.environment)
            if (value.compare(0, 5, "PATH=") == 0) value = "PATH=" + directory + "/steamrtarm64:/system/bin";
        const std::string dumps = base + "/client-dumps";
        if (!EnsureDirectory(dumps))
            return "{\"status\":\"steam_dump_directory_failed\",\"gnuExecutionVerified\":false}";
        session.environment.emplace_back("BREAKPAD_DUMP_LOCATION=" + dumps);
        const std::string temporary = base + "/client-tmp";
        if (!EnsureDirectory(temporary))
            return "{\"status\":\"steam_tmp_directory_failed\",\"gnuExecutionVerified\":false}";
        session.environment.emplace_back("STEAMOHOS_TMP_ROOT=" + temporary);
        for (auto& value : session.environment)
            if (value.compare(0, 7, "TMPDIR=") == 0) value = "TMPDIR=" + temporary;
        for (auto& value : session.environment)
            if (value.compare(0, 5, "HOME=") == 0) value = "HOME=" + home;
        session.clientOptions.workingDirectory = steamRoot;
        session.environment.emplace_back("XDG_CONFIG_HOME=" + home + "/config");
        session.environment.emplace_back("XDG_DATA_HOME=" + home + "/data");
        session.argv = {loader, "--inhibit-cache", "--library-path", directory + ":" + directory + "/steamrtarm64:" + directory + "/steamrtarm64/libs:" + directory + "/steamrtarm64/video",
            directory + "/steamrtarm64/steam", "-nobootstrapperupdate", "-skipinitialbootstrap",
            "-noverifyfiles", "-norepairfiles", "-nocrashdialog", "-no-cef-sandbox"};
        if (!ReadClientSettings(base + "/client-launch.conf", session))
            return "{\"status\":\"steam_launch_settings_invalid\",\"gnuExecutionVerified\":false}";
        // The original login window is 700x440; use a usable desktop rather
        // than the small protocol-fixture geometry. No client window is resized.
        auto geometry=std::find(serverArgs.begin(),serverArgs.end(),"-geometry");
        if(geometry!=serverArgs.end() && geometry+1!=serverArgs.end())*(geometry+1)=session.desktopGeometry;
        // Rootful Xwayland requests its own Wayland fullscreen surface. Keep
        // the tested standalone host instead of routing through Wine desktop.
        if(session.desktopFullscreen)serverArgs.emplace_back("-fullscreen");
        if(session.socketMetadata) {
            session.environment.emplace_back("STEAMOHOS_SOCKET_METADATA=1");
            session.environment.emplace_back("STEAMOHOS_SOCKET_METADATA_ROOT="+session.directory);
        }
        if(session.traceSignals) session.environment.emplace_back("STEAMOHOS_MODULE_DIAGNOSTICS=1");
        if(session.steamClientSourceProbe)
            session.environment.emplace_back("STEAMOHOS_STEAMCLIENT_SOURCE_TEST=1");
        if(session.steamClientRpcProbe)
            session.environment.emplace_back("STEAMOHOS_STEAMCLIENT_RPC=1");
        if(session.steamClientGameBridge)
            session.environment.emplace_back("STEAMOHOS_STEAMCLIENT_GAME_BRIDGE=1");
        if(session.steamClientReadOnlyProbe||session.steamClientGameBridge)
            session.argv.insert(session.argv.begin()+4,directory+"/steamohos_gnu_steam_session");
        // Full-screen runs without an X11 window supervisor. Client-owned
        // mapped/parked windows must not be moved back into the viewport.
        // Bounds enforcement remains an explicit window-mode diagnostic.
        if(session.desktopSession&&!session.desktopBounds)
            session.environment.emplace_back("STEAMOHOS_DESKTOP_OBSERVE=1");
        if (std::find(session.argv.begin(),session.argv.end(),"-gamepadui")!=session.argv.end())
            session.environment.emplace_back("STEAMOHOS_DISABLE_CAPTION_DRAG=1");
        if(session.desktopBounds)session.environment.emplace_back("STEAMOHOS_WINDOW_BOUNDS=1");
        if(session.desktopSession) {
            session.argv.insert(session.argv.begin()+4,directory+"/steamohos_gnu_desktop");
            if(session.desktopInputProbe)session.environment.emplace_back("STEAMOHOS_DESKTOP_INPUT=1");
        }
        if(session.wineWindowHost) {
            session.environment.emplace_back("STEAMOHOS_WINE_WINDOW_BRIDGE=1");
            if(!session.desktopSession)session.argv.insert(session.argv.begin()+4,directory+"/steamohos_gnu_desktop");
        }
        if(session.softwareGL) {
            for(auto& value:session.environment)if(value.compare(0,11,"LD_PRELOAD=")==0)value+=":"+directory+"/libsteamohos_software_gl.so";
            session.environment.emplace_back("STEAMOHOS_SOFTWARE_GL=1");
            session.environment.emplace_back("LIBGL_ALWAYS_SOFTWARE=1");
            session.environment.emplace_back("XLIB_NO_SHM=1");
            if(session.sharedVirpipe) {
                struct stat receiver{};
                if(vtestSocket.empty() || vtestSocket[0]!='/' || vtestSocket.size()>=sizeof(sockaddr_un::sun_path) ||
                    vtestSocket.find_first_of("\r\n")!=std::string::npos ||
                    stat(vtestSocket.c_str(),&receiver) || !S_ISSOCK(receiver.st_mode))
                    return "{\"status\":\"shared_vtest_receiver_unavailable\",\"gnuExecutionVerified\":false}";
                session.environment.emplace_back("GALLIUM_DRIVER=virpipe");
                session.environment.emplace_back("VTEST_SOCKET_NAME="+vtestSocket);
            } else session.environment.emplace_back("GALLIUM_DRIVER=softpipe");
        }
    }
    NativeExecOptions options;
    options.libcForkOnly=forkBackend;
    session.clientOptions.libcForkOnly=forkBackend;
    options.passFds = {session.listener, session.readyWrite};
    options.parentTask = Session::RunClient; options.context = &session;
    // Allow readiness and bounded preflights before the client's own budget.
    const auto server = RunNativeLoader(serverArgs, serverEnvironment, steamClient ? session.clientTimeout + 20000 : 9000, options);
    const auto& client = session.client;
    const bool verified = !steamClient && session.ready && server.reaped && server.exitCode == 0 && !server.timedOut &&
        server.hostTpUnchanged && client.reaped && client.exitCode == 0 && client.hostTpUnchanged &&
        client.output.find("\"status\":\"x11_drawable_verified\"") != std::string::npos;
    return "{\"schemaVersion\":1,\"executionModel\":\"native_aarch64\",\"selectedBackend\":" +
        JsonString(externalRoot.empty() ? "file_backed_gnu_rtld" : "native_memory_gnu_rtld") +
        ",\"externalStorage\":" + (externalRoot.empty() ? "false" : "true") + "," +
        "\"status\":" + JsonString(session.connectionIdentityOnly ? "gnu_connection_identity_diagnostic" : steamClient ? "steam_native_attempted" : verified ? "xwayland_x11_verified" : "xwayland_x11_failed") +
        ",\"gnuExecutionVerified\":" + (verified ? "true" : "false") +
        ",\"x11Verified\":" + (verified ? "true" : "false") +
        ",\"displayReady\":" + (session.ready ? "true" : "false") +
        ",\"displayReadyErrno\":" + std::to_string(session.readyError) +
        ",\"mesaDriver\":" + JsonString(session.sharedVirpipe ? "virpipe" : session.softwareGL ? "softpipe" : "system") +
        ",\"graphicsSelfTestRequested\":" + (session.graphicsSelfTest || session.graphicsLoaderDiagnostic ? "true" : "false") +
        ",\"steamClientReadOnlyProbeRequested\":" + (session.steamClientReadOnlyProbe ? "true" : "false") +
        ",\"steamClientSourceProbeRequested\":" + (session.steamClientSourceProbe ? "true" : "false") +
        ",\"steamClientRpcProbeRequested\":" + (session.steamClientRpcProbe ? "true" : "false") +
        ",\"steamClientGameBridgeRequested\":" + (session.steamClientGameBridge ? "true" : "false") +
        ",\"desktopGeometry\":" + JsonString(steamClient ? session.desktopGeometry : "640x360") +
        ",\"sharedReceiverVerified\":" + (session.sharedVirpipe && session.glxPreflight.exitCode==0 &&
            session.glxPreflight.output.find("native_glx_verified")!=std::string::npos &&
            session.glxPreflight.output.find("renderer=virgl")!=std::string::npos ? "true" : "false") +
        ",\"steamProcessAttempted\":" + (steamClient && client.stage != "not_started" ? "true" : "false") +
        ",\"layoutStrategy\":" + JsonString(externalRoot.empty() ? "separate_data_and_signed_code" : "external_code_and_data") + ",\"steamClientStarted\":false,\"inputVerified\":false,\"gpuAccelerationVerified\":false,\"server\":" +
        NativeExecJson(server) + ",\"ipcPreflight\":" + NativeExecJson(session.ipcPreflight) +
        ",\"connectionIdentityPreflight\":" + NativeExecJson(session.connectionIdentityPreflight) +
        ",\"loaderPreflight\":" + NativeExecJson(session.loaderPreflight) +
        ",\"xlibPreflight\":" + NativeExecJson(session.xlibPreflight) + ",\"glxPreflight\":" + NativeExecJson(session.glxPreflight) + ",\"client\":" + NativeExecJson(client) +
        (steamClient ? ",\"webhelperLog\":"+ReadHelperLog((externalRoot.empty()?base:externalRoot)+"/client/.steam/steam/logs/steamwebhelper.log") +
            ",\"cefLog\":"+ReadHelperLog((externalRoot.empty()?base:externalRoot)+"/client/.steam/steam/logs/cef_log.txt") +
            ",\"webhelperDetailLog\":"+ReadHelperLog((externalRoot.empty()?base:externalRoot)+"/client/.steam/steam/logs/webhelper.txt") +
            ",\"consoleLog\":"+ReadHelperLog((externalRoot.empty()?base:externalRoot)+"/client/.steam/steam/logs/console_log.txt")+
            ",\"cloudLog\":"+ReadHelperLog((externalRoot.empty()?base:externalRoot)+"/client/.steam/steam/logs/cloud_log.txt") : "") + '}';
}
}
