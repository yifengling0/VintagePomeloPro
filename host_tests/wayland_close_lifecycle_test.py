"""Replay production close requests across the host/event-thread boundary.

A client may ignore close or display a save prompt; configure and later closes
must keep working until protocol destruction. Queued stale identities must drop.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
server=(root/'entry/src/main/cpp/compositor/wayland_server.cpp').read_text()
inputs=(root/'entry/src/main/cpp/input/input_manager.cpp').read_text()
def body(source, signature):
    start=source.index(signature)
    return source[start:source.index('\n}',start)+2]

# Compile the real queue dispatch branch together with both production methods.
branch=re.search(r'case InputQueue::Event::CLOSE_TOPLEVEL:([\s\S]*?)break;',inputs)
assert branch
stub=r'''
#include <cassert>
#include <cstdint>
#include <map>
#include <vector>
#include <thread>
struct wl_resource { bool alive=true; unsigned closes=0; };
static const auto hostThread=std::this_thread::get_id();
static void xdg_toplevel_send_close(wl_resource* r) {
    assert(std::this_thread::get_id()!=hostThread && r && r->alive); ++r->closes;
}
static bool x11_toplevel_owns(uint32_t id) { return id==99; }
static unsigned x11Closes=0;
static void x11_toplevel_close(uint32_t id) { assert(id==99);++x11Closes; }
#define OH_LOG_INFO(...) ((void)0)
struct Registry {
    std::map<uint32_t,wl_resource*> resources;
    wl_resource* FindToplevelResource(uint32_t id) {
        const auto found=resources.find(id);return found==resources.end()?nullptr:found->second;
    }
};
struct InputQueue {
    struct Event { enum Type { CLOSE_TOPLEVEL } type; uint32_t tl; wl_resource* surface; };
    std::vector<Event> events;
    void Enqueue(Event::Type type,uint32_t id,wl_resource* r,int,int,int,int) { events.push_back({type,id,r}); }
    unsigned flushes=0;
    void FlushClients() { assert(std::this_thread::get_id()!=hostThread); ++flushes; }
};
struct InputManager {
    InputQueue queue_;
    static InputManager* GetInstance() { static InputManager value;return &value; }
    void RequestToplevelClose(uint32_t,wl_resource*);
    void FlushQueue();
};
struct WaylandServer {
    Registry toplevelMgr_;
    static WaylandServer* GetInstance() { static WaylandServer value;return &value; }
    void SendToplevelClose(uint32_t);
    void DispatchToplevelClose(uint32_t,wl_resource*);
};
'''
dispatch='''
void InputManager::FlushQueue() {
    auto batch=queue_.events;queue_.events.clear();
    for(auto& ev:batch) switch(ev.type) { case InputQueue::Event::CLOSE_TOPLEVEL:
'''+branch[1]+'''break; }
    queue_.FlushClients();
}
'''
main=r'''
static void flush() { std::thread event([] { InputManager::GetInstance()->FlushQueue(); });event.join(); }
int main() {
    auto* s=WaylandServer::GetInstance();auto* input=InputManager::GetInstance();
    wl_resource game,other;
    s->toplevelMgr_.resources[7]=&game;s->toplevelMgr_.resources[8]=&other;
    s->SendToplevelClose(7);
    assert(game.closes==0 && input->queue_.events.size()==1);
    flush();assert(game.closes==1 && input->queue_.flushes==1);
    assert(s->toplevelMgr_.FindToplevelResource(7)==&game);
    // Ignore close/save dialog: configuration lookup and another request work.
    s->SendToplevelClose(7);flush();assert(game.closes==2 && other.closes==0);
    s->SendToplevelClose(7);
    game.alive=false;s->toplevelMgr_.resources.erase(7);flush();assert(game.closes==2);
    // A resource replaced before dequeue cannot receive an old request.
    game.alive=true;s->toplevelMgr_.resources[7]=&game;s->SendToplevelClose(7);
    s->toplevelMgr_.resources[7]=&other;flush();assert(other.closes==0);
    s->SendToplevelClose(0);s->SendToplevelClose(1234);assert(input->queue_.events.empty());
    s->SendToplevelClose(99);assert(x11Closes==1 && input->queue_.events.empty());
}
'''
with tempfile.TemporaryDirectory(prefix='vp-close-lifecycle-') as directory:
    cpp=Path(directory)/'test.cpp';binary=Path(directory)/'test'
    cpp.write_text(stub+body(inputs,'void InputManager::RequestToplevelClose(')+
        body(server,'void WaylandServer::SendToplevelClose(')+
        body(server,'void WaylandServer::DispatchToplevelClose(')+dispatch+main)
    subprocess.run(['g++','-std=c++17','-pthread','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-omit-frame-pointer',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=15,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
print('PASS: event-thread close/flush, ignored close, repeat, destroyed/replaced resource, exact X11 routing')
