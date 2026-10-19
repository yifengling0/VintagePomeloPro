#pragma once
#include "launch_wire.h"
#include <mutex>
#include <sys/types.h>
namespace steamohos {
class LaunchBroker {
public:
    ~LaunchBroker();
    bool Start(const std::string& base, const std::string& library);
    void Stop();
    void SetAncestor(pid_t pid);
    bool OwnsProcess(pid_t pid);
    std::string Parameters();
    std::vector<LaunchRequest> Drain();
    bool Reply(uint32_t id, uint32_t kind, uint32_t code);
private:
    struct Client { int fd; pid_t pid; uint32_t id; uint64_t since; bool delivered=false; std::string bytes; };
    void Close();
    bool Validate(LaunchRequest& request);
    std::mutex mutex;
    int listener=-1;
    pid_t ancestor=-1;
    uint64_t ancestorStart=0;
    uint32_t nextId=0;
    std::string directory, socketPath, nonce, library;
    std::vector<Client> clients;
};
LaunchBroker& GameLaunchBroker();
}
