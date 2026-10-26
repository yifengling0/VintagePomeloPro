#include "proton_probe.h"
#include "proton_game_paths.h"
#include "native_exec.h"
#include <cerrno>
#include <chrono>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cstdlib>
#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace steamohos {
namespace {
constexpr const char* Root = "/data/storage/el2/base/files/steamohos-native/proton-probe";
bool NativeElf(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    Elf64_Ehdr header{};
    const bool ok = read(fd, &header, sizeof(header)) == sizeof(header) &&
        !memcmp(header.e_ident, ELFMAG, SELFMAG) && header.e_ident[EI_CLASS] == ELFCLASS64 &&
        header.e_ident[EI_DATA] == ELFDATA2LSB && header.e_machine == EM_AARCH64;
    close(fd);
    return ok;
}
bool Succeeded(const NativeExecResult& result) {
    return result.reaped && result.hostTpUnchanged && !result.setupError && !result.execError &&
        !result.waitError && !result.timedOut && !result.signal && result.exitCode == 0;
}
bool WritePrivateFile(const std::string& path, const std::string& bytes) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { close(fd); return false; }
        offset += static_cast<size_t>(count);
    }
    const bool ok = !fsync(fd);
    close(fd);
    return ok;
}
bool CDriveRecord(const std::string& root, char drive = 'c', const std::string& target = {}, bool replaceOwned = false) {
    const std::string directory = root + "/prefix/dosdevices";
    if (mkdir(directory.c_str(), 0700) && errno != EEXIST) return false;
    const std::string path = directory + "/" + drive + ":";
    const std::string bytes = "STEAMOHOS_CLOUD_LINK_V1\n" + (target.empty() ? root + "/prefix/drive_c" : target);
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        size_t written = 0;
        while (written < bytes.size()) {
            const ssize_t result = write(fd, bytes.data() + written, bytes.size() - written);
            if (result < 0 && errno == EINTR) continue;
            if (result <= 0) { close(fd); unlink(path.c_str()); return false; }
            written += static_cast<size_t>(result);
        }
        const bool ok = !fsync(fd);
        close(fd);
        if (!ok) unlink(path.c_str());
        return ok;
    }
    if (errno != EEXIST) return false;
    fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    char verify[4096 + 64];
    struct stat status{};
    const ssize_t count = read(fd, verify, sizeof(verify));
    const bool owned = count >= 0 && !fstat(fd, &status) && S_ISREG(status.st_mode) &&
        status.st_uid == getuid() && status.st_size == count;
    const bool ok = owned && count == static_cast<ssize_t>(bytes.size()) &&
        !memcmp(bytes.data(), verify, bytes.size());
    close(fd);
    // GNU sessions are serialized and their server is reaped before the next
    // one. Rebind only our valid private path record, never a user's directory,
    // symlink or unknown file. C: and Z: retain exact verification.
    const std::string magic = "STEAMOHOS_CLOUD_LINK_V1\n/";
    if (!ok && replaceOwned && owned && count >= static_cast<ssize_t>(magic.size()) &&
        !memcmp(verify, magic.data(), magic.size())) return WritePrivateFile(path, bytes);
    return ok;
}
bool LoaderReference(const std::string& source, const std::string& path) {
    // OHOS forbids symlink creation here, and the native loader deliberately
    // requires O_NOFOLLOW. Copy only the small bootstrap dependency closure,
    // verify every byte, and reuse it on subsequent runs.
    const int input = open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (input < 0) return false;
    int output = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    bool created = false;
    if (output < 0 && errno == ENOENT) {
        output = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        created = output >= 0;
    }
    bool ok = output >= 0;
    off_t offset = 0;
    char bytes[65536], verify[65536];
    while (ok) {
        const ssize_t count = pread(input, bytes, sizeof(bytes), offset);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { ok = false; break; }
        if (!count) break;
        if (created) {
            ssize_t written = 0;
            while (written < count) {
                const ssize_t result = pwrite(output, bytes + written, static_cast<size_t>(count - written), offset + written);
                if (result < 0 && errno == EINTR) continue;
                if (result <= 0) { ok = false; break; }
                written += result;
            }
        }
        if (!ok || pread(output, verify, static_cast<size_t>(count), offset) != count ||
            memcmp(bytes, verify, static_cast<size_t>(count))) { ok = false; break; }
        offset += count;
    }
    struct stat st{};
    if (ok && (fstat(output, &st) || st.st_size != offset || !offset)) ok = false;
    if (created && ok && fsync(output)) ok = false;
    close(input);
    if (output >= 0) close(output);
    if (created && !ok) unlink(path.c_str());
    return ok;
}

bool SafeEndpoint(const std::string& path) {
    struct stat st{};
    return !path.empty() && path[0]=='/' && path.size()<sizeof(sockaddr_un::sun_path) &&
        path.find_first_of("\r\n")==std::string::npos && !lstat(path.c_str(),&st) &&
        S_ISSOCK(st.st_mode) && st.st_uid==geteuid();
}
bool InstallProbeModules(const std::string& root,const std::string& package) {
    std::ifstream list(root+"/prefix-modules.list");
    if(!list)return false;
    std::string line;unsigned count=0;
    while(std::getline(list,line)) {
        if(++count>2048 || line.size()>128)return false;
        const auto split=line.find('/');
        if(split==std::string::npos || line.find('/',split+1)!=std::string::npos)return false;
        const std::string arch=line.substr(0,split),name=line.substr(split+1);
        if((arch!="i386-windows" && arch!="aarch64-windows") || name.empty() ||
            name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_.-")!=std::string::npos)return false;
        const std::string target=root+"/prefix/drive_c/windows/"+(arch=="i386-windows"?"syswow64":"system32");
        struct stat st{};
        if((mkdir(target.c_str(),0700)&&errno!=EEXIST) || lstat(target.c_str(),&st) ||
            !S_ISDIR(st.st_mode) || st.st_uid!=geteuid())return false;
        if(!LoaderReference(package+"/files/lib/wine/"+line,target+'/'+name))return false;
    }
    return count>0;
}
std::string MediaReferenceFailure;
bool ReferenceMediaRuntime(const std::string& root,const std::string& runtime) {
    std::ifstream list(root+"/runtime-modules.list");std::string name;unsigned count=0;
    if(!list)return false;
    while(std::getline(list,name)) {
        const std::string source = access((runtime+'/'+name).c_str(), R_OK) == 0 ?
            runtime+'/'+name : runtime+"/steamrtarm64/"+name;
        if(++count>128 || name.empty() || name.size()>96 ||
            name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_+.-")!=std::string::npos ||
            !LoaderReference(source,root+'/'+name)) {
            MediaReferenceFailure=name+":"+std::to_string(errno);return false;
        }
    }
    if(!count)return false;
    if(!LoaderReference(runtime+"/libsteamohos_software_gl.so",root+"/libGL.so.1")) {
        MediaReferenceFailure="libGL.so.1:"+std::to_string(errno);return false;
    }
    return true;
}
struct ProbeX11 {
    std::string directory,socketPath;
    int listener=-1,readFd=-1,writeFd=-1;
    bool ready=false;
    NativeExecResult wine;
    NativeExecOptions wineOptions;
    std::vector<std::string> arguments,environment;
    ~ProbeX11(){
        for(int fd:{listener,readFd,writeFd})if(fd>=0)close(fd);
        if(!socketPath.empty())unlink(socketPath.c_str());
        if(!directory.empty())rmdir(directory.c_str());
    }
    bool Setup(const std::string& root) {
        std::string pattern=root+"/tmp/x11-XXXXXX";
        char* path=mkdtemp(&pattern[0]);if(!path)return false;
        directory=path;socketPath=directory+"/x11";
        listener=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
        sockaddr_un address{};address.sun_family=AF_UNIX;
        if(listener<0 || socketPath.size()>=sizeof(address.sun_path))return false;
        strcpy(address.sun_path,socketPath.c_str());
        if(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || listen(listener,4))return false;
        int fds[2];if(pipe2(fds,O_CLOEXEC|O_NONBLOCK))return false;
        readFd=fds[0];writeFd=fds[1];return true;
    }
    static void Start(void* data,int serverPid) {
        auto& session=*static_cast<ProbeX11*>(data);
        close(session.writeFd);session.writeFd=-1;
        std::string result;
        const auto start=std::chrono::steady_clock::now();
        while(std::chrono::steady_clock::now()-start<std::chrono::seconds(6)) {
            pollfd fd{session.readFd,POLLIN,0};int code=poll(&fd,1,100);
            if(code<0&&errno==EINTR)continue;
            if(code<0)break;
            if(!code)continue;
            char bytes[16];ssize_t n=read(session.readFd,bytes,sizeof(bytes));
            if(n>0){result.append(bytes,static_cast<size_t>(n));if(result.find('\n')!=std::string::npos||result.size()>16)break;}
            else if(!n || (errno!=EAGAIN&&errno!=EINTR))break;
        }
        session.ready=result=="57\n";
        if(session.ready)session.wine=RunNativeLoader(session.arguments,session.environment,40000,session.wineOptions);
        // Only the server launched for this bounded diagnostic is terminated.
        if(serverPid>0)kill(serverPid,SIGTERM);
    }
};
}

