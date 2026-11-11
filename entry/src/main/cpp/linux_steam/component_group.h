#pragma once
#include <cerrno>
#include <cctype>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
namespace steamohos {
inline std::string ReadOwnedRecord(const std::string& path,size_t limit=1048576) {
    int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return {};
    struct stat st{};std::string value;
    if(!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&st.st_nlink==1&&st.st_size>0&&st.st_size<=static_cast<off_t>(limit)) {
        value.resize(st.st_size);size_t used=0;
        while(used<value.size()){ssize_t n=read(fd,&value[used],value.size()-used);if(n<0&&errno==EINTR)continue;if(n<=0)break;used+=n;}
        if(used!=value.size())value.clear();
    }
    close(fd);return value;
}
// Adapter-written records contain one unambiguous fixed-width identity.
// Read the real receipt under the shared lease; reject missing/duplicate IDs.
inline std::string ReceiptId(const std::string& text,const std::string& key="id") {
    const auto token="\""+key+"\"";size_t at=text.find(token);
    if(at==text.npos||text.find(token,at+token.size())!=text.npos)return {};
    at+=token.size();auto space=[&](){while(at<text.size()&&std::isspace(static_cast<unsigned char>(text[at])))++at;};
    space();if(at>=text.size()||text[at++]!=':')return {};space();
    if(at>=text.size()||text[at++]!='"'||at+64>=text.size()||text[at+64]!='"')return {};
    const auto id=text.substr(at,64);return id.find_first_not_of("0123456789abcdef")==id.npos?id:std::string();
}
inline std::string CurrentComponentGroup(const std::string& root) {
    const char* names[]={"component-installed.json","media-installed.json","modules-installed.json","dxvk-support/manifest.json","../proton-runtimes/selection.json"};
    const char* keys[]={"componentId","mediaId","modulesId","dxvkId","runtime"};
    std::string result="{\"schemaVersion\":2";
    for(unsigned i=0;i<5;++i) {
        const auto id=ReceiptId(ReadOwnedRecord(root+"/"+names[i]),i==4?"activeId":"id");
        if(id.empty())return {};
        result+=",\""+std::string(keys[i])+"\":\""+id+"\"";
    }
    return result+"}";
}
inline bool ComponentGroupReady(const std::string& root,std::string* identity=nullptr) {
    const auto current=CurrentComponentGroup(root);
    const bool ready=!current.empty()&&ReadOwnedRecord(root+"/component-group-ready.json",4096)==current;
    if(identity)*identity=ready?current:std::string();
    return ready;
}
}
