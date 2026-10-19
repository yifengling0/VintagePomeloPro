#include "launch_broker.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace steamohos {
namespace {
uint64_t Now() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
bool Identity(pid_t pid, pid_t& parent, uint64_t& start) {
    std::ifstream file("/proc/"+std::to_string(pid)+"/stat");std::string line;std::getline(file,line);
    const auto at=line.rfind(')');if(at==std::string::npos)return false;
    std::istringstream values(line.substr(at+1));std::string value;
    for(unsigned field=3;field<=22;++field){if(!(values>>value))return false;
        if(field==4)parent=static_cast<pid_t>(strtol(value.c_str(),nullptr,10));
        if(field==22)start=strtoull(value.c_str(),nullptr,10);}
    return true;
}
bool Descendant(pid_t pid,pid_t ancestor,uint64_t stamp) {
    if(ancestor<=0||!stamp)return false;
    for(unsigned n=0;n<64&&pid>1;++n){pid_t parent=0;uint64_t start=0;
        if(!Identity(pid,parent,start))return false;
        if(pid==ancestor)return start==stamp;
        if(parent==pid)return false;
        pid=parent;}
    return false;
}
std::string Real(const std::string& path) { char out[4096];return realpath(path.c_str(),out)?out:""; }
bool Under(const std::string& path,const std::string& root) {
    return !root.empty()&&path.size()>root.size()&&path.compare(0,root.size(),root)==0&&path[root.size()]=='/';
}
bool Id(const std::string& value) { return !value.empty()&&value.size()<=20&&value.find_first_not_of("0123456789")==std::string::npos&&value!="0"; }
bool Pe(const std::string& path) {
    int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return false;
    unsigned char header[64],signature[6];struct stat st{};
    bool ok=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&pread(fd,header,sizeof(header),0)==sizeof(header)&&header[0]=='M'&&header[1]=='Z';
    if(ok){uint32_t offset=header[60]|header[61]<<8|header[62]<<16|uint32_t(header[63])<<24;
        ok=offset>=64&&offset<16777216&&pread(fd,signature,sizeof(signature),offset)==sizeof(signature)&&
            !memcmp(signature,"PE\0\0",4)&&((signature[4]==0x64&&signature[5]==0x86)||(signature[4]==0x4c&&signature[5]==1));}
    close(fd);return ok;
}
}
LaunchBroker::~LaunchBroker(){Stop();}
void LaunchBroker::Close(){
    for(const auto& c:clients)close(c.fd);
    clients.clear();
    if(listener>=0)close(listener);
    listener=-1;
    if(!socketPath.empty())unlink(socketPath.c_str());
    if(!directory.empty())rmdir(directory.c_str());
    directory.clear();socketPath.clear();nonce.clear();library.clear();ancestor=-1;ancestorStart=0;
}
bool LaunchBroker::Start(const std::string& base,const std::string& root){
    std::lock_guard<std::mutex> guard(mutex);if(listener>=0)return false;
    library=Real(root);if(library.empty())return false;
    unsigned char random[32];int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
    bool ok=fd>=0&&read(fd,random,sizeof(random))==sizeof(random);if(fd>=0)close(fd);if(!ok){Close();return false;}
    char digits[]="0123456789abcdef";for(auto b:random){nonce+=digits[b>>4];nonce+=digits[b&15];}
    std::string pattern=base+"/launch-XXXXXX";std::vector<char> path(pattern.begin(),pattern.end());path.push_back(0);
    char* created=mkdtemp(path.data());if(!created){Close();return false;}directory=created;socketPath=directory+"/s";
    sockaddr_un address{};address.sun_family=AF_UNIX;
    if(socketPath.size()>=sizeof(address.sun_path)){Close();return false;}strcpy(address.sun_path,socketPath.c_str());
    listener=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(listener<0||bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))||chmod(socketPath.c_str(),0600)||listen(listener,4)){Close();return false;}
    return true;
}
void LaunchBroker::Stop(){std::lock_guard<std::mutex> guard(mutex);Close();}
void LaunchBroker::SetAncestor(pid_t pid){std::lock_guard<std::mutex> guard(mutex);ancestor=pid;pid_t parent=0;if(!Identity(pid,parent,ancestorStart))ancestorStart=0;}
bool LaunchBroker::OwnsProcess(pid_t pid) {
    std::lock_guard<std::mutex> guard(mutex);
    return listener >= 0 && Descendant(pid, ancestor, ancestorStart);
}
std::string LaunchBroker::Parameters(){std::lock_guard<std::mutex> guard(mutex);return listener>=0?socketPath+"\n"+nonce:"";}
bool LaunchBroker::Validate(LaunchRequest& r){
    if(r.nonce!=nonce||(r.verb!="run"&&r.verb!="waitforexitandrun"))return false;
    std::string executable=r.argv.front();
    if(executable.empty()||executable.front()!='/')return false;
    executable=Real(executable);
    const auto client=Real(library+"/../client/.steam/steam");
    const auto common=Real(library+"/steamapps/common");
    r.install=Real(r.install);r.cwd=Real(r.cwd);r.role="game";
    // Linux Steam ships these PE helpers itself. ArkTS additionally checks the
    // pinned official content digest before launching; other client PE files
    // (including legacy steamclient DLLs) never acquire an execution exception.
    if(!client.empty()&&executable==client+"/legacycompat/iscriptevaluator.exe")r.role="install_script";
    else if(!client.empty()&&executable==client+"/bin/d3ddriverquery64.exe")r.role="driver_query";
    const bool helper=r.role!="game",query=r.role=="driver_query";
    if(!(query&&r.appId=="0")&&!Id(r.appId))return false;
    if(query){if(r.install!=client)return false;}
    else if(common.empty()||!Under(common,library)||!Under(r.install,common))return false;
    const auto cwdRoot=helper?client:r.install;
    if(r.cwd!=cwdRoot&&!Under(r.cwd,cwdRoot))return false;
    if((!helper&&!Under(executable,r.install))||!Pe(executable))return false;
    r.argv.front()=executable;
    for(const auto& e:r.environment){auto at=e.find('=');if(at==std::string::npos)return false;
        const auto key=e.substr(0,at),value=e.substr(at+1);
        if(key=="HMOSDECK_STEAM_BRIDGE") {
            if(helper)return false;
            // Only the authenticated GNU tool can supply this per-game local
            // capability. Do not accept arbitrary Wine environment overrides.
            const auto first=value.find(':'),last=value.rfind(':');
            if(first==std::string::npos||last!=first+65||first==0||first>5||value.substr(last+1)!=r.appId)return false;
            const auto port=value.substr(0,first),secret=value.substr(first+1,64);
            if(port.find_first_not_of("0123456789")!=std::string::npos||strtoul(port.c_str(),nullptr,10)==0||strtoul(port.c_str(),nullptr,10)>65535||secret.find_first_not_of("0123456789abcdef")!=std::string::npos)return false;
        } else {
            if((key!="SteamAppId"&&key!="SteamGameId"&&key!="SteamOverlayGameId")||(!Id(value)&&!(query&&value=="0")))return false;
            if(key=="SteamAppId"&&value!=r.appId)return false;
        }}
    r.nonce.clear();return true;
}
std::vector<LaunchRequest> LaunchBroker::Drain(){
    std::lock_guard<std::mutex> guard(mutex);std::vector<LaunchRequest> events;if(listener<0)return events;
    for(unsigned n=0;n<4;++n){int fd=accept4(listener,nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK);if(fd<0)break;
        ucred peer{};socklen_t size=sizeof(peer);
        if(clients.size()>=4||getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&size)||peer.uid!=geteuid()||!Descendant(peer.pid,ancestor,ancestorStart)){close(fd);continue;}
        clients.push_back({fd,peer.pid,++nextId,Now(),false,{}});}
    for(auto it=clients.begin();it!=clients.end();){bool closeClient=false;char bytes[65540];
        ssize_t n=recv(it->fd,bytes,sizeof(bytes),0);
        if(n>0){if(it->delivered)closeClient=true;else it->bytes.append(bytes,static_cast<size_t>(n));}
        else if(!n||(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR))closeClient=true;
        if(!it->delivered){
            if(it->bytes.size()>LaunchMaxBytes+4||Now()-it->since>5000)closeClient=true;
            if(!closeClient&&it->bytes.size()>=4){uint32_t size;memcpy(&size,it->bytes.data(),4);size=ntohl(size);
                if(size>LaunchMaxBytes)closeClient=true;
                else if(it->bytes.size()>=size+4){LaunchRequest r;
                    if(it->bytes.size()!=size+4||!DecodeLaunch(it->bytes.substr(4),r)||!Validate(r))closeClient=true;
                    else {r.id=it->id;it->delivered=true;it->bytes.clear();events.push_back(r);}}}
        }
        if(closeClient){if(it->delivered){LaunchRequest r;r.id=it->id;r.cancel=true;events.push_back(r);}close(it->fd);it=clients.erase(it);}else ++it;
    }
    return events;
}
bool LaunchBroker::Reply(uint32_t id,uint32_t kind,uint32_t code){
    std::lock_guard<std::mutex> guard(mutex);
    auto it=std::find_if(clients.begin(),clients.end(),[&](const Client& c){return c.id==id&&c.delivered;});
    if(it==clients.end()||kind<1||kind>4)return false;
    uint32_t reply[]={htonl(LaunchReplyMagic),htonl(kind),htonl(code)};
    bool ok=send(it->fd,reply,sizeof(reply),MSG_NOSIGNAL|MSG_DONTWAIT)==sizeof(reply);
    if(kind!=1||!ok){close(it->fd);clients.erase(it);}return ok;
}
LaunchBroker& GameLaunchBroker(){static LaunchBroker broker;return broker;}
}
