#!/usr/bin/env python3
"""Run Wine's decoration and client-inset policy for both build targets."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'thirdparty/wine-valve/dlls/winex11.drv/window.c').read_text()

def function(name):
    start = source.index('static ', source.rfind('/***********************************************************************', 0, source.index(name + '(')))
    end = source.index('\n}\n', start) + 2
    return source[start:end]

test = r'''
#include <cassert>
#include <algorithm>
#include <cstring>
using std::max;
using DWORD=unsigned; using CARD32=unsigned;
constexpr unsigned WS_CAPTION=3, WS_SYSMENU=4, WS_MINIMIZEBOX=8, WS_MAXIMIZEBOX=16;
constexpr unsigned WS_EX_TOOLWINDOW=1, WS_EX_LAYERED=2;
constexpr unsigned MWM_DECOR_TITLE=1, MWM_DECOR_BORDER=2, MWM_DECOR_MENU=4;
constexpr unsigned MWM_DECOR_MINIMIZE=8, MWM_DECOR_MAXIMIZE=16;
constexpr int XA_CARDINAL=6,PropModeReplace=0;
#define x11drv_atom(a) 42
struct Rect {int left,top,right,bottom;};
struct x11drv_win_data {int whole_window=1;bool managed=true;
 struct {Rect client{4,23,964,663},visible{0,0,968,667};} rects;
 CARD32 frame_extents_written[4]{};void* display=nullptr;};
int writes=0;long last[4]{};
bool X11DRV_HasWindowManager(const char*) {return false;}
void XChangeProperty(void*,int,int,int,int,int,unsigned char* values,int count) {
 assert(count==4);++writes;std::memcpy(last,values,sizeof(last));
}
DECORATIONS
EXTENTS
int main() {
 const unsigned all=WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_MAXIMIZEBOX;
 x11drv_win_data data;
#ifdef __OHOS__
 assert(get_mwm_decorations_for_style(all,0)==0);
 sync_frame_extents(&data);
 assert(writes==1&&last[0]==4&&last[1]==4&&last[2]==23&&last[3]==4);
#else
 assert(get_mwm_decorations_for_style(all,0)==31);
 sync_frame_extents(&data);assert(writes==0); // Other window managers retain ownership.
 data.managed=false;sync_frame_extents(&data);assert(writes==1);
#endif
 assert(get_mwm_decorations_for_style(0,0)==0); // Borderless remains borderless.
 assert(get_mwm_decorations_for_style(all,WS_EX_LAYERED)==0);
 assert(get_mwm_decorations_for_style(all,WS_EX_TOOLWINDOW)==0);
 sync_frame_extents(&data);assert(writes==1); // No per-frame property spam.
 data.rects.visible={10,30,960,660};sync_frame_extents(&data);
 assert(writes==2&&last[0]==0&&last[1]==0&&last[2]==0&&last[3]==0);
 data.whole_window=0;data.rects.visible={0,0,1,1};sync_frame_extents(&data);
 assert(writes==2);
}
'''.replace('DECORATIONS', function('get_mwm_decorations_for_style')).replace('EXTENTS', function('sync_frame_extents'))

with tempfile.TemporaryDirectory(prefix='x11-client-decorations-') as tmp:
    p = Path(tmp)
    (p / 'test.cpp').write_text(test)
    for defines in ([], ['-D__OHOS__']):
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', *defines,
                        str(p / 'test.cpp'), '-o', str(p / 'test')], check=True)
        subprocess.run([str(p / 'test')], check=True, timeout=10)
print('PASS production Wine decorations: OHOS managed CSD/insets, generic WM ownership, borderless and unchanged geometry')
