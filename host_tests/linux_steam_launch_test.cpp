#include "linux_steam/launch_broker.h"
#include "linux_steam/window_process_message.h"
#include "linux_steam/gnu_fixture/native_game_session.h"
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
    // A late post-preparation check cannot revive a disconnected controller.
    for(bool cancel:{false,true}) {
        int peer=send(valid);auto pending=broker.Drain();assert(pending.size()==1);
        auto ticket=pending[0].id;assert(broker.Reply(ticket,1,0));
        assert(recv(peer,reply,sizeof(reply),MSG_WAITALL)==sizeof(reply));
        if(cancel)assert(write(peer,"C",1)==1);else {close(peer);peer=-1;}
        assert(!broker.Reply(ticket,1,0));LaunchRequest gone;assert(!broker.GetRequest(ticket,gone));
        if(peer>=0)close(peer);
    }
    auto rejected=[&](LaunchRequest request){int client=send(request);assert(broker.Drain().empty());char byte;assert(recv(client,&byte,1,0)==0);close(client);};
    for(const std::string backend:{"gnu-proton","gnu-proton-gl"}) {
        auto gnu=valid;gnu.environment.push_back("HMOSDECK_RUNTIME="+backend);
        int client=send(gnu);auto delivered=broker.Drain();assert(delivered.size()==1&&delivered[0].backend==backend);
        LaunchRequest snapshot;assert(broker.GetRequest(delivered[0].id,snapshot));assert(snapshot.argv==valid.argv&&snapshot.backend==backend&&snapshot.nonce.empty());
        assert(broker.Reply(delivered[0].id,2,0));assert(!broker.GetRequest(delivered[0].id,snapshot));close(client);
    }
    auto invalidBackend=valid;invalidBackend.environment.push_back("HMOSDECK_RUNTIME=unknown");rejected(invalidBackend);
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
    assert(events.size()==1&&events[0].argv[1]=="中文 空格");id=events[0].id;assert(broker.Reply(id,1,0));
    if(!aliases){
        // Use the unchanged shipped GNU tool: cold preparation exceeds its
        // original 15s handshake deadline, before actual process dispatch.
        std::this_thread::sleep_for(std::chrono::seconds(16));
        int pendingStatus=0;assert(waitpid(child,&pendingStatus,WNOHANG)==0);
    }
    assert(broker.Reply(id,1,0));Tick();int status=0;assert(waitpid(child,&status,WNOHANG)==0);
    assert(broker.Reply(id,2,23));assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==23);
    assert(!broker.OwnsProcess(child));
    }
    // Only the two official Linux client helper paths cross the game boundary.
    // Their content digest is independently checked by the ArkTS launch adapter.
    for(const std::string relative:{"legacycompat/iscriptevaluator.exe","bin/d3ddriverquery64.exe"}) {
        const std::string helper=client+"/"+relative;
        std::filesystem::create_directories(std::filesystem::path(helper).parent_path());
        {std::ofstream file(helper);file.write(reinterpret_cast<char*>(pe),sizeof(pe));}
        auto request=valid;request.argv={helper,"legacycompat\\evaluatorscript_3590.vdf"};request.cwd=client;
        const bool query=relative[0]=='b';
        if(query){request.install=client;request.appId="0";request.environment={};}
        int helperFd=send(request);events=broker.Drain();assert(events.size()==1);
        assert(events[0].role==(query?"driver_query":"install_script"));
        assert(broker.Reply(events[0].id,1,0));assert(recv(helperFd,reply,sizeof(reply),MSG_WAITALL)==sizeof(reply));
        assert(broker.Reply(events[0].id,2,7));assert(recv(helperFd,reply,sizeof(reply),MSG_WAITALL)==sizeof(reply)&&ntohl(reply[2])==7);close(helperFd);
        auto wrong=request;wrong.cwd=root;rejected(wrong);
        wrong=request;wrong.environment.push_back("HMOSDECK_STEAM_BRIDGE=1234:"+std::string(64,'a')+":"+request.appId);rejected(wrong);
        wrong=request;wrong.argv[0]=client+"/legacycompat/unknown.exe";
        {std::ofstream file(wrong.argv[0]);file.write(reinterpret_cast<char*>(pe),sizeof(pe));}rejected(wrong);
        if(!query){wrong=request;wrong.install=root;rejected(wrong);}
        pid_t child=fork();assert(child>=0);
        if(!child){setenv("STEAMOHOS_LAUNCH_SOCKET",endpoint.c_str(),1);setenv("STEAMOHOS_LAUNCH_NONCE",nonce.c_str(),1);
            setenv("STEAMOHOS_STEAM_ROOT",client.c_str(),1);setenv("SteamAppId",request.appId.c_str(),1);
            setenv("STEAM_COMPAT_INSTALL_PATH",request.install.c_str(),1);setenv("STEAMOHOS_STEAMCLIENT_GAME_BRIDGE","1",1);
            assert(!chdir(client.c_str()));execl(argv[1],argv[1],"run",helper.c_str(),request.argv[1].c_str(),nullptr);_exit(127);}
        events.clear();for(unsigned i=0;i<200&&events.empty();++i){events=broker.Drain();Tick();}
        assert(events.size()==1&&events[0].cwd==client&&events[0].role==(query?"driver_query":"install_script"));
        assert(broker.Reply(events[0].id,1,0));assert(broker.Reply(events[0].id,2,0));int status=0;
        assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    }
    // Native game wrappers report their real child exit; control must leave
    // Steam, the wrapper's parent and another running session untouched.
    const auto nativeGame=install+"/native.sh";
    {std::ofstream file(nativeGame);file<<"#!/bin/sh\nexit 23\n";}
    auto nativeRequest=valid;nativeRequest.backend="linux-native";nativeRequest.verb="observe";
    nativeRequest.argv={exe};rejected(nativeRequest); // PE never becomes native.
    nativeRequest=valid;nativeRequest.argv={nativeGame};rejected(nativeRequest); // Native never becomes Wine.
    pid_t unrelated=fork();assert(unrelated>=0);if(!unrelated){pause();_exit(0);}
    for(unsigned control:{0u,5u,6u}) {
        const pid_t wrapper=fork();assert(wrapper>=0);
        if(!wrapper) {
            setenv("STEAMOHOS_LAUNCH_SOCKET",endpoint.c_str(),1);setenv("STEAMOHOS_LAUNCH_NONCE",nonce.c_str(),1);
            setenv("SteamAppId","3590",1);NativeGameSession session;
            if(!session.Start(nativeGame,install))_exit(91);
            const pid_t game=fork();if(game<0)_exit(92);
            if(!game){setpgid(0,0);if(control)execl("/bin/sleep","sleep","20",nullptr);
                else execl("/bin/sh","sh",nativeGame.c_str(),nullptr);
                _exit(93);}
            setpgid(game,game);if(!session.Started(game))_exit(94);
            int status=0;if(session.Wait(game,status))_exit(95);
            const bool correct=control?WIFSIGNALED(status)&&WTERMSIG(status)==(control==5?SIGTERM:SIGKILL):WIFEXITED(status)&&WEXITSTATUS(status)==23;
            _exit(correct?0:96);
        }
        bool started=false,ended=false;uint32_t nativeId=0;
        for(unsigned attempt=0;attempt<500&&!ended;++attempt) {
            for(const auto& event:broker.Drain()) {
                assert(!event.cancel&&event.backend=="linux-native");nativeId=event.id;
                if(event.phase=="starting")assert(broker.Reply(event.id,1,0));
                else if(event.phase=="running") {
                    started=true;if(control)assert(broker.OwnsGameProcess(event.id,event.processId));
                    assert(!broker.OwnsGameProcess(event.id,wrapper));assert(!broker.OwnsGameProcess(event.id,unrelated));
                    if(control)assert(broker.Reply(event.id,control,0));
                } else if(event.phase=="exited") {
                    ended=true;assert(control?event.exitSignal==(control==5?SIGTERM:SIGKILL):event.exitCode==23);
                }
            }
            Tick();
        }
        assert(started&&ended);int status=0;assert(waitpid(wrapper,&status,0)==wrapper&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
        assert(!broker.OwnsGameProcess(nativeId,wrapper));assert(!kill(unrelated,0));
    }
    kill(unrelated,SIGTERM);assert(waitpid(unrelated,nullptr,0)==unrelated);
    // Bad frames never allocate unbounded buffers or produce a launch.
    fd=Connect(endpoint);uint32_t oversized=htonl(LaunchMaxBytes+1);assert(write(fd,&oversized,4)==4);assert(broker.Drain().empty());assert(recv(fd,&byte,1,0)==0);close(fd);
    broker.Stop();assert(!broker.OwnsProcess(getpid()));assert(access(endpoint.c_str(),F_OK)!=0);
    // The private supervisor reports exactly the Xwayland PID. Cross-process
    // /proc is intentionally unavailable in this production-policy fixture.
    LaunchBroker noProc([](pid_t,pid_t&,uint64_t&){return false;});
    assert(noProc.Start(root,library));noProc.SetAncestor(getpid());
    const auto restrictedParameters=noProc.Parameters();const auto separator=restrictedParameters.find('\n');
    const auto restrictedSocket=restrictedParameters.substr(0,separator);
    auto restrictedRequest=valid;restrictedRequest.nonce=restrictedParameters.substr(separator+1);
    auto restrictedSend=[&](const LaunchRequest& request){int socket=Connect(restrictedSocket);
        const auto message=EncodeLaunch(request);assert(write(socket,message.data(),message.size())==static_cast<ssize_t>(message.size()));return socket;};
    fd=restrictedSend(restrictedRequest);assert(noProc.Drain().empty());assert(recv(fd,&byte,1,0)<=0);close(fd);
    assert(!noProc.RegisterWindowProcess(getpid()+1,getppid()));
    assert(!noProc.RegisterWindowProcess(getpid(),getpid()));
    pid_t window=fork();assert(window>=0);if(!window){pause();_exit(0);}
    assert(noProc.RegisterWindowProcess(getpid(),window));
    assert(!noProc.RegisterWindowProcess(getpid(),window+1));
    assert(noProc.OwnsProcess(window));assert(!noProc.OwnsProcess(getpid()));
    auto restrictedBad=restrictedRequest;restrictedBad.nonce[0]^=1;
    fd=restrictedSend(restrictedBad);assert(noProc.Drain().empty());assert(recv(fd,&byte,1,0)==0);close(fd);
    restrictedBad=restrictedRequest;restrictedBad.cwd=root;
    fd=restrictedSend(restrictedBad);assert(noProc.Drain().empty());assert(recv(fd,&byte,1,0)==0);close(fd);
    fd=restrictedSend(restrictedRequest);events=noProc.Drain();assert(events.size()==1);
    assert(noProc.Reply(events[0].id,3,1));assert(recv(fd,reply,sizeof(reply),MSG_WAITALL)==sizeof(reply));close(fd);
    noProc.ForgetWindowProcess(getpid()+1);assert(noProc.OwnsProcess(window));
    noProc.ForgetWindowProcess(getpid());assert(!noProc.OwnsProcess(window));
    fd=restrictedSend(restrictedRequest);assert(noProc.Drain().empty());assert(recv(fd,&byte,1,0)<=0);close(fd);
    kill(window,SIGTERM);assert(waitpid(window,nullptr,0)==window);noProc.Stop();
    for(const auto& invalid: {std::string("HMOSDECK_WINDOW_V1 0\n"),std::string("HMOSDECK_WINDOW_V1 -1\n"),
        std::string("HMOSDECK_WINDOW_V1 2147483648\n"),std::string("HMOSDECK_WINDOW_V1 12x\n"),std::string(49,'x')}) {
        auto message=invalid;int parsed=-1;assert(ConsumeWindowProcessMessage(message,parsed)==-1);
    }
    std::string fragmented="HMOSDECK_WINDOW_";int parsed=-1;
    assert(ConsumeWindowProcessMessage(fragmented,parsed)==0);
    fragmented+="V1 1234\n{\"status\":\"done\"}";
    assert(ConsumeWindowProcessMessage(fragmented,parsed)==1&&parsed==1234);
    assert(fragmented=="{\"status\":\"done\"}");assert(ConsumeWindowProcessMessage(fragmented,parsed)==2);
    std::filesystem::remove_all(root);
    std::cout<<"PASS: native socket identity, NCP window attestation with hidden /proc, fragmented metadata, nonce, path/symlink/PE boundary, env allowlist, fragmented/oversized frames, cancellation, real tool lifetime/exit status\n";
}
