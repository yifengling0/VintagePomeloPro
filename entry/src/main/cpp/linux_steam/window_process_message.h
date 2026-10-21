#pragma once
#include <climits>
#include <string>
namespace steamohos {
// First message on the private NCP result socket. This descriptor stays in the
// trusted OHOS supervisor; it is never inherited by the GNU executables.
inline std::string WindowProcessMessage(int pid) {
    return "HMOSDECK_WINDOW_V1 " + std::to_string(pid) + "\n";
}
// 0: partial; 1: consumed; -1: invalid; 2: final JSON without a registration.
inline int ConsumeWindowProcessMessage(std::string& input, int& pid) {
    if(input.empty())return 0;
    if(input.front()=='{')return 2;
    const std::string prefix="HMOSDECK_WINDOW_V1 ";
    const auto end=input.find('\n');
    if(end==std::string::npos)return input.size()<=48?0:-1;
    if(end<=prefix.size()||end>48||input.compare(0,prefix.size(),prefix)!=0)return -1;
    unsigned long value=0;
    for(size_t i=prefix.size();i<end;++i) {
        if(input[i]<'0'||input[i]>'9')return -1;
        value=value*10+static_cast<unsigned>(input[i]-'0');
        if(value>INT_MAX)return -1;
    }
    if(value<=1)return -1;
    pid=static_cast<int>(value);input.erase(0,end+1);return 1;
}
}
