// Select a reviewed HAP core only for its matching official depot.
// Includes the published sigreturn fix and a scoped OHOS guest-VMA adapter.
#ifndef STEAMOHOS_NATIVE_FEX_CORE_H
#define STEAMOHOS_NATIVE_FEX_CORE_H
#include "native_fex.h"
#include <dlfcn.h>
namespace steamohos::fex {
inline std::string FileSha256(const std::string& path) {
    // libgcrypt is already in the verified minimal GNU runtime. Do not add
    // an OpenSSL dependency just to select a matching emulator executable.
    void* lib=dlopen("libgcrypt.so.20",RTLD_NOW|RTLD_LOCAL);
    if(!lib)return {};
    auto version=reinterpret_cast<const char*(*)(const char*)>(dlsym(lib,"gcry_check_version"));
    auto algorithm=reinterpret_cast<int(*)(const char*)>(dlsym(lib,"gcry_md_map_name"));
    auto create=reinterpret_cast<unsigned(*)(void**,int,unsigned)>(dlsym(lib,"gcry_md_open"));
    auto update=reinterpret_cast<void(*)(void*,const void*,size_t)>(dlsym(lib,"gcry_md_write"));
    auto finish=reinterpret_cast<unsigned char*(*)(void*,int)>(dlsym(lib,"gcry_md_read"));
    auto freeCtx=reinterpret_cast<void(*)(void*)>(dlsym(lib,"gcry_md_close"));
    void* ctx=nullptr;int algo=0;
    bool ok=version&&algorithm&&create&&update&&finish&&freeCtx&&version(nullptr);
    if(ok){algo=algorithm("SHA256");ok=algo&&create(&ctx,algo,0)==0;}
    int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    struct stat st{};ok=ok&&fd>=0&&!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_size>0&&st.st_size<=16777216;
    char buffer[32768];size_t total=0;
    while(ok) {
        ssize_t n=read(fd,buffer,sizeof(buffer));
        if(n<0&&errno==EINTR)continue;
        if(!n)break;
        if(n<0||(total+=n)>16777216){ok=false;break;}
        update(ctx,buffer,static_cast<size_t>(n));
    }
    unsigned char digest[32]{};
    if(ok){const auto data=finish(ctx,algo);ok=data&&total==static_cast<size_t>(st.st_size);if(ok)memcpy(digest,data,32);}
    if(fd>=0)close(fd);
    if(ctx)freeCtx(ctx);
    dlclose(lib);
    if(!ok)return {};
    std::string result;const char* hex="0123456789abcdef";
    for(auto b:digest){result+=hex[b>>4];result+=hex[b&15];}
    return result;
}
inline bool SelectCore(Plan& plan,unsigned machine,const char** reason=nullptr) {
    auto reject=[&](const char* text){if(reason)*reason=text;return false;};
    if(reason)*reason="matched";
    const auto code=Env("STEAMOHOS_CODE_ROOT");
    const auto support=Env("STEAMOHOS_CODE_ROOT")+"/native-game-support";
    struct stat status{};
    if(code.empty()||fs::path(code).lexically_normal().string()!=code||
       lstat(support.c_str(),&status)||!S_ISDIR(status.st_mode)||Canonical(support).empty())return reject("support_scope");
    // Unknown/new Valve builds keep their own matching core, server and thunks.
    // The app does not overwrite Valve's downloaded files or pin Steam updates.
    if(FileSha256(plan.tool+"/usr/bin/FEX")!=
       "f9cbb7a70e804b9930fed02fd9d1349d49d7808faace703063a9b4944bc3568e")return reject("depot_identity");
    if(ElfMachine(support+"/FEX")!=machine||ElfMachine(support+"/FEXServer")!=machine)return reject("core_elf");
    if(FileSha256(support+"/FEX")!="6a0f84cd9dcd6832c8dce592bbe543dfe8867938e7f782ac43c4cd27c96c85a7"||
       FileSha256(support+"/FEXServer")!="059cf9217bc5221b5865242e53647e7b4bb9fe93bb4a0ed9940fe6e72e2a1683")return reject("core_identity");
    // A core with i386 GLX fixes must never load an older ABI partner.
    if(ElfMachine(support+"/libGL-host32.so")!=machine||ElfMachine(support+"/libGL-host64.so")!=machine||
       ElfMachine(support+"/libGL-guest32.so")!=3||ElfMachine(support+"/libGL-guest64.so")!=62)return reject("gl_elf");
    if(FileSha256(support+"/libGL-host32.so")!="40f3e031ee93e5e351886b2f9d15289603cd6239bf217dd80447ce6b45b2c295"||
       FileSha256(support+"/libGL-host64.so")!="35e6741544902d2f0d9b1a1cbec529e07c0023d9e5739c200ac95eeb3b3f84fb"||
       FileSha256(support+"/libGL-guest32.so")!="fc0e9dfd7e366d3382c602a1c1c7f92b26032ff938d3e587b5ad36342d3786e1"||
       FileSha256(support+"/libGL-guest64.so")!="579289beb67b32c68dd093e3030730e0ddc45361e2a87c833485324bea26875b")return reject("gl_identity");
    plan.argv[0]=support+"/FEX";return true;
}
}
#endif