std::string ProbeGnuProton(const std::string& runtime, bool forkBackend, const std::string& waylandSocket,
    const ProtonProbeCapabilities& capabilities, const LaunchRequest* manual) {
    std::string stage;
    std::ifstream requested(std::string(Root) + "/stage");
    std::getline(requested, stage);
    if(manual)stage="manual";
    const bool prefixRequested = stage == "prefix";
    const bool controlsRequested = stage == "window-controls";
    const bool windowRequested = stage == "window" || controlsRequested;
    struct GameTrial { const char* stage; const char* directory; const char* executable; const char* appid; char drive; };
    static constexpr GameTrial games[] = {
        {"newpal", "NewPAL", "NewPAL_Release.exe", "695630", 'g'},
        {"pal4", "Chinese Paladin 4", "launch.exe", "681820", 'h'},
        {"senrenbanka", "SenrenBanka_trial", "SenrenBanka.exe", "1221010", 'i'},
    };
    const GameTrial* selectedGame = nullptr;
    for (const auto& game : games) if (stage == game.stage) selectedGame = &game;
    const bool gameRequested = selectedGame != nullptr || manual;
    const bool vulkanRequested = stage == "dxvk11-x86" ||
        (manual ? manual->backend=="gnu-proton" : gameRequested && access((std::string(Root) + "/game-dxvk").c_str(), R_OK) == 0);
    const bool localeRequested = stage == "locale" || stage == "locale-configure";
    const bool d3dRequested = stage == "d3d9-x86" || stage == "dxvk11-x86";
    const bool x86Requested = stage == "x86" || d3dRequested || gameRequested || localeRequested;
    const bool initializeRequested = stage == "init";
    const bool graphicsRequested = stage == "glx";
    // The standard GNU tool exposes both APIs: legacy D3D9 stays on the
    // proven WineD3D/virgl path, while D3D11 uses DXVK/Venus. Enabling Vulkan
    // must not remove the native EGL transport needed by a D3D9 application.
    const bool sharedGraphicsRequested = stage == "gl-shared" || gameRequested ||
        (d3dRequested && !vulkanRequested);
    const bool audioRequested = stage == "audio" || gameRequested;
    const bool inputActionsRequested = stage == "input-actions";
    const bool inputRequested = stage == "input" || inputActionsRequested || gameRequested;
    const bool mediaRequested = graphicsRequested || sharedGraphicsRequested || vulkanRequested || audioRequested || inputRequested || localeRequested;
    const bool x64Requested = stage == "x64" || windowRequested || (mediaRequested && !x86Requested);
    const auto failure = [&](const char* status, bool bootstrap = false) {
        return "{\"schemaVersion\":1,\"status\":" + JsonString(status) +
            ",\"bootstrapVerified\":" + (bootstrap ? "true" : "false") +
            ",\"prefixRequested\":" + (prefixRequested ? "true" : "false") +
            ",\"x64Requested\":" + (x64Requested ? "true" : "false") +
            ",\"windowRequested\":" + (windowRequested ? "true" : "false") +
            ",\"prefixVerified\":false,\"x64Verified\":false,\"windowVerified\":false,"
            "\"graphicsVerified\":false,\"gameVerified\":false}";
    };
    if (!stage.empty() && stage != "bootstrap" && stage != "native-present" && stage != "native-vulkan" &&
        !prefixRequested && !x64Requested && !x86Requested && !initializeRequested)
        return failure("proton_unsupported_stage");
    const std::string root(Root), package = root + "/package";
    const std::string wine = package + "/files/lib/wine/aarch64-unix/wine";
    const std::string server = package + "/files/bin-arm64/wineserver";
    if (!NativeElf(wine) || !NativeElf(server))
        return failure("proton_package_missing_or_wrong_arch");
    for (const auto& path : {root + "/home", root + "/prefix", root + "/tmp"}) {
        struct stat st{};
        if ((mkdir(path.c_str(), 0700) && errno != EEXIST) || lstat(path.c_str(), &st) ||
            !S_ISDIR(st.st_mode) || st.st_uid != getuid())
            return failure("proton_private_directory_failed");
    }
    // Keep the product exec/rtld allowlist intact. The experiment has a separate
    // code root, without modifying or duplicating the full Steam runtime.
    for (const char* name : {"ld-linux-aarch64.so.1", "libc.so.6", "libdl.so.2", "libpthread.so.0",
            "librt.so.1", "libm.so.6", "libstdc++.so.6", "libgcc_s.so.1", "libsteamohos_posix.so",
            "libfreetype.so.6", "libfontconfig.so.1", "libexpat.so.1", "libpng16.so.16", "libbrotlidec.so.1", "libbrotlicommon.so.1", "libz.so.1"}) {
        if (!LoaderReference(runtime + '/' + name, root + '/' + name))
            return failure("proton_runtime_reference_failed");
    }
    // Read the host's actual CJK fonts in place; no bundled/copied font tree.
    // Wine may also discover system fonts independently of fontconfig filters.
    const std::string fonts = root + "/fonts";
    struct stat fontDirectory{};
    if ((mkdir(fonts.c_str(), 0700) && errno != EEXIST) || lstat(fonts.c_str(), &fontDirectory) ||
        !S_ISDIR(fontDirectory.st_mode) || fontDirectory.st_uid != getuid())
        return failure("proton_font_directory_failed");
    const std::string fontConfig = root + "/tmp/fonts.conf";
    const std::string xml = "<?xml version=\"1.0\"?><fontconfig><dir>" + fonts +
        "</dir><dir>/system/fonts</dir><cachedir>" + root + "/tmp/fontconfig-cache</cachedir>"
        "<selectfont><rejectfont><glob>/system/fonts/*</glob></rejectfont><acceptfont>"
        "<glob>/system/fonts/HarmonyOS_Sans.ttf</glob>"
        "<glob>/system/fonts/HarmonyOS_Sans_SC.ttf</glob>"
        "<glob>/system/fonts/HarmonyOS_Sans_TC.ttf</glob>"
        "<glob>/system/fonts/NotoSansCJK-Regular.ttc</glob>"
        "<glob>/system/fonts/NotoSerifCJK-Regular.ttc</glob>"
        "</acceptfont></selectfont>"
        "<alias><family>SimSun</family><prefer><family>Noto Serif CJK SC</family></prefer></alias>"
        "<alias><family>SimHei</family><prefer><family>HarmonyOS Sans SC</family></prefer></alias>"
        "<alias><family>Microsoft YaHei</family><prefer><family>HarmonyOS Sans SC</family></prefer></alias>"
        "<config><rescan><int>0</int></rescan></config></fontconfig>\n";
    int fontFd = open(fontConfig.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat fontStatus{};
    bool fontReady = fontFd >= 0 && !fstat(fontFd, &fontStatus) && S_ISREG(fontStatus.st_mode) &&
        fontStatus.st_uid == getuid() && !ftruncate(fontFd, 0);
    size_t fontWritten = 0;
    while (fontReady && fontWritten < xml.size()) {
        const ssize_t count = write(fontFd, xml.data() + fontWritten, xml.size() - fontWritten);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { fontReady = false; break; }
        fontWritten += static_cast<size_t>(count);
    }
    if (fontFd >= 0) close(fontFd);
    if (!fontReady) return failure("proton_font_config_failed");
    const std::string loader = root + "/ld-linux-aarch64.so.1";
    const std::string libraries = root + "/graphics:" + root + ":" + package + "/files/lib/wine/aarch64-unix:" + package + "/files/lib:" +
        package + "/files/lib/aarch64-linux-gnu";
    // Same setting/default as the existing Wine route. Wine derives LCID and
    // legacy ANSI codepage from this locale (zh_CN -> 0804/936), not UTF-8 ACP.
    std::string language=capabilities.wineLanguage;
    if(language!="zh_CN" && language!="zh_TW" && language!="ja_JP" && language!="en_US") language="zh_CN";
    const std::string locale=language+".UTF-8";
    std::vector<std::string> environment{
        "LANG="+locale, "LC_ALL="+locale, "LC_CTYPE="+locale, "LC_MESSAGES="+locale,
        "STEAMOHOS_PROTON_LOCALE="+locale, "LOCPATH="+root+"/locale", "PATH=/system/bin", "HOME=" + root + "/home",
        "FONTCONFIG_FILE=" + fontConfig, "TMPDIR=" + root + "/tmp", "WINEPREFIX=" + root + "/prefix", "WINEARCH=win64",
        "WINESERVER=" + server, "WINELOADER=" + wine, "WINEDLLPATH=" + package + "/files/lib/wine",
        // Same default as wine_env_baseline.h for the existing ARM Wine/FEX
        // route. Strict software-F80 made VLC miss audio decode deadlines.
        // Respect legacy 2 GiB PE images; LAA images retain their own flag.
        "WINE_LARGE_ADDRESS_AWARE=0",
        "FEX_X87REDUCEDPRECISION=1",
        "GLIBC_TUNABLES=glibc.pthread.rseq=0", "STEAMOHOS_NATIVE_MEMORY=1",
        "STEAMOHOS_EXTERNAL_RUNTIME=" + root, "STEAMOHOS_CODE_ROOT=" + root,
        "STEAMOHOS_TMP_ROOT=" + root + "/tmp", "XDG_RUNTIME_DIR=" + root + "/tmp",
        "STEAMOHOS_IPC_ROOT=" + root + "/tmp",
        "LD_PRELOAD=" + root + "/libsteamohos_posix.so", "WINEDEBUG=-all,err+all"};
    // Preserve the bounded runner output even if the display host is lost.
    // The native supervisor rotates this diagnostic file at 8 MiB.
    struct DiagnosticFile {
        int fd;
        ~DiagnosticFile() { if (fd >= 0) close(fd); }
    } diagnostic{open((root + "/probe-runtime.log").c_str(),
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600)};
    NativeExecOptions options;
    options.libcForkOnly = forkBackend;
    options.workingDirectory = root;
    options.cancellationFd=manual?capabilities.cancellationFd:-1;
    options.outputLimit = 131072;
    std::string steamCompatRoot;
    if(gameRequested) {
        const std::string game=manual?manual->install:std::string("/storage/Users/currentUser/Download/com.vintage.pomelopro/SteamLinuxARM64/library/steamapps/common/")+selectedGame->directory;
        const std::string executable=manual?manual->argv.front():game+"/"+selectedGame->executable;
        if(access(executable.c_str(),R_OK) || (!manual && !CDriveRecord(root,selectedGame->drive,game)))
            return failure("proton_game_directory_unavailable");
        options.workingDirectory=manual?manual->cwd:game;
        environment.emplace_back("STEAMOHOS_PROTON_GAME_ROOT="+game);
        environment.emplace_back(std::string("SteamAppId=")+(manual?manual->appId:selectedGame->appid));
        environment.emplace_back(std::string("SteamGameId=")+(manual?manual->appId:selectedGame->appid));
        if(!manual) {
            environment.emplace_back(std::string("STEAMOHOS_PROTON_GAME_DRIVE=")+selectedGame->drive);
            environment.emplace_back("STEAMOHOS_PROTON_DRIVE_ROOT="+game);
        }
        const std::string base="/data/storage/el2/base/files/steamohos-native";
        std::string external,scope;
        std::ifstream rootFile(base+"/external-root.path");std::getline(rootFile,external);
        std::ifstream log(base+"/client-native.private.log");std::getline(log,scope);
        const std::string prefix="STEAMOHOS_SYSVIPC namespace=";
        if(scope.compare(0,prefix.size(),prefix)!=0)return failure("proton_steam_session_missing");
        scope=scope.substr(prefix.size());
        const std::string namespacePrefix=base+"/x11-";
        if(scope.compare(0,namespacePrefix.size(),namespacePrefix)!=0 || scope.size()!=namespacePrefix.size()+6+8 ||
            scope.substr(scope.size()-8)!="/sysvipc" || !SafeEndpoint(scope.substr(0,scope.size()-8)+"/x11") ||
            external.empty() || external[0]!='/' || external.find("..")!=std::string::npos)
            return failure("proton_steam_session_invalid");
        for(const auto& path:{root+"/home/.steam",root+"/home/.steam/sdkarm64"}) {
            struct stat st{};
            if((mkdir(path.c_str(),0700)&&errno!=EEXIST)||lstat(path.c_str(),&st)||!S_ISDIR(st.st_mode)||st.st_uid!=getuid())
                return failure("proton_steam_sdk_directory_failed");
        }
        for(const char* name:{"libtier0_s.so","libvstdlib_s.so"})
            if(!LoaderReference(runtime+"/steamrtarm64/"+name,root+"/"+name))return failure("proton_steam_sdk_dependency_failed");
        if(!LoaderReference(runtime+"/steamrtarm64/steamclient.so",root+"/home/.steam/sdkarm64/steamclient.so"))
            return failure("proton_steam_sdk_module_failed");
        for(auto& variable:environment)if(variable.compare(0,strlen("STEAMOHOS_IPC_ROOT="),"STEAMOHOS_IPC_ROOT=")==0)
            variable="STEAMOHOS_IPC_ROOT="+base+"/client-ipc";
        environment.emplace_back("STEAMOHOS_SYSVIPC_ROOT="+scope);
        steamCompatRoot = external + "/client/.steam/steam/steamohos/compat";
        environment.emplace_back("STEAMOHOS_STEAM_ROOT="+external+"/client/.steam/steam");
        environment.emplace_back("STEAMOHOS_LIBRARY_ROOT="+external+"/library");
        environment.emplace_back("STEAM_COMPAT_CLIENT_INSTALL_PATH="+external+"/client/.steam/steam");
        environment.emplace_back("STEAM_COMPAT_LIBRARY_PATHS="+external+"/library");
        if(manual) {
            // This GNU prefix has a conventional Z: filesystem root. The
            // authenticated broker has already resolved and checked all PE paths.
            if(!CDriveRecord(root,'z',"/"))return failure("proton_z_drive_unavailable");
            // Wine's filesystem-root drive crosses OHOS ancestors that are
            // not enumerable. Give every real game the same short, scoped
            // drive while retaining Z: for client and external arguments.
            const bool realGame = manual->role == "game";
            if (realGame && !CDriveRecord(root, 'j', game, true))
                return failure("proton_game_drive_unavailable");
            environment.emplace_back(realGame ? "STEAMOHOS_PROTON_GAME_DRIVE=j" : "STEAMOHOS_PROTON_GAME_DRIVE=z");
            environment.emplace_back("STEAMOHOS_PROTON_DRIVE_ROOT=" + (realGame ? game : std::string("/")));
            for(const auto& value:manual->environment)
                if(value.compare(0,18,"SteamOverlayGameId=")==0)environment.push_back(value);
        }

    }
    options.diagnosticFd = diagnostic.fd;
    if (gameRequested && (!manual || manual->role=="game")) {
        if (!ReferenceMediaRuntime(root, runtime))
            return failure("proton_steam_runtime_dependency_missing");
        if (!NativeElf(root + "/proton-steam-ready"))
            return failure("proton_steam_readiness_probe_missing");
        auto readyEnvironment = environment;
        for (auto& variable : readyEnvironment)
            if (variable.compare(0, 11, "LD_PRELOAD=") == 0)
                variable = "LD_PRELOAD=" + root + "/libproton_probe.so";
        const auto ready = RunNativeLoader({loader, "--inhibit-cache", "--library-path", libraries,
            root + "/proton-steam-ready"}, readyEnvironment, 45000, options);
        if (!Succeeded(ready) || ready.output.find("GNU_STEAM_READY session_ready=1 ") == std::string::npos) {
            // Keep the actual loader/IPC result. A missing SDK, a terminated
            // probe and an unavailable local account must not look identical.
            const char* status = ready.cancelled ? "proton_game_cancelled" :
                ready.exitCode == 5 ? "proton_steam_account_not_ready" : "proton_steam_readiness_failed";
            auto result = failure(status);
            result.pop_back();
            return result + ",\"steamReadiness\":" + NativeExecJson(ready) + "}";
        }
    }
    if (stage == "native-present" || stage == "native-vulkan") {
        const bool vulkan=stage=="native-vulkan";
        const std::string fixture=root+(vulkan?"/proton-vulkan":"/proton-shared-present");
        if (!SafeEndpoint(capabilities.vtestSocket) || !SafeEndpoint(waylandSocket) ||
            !NativeElf(root+"/libproton_probe.so") || !NativeElf(fixture) ||
            !NativeElf(root+"/graphics/libEGL-proton.so.1") || !ReferenceMediaRuntime(root,runtime))
            return failure("gnu_shared_present_dependencies_missing");
        for(auto& variable:environment)
            if(variable.compare(0,11,"LD_PRELOAD=")==0)variable="LD_PRELOAD="+root+"/libproton_probe.so";
        environment.emplace_back("WAYLAND_DISPLAY="+waylandSocket);
        environment.emplace_back("GALLIUM_DRIVER=virpipe");
        environment.emplace_back("VTEST_SOCKET_NAME="+capabilities.vtestSocket);
        environment.emplace_back("LIBGL_ALWAYS_SOFTWARE=1");
        environment.emplace_back("LIBGL_DRIVERS_PATH="+root+"/graphics");
        environment.emplace_back("WINEHUA_VTEST_PRESENT=1");
        auto result=RunNativeLoader({loader,"--inhibit-cache","--library-path",libraries,
            fixture},environment,40000,options);
        if(vulkan) {
            const bool queue=Succeeded(result)&&result.output.find("GNU_VULKAN_QUEUE_OK")!=std::string::npos;
            const bool render=access((root+"/vulkan-render").c_str(),F_OK)==0;
            const bool submitted=Succeeded(result)&&result.output.find("GNU_VULKAN_PRESENT_OK")!=std::string::npos;
            return "{\"schemaVersion\":1,\"status\":"+JsonString(render?(submitted?"gnu_vulkan_present_submitted":"gnu_vulkan_present_failed"):(queue?"gnu_vulkan_queue_verified":"gnu_vulkan_queue_failed"))+
                ",\"vulkanQueueVerified\":"+(queue?"true":"false")+
                ",\"vulkanPresentationRequested\":"+(render?"true":"false")+
                ",\"graphicsTransportSubmitted\":"+(submitted?"true":"false")+
                ",\"gameVerified\":false,\"prefix\":"+NativeExecJson(result)+"}";
        }
        const bool passed=Succeeded(result)&&result.output.find("GNU_SHARED_PRESENT_OK")!=std::string::npos;
        return "{\"schemaVersion\":1,\"status\":"+JsonString(passed?"gnu_shared_present_submitted":"gnu_shared_present_failed")+
            ",\"graphicsTransportSubmitted\":"+(passed?"true":"false")+
            ",\"graphicsTransportVerified\":false"+
            ",\"gameVerified\":false,\"prefix\":"+NativeExecJson(result)+"}";
    }
    const auto dependencies = RunNativeLoader({loader, "--inhibit-cache", "--library-path", libraries,
        "--list", wine}, environment, 5000, options);
    const auto wineVersion = RunNativeLoader({loader, "--inhibit-cache", "--library-path", libraries,
        wine, "--version"}, environment, 5000, options);
    const auto serverVersion = RunNativeLoader({loader, "--inhibit-cache", "--library-path", libraries,
        server, "--version"}, environment, 5000, options);
    const bool verified = Succeeded(dependencies) && Succeeded(wineVersion) && Succeeded(serverVersion) &&
        wineVersion.output.find("wine-") != std::string::npos &&
        serverVersion.output.find("Wine") != std::string::npos;
    NativeExecResult prefix, display;
    bool displayReady = false;
    std::string displayXkb;
    const bool probeShim = NativeElf(root + "/libproton_probe.so");
    if ((prefixRequested || x64Requested || x86Requested || initializeRequested) && !verified) return failure("proton_bootstrap_failed");
    if (prefixRequested || x64Requested || x86Requested || initializeRequested) {
        if (!probeShim || !CDriveRecord(root))
            return failure("proton_test_shim_or_drive_missing", true);
        // These modules are loaded by absolute Windows paths, rather than by
        // the ordinary builtin search. Stage only this isolated prefix and
        // compare existing bytes; never overwrite a different installed DLL.
        for (const auto& directory : {root + "/prefix/drive_c", root + "/prefix/drive_c/windows",
                root + "/prefix/drive_c/windows/system32", root + "/prefix/drive_c/windows/system32/drivers"}) {
            struct stat st{};
            if ((mkdir(directory.c_str(), 0700) && errno != EEXIST) || lstat(directory.c_str(), &st) ||
                !S_ISDIR(st.st_mode) || st.st_uid != getuid())
                return failure("proton_module_directory_failed", true);
        }
        for (const char* name : {"libarm64ecfex.dll", "win32k.sys", "dxgkrnl.sys", "dxgmms1.sys", "mountmgr.sys"}) {
            const std::string subdir = !strcmp(name,"libarm64ecfex.dll") || !strcmp(name,"win32k.sys") ? "" : "drivers/";
            if (!LoaderReference(package + "/files/lib/wine/aarch64-windows/" + name,
                    root + "/prefix/drive_c/windows/system32/" + subdir + name))
                return failure("proton_prefix_module_missing_or_changed", true);
        }
        if (x86Requested) {
            for (const char* name : {"libwow64fex.dll", "wow64.dll", "wow64win.dll"})
                if (!LoaderReference(package + "/files/lib/wine/aarch64-windows/" + name,
                        root + "/prefix/drive_c/windows/system32/" + name))
                    return failure("proton_wow64_module_missing_or_changed", true);
            if (!LoaderReference(package + "/files/lib/wine/i386-windows/cmd.exe",
                    root + "/prefix/drive_c/proton-x86-cmd.exe"))
                return failure("proton_x86_program_missing", true);
        }
        if ((x86Requested || mediaRequested || initializeRequested) && !InstallProbeModules(root, package))
            return failure("proton_prefix_dependency_closure_missing_or_changed", true);
        if (mediaRequested) {
            if (!ReferenceMediaRuntime(root, runtime))
                return failure(("proton_media_reference_failed:"+MediaReferenceFailure).c_str(), true);
            if ((graphicsRequested && !NativeElf(root + "/Xwayland")) ||
                (!gameRequested && !localeRequested && access((root+(d3dRequested ? "/prefix/drive_c/proton-x86-cube.exe" : "/prefix/drive_c/proton-x64-media.exe")).c_str(),R_OK)))
                return failure("proton_media_fixture_missing", true);
        }
        if (sharedGraphicsRequested) {
            if (!SafeEndpoint(capabilities.vtestSocket) ||
                !NativeElf(root + "/graphics/libproton_egl.so") ||
                !NativeElf(root + "/graphics/libEGL-proton.so.1"))
                return failure("proton_native_egl_dependencies_missing", true);
            environment.emplace_back("GALLIUM_DRIVER=virpipe");
            environment.emplace_back("VTEST_SOCKET_NAME=" + capabilities.vtestSocket);
            environment.emplace_back("LIBGL_ALWAYS_SOFTWARE=1");
            environment.emplace_back("LIBGL_DRIVERS_PATH=" + root + "/graphics");
            environment.emplace_back("WINEHUA_VTEST_PRESENT=1");
            environment.emplace_back("STEAMOHOS_PROTON_NATIVE_EGL=1");
        }
        if (vulkanRequested) {
            if (!SafeEndpoint(capabilities.vtestSocket) || !NativeElf(root+"/graphics/libproton_vulkan.so"))
                return failure("proton_vulkan_dependencies_missing",true);
            environment.emplace_back("VTEST_SOCKET_NAME="+capabilities.vtestSocket);
            environment.emplace_back("VN_DEBUG=vtest");
            environment.emplace_back("VN_PERF=no_fence_feedback,no_query_feedback,no_semaphore_feedback,no_multi_ring");
            for (const char* name : {"VN_WINEHUA_STRONG_RING_BARRIER", "VN_WINEHUA_REMOTE_MEMORY_SYNC",
                    "VN_WINEHUA_PERSISTENT_MAP_SYNC", "VN_WINEHUA_DIRECT_FENCE_WAIT"})
                environment.emplace_back(std::string(name)+"=1");
            environment.emplace_back("STEAMOHOS_PROTON_NATIVE_VULKAN=1");
            if (gameRequested) {
                environment.emplace_back("WINEPATH=C:\\dxvk26");
            }
            environment.emplace_back("DXVK_LOG_PATH="+root+"/tmp");
            environment.emplace_back("DXVK_LOG_LEVEL=info");
        }
        if (inputRequested) {
            if(!SafeEndpoint(capabilities.gamepadSocket) || !NativeElf(root+"/libSDL2-2.0.so.0"))
                return failure("proton_gamepad_capability_unavailable",true);
            environment.emplace_back("STEAMOHOS_GAMEPAD_SOCKET="+capabilities.gamepadSocket);
            environment.emplace_back("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1");
        }
        if (audioRequested) {
            if (capabilities.audioFd<0 || !SafeEndpoint(capabilities.audioSocket))
                return failure("proton_audio_capability_unavailable", true);
            const std::string config=root+"/tmp/asound.conf";
            const std::string plugin=NativeElf(root+"/libasound_module_pcm_proton.so") ?
                root+"/libasound_module_pcm_proton.so" : "libasound_module_pcm_hmosdeck.so";
            const std::string body="pcm_type.hmosdeck { lib \""+plugin+"\" }\n"
                "pcm.hmosdeck { type hmosdeck hint { show on description \"HmosDeck OHOS audio\" } }\n"
                "pcm.!default { type plug slave.pcm \"hmosdeck\" hint { show on description \"HmosDeck OHOS audio\" } }\n";
            int fd=open(config.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
            const bool ready=fd>=0&&write(fd,body.data(),body.size())==static_cast<ssize_t>(body.size());
            if(fd>=0)close(fd);
            if(!ready)return failure("proton_audio_config_failed",true);
            options.passFds.push_back(capabilities.audioFd);
            environment.emplace_back("STEAMOHOS_AUDIO_BOOTSTRAP_FD="+std::to_string(capabilities.audioFd));
            environment.emplace_back("STEAMOHOS_AUDIO_BOOTSTRAP_SOCKET="+capabilities.audioSocket);
            environment.emplace_back("ALSA_CONFIG_PATH="+config);
            environment.emplace_back("ALSA_PLUGIN_DIR="+root);
        }
        if (windowRequested || mediaRequested || initializeRequested) {
            if (!LoaderReference(package + "/files/lib/wine/aarch64-windows/winewayland.drv",
                    root + "/prefix/drive_c/windows/system32/winewayland.drv"))
                return failure("proton_wayland_driver_missing_or_changed", true);
            const std::string desktopSlot = root + "/desktop-probe.once";
            struct stat slot{};
            if (!lstat(desktopSlot.c_str(), &slot)) {
                if (!S_ISREG(slot.st_mode) || slot.st_uid != getuid() || unlink(desktopSlot.c_str()))
                    return failure("proton_desktop_slot_reset_failed", true);
            } else if (errno != ENOENT) return failure("proton_desktop_slot_reset_failed", true);
            struct stat socket{};
            if (waylandSocket.empty() || waylandSocket[0] != '/' || waylandSocket.size() >= 108 ||
                lstat(waylandSocket.c_str(), &socket) || !S_ISSOCK(socket.st_mode))
                return failure("proton_wayland_socket_missing", true);
            for (const char* name : {"libwayland-client.so.0", "libffi.so.8"})
                if (!LoaderReference(runtime + '/' + name, root + '/' + name))
                    return failure("proton_wayland_dependency_missing", true);
            environment.emplace_back("WAYLAND_DISPLAY=" + waylandSocket);
            std::string externalRoot;
            std::ifstream externalRootFile("/data/storage/el2/base/files/steamohos-native/external-root.path");
            std::getline(externalRootFile, externalRoot);
            if (externalRoot.empty() || externalRoot[0] != '/' ||
                access((externalRoot + "/xkb-data/rules/evdev").c_str(), R_OK))
                return failure("proton_xkb_data_missing", true);
            displayXkb=externalRoot+"/xkb-data";
            environment.emplace_back("XKB_CONFIG_ROOT=" + displayXkb);
            environment.emplace_back("XLOCALEDIR=" + externalRoot + "/x11-locale");
            environment.emplace_back(std::string("WINEDLLOVERRIDES=")+
                (graphicsRequested ? "winewayland.drv=d" : "winex11.drv=d")+
                (audioRequested ? ";winepulse.drv=d;winealsa.drv=b" : "")+
                (vulkanRequested ? ";d3d9=b;ddraw=b;wined3d=b;d3d11=n;dxgi=n;winevulkan=b" :
                    (d3dRequested || gameRequested) ? ";d3d9=b;ddraw=b;wined3d=b" : ""));
        }
        auto prefixEnvironment = environment;
        prefixEnvironment.emplace_back("WINELOADERNOEXEC=1");
        if (probeShim) {
            for (auto& variable : prefixEnvironment)
                if (variable.compare(0, 11, "LD_PRELOAD=") == 0)
                    variable = "LD_PRELOAD=" + root + "/libproton_probe.so";
        }
        // Register helpers can start the private wineserver too. Reap it on
        // every subsequent exit, including failed registration/setup paths.
        struct PrivateServerCleanup {
            std::vector<std::string> arguments;
            const std::vector<std::string>& environment;
            const NativeExecOptions& options;
            ~PrivateServerCleanup() { auto cleanupOptions=options;cleanupOptions.cancellationFd=-1;cleanupOptions.persistentSession=false;RunNativeLoader(arguments, environment, 5000, cleanupOptions); }
        } cleanup{{loader, "--inhibit-cache", "--library-path", libraries, server, "-k"},
                  prefixEnvironment, options};
        // Setup programs use the same working display driver as the game.
        // Their USER32/OLE imports can initialize the Wine desktop even when
        // their main operation only edits registry data. Disabling both display
        // drivers here leaves that bootstrap without a usable driver.
        const auto& provisioningEnvironment = prefixEnvironment;
        if (gameRequested) {
            // Run once per private prefix/configuration, before any game or
            // prerequisite helper. Use Windows registry APIs, never edit live
            // system.reg. Reuse the successful result on subsequent launches.
            const std::string localeHelper=root+"/prefix/drive_c/proton-x86-locale.exe";
            struct stat helper{};
            if(lstat(localeHelper.c_str(),&helper) || !S_ISREG(helper.st_mode) || helper.st_uid!=getuid())
                return failure("proton_locale_helper_missing",true);
            const std::string state="native-cjk-v1\n"+language+"\n"+
                std::to_string(helper.st_size)+":"+std::to_string(helper.st_mtime)+"\n";
            const std::string statePath=root+"/prefix/hmosdeck-locale.state";
            std::ifstream previous(statePath);
            const std::string existing((std::istreambuf_iterator<char>(previous)),std::istreambuf_iterator<char>());
            if(existing!=state) {
                const auto configured=RunNativeLoader({loader,"--inhibit-cache","--library-path",libraries,
                    wine,"C:\\proton-x86-locale.exe","--configure",language},provisioningEnvironment,120000,options);
                if(!Succeeded(configured) || configured.output.find("HMOSDECK_GNU_LOCALE_CONFIGURED")==std::string::npos)
                    return "{\"schemaVersion\":1,\"status\":\"proton_locale_setup_failed\",\"runtimeBackend\":\"gnu-proton\",\"localeSetup\":"+
                        NativeExecJson(configured)+"}";
                if(!WritePrivateFile(statePath,state))return failure("proton_locale_state_write_failed",true);
                // The helper can release its desktop. Give the actual game a
                // fresh attempt, retaining the recursion guard per invocation.
                if(unlink((root+"/desktop-probe.once").c_str()) && errno!=ENOENT)
                    return failure("proton_locale_desktop_slot_reset_failed",true);
            }
        }
        if (gameRequested) {
            // A selectively installed WoW64 closure needs its own COM view.
            // Register using the original 32-bit modules, rather than writing
            // guessed CLSID entries or relying on 64-bit-only wineboot state.
            std::string mediaState="wow64-media-v1\n";
            for(const char* name : {"mmdevapi.dll","dsound.dll","dinput8.dll","msctf.dll"}) {
                struct stat module{};
                if(stat((package+"/files/lib/wine/i386-windows/"+name).c_str(),&module))
                    return failure("proton_wow64_media_module_missing",true);
                mediaState+=std::string(name)+":"+std::to_string(module.st_size)+":"+std::to_string(module.st_mtime)+"\n";
            }
            const std::string mediaStatePath=root+"/prefix/hmosdeck-media.state";
            std::ifstream previousMedia(mediaStatePath);
            const std::string existingMedia((std::istreambuf_iterator<char>(previousMedia)),std::istreambuf_iterator<char>());
            if(existingMedia!=mediaState) {
                const auto mediaRegistration = RunNativeLoader({loader, "--inhibit-cache", "--library-path", libraries,
                    wine, "C:\\windows\\syswow64\\regsvr32.exe", "/s", "mmdevapi.dll", "dsound.dll", "dinput8.dll", "msctf.dll"},
                    provisioningEnvironment, 120000, options);
                if (!Succeeded(mediaRegistration))
                    return "{\"schemaVersion\":1,\"status\":\"proton_wow64_media_registration_failed\",\"runtimeBackend\":\"gnu-proton\",\"mediaSetup\":"+
                        NativeExecJson(mediaRegistration)+"}";
                if(!WritePrivateFile(mediaStatePath,mediaState))return failure("proton_wow64_media_state_write_failed",true);
            }
            // Each completed helper may release its Wine desktop. The guard
            // bounds recursive explorer launches within one invocation only.
            if(unlink((root+"/desktop-probe.once").c_str()) && errno!=ENOENT)
                return failure("proton_media_desktop_slot_reset_failed",true);
            // Match Proton's prefix layout using compatibility DLLs shipped
            // with the Linux client. GE ntdll redirects their exports to the
            // original lsteamclient; authentication stays in Linux Steam.
            for (const auto& path : {root + "/prefix/drive_c/Program Files (x86)",
                    root + "/prefix/drive_c/Program Files (x86)/Steam"}) {
                struct stat st{};
                if ((mkdir(path.c_str(), 0700) && errno != EEXIST) || lstat(path.c_str(), &st) ||
                    !S_ISDIR(st.st_mode) || st.st_uid != geteuid())
                    return failure("proton_steam_stub_directory_failed", true);
            }
            const std::string steamDirectory = root + "/prefix/drive_c/Program Files (x86)/Steam";
            for (const char* name : {"steamclient.dll", "steamclient64.dll", "Steam.dll"})
                if (!LoaderReference(steamCompatRoot + '/' + name, steamDirectory + '/' + name))
                    return failure("proton_steam_compat_module_failed", true);
            // This is GE's small launch helper, not a Windows Steam client.
            if (!LoaderReference(package + "/files/lib/wine/aarch64-windows/steam.exe",
                                 steamDirectory + "/steam.exe"))
                return failure("proton_steam_launch_helper_failed", true);
            const std::string registry =
                "REGEDIT4\r\n\r\n"
                "[HKEY_CURRENT_USER\\Software\\Valve\\Steam]\r\n"
                "\"SteamPath\"=\"C:\\\\Program Files (x86)\\\\Steam\"\r\n"
                "\"SteamExe\"=\"C:\\\\Program Files (x86)\\\\Steam\\\\steam.exe\"\r\n\r\n"
                "[HKEY_CURRENT_USER\\Software\\Valve\\Steam\\ActiveProcess]\r\n"
                "\"SteamPath\"=\"C:\\\\Program Files (x86)\\\\Steam\"\r\n"
                "\"SteamClientDll\"=\"C:\\\\Program Files (x86)\\\\Steam\\\\steamclient.dll\"\r\n"
                "\"SteamClientDll64\"=\"C:\\\\Program Files (x86)\\\\Steam\\\\steamclient64.dll\"\r\n";
            if (!WritePrivateFile(root + "/prefix/drive_c/proton-steam.reg", registry))
                return failure("proton_steam_registry_file_failed", true);
            const auto registration = RunNativeLoader({loader, "--inhibit-cache", "--library-path", libraries,
                wine, "C:\\windows\\system32\\reg.exe", "import", "C:\\proton-steam.reg"},
                provisioningEnvironment, 120000, options);
            if (!Succeeded(registration))
                return "{\"schemaVersion\":1,\"status\":\"proton_steam_registry_import_failed\",\"runtimeBackend\":\"gnu-proton\",\"steamRegistrySetup\":"+
                    NativeExecJson(registration)+"}";
            // Registration is a separate Wine invocation and can release its
            // desktop. Give the game its own bounded desktop attempt, while
            // retaining the recursion guard within each invocation.
            if (unlink((root + "/desktop-probe.once").c_str()) && errno != ENOENT)
                return failure("proton_game_desktop_slot_reset_failed", true);
        }
        std::string gameCommand;
        if(manual) gameCommand=ProtonGameWindowsPath(manual->argv.front(), manual->role=="game" ? manual->install : std::string());
        else if(gameRequested)gameCommand=std::string(1,selectedGame->drive-'a'+'A')+":\\"+selectedGame->executable;
        const std::string command = localeRequested ? "C:\\proton-x86-locale.exe" : initializeRequested ? "wineboot.exe" : gameRequested ? gameCommand : vulkanRequested ? "C:\\dxvk26\\proton-x86-cube.exe" : d3dRequested ? "C:\\proton-x86-cube.exe" : mediaRequested ? "C:\\proton-x64-media.exe" : x86Requested ? "C:\\proton-x86-cmd.exe" : windowRequested ? "C:\\proton-x64-window.exe" : x64Requested ? "C:\\proton-x64-cmd.exe" : "cmd.exe";
        const std::string marker = x86Requested ? "HMOSDECK_GNU_X86_OK" : x64Requested ? "HMOSDECK_GNU_X64_OK" : "HMOSDECK_GNU_WINE_OK";
        if (x64Requested && !LoaderReference(package + "/files/lib/wine/x86_64-windows/cmd.exe",
                root + "/prefix/drive_c/proton-x64-cmd.exe"))
            return failure("proton_x64_program_missing", true);
        const std::string d3dResult=root+"/prefix/drive_c/proton-d3d-result.json";
        if(d3dRequested) unlink(d3dResult.c_str());
        std::vector<std::string> arguments = gameRequested ?
            std::vector<std::string>{loader,"--inhibit-cache","--library-path",libraries,wine,"C:\\windows\\system32\\steam.exe",command} : d3dRequested ?
            std::vector<std::string>{loader,"--inhibit-cache","--library-path",libraries,wine,command,
                stage=="dxvk11-x86"?"--d3d11":"--d3d9","--automation","--seconds","12","--result","C:\\proton-d3d-result.json"} : localeRequested ?
            std::vector<std::string>{loader,"--inhibit-cache","--library-path",libraries,wine,command} : initializeRequested ?
            std::vector<std::string>{loader, "--inhibit-cache", "--library-path", libraries, wine, command, "-u"} : mediaRequested ?
            std::vector<std::string>{loader, "--inhibit-cache", "--library-path", libraries, wine, command, sharedGraphicsRequested ? "gl-shared" : graphicsRequested ? "gl" : audioRequested ? "audio" : inputActionsRequested ? "input-actions" : "input"} : windowRequested ?
            std::vector<std::string>{loader, "--inhibit-cache", "--library-path", libraries, wine, command, controlsRequested ? "controls" : "smoke"} :
            std::vector<std::string>{loader, "--inhibit-cache", "--library-path", libraries, wine, command, "/c", "echo " + marker};
        if(stage=="locale-configure")arguments.emplace_back("--configure");
        if(manual) {
            if(manual->role!="game")arguments.erase(arguments.end()-2);
            for(size_t index=1;index<manual->argv.size();++index) {
                std::string argument=manual->argv[index];
                if(!argument.empty()&&argument[0]=='/'&&access(argument.c_str(),F_OK)==0) {
                    argument=ProtonGameWindowsPath(argument, manual->role=="game" ? manual->install : std::string());
                }
                arguments.push_back(argument);
            }
        }
        if (graphicsRequested) {
            if(!SafeEndpoint(capabilities.vtestSocket))return failure("proton_shared_gpu_receiver_unavailable",true);
            ProbeX11 session;
            if(!session.Setup(root))return failure("proton_x11_setup_failed",true);
            prefixEnvironment.emplace_back("DISPLAY=:57");
            prefixEnvironment.emplace_back("STEAMOHOS_X11_SOCKET="+session.socketPath);
            prefixEnvironment.emplace_back("STEAMOHOS_XKBCOMP_LIBRARY="+root+"/libsteamohos_xkbcomp.so");
            prefixEnvironment.emplace_back("GALLIUM_DRIVER=virpipe");
            prefixEnvironment.emplace_back("VTEST_SOCKET_NAME="+capabilities.vtestSocket);
            prefixEnvironment.emplace_back("XLIB_NO_SHM=1");
            prefixEnvironment.emplace_back("LIBGL_ALWAYS_SOFTWARE=1");
            prefixEnvironment.emplace_back("LIBGL_DRIVERS_PATH="+root+"/graphics");
            session.arguments=arguments;session.environment=prefixEnvironment;session.wineOptions=options;
            auto serverOptions=options;
            serverOptions.passFds={session.listener,session.writeFd};
            serverOptions.parentTask=ProbeX11::Start;serverOptions.context=&session;
            display=RunNativeLoader({loader,"--inhibit-cache","--library-path",libraries,
                NativeElf(root+"/graphics/Xwayland") ? root+"/graphics/Xwayland" : root+"/Xwayland",
                ":57","-xkbdir",displayXkb,"-listenfd",std::to_string(session.listener),
                "-displayfd",std::to_string(session.writeFd),"-shm","-geometry","960x540",
                "-nolisten","tcp","-ac","-extension","MIT-SHM","-noreset","-verbose","0"},
                prefixEnvironment,50000,serverOptions);
            prefix=session.wine;displayReady=session.ready;
        } else {
            // A representative game is an interactive trial, not a 2-minute
            // bootstrap probe. Keep an explicit ceiling without cutting off
            // movies/menu input and making a timer look like a game crash.
            const uint32_t timeoutMs = gameRequested ? 600000 : controlsRequested ? 150000 : initializeRequested ? 120000 : 40000;
            options.persistentSession=manual && manual->role=="game";
            prefix = RunNativeLoader(arguments, prefixEnvironment,options.persistentSession?0:timeoutMs, options);
        }
        // cleanup targets only this diagnostic prefix, including early returns.
    }
    const bool graphicsVerified=graphicsRequested && displayReady && Succeeded(prefix) &&
        prefix.output.find("HMOSDECK_GNU_GPU_OK")!=std::string::npos;
    const bool graphicsTransportSubmitted=(sharedGraphicsRequested || vulkanRequested) && Succeeded(prefix) &&
        (d3dRequested || prefix.output.find("HMOSDECK_GNU_GPU_SHARED_SUBMITTED")!=std::string::npos) &&
        (prefix.output.find("GNU_WINE_NATIVE_EGL frames=")!=std::string::npos || prefix.output.find("GNU_WINE_VULKAN frames=")!=std::string::npos);
    const bool audioVerified=audioRequested && Succeeded(prefix) &&
        prefix.output.find("HMOSDECK_GNU_AUDIO_OK")!=std::string::npos;
    const bool inputEnumerated=inputRequested && Succeeded(prefix) &&
        prefix.output.find("HMOSDECK_GNU_INPUT_ENUMERATED")!=std::string::npos;
    const bool inputActionsVerified=inputActionsRequested && Succeeded(prefix) &&
        prefix.output.find("HMOSDECK_GNU_INPUT_ACTIONS_OK")!=std::string::npos;
    const bool x86Verified = x86Requested && Succeeded(prefix) &&
        (d3dRequested ? graphicsTransportSubmitted : prefix.output.find("HMOSDECK_GNU_X86_OK") != std::string::npos);
    const bool prefixVerified = prefixRequested && Succeeded(prefix) &&
        prefix.output.find("HMOSDECK_GNU_WINE_OK") != std::string::npos;
    const bool windowVerified = windowRequested && Succeeded(prefix) &&
        prefix.output.find("HMOSDECK_GNU_X64_WINDOW_OK") != std::string::npos;
    const bool x64Verified = x64Requested && Succeeded(prefix) &&
        (windowVerified || graphicsVerified || graphicsTransportSubmitted || audioVerified || inputEnumerated || prefix.output.find("HMOSDECK_GNU_X64_OK") != std::string::npos);
    return "{\"schemaVersion\":1,\"executionModel\":\"native_gnu_aarch64\","
        "\"distribution\":\"GE-Proton11-11-aarch64\",\"status\":" +
        JsonString(gameRequested ? (prefix.cancelled ? "proton_game_cancelled" : prefix.timedOut ? "proton_game_trial_expired" : Succeeded(prefix) ? "proton_game_exited" : "proton_game_failed") : vulkanRequested ? (graphicsTransportSubmitted ? "proton_vulkan_submitted" : "proton_vulkan_failed") : sharedGraphicsRequested ? (graphicsTransportSubmitted ? "proton_native_egl_submitted" : "proton_native_egl_failed") : initializeRequested ? (Succeeded(prefix) ? "proton_prefix_updated" : "proton_prefix_update_failed") : graphicsRequested ? (graphicsVerified ? "proton_gpu_verified" : "proton_gpu_failed") : audioRequested ? (audioVerified ? "proton_audio_verified" : "proton_audio_failed") : inputRequested ? (inputEnumerated ? "proton_input_enumerated" : "proton_input_failed") : x86Requested ? (x86Verified ? "proton_x86_verified" : "proton_x86_failed") : windowRequested ? (windowVerified ? "proton_window_verified" : "proton_window_failed") : x64Requested ? (x64Verified ? "proton_x64_verified" : "proton_x64_failed") :
            prefixRequested ? (prefixVerified ? "proton_prefix_verified" : "proton_prefix_failed") :
            (verified ? "proton_bootstrap_verified" : "proton_bootstrap_failed")) +
        ",\"runtimeBackend\":\"gnu-proton\",\"graphicsBackend\":"+JsonString(vulkanRequested?(gameRequested?"d3d9-wined3d-virgl+d3d11-dxvk26-venus-nativeimage":"dxvk26-venus-nativeimage"):"wined3d-virgl-nativeimage")+
        ",\"launchSource\":"+JsonString(manual?"linux-steam":"standalone-probe")+
        ",\"appId\":"+JsonString(manual?manual->appId:selectedGame?selectedGame->appid:"")+
        ",\"bootstrapVerified\":" + (verified ? "true" : "false") +
        ",\"prefixRequested\":" + (prefixRequested ? "true" : "false") +
        ",\"experimentalPosixShim\":" + (probeShim ? "true" : "false") +
        ",\"prefixVerified\":" + (prefixVerified ? "true" : "false") +
        ",\"x64Requested\":" + (x64Requested ? "true" : "false") +
        ",\"x64Verified\":" + (x64Verified ? "true" : "false") +
        ",\"windowRequested\":" + (windowRequested ? "true" : "false") +
        ",\"windowVerified\":" + (windowVerified ? "true" : "false") +
        ",\"x86Requested\":" + (x86Requested ? "true" : "false") +
        ",\"x86Verified\":" + (x86Verified ? "true" : "false") +
        ",\"prefix\":" + NativeExecJson(prefix) +
        ",\"graphicsVerified\":" + (graphicsVerified ? "true" : "false") +
        ",\"graphicsRequested\":" + (graphicsRequested ? "true" : "false") +
        ",\"sharedGraphicsRequested\":" + (sharedGraphicsRequested ? "true" : "false") +
        ",\"graphicsTransportSubmitted\":" + (graphicsTransportSubmitted ? "true" : "false") +
        ",\"graphicsTransportVerified\":false" +
        ",\"audioRequested\":" + (audioRequested ? "true" : "false") +
        ",\"inputRequested\":" + (inputRequested ? "true" : "false") +
        ",\"prefixUpdateRequested\":" + (initializeRequested ? "true" : "false") +
        ",\"prefixUpdated\":" + (initializeRequested && Succeeded(prefix) ? "true" : "false") +
        ",\"audioVerified\":" + (audioVerified ? "true" : "false") +
        ",\"inputEnumerated\":" + (inputEnumerated ? "true" : "false") +
        ",\"inputActionsVerified\":" + (inputActionsVerified ? "true" : "false") +
        ",\"gameRequested\":" + (gameRequested ? "true" : "false") +
        ",\"gameTrialLimitMs\":" + std::to_string(gameRequested && !manual ? 600000 : 0) +
        ",\"gameTrialExpired\":" + (gameRequested && prefix.timedOut ? "true" : "false") +
        ",\"gameVerified\":false,\"displayReady\":" + (displayReady ? "true" : "false") +
        ",\"display\":" + NativeExecJson(display) + ',' +
        "\"dependencies\":" + NativeExecJson(dependencies) +
        ",\"wineVersion\":" + NativeExecJson(wineVersion) +
        ",\"serverVersion\":" + NativeExecJson(serverVersion) + '}';
}
}
