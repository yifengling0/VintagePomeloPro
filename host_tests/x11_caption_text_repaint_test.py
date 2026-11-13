#!/usr/bin/env python3
"""Exercise production WM_SETTEXT repaint geometry, routing and DC lifetime."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];B=R/'build/host_tests/x11-caption-text';B.mkdir(parents=True,exist_ok=True)
s=(R/'thirdparty/wine-valve/dlls/win32u/defwnd.c').read_text(encoding='utf-8');a=s.index('static BOOL repaint_nc_caption_text(');b=s.index('static LRESULT handle_nc_activate',a)
code=r'''
#define __OHOS__ 1
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
using BOOL=int;using DWORD=unsigned;using WORD=unsigned short;using HWND=int;using HDC=int;using HRGN=int;using LPARAM=long;
struct RECT{int left,top,right,bottom;};struct WND{DWORD dwStyle,dwExStyle;WORD flags;};
enum{FALSE=0,TRUE=1,WS_VISIBLE=1,WS_CAPTION=6,WS_MINIMIZE=8,WS_SYSMENU=16,WS_MAXIMIZEBOX=32,WS_MINIMIZEBOX=64,WS_EX_LAYOUTRTL=1,WS_EX_TOOLWINDOW=2,WIN_NCACTIVATED=1,COORDS_WINDOW=1,COORDS_SCREEN=2,SM_CYSMCAPTION=1,SM_CYCAPTION=2,SM_CXSMICON=3,DCX_USESTYLE=1,DCX_WINDOW=2,DCX_INTERSECTRGN=4,GA_PARENT=1,WM_PARENTNOTIFY=2,WM_NCPAINT=3};
#define WND_OTHER_PROCESS ((WND*)-1)
WND window{};int fail=0,locked=0,icon=1,draws=0,regions=0,dcs=0,posts=0;bool tiny=false;RECT captured{};
WND* get_win_ptr(HWND h){assert(h==9);if(fail==1)return nullptr;if(fail==2)return WND_OTHER_PROCESS;++locked;return &window;}
void release_win_ptr(WND* w){assert(w==&window&&locked==1);--locked;}
int get_thread_dpi(){return 144;}
void get_inside_rect(HWND,int,RECT* r,DWORD,DWORD){assert(!locked);*r={7,11,tiny?7:967,651};}
int get_system_metrics(int i){return i==SM_CXSMICON?16:(i==SM_CYSMCAPTION?24:32);}
int get_nc_icon_for_window(HWND){assert(!locked);return icon;}
int caption_button_size(){return 20;}
void get_window_rect_rel(HWND,int coords,RECT* r,int dpi){assert(coords==COORDS_SCREEN&&dpi==144);*r={100,200,1100,900};}
HRGN NtGdiCreateRectRgn(int l,int t,int r,int b){captured={l,t,r,b};if(fail==3)return 0;++regions;return 7;}
HDC NtUserGetDCEx(HWND,HRGN r,int flags){assert(r==7&&regions==1&&flags==7);if(fail==4)return 0;++dcs;return 8;}
void NtGdiDeleteObjectApp(HRGN r){assert(r==7&&regions==1&&!dcs);--regions;}
void draw_nc_caption(HDC h,RECT* r,HWND,DWORD,DWORD,BOOL active,BOOL text_only){assert(h==8&&dcs==1&&!locked&&text_only);assert(active==(window.flags&WIN_NCACTIVATED));assert(captured.left==100+r->left&&captured.top==200+r->top&&captured.right==100+r->right&&captured.bottom==200+r->bottom);++draws;}
void NtUserReleaseDC(HWND,HDC h){assert(h==8&&regions==1&&dcs==1);--regions;--dcs;}
HWND NtUserGetAncestor(HWND,int){return 5;}HWND get_desktop_window(){return 5;}
void NtUserPostMessage(HWND p,int,int,LPARAM h){assert(p==5&&h==9&&!dcs);++posts;}
'''
code+=s[a:b]
code+=r'''
void reset(){assert(!locked&&!regions&&!dcs);window={WS_VISIBLE|WS_CAPTION|WS_SYSMENU|WS_MAXIMIZEBOX,0,WIN_NCACTIVATED};fail=draws=posts=0;icon=1;tiny=false;setenv("WINEHUA_DISPLAY_ROUTE","x11",1);unsetenv("WINEHUA_CSD_TEXT_ONLY");}
void fallback(){assert(!repaint_nc_caption_text(9)&&!draws&&!posts&&!locked&&!regions&&!dcs);}
int main(){
 reset();assert(repaint_nc_caption_text(9)&&draws==1&&posts==1);assert(captured.left==125&&captured.right==100+901&&captured.top==211&&captured.bottom==242);
 reset();window.dwExStyle=WS_EX_TOOLWINDOW;window.flags=0;assert(repaint_nc_caption_text(9));assert(captured.left==107&&captured.bottom==234);
 reset();icon=0;window.dwStyle=WS_VISIBLE|WS_CAPTION|WS_SYSMENU;assert(repaint_nc_caption_text(9));assert(captured.left==107&&captured.right==1045);
 reset();window.dwStyle=WS_VISIBLE|WS_CAPTION;assert(repaint_nc_caption_text(9));assert(captured.right==1067);
 for(int f=1;f<=4;++f){reset();fail=f;fallback();}
 reset();unsetenv("WINEHUA_DISPLAY_ROUTE");fallback();reset();setenv("WINEHUA_DISPLAY_ROUTE","wayland",1);fallback();
 reset();setenv("WINEHUA_CSD_TEXT_ONLY","0",1);fallback();reset();window.dwExStyle=WS_EX_LAYOUTRTL;fallback();
 reset();window.dwStyle&=~WS_VISIBLE;fallback();reset();window.dwStyle|=WS_MINIMIZE;fallback();reset();window.dwStyle&=~WS_CAPTION;fallback();
 reset();tiny=true;fallback();
 std::cout<<"PASS title-only repaint: X11 route gate, live style/DPI/activation, icon/buttons excluded, tool/hidden/RTL/iconic cases, allocation failure and DC/region lifetime\n";
}
'''
p=B/'probe.cpp';p.write_text(code,encoding='utf-8');subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(B/'probe')],check=True);subprocess.run([str(B/'probe')],check=True)
assert '!repaint_nc_caption_text( hwnd )' in s
