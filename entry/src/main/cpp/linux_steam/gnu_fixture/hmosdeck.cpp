// Native ARM64 Steam compatibility tool. Does not load Wine or translate code.
#include "../launch_wire.h"
#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <poll.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <dirent.h>
#include <strings.h>
#include <vector>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <fstream>
#include <chrono>

namespace {
volatile sig_atomic_t cancelled=0;
void Cancel(int){cancelled=1;}
const char* Env(const char* key){const char* value=getenv(key);return value?value:"";}
bool Transfer(int fd,void* buffer,size_t count,bool write){
    auto* p=static_cast<char*>(buffer);
    while(count){ssize_t n=write?send(fd,p,count,MSG_NOSIGNAL):recv(fd,p,count,0);
        if(n<0&&errno==EINTR&&!cancelled)continue;
        if(n<=0)return false;
        p+=n;count-=static_cast<size_t>(n);}
    return true;
}
struct GameBridge {
 pid_t child=-1;std::string directory,endpoint,credential;
 ~GameBridge(){if(child>0){kill(child,SIGTERM);bool reaped=false;for(unsigned i=0;i<50;++i){pid_t value=waitpid(child,nullptr,WNOHANG);if(value==child||(value<0&&errno==ECHILD)){reaped=true;break;}poll(nullptr,0,10);}if(!reaped){kill(child,SIGKILL);waitpid(child,nullptr,0);}}if(!endpoint.empty())unlink(endpoint.c_str());if(!directory.empty())rmdir(directory.c_str());}
 bool Start(const std::string& application){
  if(strcmp(Env("STEAMOHOS_STEAMCLIENT_GAME_BRIDGE"),"1"))return true;
  const std::string root=Env("STEAMOHOS_CODE_ROOT"),tmp=Env("STEAMOHOS_TMP_ROOT");
  if(root.empty()||root[0]!='/'||tmp.empty()||tmp[0]!='/'||!*Env("STEAMOHOS_STEAMCLIENT_EXPECTED_PID"))return false;
  std::string pattern=tmp+"/game-api-XXXXXX";std::vector<char> path(pattern.begin(),pattern.end());path.push_back(0);char* created=mkdtemp(path.data());if(!created)return false;
  directory=created;endpoint=directory+"/endpoint";std::string helper=root+"/steamohos_gnu_steam_bridge";
  pid_t owner=getpid();child=fork();if(child<0)return false;
  if(!child){if(prctl(PR_SET_PDEATHSIG,SIGKILL)||getppid()!=owner)_exit(126);char* args[]={(char*)helper.c_str(),(char*)"--endpoint",(char*)endpoint.c_str(),nullptr};execv(helper.c_str(),args);_exit(127);}
  for(unsigned tries=0;tries<500;++tries){
   if(waitpid(child,nullptr,WNOHANG)==child){child=-1;return false;}
   std::ifstream file(endpoint);std::string header,port,secret,app,extra;
   if(std::getline(file,header)&&std::getline(file,port)&&std::getline(file,secret)&&std::getline(file,app)&&!std::getline(file,extra)){
    if(header!="HMOSDECK-BRIDGE/1"||secret.size()!=64||secret.find_first_not_of("0123456789abcdef")!=secret.npos||app!=application||port.empty()||port.size()>5||port.find_first_not_of("0123456789")!=port.npos)return false;
    unsigned long number=strtoul(port.c_str(),nullptr,10);if(!number||number>65535)return false;credential=port+":"+secret+":"+app;return true;
   }
   poll(nullptr,0,10);
  }
  return false;
 }
};

}
int main(int argc,char** argv){
    // Replacement for the missing SDK script only. Run the verified official
    // reaper in this process; it retains its own process/lifetime semantics.
    if(argc>=2&&!strcmp(argv[1],"--")){
        const std::string root=Env("STEAMOHOS_CODE_ROOT");
        const std::string reaper=root+"/steamrtarm64/reaper";
        struct stat status{};
        if(argc<3||root.empty()||root[0]!='/'||argv[2]!=reaper||
           lstat(reaper.c_str(),&status)||!S_ISREG(status.st_mode)){
            fputs("HMOSDECK: invalid SDK reaper target\n",stderr);return 64;}
        execv(reaper.c_str(),argv+2);
        fputs("HMOSDECK: official reaper execution failed\n",stderr);return 71;
    }
    if(argc==3&&(!strcmp(argv[1],"getcompatpath")||!strcmp(argv[1],"getnativepath"))){
        const std::string input=argv[2],zroot=Env("STEAMOHOS_Z_ROOT"),prefix=Env("STEAMOHOS_WINE_PREFIX");
        std::string path,base,relative;char drive=0;
        if(!strcmp(argv[1],"getcompatpath")) {
            if(input.empty()||input[0]!='/')return 64;
            if(!prefix.empty()&&input.compare(0,prefix.size()+9,prefix+"/drive_c/")==0) {
                drive='C';relative=input.substr(prefix.size()+9);
            } else if(!zroot.empty()&&input.compare(0,zroot.size()+1,zroot+"/")==0) {
                drive='Z';relative=input.substr(zroot.size()+1);
            } else return 64;
        } else {
            if(input.size()<3||input[1]!=':'||(input[2]!='\\'&&input[2]!='/'))return 64;
            drive=(char)toupper((unsigned char)input[0]);
            if(drive=='C'&&!prefix.empty()&&prefix[0]=='/')base=prefix+"/drive_c";
            else if(drive=='Z'&&!zroot.empty()&&zroot[0]=='/')base=zroot;
            else return 64;
            relative=input.substr(3);
        }
        // Normalize separators and dot components before crossing the boundary.
        // A Windows path cannot escape its selected drive or become a UNC path.
        for(char& c:relative)if(c=='\\')c='/';
        std::vector<std::string> components;size_t at=0;
        while(at<=relative.size()) {
            size_t end=relative.find('/',at);if(end==std::string::npos)end=relative.size();
            std::string component=relative.substr(at,end-at);
            if(component=="..") {if(components.empty())return 64;components.pop_back();}
            else if(!component.empty()&&component!=".") {
                if(component.find(':')!=std::string::npos)return 64;
                components.push_back(component);
            }
            at=end+1;
        }
        if(!strcmp(argv[1],"getcompatpath")) {
            path=std::string(1,drive)+":\\";
            for(size_t i=0;i<components.size();++i){if(i)path+='\\';path+=components[i];}
        } else {
            path=base;
            for(const auto& component:components) {
                std::string actual=component;
                // Match Wine's case-insensitive drive names without changing
                // case-sensitive Linux Steam or its separate Windows library.
                if(DIR* directory=opendir(path.c_str())) {
                    while(dirent* entry=readdir(directory))if(!strcasecmp(entry->d_name,component.c_str())) {
                        actual=entry->d_name;break;
                    }
                    closedir(directory);
                }
                path+="/"+actual;
            }
        }
        fprintf(stderr,"HMOSDECK: path_verb=%s drive=%c mapped=1\n",argv[1],drive);
        puts(path.c_str());return 0;
    }
    if(argc<3||(strcmp(argv[1],"run")&&strcmp(argv[1],"waitforexitandrun"))){
        fputs("HMOSDECK: unsupported compatibility-tool verb\n",stderr);return 64;}
    const char* endpoint=Env("STEAMOHOS_LAUNCH_SOCKET");const char* nonce=Env("STEAMOHOS_LAUNCH_NONCE");
    if(endpoint[0]!='/'||strlen(endpoint)>=sizeof(sockaddr_un::sun_path)||strlen(nonce)!=64){
        fputs("HMOSDECK: host launch service unavailable\n",stderr);return 69;}
    char cwd[4096];if(!getcwd(cwd,sizeof(cwd)))return 72;
    steamohos::LaunchRequest request;request.nonce=nonce;request.appId=Env("SteamAppId");
    if(request.appId.empty())request.appId=Env("STEAM_COMPAT_APP_ID");
    request.verb=argv[1];request.cwd=cwd;request.install=Env("STEAM_COMPAT_INSTALL_PATH");
    if(request.install.empty())request.install=cwd;
    char installed[4096];
    if(!realpath(request.install.c_str(),installed)) {
        fputs("HMOSDECK: install path cannot be resolved\n",stderr);return 72;
    }
    // Resolve GNU library aliases before crossing into native OHOS, where
    // the GNU file adapter is deliberately absent. The broker still validates
    // the real library boundary and PE identity independently.
    request.install=installed;
    const std::string base=installed;
    const std::string originalCwd=request.cwd;
    if(argc>122)return 64;
    for(int i=2;i<argc;++i){if(strlen(argv[i])>16384)return 64;request.argv.emplace_back(argv[i]);}
    if(request.argv[0].empty())return 64;
    if(request.argv[0][0]!='/')request.argv[0]=request.cwd+"/"+request.argv[0];
    char executable[4096];
    if(!realpath(request.argv[0].c_str(),executable)) {
        fputs("HMOSDECK: executable path cannot be resolved\n",stderr);return 72;
    }
    request.argv[0]=executable;
    char clientPath[4096];const char* steamRoot=Env("STEAMOHOS_STEAM_ROOT");
    const std::string client=*steamRoot&&realpath(steamRoot,clientPath)?clientPath:"";
    const bool installer=!client.empty()&&request.argv[0]==client+"/legacycompat/iscriptevaluator.exe";
    const bool driver=!client.empty()&&request.argv[0]==client+"/bin/d3ddriverquery64.exe";
    const bool helper=installer||driver;
    const auto& cwdRoot=helper?client:base;
    request.cwd=originalCwd;
    if(request.cwd!=cwdRoot&&request.cwd.compare(0,cwdRoot.size()+1,cwdRoot+"/")!=0)request.cwd=cwdRoot;
    if(driver){request.install=client;if(request.appId.empty())request.appId="0";}
    for(const char* key:{"SteamAppId","SteamGameId","SteamOverlayGameId"}){
        const char* value=Env(key);if(*value)request.environment.push_back(std::string(key)+"="+value);}
    GameBridge bridge;if(!helper&&!bridge.Start(request.appId)){fputs("HMOSDECK: experimental Steam API helper unavailable\n",stderr);return 73;}
    if(!bridge.credential.empty())request.environment.push_back("HMOSDECK_STEAM_BRIDGE="+bridge.credential);
    std::string frame=steamohos::EncodeLaunch(request);if(frame.size()>steamohos::LaunchMaxBytes+4)return 64;
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return 69;
    sockaddr_un address{};address.sun_family=AF_UNIX;strcpy(address.sun_path,endpoint);
    timeval timeout{10,0};setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))||!Transfer(fd,frame.data(),frame.size(),true)){close(fd);return 69;}
    struct sigaction action{};action.sa_handler=Cancel;sigemptyset(&action.sa_mask);
    sigaction(SIGTERM,&action,nullptr);sigaction(SIGINT,&action,nullptr);sigaction(SIGHUP,&action,nullptr);
    bool accepted=false;unsigned idle=0;uint32_t reply[3];size_t bytes=0;
    for(;;){if(cancelled){char cancel='C';send(fd,&cancel,1,MSG_NOSIGNAL);close(fd);return 130;}
        pollfd event{fd,POLLIN,0};int ready=poll(&event,1,1000);
        if(ready<0&&errno==EINTR)continue;
        if(ready<0){close(fd);return 69;}
        // Only the initial acceptance has a deadline; game lifetime belongs to host.
        if(!ready){if(!accepted&&++idle>=15){close(fd);return 69;}continue;}
        ssize_t n=recv(fd,reinterpret_cast<char*>(reply)+bytes,sizeof(reply)-bytes,0);
        if(n<=0){close(fd);return 69;}bytes+=static_cast<size_t>(n);if(bytes<sizeof(reply))continue;bytes=0;
        const auto magic=ntohl(reply[0]),kind=ntohl(reply[1]),code=ntohl(reply[2]);
        if(magic!=steamohos::LaunchReplyMagic){close(fd);return 76;}
        if(kind==1){if(accepted){close(fd);return 76;}accepted=true;
            fprintf(stderr,"HMOSDECK: OHOS Wine accepted appId=%s; Steam API authentication unverified\n",request.appId.c_str());continue;}
        close(fd);
        if(kind==2&&accepted)return static_cast<int>(code<=255?code:1);
        fputs(kind==4?"HMOSDECK: process ended; exit status unavailable\n":"HMOSDECK: host rejected game launch\n",stderr);
        return 70;
    }
}
