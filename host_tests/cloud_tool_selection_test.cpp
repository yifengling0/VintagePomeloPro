#include "../entry/src/main/cpp/linux_steam/cloud_tool_selection.h"
#include <cassert>
#include <string>

static std::string Config(const std::string &mapping) {
    return "\"InstallConfigStore\" { \"Software\" { \"Valve\" { \"Steam\" { \"CompatToolMapping\" { "+mapping+" } } } } }";
}
int main() {
    const auto config=Config("\"0\" { \"name\" \"hmosdeck_gnu_proton\" } \"42\" { \"name\" \"hmosdeck_proton\" } \"43\" { \"name\" \"custom-proton\" }");
    assert(CloudToolFromVdf(config.c_str(),"41")==2);
    assert(CloudToolFromVdf(config.c_str(),"42")==1);
    assert(CloudToolFromVdf(config.c_str(),"43")==0);
    const auto longValue="\"Unrelated\" \""+std::string(8192,'x')+"\" "+config;
    assert(CloudToolFromVdf(longValue.c_str(),"41")==2);
    assert(CloudToolFromVdf(("\"Brace\" \"}\" "+config).c_str(),"41")==2);
    assert(CloudToolFromVdf((config+" \"unterminated").c_str(),"41")==0);
    assert(CloudToolFromVdf((config+" }").c_str(),"41")==0);
    assert(CloudToolFromVdf(config.substr(0,config.size()-1).c_str(),"41")==0);
    assert(CloudToolFromVdf("\"Other\" { \"CompatToolMapping\" { \"0\" { \"name\" \"hmosdeck_gnu_proton\" } } }","41")==0);
    assert(CloudToolFromVdf(Config("\"42\" { \"nested\" { \"name\" \"hmosdeck_proton\" } }").c_str(),"42")==0);
    assert(CloudToolFromVdf(Config("// comment\n\"0\" { \"Name\" \"hmosdeck_gnu_proton_gl\" }").c_str(),"41")==2);
}
