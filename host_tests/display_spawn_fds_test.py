#!/usr/bin/env python3
"""Exercise production FD duplication/ownership with real pipe descriptors."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
(root/'build').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='display-fd-test-', dir=root/'build') as directory:
    temp=Path(directory)
    stub=temp/'AbilityKit/native_child_process.h'
    stub.parent.mkdir()
    stub.write_text('#pragma once\nstruct NativeChildProcess_Fd { char* fdName; int fd; NativeChildProcess_Fd* next; };\n')
    (temp/'test.cpp').write_text(r'''#include "display/spawn_fds.h"
#include <cassert>
int main() {
 int pipes[2]; assert(pipe(pipes)==0);
 NativeChildProcess_Fd node{nullptr,pipes[0],nullptr};
 int duplicate=-1;
 {DisplaySpawnFds copies(&node);assert(copies.Valid());duplicate=copies.Head()->fd;
  assert(duplicate!=pipes[0]);assert(fcntl(duplicate,F_GETFD)&FD_CLOEXEC);}
 assert(fcntl(duplicate,F_GETFD)==-1);assert(fcntl(pipes[0],F_GETFD)>=0);
 {DisplaySpawnFds copies(&node);duplicate=copies.Head()->fd;close(duplicate);copies.Consumed();}
 assert(fcntl(pipes[0],F_GETFD)>=0); // consumed phone copy never closes original
 NativeChildProcess_Fd invalid{nullptr,-1,nullptr};
 node.next=&invalid;
 {DisplaySpawnFds copies(&node);assert(!copies.Valid());duplicate=copies.Head()->fd;}
 assert(fcntl(duplicate,F_GETFD)==-1);assert(fcntl(pipes[0],F_GETFD)>=0);
 close(pipes[0]);close(pipes[1]);
}
''')
    subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(temp),'-I'+str(root/'entry/src/main/cpp'),str(temp/'test.cpp'),'-o',str(temp/'test')],check=True)
    subprocess.run([str(temp/'test')],check=True)
print('PASS display spawn FDs: isolated descriptors, NCP release, phone consume, partial failure cleanup')
