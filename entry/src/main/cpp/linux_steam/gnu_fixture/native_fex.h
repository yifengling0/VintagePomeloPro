// Native Linux compatibility-tool contract, shared by launcher and GNU exec.
// This never selects PE depots or intercepts the Windows Wine launch broker.
#ifndef STEAMOHOS_NATIVE_FEX_H
#define STEAMOHOS_NATIVE_FEX_H
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
namespace steamohos::fex {
namespace fs = std::filesystem;
inline std::string Env(const char* key) { const char* v=getenv(key);return v?v:""; }
inline bool Below(const std::string& path,const std::string& root) {
    return !root.empty() && path.size()>root.size() && path.compare(0,root.size(),root)==0 && path[root.size()]=='/';
}
inline std::string Canonical(const std::string& path) {
    if(path.empty())return {};
    char resolved[PATH_MAX];return realpath(path.c_str(),resolved)?resolved:std::string{};
}
inline std::vector<std::string> Libraries() {
    std::vector<std::string> result;
    for(const char* key:{"STEAMOHOS_LIBRARY_ROOT","STEAMOHOS_STEAM_ROOT"}) {
        auto root=Canonical(Env(key));
        if(!root.empty()&&std::find(result.begin(),result.end(),root)==result.end())result.push_back(root);
    }
    return result;
}
inline std::string Tool(const std::string& path) {
    auto actual=Canonical(path);
    for(const auto& lib:Libraries()) {
        auto root=lib+"/steamapps/common/FEX-Emu";
        // Canonical comparison also rejects a replaced/symlinked FEX package.
        if(actual==root+"/fex-compat-tool" || actual==root+"/FEXCompatTool")return root;
    }
    return {};
}
inline bool Active() {return Env("STEAMOHOS_NATIVE_FEX")=="1";}
inline bool Helper(const std::string& path) {
    if(!Active())return false;
    auto actual=Canonical(path),root=Env("STEAMOHOS_NATIVE_FEX_TOOL");
    if(root.empty()||Canonical(root)!=root)return false;
    bool library=false;for(const auto& lib:Libraries())if(root==lib+"/steamapps/common/FEX-Emu")library=true;
    if(!library)return false;
    // HAP-owned pair is constrained to this selected runtime. Depot updates
    // never get to redirect execution to another shared-storage directory.
    const auto code=Env("STEAMOHOS_CODE_ROOT");
    const auto directory=code+"/native-game-support";
    struct stat supportStatus{};
    // realpath(code) aliases Steam's client data; runtime-only companions do
    // not. Validate this dedicated directory instead of comparing those two
    // different namespaces. The app installer already checks all its parents.
    const auto support=Canonical(directory);
    if(!code.empty()&&fs::path(code).lexically_normal().string()==code&&
       !lstat(directory.c_str(),&supportStatus)&&S_ISDIR(supportStatus.st_mode)&&!support.empty())
        for(const char* name:{"FEX","FEXServer"})if(actual==support+"/"+name)return true;
    for(const char* name:{"FEX","FEXServer","FEXServerManager","FEXBash"})
        if(actual==root+"/usr/bin/"+name)return true;
    return false;
}
inline unsigned ElfMachine(const std::string& path) {
    const auto actual=Canonical(path);if(actual.empty())return 0;
    int fd=open(actual.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return 0;
    unsigned char bytes[20];ssize_t n=read(fd,bytes,sizeof(bytes));close(fd);
    if(n!=sizeof(bytes)||memcmp(bytes,"\177ELF",4)||bytes[5]!=1)return 0;
    unsigned machine=bytes[18]|(bytes[19]<<8);
    return (bytes[4]==2&&(machine==62||machine==183))||(bytes[4]==1&&machine==3)?machine:0;
}
struct Plan {
    std::string tool,rootfs,compat,install,runtime,error;
    std::vector<std::string> argv;
};
// Steam launch options may name an executable script rather than an ELF.
// Preserve the script and its arguments; execute its interpreter from the
// selected guest runtime, never through the OHOS host shell or eval.
inline bool ReadInterpreter(const std::string& path, std::string& interpreter,
                            std::string& argument) {
    int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return false;
    char bytes[257]{};const ssize_t n=read(fd,bytes,256);close(fd);
    if(n<3||bytes[0]!='#'||bytes[1]!='!')return false;
    const char* end=static_cast<const char*>(memchr(bytes,'\n',n));
    if(!end||memchr(bytes,0,end-bytes))return false;
    std::string line(bytes+2,static_cast<size_t>(end-bytes-2));
    const auto begin=line.find_first_not_of(" \t");if(begin==std::string::npos)return false;
    line.erase(0,begin);
    const auto stop=line.find_first_of(" \t\r");
    interpreter=line.substr(0,stop);
    if(interpreter.empty()||interpreter[0]!='/'||
       fs::path(interpreter).lexically_normal().string()!=interpreter)return false;
    argument.clear();
    if(stop!=std::string::npos) {
        const auto first=line.find_first_not_of(" \t",stop);
        if(first!=std::string::npos)argument=line.substr(first);
        const auto last=argument.find_last_not_of(" \t\r");
        argument=last==std::string::npos?std::string{}:argument.substr(0,last+1);
    }
    return true;
}
// The launcher inspects an installed root before activating FEX. Its absolute
// depot links already have guest-root semantics during that inspection.
// This process is single-threaded; restore both capability variables on exit.
class RootInspection {
    std::string priorRoot=Env("STEAMOHOS_NATIVE_FEX_ROOTFS"),priorActive=Env("STEAMOHOS_NATIVE_FEX");
    bool hadRoot=getenv("STEAMOHOS_NATIVE_FEX_ROOTFS")!=nullptr,hadActive=getenv("STEAMOHOS_NATIVE_FEX")!=nullptr;
public:
    bool valid;
    explicit RootInspection(const std::string& root):valid(
        setenv("STEAMOHOS_NATIVE_FEX_ROOTFS",root.c_str(),1)==0&&setenv("STEAMOHOS_NATIVE_FEX","1",1)==0) {}
    ~RootInspection() {
        if(hadRoot)setenv("STEAMOHOS_NATIVE_FEX_ROOTFS",priorRoot.c_str(),1);else unsetenv("STEAMOHOS_NATIVE_FEX_ROOTFS");
        if(hadActive)setenv("STEAMOHOS_NATIVE_FEX",priorActive.c_str(),1);else unsetenv("STEAMOHOS_NATIVE_FEX");
    }
};
inline bool Reject(Plan& plan,const char* stage) {plan.error=stage;return false;}
inline bool RestoreRuntimeUsr(const std::string&,const std::string&,std::string&);
inline bool Runtime(const std::string& path,std::string& package,std::string& family) {
    const auto actual=Canonical(path);
    for(const auto& lib:Libraries())for(const char* name:{"SteamLinuxRuntime_soldier","SteamLinuxRuntime_sniper","SteamLinuxRuntime"}) {
        const std::string base=lib+"/steamapps/common/"+name;
        if(actual==base+"/_v2-entry-point"||actual==base+"/scout-on-soldier-entry-point-v2") {
            package=base;family=name;return true;
        }
    }
    return false;
}
inline bool FindRoot(Plan& plan) {
    std::vector<std::string> roots;
    std::error_code error;
    unsigned count=0;
    for(fs::directory_iterator it(plan.runtime,error),end;!error&&it!=end;it.increment(error)) {
        if(++count>512)return Reject(plan,"runtime_directory_limit");
        auto name=it->path().filename().string();
        if(name.find("soldier_platform_")!=0&&name.find("sniper_platform_")!=0)continue;
        auto root=it->path().string()+"/files";
        if(Canonical(root)!=root)continue;
        RootInspection scoped(root);if(!scoped.valid)return Reject(plan,"runtime_rootfs_scope_failed");
        const auto manifest=it->path().string()+"/usr-mtree.txt.gz";
        if(!Canonical(manifest).empty()&&!RestoreRuntimeUsr(root,manifest,plan.error))return false;
        for(const char* ld:{"/lib64/ld-linux-x86-64.so.2","/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"})
            if(ElfMachine(root+ld)==62){roots.push_back(root);break;}
    }
    if(error)return Reject(plan,"runtime_directory_unreadable");
    if(roots.size()!=1)return Reject(plan,roots.empty()?"runtime_rootfs_missing":"runtime_rootfs_ambiguous");
    plan.rootfs=roots.front();return true;
}
inline bool MakePlan(int argc,char* const argv[],Plan& plan) {
    if(argc<3||argc>120)return Reject(plan,"invalid_arguments");
    for(int i=0;i<argc;++i)if(!argv[i]||strnlen(argv[i],16384)>=16384)return Reject(plan,"argument_limit");
    plan.tool=Tool(argv[0]);if(plan.tool.empty())return Reject(plan,"untrusted_tool_entry");
    if(strcmp(argv[1],"run")&&strcmp(argv[1],"waitforexitandrun"))return Reject(plan,"unsupported_verb");
    plan.compat=Canonical(Env("STEAM_COMPAT_DATA_PATH"));
    // The compatibility directory may not exist yet; only create it below an
    // existing library's compatdata, with a bounded numeric Steam AppID.
    const std::string original=Env("STEAM_COMPAT_DATA_PATH");
    std::string requested=original;
    if(!plan.compat.empty())requested=plan.compat;
    else {
        auto parent=Canonical(fs::path(requested).parent_path().string());
        if(!parent.empty())requested=parent+"/"+fs::path(requested).filename().string();
    }
    bool scoped=false;
    for(const auto& lib:Libraries()) {
        auto apps=Canonical(lib+"/steamapps");if(apps.empty())continue;
        std::string prefix=apps+"/compatdata/";
        // Cloud metadata is intentionally redirected into our private IPC
        // directory. Validate the logical library path AND the exact mapped
        // destination, rather than rejecting that existing filesystem layer.
        const std::string logical=lib+"/steamapps/compatdata/";
        const std::string normalized=fs::path(original).lexically_normal().string();
        const bool fromLibrary=normalized.compare(0,logical.size(),logical)==0;
        if(!fromLibrary&&requested.compare(0,prefix.size(),prefix))continue;
        std::string id=fromLibrary?normalized.substr(logical.size()):requested.substr(prefix.size());
        if(id.empty()||id.size()>10||id[0]=='0'||id.find_first_not_of("0123456789")!=std::string::npos)continue;
        if(std::stoull(id)>4294967295ULL)continue;
        const auto ipc=Canonical(Env("STEAMOHOS_IPC_ROOT"));
        const auto privateBase=ipc.empty()?std::string{}:ipc+"/hmosdeck-cloud-v2-compatdata/";
        if(requested!=prefix+id && (!fromLibrary||privateBase.empty()||requested!=privateBase+id))continue;
        // Arbitrary compatdata symlinks outside those two scopes still fail.
        plan.compat=requested;scoped=true;
    }
    if(!scoped)return Reject(plan,"compatdata_outside_library");
    plan.install=Canonical(Env("STEAM_COMPAT_INSTALL_PATH"));
    bool installed=false;
    for(const auto& lib:Libraries())if(Below(plan.install,lib+"/steamapps/common"))installed=true;
    if(!installed)return Reject(plan,"install_path_outside_library");
    int index=2;if(index<argc&&!strcmp(argv[index],"--"))++index;
    for(unsigned depth=0;index<argc&&depth<4;++depth) {
        std::string package,family;if(!Runtime(argv[index],package,family))break;
        const auto entry=fs::path(argv[index]).filename().string();++index;
        if(entry=="_v2-entry-point") {
            if(index>=argc|| (strcmp(argv[index],"--verb=waitforexitandrun")&&strcmp(argv[index],"--verb=run")))return Reject(plan,"runtime_verb_missing");
            ++index;if(index>=argc||strcmp(argv[index],"--"))return Reject(plan,"runtime_separator_missing");++index;
            if(family!="SteamLinuxRuntime") {
                if(!plan.runtime.empty()&&plan.runtime!=package)return Reject(plan,"nested_runtime_conflict");
                plan.runtime=package;
            }
        } else if(index<argc&&!strcmp(argv[index],"--"))++index;
    }
    if(index>=argc)return Reject(plan,"game_command_missing");
    auto game=Canonical(argv[index]);
    if(!Below(game,plan.install))return Reject(plan,"game_command_outside_install");
    const unsigned machine=ElfMachine(game);
    std::string interpreter,interpreterArgument;
    const bool script=machine!=62&&machine!=3;
    if(script&&!ReadInterpreter(game,interpreter,interpreterArgument))return Reject(plan,"game_not_x86_elf_or_script");
    if(plan.runtime.empty())return Reject(plan,"runtime_wrapper_missing");
    if(!FindRoot(plan))return false;
    plan.argv.push_back(plan.tool+"/usr/bin/FEX");
    if(script) {
        RootInspection scope(plan.rootfs);
        if(!scope.valid)return Reject(plan,"runtime_rootfs_scope_failed");
        const auto executable=Canonical(plan.rootfs+interpreter);
        const unsigned architecture=ElfMachine(executable);
        if(!Below(executable,plan.rootfs)||(architecture!=62&&architecture!=3))
            return Reject(plan,"script_interpreter_missing_or_invalid");
        plan.argv.push_back(executable);
        if(!interpreterArgument.empty())plan.argv.push_back(interpreterArgument);
    }
    plan.argv.push_back(game);
    for(++index;index<argc;++index)plan.argv.emplace_back(argv[index]);
    return true;
}
}
#include "runtime_mtree.h"
#endif
