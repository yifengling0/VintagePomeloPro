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
    std::string requested=Env("STEAM_COMPAT_DATA_PATH");
    if(!plan.compat.empty())requested=plan.compat;
    else {
        auto parent=Canonical(fs::path(requested).parent_path().string());
        if(!parent.empty())requested=parent+"/"+fs::path(requested).filename().string();
    }
    bool scoped=false;
    for(const auto& lib:Libraries()) {
        auto apps=Canonical(lib+"/steamapps");if(apps.empty())continue;
        std::string prefix=apps+"/compatdata/";
        if(requested.compare(0,prefix.size(),prefix))continue;
        std::string id=requested.substr(prefix.size());
        if(id.empty()||id.size()>10||id[0]=='0'||id.find_first_not_of("0123456789")!=std::string::npos)continue;
        if(std::stoull(id)>4294967295ULL)continue;
        // Do not follow a user-provided compatdata symlink outside the library.
        if(!plan.compat.empty()&&plan.compat!=requested)continue;
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
    if(machine!=62&&machine!=3)return Reject(plan,"game_not_x86_elf");
    if(plan.runtime.empty())return Reject(plan,"runtime_wrapper_missing");
    if(!FindRoot(plan))return false;
    plan.argv.push_back(plan.tool+"/usr/bin/FEX");
    plan.argv.push_back(game);
    for(++index;index<argc;++index)plan.argv.emplace_back(argv[index]);
    return true;
}
}
#endif
