"""Exercise the production async cursor-lock worker with delayed window IPC."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'entry/src/main/cpp/input/pointer_extras.cpp').read_text()
start = source.index('void PointerExtras::ApplyHostCursorLock(')
method = source[start:source.index('\n}', start) + 2]
start = source.index('void PointerExtras::OnRelativePointerDestroyed(')
destroy = source[start:source.index('\n}', start) + 2]
fixture = r'''
#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
template<class... Args> void IgnoreLog(Args&&...) {}
#define OH_LOG_INFO(...) IgnoreLog(__VA_ARGS__)
#define OH_LOG_WARN(...) IgnoreLog(__VA_ARGS__)
#define LOG_APP 0
using namespace std::chrono_literals;
struct wl_client {int id;};
struct wl_resource {int id;};
std::mutex ipc;
std::condition_variable changed;
bool blocked=false,entered=false,fail=false,hostLocked=false;
int lockCalls=0,unlockCalls=0;
int32_t OH_WindowManager_LockCursor(int32_t, bool) {
    std::unique_lock<std::mutex> lk(ipc);
    ++lockCalls;entered=true;changed.notify_all();
    changed.wait(lk,[]{return !blocked;});
    if (fail) return 1;
    hostLocked=true;return 0;
}
int32_t OH_WindowManager_UnlockCursor(int32_t) {
    std::lock_guard<std::mutex> lk(ipc);
    ++unlockCalls;hostLocked=false;return 0;
}
struct PointerExtras {
    struct RelativePointer {wl_resource* resource;wl_resource* pointer;wl_client* client;};
    std::vector<RelativePointer> relativePointers_;
    wl_client* cursorLockClient_=nullptr;
    std::function<void(const char*)> relativeBaselineSink_;
    static PointerExtras* GetInstance() {static PointerExtras p;return &p;}
    static void OnRelativePointerDestroyed(wl_resource*);
    std::mutex mutex_;
    std::vector<int32_t> hostWindowIds_{42};
    int32_t lockedWindowId_=0;
    bool etsLockNotified_=false;
    uint32_t lockedToplevelId_=0;
    uint64_t cursorLockGeneration_=0;
    bool cursorLockWorkerRunning_=false,cursorLockIsShell_=true;
    uint32_t root=1;
    bool desktop=true;
    uint32_t* desktopRootToplevelId_=&root;
    bool* desktopMode_=&desktop;
    std::function<void(bool,uint32_t)> lockCallback_;
    void ApplyHostCursorLock(bool,uint32_t);
};
METHOD
DESTROY
std::mutex notifications;
std::vector<std::pair<bool,uint32_t>> events;
void settled(PointerExtras& p) {
    auto until=std::chrono::steady_clock::now()+2s;
    for (;;) {
        {std::lock_guard<std::mutex> lk(p.mutex_);if(!p.cursorLockWorkerRunning_)return;}
        if(std::chrono::steady_clock::now()>until)std::abort();
        std::this_thread::sleep_for(1ms);
    }
}
int checks=0;
#define CHECK(x) do {++checks;if(!(x)){std::printf("FAIL line %d\n",__LINE__);std::abort();}} while(0)
int main() {
    PointerExtras p;
    p.lockCallback_=[](bool on,uint32_t tl){std::lock_guard<std::mutex> lk(notifications);events.push_back({on,tl});};
    blocked=true;
    p.ApplyHostCursorLock(true,10);
    {std::unique_lock<std::mutex> lk(ipc);CHECK(changed.wait_for(lk,2s,[]{return entered;}));}
    // The game exits while the synchronous LockCursor request is still pending.
    p.ApplyHostCursorLock(false,0);
    {std::lock_guard<std::mutex> lk(ipc);blocked=false;changed.notify_all();}
    settled(p);
    CHECK(!hostLocked);CHECK(p.lockedWindowId_==0);CHECK(unlockCalls==1);
    CHECK(events.size()==1 && !events.back().first);
    p.ApplyHostCursorLock(true,10);settled(p);
    CHECK(hostLocked);CHECK(events.back()==std::make_pair(true,10u));
    const auto locks=lockCalls;
    p.ApplyHostCursorLock(true,13);settled(p);
    CHECK(lockCalls==locks);CHECK(events.back()==std::make_pair(true,13u));
    p.ApplyHostCursorLock(true,1);settled(p);
    CHECK(!hostLocked);CHECK(lockCalls==locks);
    p.ApplyHostCursorLock(false,0);settled(p);CHECK(!events.back().first);
    fail=true;
    p.ApplyHostCursorLock(true,10);settled(p);
    CHECK(events.back().first);CHECK(!hostLocked);
    p.ApplyHostCursorLock(false,0);settled(p);CHECK(!events.back().first);
    fail=false;entered=false;blocked=true;
    p.ApplyHostCursorLock(true,10);
    {std::unique_lock<std::mutex> lk(ipc);CHECK(changed.wait_for(lk,2s,[]{return entered;}));}
    p.ApplyHostCursorLock(false,0);p.ApplyHostCursorLock(true,13);
    {std::lock_guard<std::mutex> lk(ipc);blocked=false;changed.notify_all();}
    settled(p);CHECK(hostLocked);CHECK(events.back()==std::make_pair(true,13u));
    p.ApplyHostCursorLock(false,0);settled(p);CHECK(!hostLocked);
    auto& owner=*PointerExtras::GetInstance();
    wl_client game{1},shell{2},steam{3};
    wl_resource gameA{1},gameB{2},shellA{3},steamA{4};
    owner.relativePointers_={{&gameA,nullptr,&game},{&gameB,nullptr,&game},
                             {&shellA,nullptr,&shell},{&steamA,nullptr,&steam}};
    owner.cursorLockClient_=&game;
    owner.ApplyHostCursorLock(true,10);settled(owner);CHECK(hostLocked);
    PointerExtras::OnRelativePointerDestroyed(&shellA);settled(owner);CHECK(hostLocked);
    PointerExtras::OnRelativePointerDestroyed(&gameA);settled(owner);CHECK(hostLocked);
    PointerExtras::OnRelativePointerDestroyed(&gameB);settled(owner);
    CHECK(!hostLocked);CHECK(owner.relativePointers_.size()==1);
    CHECK(owner.cursorLockClient_==nullptr);
    owner.relativePointers_.push_back({&gameA,nullptr,&game});
    owner.cursorLockClient_=&steam;
    owner.ApplyHostCursorLock(true,13);settled(owner);CHECK(hostLocked);
    PointerExtras::OnRelativePointerDestroyed(&gameA);settled(owner);CHECK(hostLocked);
    PointerExtras::OnRelativePointerDestroyed(&steamA);settled(owner);CHECK(!hostLocked);
    std::printf("host_cursor_lock: %d checks / 0 failures\n",checks);
}
'''.replace('METHOD', method).replace('DESTROY', destroy)
with tempfile.TemporaryDirectory(prefix='vp-cursor-lock-') as folder:
    folder = Path(folder)
    (folder / 'test.cpp').write_text(fixture)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-pthread', str(folder / 'test.cpp'), '-o', str(folder / 'test')], check=True)
    subprocess.run([str(folder / 'test')], check=True, timeout=10)
