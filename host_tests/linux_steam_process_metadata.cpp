#include "linux_steam/cooperative_process.h"
#include <cassert>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <pthread.h>
#include <signal.h>
#include <thread>

int main(int argc,char** argv) {
    assert(argc==2);auto observe=reinterpret_cast<void(*)(int)>(dlsym(RTLD_DEFAULT,"SteamOhosObserveSocket"));
    auto stop=reinterpret_cast<void(*)()>(dlsym(RTLD_DEFAULT,"HslProcessRuntimeStopV1"));assert(observe&&stop);
    sigset_t before,after;assert(!pthread_sigmask(SIG_SETMASK,nullptr,&before));
    int own=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);assert(own>=0);observe(own);
    assert(!pthread_sigmask(SIG_SETMASK,nullptr,&after));
    for(int signal=1;signal<NSIG;++signal)assert(sigismember(&before,signal)==sigismember(&after,signal));
    for(unsigned i=0;i<64;++i) {
        int fd=steamohos::BorrowProcessStat(argv[1],getpid());assert(fd>=0);
        char bytes[4096]{};auto count=read(fd,bytes,sizeof(bytes));assert(count>0);
        assert(std::string(bytes,count).find(std::to_string(getpid())+" (")==0);
        assert(write(fd,"bad",3)<0&&errno==EBADF);close(fd);
    }
    // Terminal worker shutdown wakes poll and joins without touching an
    // application's descriptor or changing its signal mask/dispositions.
    const auto start=std::chrono::steady_clock::now();stop();stop();
    assert(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(200));
    assert(fcntl(own,F_GETFD)>=0);close(own);
    assert(steamohos::BorrowProcessStat(argv[1],getpid())<0);
    std::cout<<"PASS: real read-only process snapshots, caller signal mask, terminal owned-worker shutdown\n";
}
