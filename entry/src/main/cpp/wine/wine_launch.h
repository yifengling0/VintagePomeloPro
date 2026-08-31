#pragma once

#include <string>
#include <napi/native_api.h>

struct LaunchParams {
    std::string exePath;
    std::string sockPath;
    std::string libPath;
    std::string homeDir;      // 用户 Download 目录 (Z: 映射)
    std::string sockDir;
    std::string sockName;
    std::string winehuaBin;
    std::string prefixDir;
    std::string d3dBackend = "dxvk_legacy";
    // Box64 dynarec 全局档位 ("K=V;K=V;..."), 来自设置页兼容预设; 空 = 出厂基线。
    // native 只放行 BOX64_DYNAREC_* 行, 经 wineboot/wineserver __env= 与会话 env 注入。
    std::string compatEnvStr;
    bool automationMode = false;
};

void LaunchThreadFunc(LaunchParams* p);
bool IsWinePrefixInitialized(const std::string& prefixDir);
bool IsWinePrefixInitialized();
