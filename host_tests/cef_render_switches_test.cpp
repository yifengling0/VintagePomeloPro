#include "proc/cef_render_switches.h"
#include <cassert>
#include <iostream>

int main() {
    using winehua::CefRenderSwitchSummary;
    const auto summary = CefRenderSwitchSummary({"steamwebhelper.exe", "--disable-gpu",
        "--single-process", "--use-angle=vulkan", "--access-token=secret",
        "--cookie=private", "--user-data-dir=C:\\account"},
        {"WINEHUA_CEF_FORCE_SOFTWARE=compositing", "PASSWORD=secret", "HODLL=libwow64fex.dll"});
    assert(summary.find("--disable-gpu") != std::string::npos);
    assert(summary.find("--single-process") != std::string::npos);
    assert(summary.find("--use-angle=vulkan") != std::string::npos);
    assert(summary.find("WINEHUA_CEF_FORCE_SOFTWARE=compositing") != std::string::npos);
    for (const char* secret : {"secret", "private", "account", "HODLL", "PASSWORD"})
        assert(summary.find(secret) == std::string::npos);
    const auto invalid = CefRenderSwitchSummary({"x--disable-gpu", "--use-angle=secret\nforged-log"},
        {"WINEHUA_CEF_FORCE_GPU=credential", "XWINEHUA_CEF_FORCE_SOFTWARE=1"});
    assert(invalid == "--use-angle=<other> WINEHUA_CEF_FORCE_GPU=<other>");
    assert(CefRenderSwitchSummary({"steamwebhelper.exe"}, {}) == "none-recorded");
    std::cout << "cef_render_switches_test PASS\n";
}
