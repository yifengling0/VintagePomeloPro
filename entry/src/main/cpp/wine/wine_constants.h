#ifndef WINE_CONSTANTS_H
#define WINE_CONSTANTS_H

// -- WineHua 全局路径常量 --
// OHOS 应用 sandbox 基础路径
#define WINE_FILES_DIR       "/data/storage/el2/base/files"

// Wine prefix (Wine 运行时数据: registry, drive_c, .wineserver socket)
#define WINE_PREFIX          "/data/storage/el2/base/files/.wine"

// Isolated prefix used by milestone smoke runs.  It is never removed as part
// of the normal user's prefix lifecycle.
#define WINE_SMOKE_PREFIX    "/data/storage/el2/base/files/.wine-smoke"

// Wine runtime and the non-interactive HOME used by automation.
#define WINE_RUNTIME_ROOT    "/data/storage/el2/base/files/wine"
#define WINE_RUNTIME_BIN     WINE_RUNTIME_ROOT "/bin"
#define WINE_AUTOMATION_HOME "/data/storage/el2/base/files/home"

// Broker Unix socket 路径 (主进程 <-> 子进程通信)
#define WINE_BROKER_SOCKET   "/data/storage/el2/base/files/.wine_broker"

// Wine 临时文件目录 (TMPDIR)
#define WINE_TMPDIR          "/data/storage/el2/base/cache"

// Wine 子进程 stderr 日志目录
#define WINE_LOG_DIR         "/data/storage/el2/base/temp"

#endif // WINE_CONSTANTS_H
