#pragma once
#include <atomic>
#include <string>
#include <thread>
namespace steamohos {
// CEF closes inherited descriptors. This private, per-session endpoint forwards
// a same-UID client's control FD to the existing AudioBroker bootstrap channel.
class AudioBootstrapRelay {
public:
    ~AudioBootstrapRelay();
    bool Start(int bootstrap,const std::string& root="/data/storage/el2/base/files/steamohos-native");
    const std::string& Path() const { return path_; }
private:
    void Run();
    int listener_=-1,bootstrap_=-1;
    std::string directory_,path_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
};
}
