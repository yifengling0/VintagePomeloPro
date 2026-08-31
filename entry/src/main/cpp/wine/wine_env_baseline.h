#ifndef WINE_ENV_BASELINE_H
#define WINE_ENV_BASELINE_H

/**
 * wine_env_baseline.h — Wine 环境基线单源 (header-only)
 *
 * 主进程 (wine_env.cpp BuildWineEnv) 与子进程 (wine_child.cpp setup_wine_env)
 * 从同一张表生成公共键, 各自只保留真分歧键:
 *   - XDG_RUNTIME_DIR / WAYLAND_DISPLAY (主: 合成器 socket 参数; 子: prefix/固定名)
 *   - LD_LIBRARY_PATH 系 (主: 按图形后端拼 runtimeLibPath; 子: 系统原生路径)
 *   - 仅主进程序列化: LANG/GST_PLUGIN_PATH/WINEDEBUG 基线
 *   - 仅子进程: WINEBINDIR/WINEUNIXDIR、PROCESSBROKER、WINEDEBUG profile
 *   - 窗口模式 (WINEHUA_DESKTOP_MODE / SIMULATE_RESOLUTION): 取值只在
 *     WindowingModeFor; 父进程 BuildWineEnv Layer 4 写出 (与 master 同层);
 *     子进程不进基线表, 只在 __env 缺键时 EnsureWindowingModeEnv 补同一对值
 *
 * header-only: wine_child 是独立 libwine_child.so, 不链 entry obj。
 *
 * Box64 出厂表是安全底 (无 ArkTS 的路径也靠它)。兼容档位键清单与取值的
 * policy 在 ArkTS AppModels.resolveBox64PresetEnv; native 只留本表 + 前缀门。
 */

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "wine/wine_constants.h"

namespace winehua {

#ifdef __aarch64__
inline const std::vector<std::pair<std::string, std::string>>& Box64PerfTable() {
    static const std::vector<std::pair<std::string, std::string>> kTable = {
        {"BOX64_LOG", "0"},
        {"BOX64_NOBANNER", "1"},
        {"BOX64_SHOWSEGV", "1"},
        {"BOX64_DYNAREC_SAFEFLAGS", "1"},
        {"BOX64_DYNAREC_BIGBLOCK", "3"},
        {"BOX64_DYNAREC_CALLRET", "2"},
        {"BOX64_DYNAREC_FORWARD", "1024"},
        {"BOX64_DYNAREC_WEAKBARRIER", "2"},
        {"BOX64_AVX", "0"},
        // Box64 0.4.3 dynarec 对 AES-NI/PCLMULQDQ 翻译有误; 关 cpuid 位后
        // GnuTLS 回退纯 C。详见原 SetBox64PerfEnv 注释。
        {"BOX64_AES", "0"},
        {"BOX64_PCLMULQDQ", "0"},
        // DOS MZ exe 无 PE 边界检查 → explorer 浏览目录 SIGSEGV。
        {"BOX64_DYNAREC_VOLATILE_METADATA", "0"},
    };
    return kTable;
}
#endif

inline void SetBox64PerfEnv() {
#ifdef __aarch64__
    for (const auto& kv : Box64PerfTable())
        setenv(kv.first.c_str(), kv.second.c_str(), 1);
#endif
}

inline void AppendBox64PerfStrings(std::vector<std::string>& env) {
#ifdef __aarch64__
    for (const auto& kv : Box64PerfTable())
        env.push_back(kv.first + "=" + kv.second);
#else
    (void)env;
#endif
}

struct WineBaselinePaths {
    std::string binDir;
    std::string homeDir;
    std::string prefixDir;
};

inline std::vector<std::string> BuildWineBaselineLines(const WineBaselinePaths& p) {
    const std::string& binDir = p.binDir;
    const std::string shareDir = binDir + "/../share";
    const std::string prefix = p.prefixDir.empty() ? std::string(WINE_PREFIX) : p.prefixDir;
    std::string dllPath = binDir + "/x86_64-windows:" + binDir + "/i386-windows:" + binDir;
#ifndef __aarch64__
    dllPath += ":/data/storage/el1/bundle/libs/x86_64";
#endif

    std::vector<std::string> lines = {
        "WINEPREFIX=" + prefix,
        "WINEDATADIR=" + shareDir + "/wine",
        "WINEDLLDIR=" + binDir + "/x86_64-unix",
        "WINEDLLDIR0=" + binDir + "/x86_64-windows",
        "WINEDLLDIR1=" + binDir + "/i386-windows",
        "WINEDLLDIR2=" + binDir,
        "WINEDLLPATH=" + dllPath,
        "XKB_CONFIG_ROOT=" + shareDir + "/X11/xkb",
        "PATH=/usr/local/bin:/data/app/bin:/usr/bin:/vendor/bin:" + binDir +
            "/x86_64-windows:" + binDir + "/i386-windows:" + binDir,
        "TMPDIR=" WINE_TMPDIR,
        "MIDI_SOUNDFONT_PATH=" + binDir + "/../audio/winehua-gm.sf2",
    };
    if (!p.homeDir.empty())
        lines.insert(lines.begin(), "HOME=" + p.homeDir);
    return lines;
}

inline void ApplyEnvLinesToEnviron(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        const size_t sep = line.find('=');
        if (sep == std::string::npos || sep == 0) continue;
        setenv(line.substr(0, sep).c_str(), line.substr(sep + 1).c_str(), 1);
    }
}

// 窗口模式契约 (winewayland.drv + win32u CDS)。与 master BuildWineEnv Layer 4
// 同一对键; 本仓始终显式写出 SIMULATE=0|1 (master 桌面会话省略该键, 语义等同 off)。
struct WindowingModeEnv {
    const char* desktopMode;          // "0" 融合独立窗口 / "1" 桌面 subsurface
    const char* simulateResolution;   // "1" 融合 CDS 全屏适配 / "0" 桌面合成器缩放
};

inline WindowingModeEnv WindowingModeFor(bool desktopMode) {
    return desktopMode ? WindowingModeEnv{"1", "0"} : WindowingModeEnv{"0", "1"};
}

inline void AppendWindowingModeLines(std::vector<std::string>& env, bool desktopMode) {
    const WindowingModeEnv m = WindowingModeFor(desktopMode);
    env.push_back(std::string("WINEHUA_DESKTOP_MODE=") + m.desktopMode);
    env.push_back(std::string("WINEHUA_SIMULATE_RESOLUTION=") + m.simulateResolution);
}

// 子进程 __env 之后: 只补缺键, 不改已有值。缺省按融合 (独立窗口 + CDS)。
inline void EnsureWindowingModeEnv() {
    const char* dm = getenv("WINEHUA_DESKTOP_MODE");
    const bool desktop = dm && dm[0] && atoi(dm) != 0;
    const WindowingModeEnv m = WindowingModeFor(desktop);
    if (!dm || !dm[0])
        setenv("WINEHUA_DESKTOP_MODE", m.desktopMode, 1);
    const char* sim = getenv("WINEHUA_SIMULATE_RESOLUTION");
    if (!sim || !sim[0])
        setenv("WINEHUA_SIMULATE_RESOLUTION", m.simulateResolution, 1);
}

} // namespace winehua

#endif // WINE_ENV_BASELINE_H
