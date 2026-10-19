#include "linux_steam/launch_broker.h"
#include <cassert>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
using namespace steamohos;
static void Tick(){std::this_thread::sleep_for(std::chrono::milliseconds(10));}
static int Connect(const std::string& endpoint){int fd=socket(AF_UNIX,SOCK_STREAM,0);assert(fd>=0);sockaddr_un a{};a.sun_family=AF_UNIX;strcpy(a.sun_path,endpoint.c_str());assert(!connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a)));return fd;}
int main(int argc,char** argv){
    assert(argc==3);char pattern[]="/tmp/hmosdeck-launch-XXXXXX";std::string root=mkdtemp(pattern);
    std::string library=root+"/SteamLinuxARM64/library",install=library+"/steamapps/common/仙剑 测试";
    std::filesystem::create_directories(install);std::string exe=install+"/game.exe";
    unsigned char pe[128]{};pe[0]='M';pe[1]='Z';pe[60]=64;memcpy(pe+64,"PE\0\0\x64\x86",6);
    {std::ofstream file(exe);file.write(reinterpret_cast<char*>(pe),sizeof(pe));}
    LaunchBroker broker;assert(broker.Start(root,library));assert(!broker.Start(root,library));broker.SetAncestor(getpid());
    assert(broker.OwnsProcess(getpid()));
    assert(!broker.OwnsProcess(getppid()) && !broker.OwnsProcess(-1));
    auto parameters=broker.Parameters();size_t split=parameters.find('\n');std::string endpoint=parameters.substr(0,split),nonce=parameters.substr(split+1);
    LaunchRequest valid;valid.nonce=nonce;valid.appId="3590";valid.verb="waitforexitandrun";valid.cwd=install;valid.install=install;valid.argv={exe,"中文 空格","a\"b","$(not-a-command)"};valid.environment={"SteamAppId=3590"};
    auto send=[&](const LaunchRequest& request){int fd=Connect(endpoint);std::string frame=EncodeLaunch(request);assert(sendto(fd,frame.data(),frame.size(),MSG_NOSIGNAL,nullptr,0)==static_cast<ssize_t>(frame.size()));return fd;};
    int fd=send(valid);auto events=broker.Drain();assert(events.size()==1&&events[0].argv==valid.argv&&events[0].nonce.empty());
    auto id=events[0].id;assert(broker.Reply(id,1,0));uint32_t reply[3];assert(recv(fd,reply,sizeof(reply),MSG_WAITALL)==sizeof(reply));assert(ntohl(reply[1])==1);
    assert(broker.Reply(id,2,23));assert(recv(fd,reply,sizeof(reply),MSG_WAITALL)==sizeof(reply)&&ntohl(reply[2])==23);close(fd);
    auto rejected=[&](LaunchRequest request){int client=send(request);assert(broker.Drain().empty());char byte;assert(recv(client,&byte,1,0)==0);close(client);};
    auto bad=valid;bad.nonce[0]=bad.nonce[0]=='0'?'1':'0';rejected(bad);
    bad=valid;bad.environment={"LD_PRELOAD=/tmp/evil"};rejected(bad);
    for(const std::string& token:{"0:"+std::string(64,'a')+":3590","65536:"+std::string(64,'a')+":3590","1234:"+std::string(63,'a')+":3590","1234:"+std::string(64,'g')+":3590","1234:"+std::string(64,'a')+":3591"}) {
        bad=valid;bad.environment.push_back("HMOSDECK_STEAM_BRIDGE="+token);rejected(bad);
    }
    auto bridge=valid;bridge.environment.push_back("HMOSDECK_STEAM_BRIDGE=1234:"+std::string(64,'a')+":3590");
    int bridged=send(bridge);auto bridgedEvents=broker.Drain();assert(bridgedEvents.size()==1&&bridgedEvents[0].environment==bridge.environment);
    assert(broker.Reply(bridgedEvents[0].id,3,1));assert(recv(bridged,reply,sizeof(reply),MSG_WAITALL)==sizeof(reply));close(bridged);

    bad=valid;bad.appId="0";rejected(bad);
    bad=valid;bad.environment={"SteamAppId=999"};rejected(bad);
    std::filesystem::create_symlink("/bin/true",install+"/escaped.exe");bad=valid;bad.argv[0]=install+"/escaped.exe";rejected(bad);
    bad=valid;bad.cwd=root;rejected(bad);
    int partial=Connect(endpoint);auto frame=EncodeLaunch(valid);assert(write(partial,frame.data(),3)==3);assert(broker.Drain().empty());
    assert(write(partial,frame.data()+3,frame.size()-3)==static_cast<ssize_t>(frame.size()-3));events=broker.Drain();assert(events.size()==1);
    id=events[0].id;close(partial);events=broker.Drain();assert(events.size()==1&&events[0].id==id&&events[0].cancel);
    broker.SetAncestor(1);int denied=send(valid); // Root PID 1 is deliberately disallowed as the session anchor.
    broker.SetAncestor(-1);assert(broker.Drain().empty());char byte;{ssize_t n=recv(denied,&byte,1,0);assert(n==0||(n<0&&errno==ECONNRESET));}close(denied);
    broker.SetAncestor(getpid());
    // Real compatibility-tool process stays alive after acceptance and propagates the actual exit status.
    const std::string client=root+"/SteamLinuxARM64/client/.steam/steam";
    const std::string aliasInstall=client+"/steamapps/common/仙剑 测试";
    std::filesystem::create_directories(client+"/steamapps");
    {std::ofstream marker(library+"/steamapps/.hmosdeck-single-library-v2");marker<<library;}
    for(bool aliases:{false,true}) {
    pid_t child=fork();assert(child>=0);
    if(!child){setenv("STEAMOHOS_LAUNCH_SOCKET",endpoint.c_str(),1);setenv("STEAMOHOS_LAUNCH_NONCE",nonce.c_str(),1);
        setenv("SteamAppId","3590",1);setenv("STEAM_COMPAT_INSTALL_PATH",(aliases?aliasInstall:install).c_str(),1);assert(!chdir(install.c_str()));
        if(aliases){setenv("LD_PRELOAD",argv[2],1);setenv("STEAMOHOS_STEAM_ROOT",client.c_str(),1);setenv("STEAMOHOS_LIBRARY_ROOT",library.c_str(),1);}
        const std::string target=aliases?aliasInstall+"/game.exe":exe;
        execl(argv[1],argv[1],"waitforexitandrun",target.c_str(),"中文 空格",nullptr);_exit(127);}
    events.clear();for(unsigned i=0;i<200&&events.empty();++i){events=broker.Drain();Tick();}
    assert(broker.OwnsProcess(child));
    assert(events.size()==1&&events[0].argv[1]=="中文 空格");id=events[0].id;assert(broker.Reply(id,1,0));Tick();int status=0;assert(waitpid(child,&status,WNOHANG)==0);
    assert(broker.Reply(id,2,23));assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==23);
    assert(!broker.OwnsProcess(child));
    }
    // Bad frames never allocate unbounded buffers or produce a launch.
    fd=Connect(endpoint);uint32_t oversized=htonl(LaunchMaxBytes+1);assert(write(fd,&oversized,4)==4);assert(broker.Drain().empty());assert(recv(fd,&byte,1,0)==0);close(fd);
    broker.Stop();assert(!broker.OwnsProcess(getpid()));assert(access(endpoint.c_str(),F_OK)!=0);std::filesystem::remove_all(root);
    std::cout<<"PASS: native socket identity, nonce, path/symlink/PE boundary, env allowlist, fragmented/oversized frames, cancellation, real tool lifetime/exit status\n";
}
