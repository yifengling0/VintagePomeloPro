#include "dll_overrides.h"
#include "env_profiles.h"
#include "wine_env.h"
#include "wine_d3d_policy.h"

#include <algorithm>
#include <cassert>
#include <cstdio>

// Platform-free dependencies for the actual BuildSessionEnv pipeline.
void UpsertEnvLine(std::vector<std::string>& env, const std::string& line)
{
    const auto equal = line.find('=');
    const std::string prefix = line.substr(0, equal + 1);
    env.erase(std::remove_if(env.begin(), env.end(), [&](const auto& item) {
        return item.rfind(prefix, 0) == 0;
    }), env.end());
    env.push_back(line);
}

std::vector<std::string> BuildWineEnv(const std::string&, const std::string&,
        const std::string&, const std::string&, int, const std::string&,
        const std::string&, const std::string&)
{
    return {"WINEPREFIX=/test-prefix"};
}

void AppendD3dBackendEnv(std::vector<std::string>& env, const std::string&,
                       const std::string& backend, const std::string&)
{
    UpsertEnvLine(env, std::string("WINEDLLOVERRIDES=") +
        winehua::DxvkDllOverrides(backend == "dxvk_modern_2_6"));
}

void AppendVulkanRuntimeEnv(std::vector<std::string>&, const std::string&) {}

static std::string Value(const std::vector<std::string>& env, const std::string& key)
{
    for (const auto& line : env) if (line.rfind(key + '=', 0) == 0)
        return line.substr(key.size() + 1);
    return {};
}

int main()
{
    using winehua::MergeDllOverrides;
    std::string out;
    assert(MergeDllOverrides("d3d11=n;dxgi=n;vulkan-1=b",
                             "vcruntime140,vcruntime140_1=b", out));
    assert(out == "d3d11=n;dxgi=n;vulkan-1=b;vcruntime140=b;vcruntime140_1=b");
    assert(MergeDllOverrides("D3D11,DXGI=n,b;*vcruntime140=n,b",
                             "d3d11.dll=b; VCRUNTIME140_1.DLL = builtin,native", out));
    assert(out == "d3d11=b;dxgi=n,b;*vcruntime140=n,b;vcruntime140_1=b,n");
    assert(MergeDllOverrides("foo,bar=n;foo=b", "bar=;baz=disabled", out));
    assert(out == "foo=b;bar=;baz=");
    assert(MergeDllOverrides(";foo=n;;", "foo=b,b,n", out));
    assert(out == "foo=b,n");
    assert(MergeDllOverrides("foo=n", "", out) && out.empty());
    for (const std::string bad : {"foo", "=b", "foo,=b", "foo=b;bar=unexpected",
                                   "foo=b=n", "foo=b\nbar=n", "foo=disabled,b"}) {
        out = "unchanged";
        assert(!MergeDllOverrides("d3d11=n", bad, out) && out == "unchanged");
    }
    assert(MergeDllOverrides("d3d11 dxgi=n,b", "DXGI=b", out));
    assert(out == "d3d11=n,b;dxgi=b");
    out = "unchanged";
    assert(!MergeDllOverrides("invalid-baseline", "foo=b", out) && out == "unchanged");

    winehua::SessionEnvPolicy p;
    p.d3dBackend = "dxvk_modern_2_6";
    p.extraEnv = {"WINEDLLOVERRIDES=vcruntime140,vcruntime140_1=b", "WINEHUA_WINEDEBUG=-all,+loaddll"};
    auto env = winehua::BuildSessionEnv(p);
    assert(Value(env, "WINEDLLOVERRIDES") ==
           "ddraw=b;d3d8=b;d3d9=b;d3d11=n;dxgi=n;vulkan-1=b;vcruntime140=b;vcruntime140_1=b");
    assert(Value(env, "WINEHUA_WINEDEBUG") == "-all,+loaddll");
    p.extraEnv.push_back("WINEDLLOVERRIDES=vcruntime140=n,b");
    assert(Value(winehua::BuildSessionEnv(p), "WINEDLLOVERRIDES") ==
           "ddraw=b;d3d8=b;d3d9=b;d3d11=n;dxgi=n;vulkan-1=b;vcruntime140=n,b;vcruntime140_1=b");
    p.d3dBackend = "dxvk_legacy";
    auto legacy = Value(winehua::BuildSessionEnv(p), "WINEDLLOVERRIDES");
    assert(legacy == "ddraw=b;d3d8=b;d3d9=b;d3d10=n;d3d10_1=n;d3d10core=n;d3d11=n;dxgi=n;vulkan-1=b;vcruntime140=n,b;vcruntime140_1=b");
    p.extraEnv.push_back("WINEDLLOVERRIDES=d3d9=n,b");
    legacy = Value(winehua::BuildSessionEnv(p), "WINEDLLOVERRIDES");
    assert(legacy.find("d3d9=n,b;") != std::string::npos);
    assert(legacy.find("d3d11=n;dxgi=n;") != std::string::npos);
    p.extraEnv.pop_back();
    p.d3dBackend = "dxvk_modern_2_6";
    p.extraEnv.push_back("WINEDLLOVERRIDES=broken");
    assert(Value(winehua::BuildSessionEnv(p), "WINEDLLOVERRIDES") ==
           "ddraw=b;d3d8=b;d3d9=b;d3d11=n;dxgi=n;vulkan-1=b;vcruntime140=n,b;vcruntime140_1=b");
    p.extraEnv.push_back("WINEDLLOVERRIDES=");
    assert(Value(winehua::BuildSessionEnv(p), "WINEDLLOVERRIDES").empty());
    std::puts("DLL override merge and production environment pipeline passed");
}
