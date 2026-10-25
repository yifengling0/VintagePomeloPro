#pragma once
#include "launch_wire.h"
#include <mutex>
#include <sys/types.h>
namespace steamohos {
using ProcessIdentityReader = bool (*)(pid_t, pid_t&, uint64_t&);
class LaunchBroker {
public:
    explicit LaunchBroker(ProcessIdentityReader reader = nullptr);
    ~LaunchBroker();
    bool Start(const std::string& base, const std::string& library);
    void Stop();
    void SetAncestor(pid_t pid);
    bool OwnsProcess(pid_t pid);
    bool RegisterWindowProcess(pid_t supervisor, pid_t window);
    void ForgetWindowProcess(pid_t supervisor);
    std::string Parameters();
    std::vector<LaunchRequest> Drain();
    bool GetRequest(uint32_t id, LaunchRequest& request);
    bool Reply(uint32_t id, uint32_t kind, uint32_t code);
private:
    struct Client { int fd; pid_t pid; uint32_t id; uint64_t since; bool delivered=false; std::string bytes; LaunchRequest request; };
    void Close();
    bool Validate(LaunchRequest& request);
    std::mutex mutex;
    int listener=-1;
    pid_t ancestor=-1;
    uint64_t ancestorStart=0;
    pid_t windowProcess=-1;
    ProcessIdentityReader identityReader;
    uint32_t nextId=0;
    std::string directory, socketPath, nonce, library;
    std::vector<Client> clients;
};
LaunchBroker& GameLaunchBroker();
}
