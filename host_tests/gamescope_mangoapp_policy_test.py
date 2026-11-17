#!/usr/bin/env python3
"""Run the actual patched queue gate and initializer without Linux IPC."""
from pathlib import Path
import subprocess
import sys
import tempfile
from source_function import function

source = Path(sys.argv[1]).read_text()
gate = function(source, 'static bool mangoapp_queue_enabled()')
initialize = function(source, 'void init_mangoapp()')
harness = r'''
#include <cassert>
#include <cstdlib>
#include <cstring>
bool env_to_bool(const char* value) { return value && !strcmp(value, "1"); }
int calls = 0, msgid = 0; bool inited = false;
constexpr int IPC_CREAT = 01000;
int ftok(const char*, int) { return 42; }
int msgget(int, int) { ++calls; return 12; }
'''+gate+'\n'+initialize+r'''
int main(int count, char**) {
    if (count > 1) setenv("GAMESCOPE_MANGOAPP_SOCKET_DISABLE", "1", 1);
    else unsetenv("GAMESCOPE_MANGOAPP_SOCKET_DISABLE");
    const bool expected = !HAVE_OHOS_BACKEND && count == 1;
    assert(mangoapp_queue_enabled() == expected);
    init_mangoapp();
    assert(calls == (expected ? 1 : 0));
    assert(inited == expected);
    assert(msgid == (expected ? 12 : 0));
}
'''
with tempfile.TemporaryDirectory(prefix='gamescope-mangoapp-') as temporary:
    root = Path(temporary)
    cpp = root/'policy.cpp'; cpp.write_text(harness)
    for platform in (0, 1):
        binary = root/('policy-'+str(platform))
        subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        '-DHAVE_OHOS_BACKEND='+str(platform), cpp, '-o', binary], check=True)
        for arguments in ([], ['disabled']):
            subprocess.run([binary, *arguments], check=True)
print('Gamescope actual Mangoapp gate/init: OHOS never calls SysV IPC; desktop opt-out preserved: PASS')
