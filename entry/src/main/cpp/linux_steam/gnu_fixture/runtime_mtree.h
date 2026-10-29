// Steam Runtime's minimized /usr tree omits links listed in usr-mtree.txt.gz.
// Rehydrate only that metadata; never copy or replace any depot file payload.
#ifndef STEAMOHOS_RUNTIME_MTREE_H
#define STEAMOHOS_RUNTIME_MTREE_H
#include <dlfcn.h>
#include <cstdio>
#include <sstream>
#include <utility>
namespace steamohos::fex {
inline bool MtreeDecode(const std::string& input,std::string& output) {
    output.clear();
    for(size_t i=0;i<input.size();++i) {
        unsigned char c=input[i];
        if(c=='\\') {
            if(i+3>=input.size())return false;
            unsigned value=0;
            for(unsigned j=1;j<=3;++j) {
                if(input[i+j]<'0'||input[i+j]>'7')return false;
                value=value*8+input[i+j]-'0';
            }
            if(!value||value>255)return false;
            c=value;i+=3;
        }
        if(!c||c=='\n'||c=='\r')return false;
        output+=static_cast<char>(c);
    }
    return output.size()<PATH_MAX;
}
inline bool RestoreRuntimeUsr(const std::string& root,const std::string& manifest,std::string& error) {
    void* z=dlopen("libz.so.1",RTLD_NOW|RTLD_LOCAL);
    if(!z){error="runtime_mtree_zlib_missing";return false;}
    auto openGz=reinterpret_cast<void*(*)(int,const char*)>(dlsym(z,"gzdopen"));
    auto readGz=reinterpret_cast<int(*)(void*,void*,unsigned)>(dlsym(z,"gzread"));
    auto closeGz=reinterpret_cast<int(*)(void*)>(dlsym(z,"gzclose"));
    if(!openGz||!readGz||!closeGz){dlclose(z);error="runtime_mtree_zlib_symbols";return false;}
    int fd=open(manifest.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0){dlclose(z);error="runtime_mtree_missing";return false;}
    struct stat status{};
    if(fstat(fd,&status)||!S_ISREG(status.st_mode)||status.st_size<=0||status.st_size>4*1024*1024) {
        close(fd);dlclose(z);error="runtime_mtree_size";return false;
    }
    void* stream=openGz(fd,"rb");
    if(!stream){close(fd);dlclose(z);error="runtime_mtree_open";return false;}
    std::string content;char block[16384];int count;
    while((count=readGz(stream,block,sizeof(block)))>0&&content.size()+count<=8*1024*1024)content.append(block,count);
    int closed=closeGz(stream);dlclose(z);
    if(count!=0||closed){error="runtime_mtree_read";return false;}
    std::vector<std::pair<std::string,std::string>> links;
    std::istringstream lines(content);std::string line;
    // Validate the complete bounded manifest before modifying anything.
    while(std::getline(lines,line)) {
        if(line.size()>16384){error="runtime_mtree_line_limit";return false;}
        if(line.empty()||line[0]=='#')continue;
        std::istringstream fields(line);std::string encoded,type,target,field,path;
        fields>>encoded;
        while(fields>>field) {
            if(field.compare(0,5,"type=")==0)type=field.substr(5);
            if(field.compare(0,5,"link=")==0)target=field.substr(5);
        }
        if(!MtreeDecode(encoded,path)){error="runtime_mtree_path";return false;}
        if(path=="."&&type=="dir")continue;
        if(path.compare(0,2,"./")||path.size()==2){error="runtime_mtree_path";return false;}
        path=path.substr(2);
        for(const auto& part:fs::path(path))if(part==".."||part==".") {
            error="runtime_mtree_traversal";return false;
        }
        if(type!="link")continue;
        std::string decoded;
        if(!MtreeDecode(target,decoded)||decoded.empty()||links.size()>=8192) {
            error="runtime_mtree_link_limit";return false;
        }
        // Absolute links have guest-root semantics in DepotResolve. Reject
        // targets whose normalized path could escape even that guest root.
        fs::path destination=decoded[0]=='/'?fs::path(decoded).relative_path():fs::path(path).parent_path()/decoded;
        destination=destination.lexically_normal();
        if(!destination.empty()&&*destination.begin()=="..") {error="runtime_mtree_link_escape";return false;}
        links.emplace_back(path,decoded);
    }
    // The downloaded tree represents /usr, with top-level /lib and /bin
    // aliases sharing that same tree. A tiny link provides /usr without copies.
    links.emplace_back("usr",".");
    uint64_t digest=14695981039346656037ULL;
    for(unsigned char c:content){digest^=c;digest*=1099511628211ULL;}
    const std::string magic="STEAMOHOS_RUNTIME_MTREE_V1\n";
    const std::string stamp=magic+std::to_string(digest)+"\n";
    const std::string cache=root+"/.hmosdeck-usr-mtree-v1";
    bool priorCache=false;
    int cached=open(cache.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(cached>=0) {
        char bytes[128];ssize_t n=read(cached,bytes,sizeof(bytes));close(cached);
        if(n<0||std::string(bytes,n).compare(0,magic.size(),magic)) {
            error="runtime_mtree_cache_conflict";return false;
        }
        priorCache=true;
        char alias[2];
        if(std::string(bytes,n)==stamp&&readlink((root+"/usr").c_str(),alias,sizeof(alias))==1&&alias[0]=='.'&&
           ElfMachine(root+"/lib64/ld-linux-x86-64.so.2")==62) {
            fprintf(stderr,"STEAMOHOS_NATIVE_RUNTIME layout=usr metadata_links=%zu created=0 cached=1 payload_copied=0\n",links.size());
            return true;
        }
    } else if(errno!=ENOENT){error="runtime_mtree_cache_read";return false;}
    unsigned created=0;
    for(const auto& [relative,target]:links) {
        const auto path=root+"/"+relative;
        struct stat found{};
        if(!lstat(path.c_str(),&found)) {
            char stored[PATH_MAX];ssize_t n=readlink(path.c_str(),stored,sizeof(stored));
            if(n==static_cast<ssize_t>(target.size())&&!memcmp(stored,target.data(),target.size()))continue;
            error="runtime_mtree_existing_conflict";return false;
        }
        if(errno!=ENOENT){error="runtime_mtree_lstat";return false;}
        const auto parent=fs::path(path).parent_path().string();std::error_code failure;
        fs::create_directories(parent,failure);
        if(failure||Canonical(parent)!=parent||(parent!=root&&!Below(parent,root))) {
            error="runtime_mtree_parent_scope";return false;
        }
        if(symlink(target.c_str(),path.c_str())){error="runtime_mtree_create_link";return false;}
        ++created;
    }
    if(priorCache&&unlink(cache.c_str())){error="runtime_mtree_cache_replace";return false;}
    int output=open(cache.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(output<0){error="runtime_mtree_cache_create";return false;}
    ssize_t written=write(output,stamp.data(),stamp.size());int flushed=close(output);
    if(written!=static_cast<ssize_t>(stamp.size())||flushed){unlink(cache.c_str());error="runtime_mtree_cache_write";return false;}
    fprintf(stderr,"STEAMOHOS_NATIVE_RUNTIME layout=usr metadata_links=%zu created=%u payload_copied=0\n",links.size(),created);
    return true;
}
}
#endif
