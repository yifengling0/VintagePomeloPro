#pragma once
#include <string>
namespace steamohos {
std::string ProbeXwayland(const std::string& directory, const std::string& socketPath, bool steamClient = false, const std::string& externalRoot = {}, const std::string& vtestSocket = {}, const std::string& launch = {}, int audioFd = -1, const std::string& audioSocket = {}, const std::string& gamepadSocket = {}, bool forkBackend = false, bool productSession = false, int ownershipFd = -1);
}
