#!/usr/bin/env python3
"""Exercise production XIM focus state and backend-aware NAPI IME queries."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
build = root / 'build/host_tests/xim-activation'
build.mkdir(parents=True, exist_ok=True)
(build/'hilog').mkdir(exist_ok=True)
(build/'hilog/log.h').write_text('#define LOG_APP 0\n#define OH_LOG_INFO(...) ((void)0)\n#define OH_LOG_ERROR(...) ((void)0)\n')
source = (root/'entry/src/main/cpp/bridge/napi_init.cpp').read_text()

def function(name):
    start = source.index('static napi_value ' + name + '(')
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

test = r'''
#include "xim_bridge.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>
using napi_env=int; using napi_callback_info=int; using napi_value=void*;
static bool x11=true;
bool winehua_display_focus_is_x11(){return x11;}
void napi_get_boolean(int,bool value,void** out){*out=reinterpret_cast<void*>(uintptr_t(value+1));}
struct TextInput {
 static TextInput* GetInstance(){static TextInput t;return &t;}
 bool IsActivated(){assert(!x11);return true;}
 bool IsEnabled(){assert(!x11);return true;}
};
QUERIES
static std::atomic<int> callbacks{0};
static void activation(int,int,int,int,int,void*){++callbacks;}
static void packet(int fd,int active,bool split=false){
 uint32_t p[]={0x57485349u,20u,uint32_t(active?1:2),0,0,0,0};
 auto* bytes=reinterpret_cast<char*>(p);
 if(split){assert(send(fd,bytes,3,MSG_NOSIGNAL)==3);bytes+=3;}
 auto n=sizeof(p)-(split?3:0);assert(send(fd,bytes,n,MSG_NOSIGNAL)==ssize_t(n));
}
static void awaitCallback(int count){
 auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
 while(callbacks.load()<count&&std::chrono::steady_clock::now()<until)
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
 assert(callbacks.load()>=count);
}
static void check(bool active){
 assert(bool(xim_bridge_is_active())==active);
 assert(uintptr_t(WineTextInputActive(0,0))==uintptr_t(active+1));
 assert(uintptr_t(WineTextInputEnabled(0,0))==uintptr_t(active+1));
}
int main(){
 xim_bridge_set_activation_callback(activation,nullptr);
 xim_bridge_set_armed(1);check(false);
 for(int cycle=0;cycle<100;cycle++){
  int peer=xim_bridge_channel_init();assert(peer>=0);check(false);
  int n=callbacks.load();packet(peer,1,true);awaitCallback(n+1);check(true);
  // Backgrounding gates a retained IC; restoring needs no second focus packet.
  xim_bridge_set_armed(0);check(false);xim_bridge_set_armed(1);check(true);
  n=callbacks.load();packet(peer,0);awaitCallback(n+1);check(false);
  n=callbacks.load();packet(peer,1);awaitCallback(n+1);check(true);
  // Server exit and a fresh channel cannot expose old activation.
  n=callbacks.load();close(peer);awaitCallback(n+1);check(false);
  xim_bridge_shutdown();check(false);
 }
 x11=false;
 assert(uintptr_t(WineTextInputActive(0,0))==2);
 assert(uintptr_t(WineTextInputEnabled(0,0))==2);
}
'''.replace('QUERIES', function('WineTextInputActive')+'\n'+function('WineTextInputEnabled'))
(build/'test.cpp').write_text(test)
flags=['-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-g','-pthread']
includes=['-I'+str(build),'-I'+str(root/'entry/src/main/cpp/display')]
subprocess.run(['cc',*flags,*includes,'-c',str(root/'entry/src/main/cpp/display/xim_bridge.c'),'-o',str(build/'bridge.o')],check=True)
subprocess.run(['g++','-std=c++17',*flags,*includes,str(build/'test.cpp'),str(build/'bridge.o'),'-o',str(build/'probe')],check=True)
subprocess.run([str(build/'probe')],check=True,timeout=15)
print('PASS production XIM activation: 100 channels, fragmented focus, background/rearm, EOF, stale state and NAPI backend dispatch')
