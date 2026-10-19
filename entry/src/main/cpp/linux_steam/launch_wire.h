#pragma once
#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace steamohos {
constexpr uint32_t LaunchReplyMagic = 0x484d5231;
constexpr size_t LaunchMaxBytes = 65536;
struct LaunchRequest {
    uint32_t id = 0;
    bool cancel = false;
    std::string nonce, appId, verb, cwd, install;
    std::vector<std::string> argv, environment;
};
inline void PutNumber(std::string& out, uint32_t n) { n=htonl(n); out.append(reinterpret_cast<char*>(&n),4); }
inline void PutString(std::string& out, const std::string& s) { PutNumber(out,s.size()); out+=s; }
inline std::string EncodeLaunch(const LaunchRequest& r) {
    std::string out; PutString(out,"HMOSDECK-LAUNCH/1");
    for (const auto* s : {&r.nonce,&r.appId,&r.verb,&r.cwd,&r.install}) PutString(out,*s);
    PutNumber(out,r.argv.size()); for(const auto& s:r.argv) PutString(out,s);
    PutNumber(out,r.environment.size()); for(const auto& s:r.environment) PutString(out,s);
    std::string frame; PutNumber(frame,out.size()); return frame+out;
}
inline bool DecodeLaunch(const std::string& data, LaunchRequest& r) {
    size_t pos=0;
    auto number=[&](uint32_t& n) { if(pos+4>data.size())return false; memcpy(&n,data.data()+pos,4);n=ntohl(n);pos+=4;return true; };
    auto text=[&](std::string& s) { uint32_t n; if(!number(n)||n>16384||n>data.size()-pos)return false;
        s=data.substr(pos,n);pos+=n;return s.find('\0')==std::string::npos; };
    std::string magic;
    if(data.size()>LaunchMaxBytes || !text(magic)||magic!="HMOSDECK-LAUNCH/1")return false;
    for(auto* s:{&r.nonce,&r.appId,&r.verb,&r.cwd,&r.install})if(!text(*s))return false;
    uint32_t count;
    if(!number(count)||!count||count>120)return false;
    r.argv.clear();r.environment.clear();
    for(uint32_t i=0;i<count;++i){std::string s;if(!text(s))return false;r.argv.push_back(s);}
    if(!number(count)||count>8)return false;
    for(uint32_t i=0;i<count;++i){std::string s;if(!text(s))return false;r.environment.push_back(s);}
    return pos==data.size();
}
}
