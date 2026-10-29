#pragma once
#include <cstdint>
#include <string_view>

namespace steamohos {
// OHOS names the vDSO [shmm] and its data [kshare]. Some kernels expose one
// additional unnamed read-only page between them. Preserve only that exact
// kernel-owned chain; ordinary neighbouring anonymous mappings are reclaimed.
class NativeKernelMaps {
    uintptr_t vdso_,page_,shareEnd_=0;
public:
    NativeKernelMaps(uintptr_t vdso,uintptr_t page):vdso_(vdso),page_(page) {}
    bool Keep(uintptr_t first,uintptr_t last,std::string_view permissions,std::string_view name) noexcept {
        while(!name.empty()&&(name.back()=='\n'||name.back()=='\r'||name.back()==' '||name.back()=='\t'))name.remove_suffix(1);
        if(name=="[kshare]") {shareEnd_=last;return true;}
        bool companion=vdso_&&shareEnd_&&first==shareEnd_&&last==vdso_&&last>first&&
            last-first<=page_&&permissions=="r--p"&&name.empty();
        shareEnd_=0;
        if(companion||(vdso_&&first<=vdso_&&vdso_<last))return true;
        return name=="[vdso]"||name=="[vvar]"||name=="[vvar_vclock]"||name=="[vectors]"||
            name=="[vsyscall]"||name=="[sigpage]"||name=="[commpage]";
    }
};
}
