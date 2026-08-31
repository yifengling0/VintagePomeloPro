#pragma once

#include <napi/native_api.h>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <initializer_list>
#include <cstdint>

// -- 进程注册表入口 --
struct WineProcessEntry {
    pid_t pid;
    std::string exeBasename;
    std::string exeFullPath;
    std::string sessionId;
    uint32_t toplevelId;
    bool running;
    uint64_t startTimestampMs;
    uint64_t endTimestampMs;
    int exitCode;
    std::string exitCodeSource;
    int stdoutFd;
    std::shared_ptr<std::atomic<bool>> readerActive;
};

// -- NAPI threadsafe 回调 (由 napi_init.cpp 设置) --
extern napi_threadsafe_function gStateTsfn;
extern std::string gSockPath;

// -- 进程注册表 --
WineProcessEntry* AddProcess(pid_t pid, const std::string& exeFullPath, int stdoutFd,
                            const std::string& sessionId = "");
void RemoveProcess(pid_t pid, int exitCode = -1,
                   const std::string& exitCodeSource = "unknown");
void KillAllProcesses();
/** SIGKILL 目标 pid 及其全部后代 (box64/wine 会 fork 游戏子进程, 只杀根
 * 进程会留下孤儿继续运行, 导致"停止程序"与 Wine 不联动)。 */
void KillProcessTree(pid_t root);
pid_t FindRunningProcessByPath(const std::string& exeFullPath);
std::string FindSessionIdForClientPid(pid_t clientPid);
bool GetProcessBySessionId(const std::string& sessionId, WineProcessEntry* result);
bool AssociateToplevelWithSession(const std::string& sessionId, pid_t clientPid,
                                  uint32_t toplevelId);
void RemoveToplevelAssociation(uint32_t toplevelId);

// -- 进程注册表只读访问 (供 NAPI handler) --
std::vector<WineProcessEntry> GetProcessListSnapshot();
bool QueryProcessSnapshot(pid_t pid, WineProcessEntry* outEntry);
/** 进程是否仍在注册表中且 running (NCP 后端存活判定的权威依据, 由系统
 *  NCP 退出回调维护; fork 后端由 ProcMon /proc 轮询维护)。 */
bool IsProcessRegisteredRunning(pid_t pid);

// -- 辅助函数 --
void LogProcessExit(const char* tag, pid_t pid, int status);
void CloseInheritedFds(std::initializer_list<int> keepFds);

// -- 信号处理 --
void sigchld_handler(int);

// -- 子进程日志读取 --
void ReaderThread(int fd, pid_t pid, std::shared_ptr<std::atomic<bool>> active);
void StartStderrLogger(int fd, const char* tag,
                       std::shared_ptr<std::atomic<bool>> done = nullptr);
