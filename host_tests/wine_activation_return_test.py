"""Replay actual Wine foreground selection against HWND/server boundaries.

This verifies a recorded same-thread fullscreen predecessor, cancellation, and
explicit Wayland activation. It does not claim real game or device success.
"""
import os
from pathlib import Path
import subprocess
import sys
import unittest

baseline = '--activation-baseline' in sys.argv
if baseline:
    sys.argv.remove('--activation-baseline')
import wine_shm_state_cache_test as base
import wayland_minimize_restore_test as minimize

TYPES = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uintptr_t HWND, HANDLE, WPARAM, LPARAM;
typedef uint32_t DWORD;
typedef int BOOL;
typedef unsigned short WCHAR;
typedef struct { int left, top, right, bottom; } RECT;
typedef struct { RECT rcMonitor; } MONITORINFO;
#define FALSE 0
#define TRUE 1
#define WS_VISIBLE 0x10000000u
#define WS_POPUP 0x80000000u
#define WS_CHILD 0x40000000u
#define WS_MINIMIZE 0x20000000u
#define WS_DISABLED 0x08000000u
#define GWL_STYLE -16
#define GW_OWNER 4
#define GW_HWNDNEXT 2
#define GW_CHILD 5
#define GA_ROOT 2
#define GA_PARENT 1
#define MONITOR_DEFAULTTOPRIMARY 1
#define WM_WINE_SETACTIVEWINDOW 0x80000006u
#define NtUserSendNotifyMessage 1
#define TRACE(...) ((void)0)
struct window { HWND hwnd, next, owner, history; DWORD style, thread; RECT rect; int alive, rect_ok; };
static struct window windows[6];
static HWND foreground, active;
static DWORD current_thread=1;
static int server_fail, foreground_calls, active_fail, hook_veto, destroy_on_activate, switch_on_activate;
static DWORD foreground_thread;
static HWND thread_active[4];
static RECT monitor_rect;
static struct window *find(HWND hwnd) {
    for (unsigned i=0;i<6;i++) if (windows[i].alive && windows[i].hwnd==hwnd) return windows+i;
    return NULL;
}
static DWORD get_window_thread(HWND hwnd, DWORD *pid) {
    struct window *w=find(hwnd); if (pid) *pid=w?w->thread:0; return w?w->thread:0;
}
static DWORD GetCurrentThreadId(void) { return current_thread; }
static HWND get_full_window_handle(HWND hwnd) { return hwnd; }
static DWORD get_window_long(HWND hwnd, int index) { assert(index==GWL_STYLE); struct window *w=find(hwnd); return w?w->style:0; }
static HWND get_desktop_window(void) { return 0xffff; }
static HWND get_window_relative(HWND hwnd, int relation) {
    if (hwnd==get_desktop_window() && relation==GW_CHILD) return windows[1].hwnd;
    struct window *w=find(hwnd); if (!w) return 0;
    if (relation==GW_OWNER) return w->owner;
    assert(relation==GW_HWNDNEXT); return w->next;
}
static HWND NtUserGetAncestor(HWND hwnd, int relation) { return relation==GA_PARENT ? get_desktop_window() : hwnd; }
static HWND NtUserGetForegroundWindow(void) { return foreground; }
static HWND get_active_window(void) { return thread_active[current_thread]; }
BOOL set_active_window(HWND hwnd, HWND *prev, BOOL mouse, BOOL focus, DWORD tid);
BOOL set_foreground_window(HWND hwnd, BOOL mouse, BOOL internal);
static void activate_other_window(HWND hwnd);
void winehua_track_foreground_transition(HWND hwnd, HWND previous, BOOL mouse, BOOL internal);
static HWND NtUserSetActiveWindow(HWND hwnd) { HWND old=get_active_window(); set_active_window(hwnd,NULL,FALSE,TRUE,0); return old; }
static MONITORINFO monitor_info_from_window(HWND hwnd, int flags) { (void)hwnd; (void)flags; return (MONITORINFO){monitor_rect}; }
static unsigned get_thread_dpi(void) { return 96; }
static BOOL NtUserGetWindowRect(HWND hwnd, RECT *rect, unsigned dpi) {
    assert(dpi==96); struct window *w=find(hwnd); if (!w || !w->rect_ok) return FALSE; *rect=w->rect; return TRUE;
}
static HANDLE NtUserGetProp(HWND hwnd, const WCHAR *name) { assert(name); struct window *w=find(hwnd); return w?w->history:0; }
static BOOL NtUserSetProp(HWND hwnd, const WCHAR *name, HANDLE value) { assert(name); struct window *w=find(hwnd); if (!w) return FALSE; w->history=value; return TRUE; }
static HANDLE NtUserRemoveProp(HWND hwnd, const WCHAR *name) { assert(name); struct window *w=find(hwnd); if (!w) return 0; HANDLE old=w->history; w->history=0; return old; }
static uintptr_t wine_server_user_handle(HWND hwnd) { return hwnd; }
static HWND wine_server_ptr_handle(uintptr_t hwnd) { return hwnd; }
enum { REQ_set_foreground_window, REQ_set_active_window };
static struct { uintptr_t handle; BOOL internal; int kind; } request_data;
static struct { uintptr_t previous; BOOL send_msg_old, send_msg_new; } reply_data;
#define SERVER_START_REQ(name) do { request_data.kind=REQ_##name; __typeof__(request_data) *req=&request_data; __typeof__(reply_data) *reply=&reply_data;
#define SERVER_END_REQ } while (0)
static int wine_server_call_err(void *request) {
    (void)request;
    if (request_data.kind==REQ_set_foreground_window) {
        foreground_calls++; if (server_fail) return 1;
        DWORD target=get_window_thread(request_data.handle,NULL);
        reply_data.previous=foreground;
        reply_data.send_msg_old=foreground && foreground_thread!=current_thread;
        foreground_thread=target ? target : current_thread;
        reply_data.send_msg_new=foreground_thread!=current_thread;
        /* Real queue.c selects a foreground INPUT, not an active HWND. The
         * same-thread active HWND changes only in set_active_window below. */
        foreground=thread_active[foreground_thread];
    } else {
        if (active_fail) return 1;
        reply_data.previous=thread_active[current_thread];
        thread_active[current_thread]=active=request_data.handle;
        if (foreground_thread==current_thread) foreground=active;
    }
    return 0;
}
static void NtUserMessageCall(HWND hwnd, unsigned msg, WPARAM wp, LPARAM lp, unsigned result, unsigned mode, BOOL ansi) {
    (void)hwnd; (void)msg; (void)wp; (void)lp; (void)result; (void)mode; (void)ansi;
    if (msg==WM_WINE_SETACTIVEWINDOW) {
        DWORD saved=current_thread; current_thread=get_window_thread(hwnd,NULL);
        set_active_window(wp,NULL,FALSE,TRUE,lp); current_thread=saved;
    }
}
static void NtUserNotifyWinEvent(unsigned event, HWND hwnd, int object, int child) { (void)event; (void)hwnd; (void)object; (void)child; }
#define EVENT_SYSTEM_FOREGROUND 3
typedef struct { BOOL fMouse; HWND hWndActive; } CBTACTIVATESTRUCT;
typedef struct { unsigned cbSize; HWND hwndActive, hwndFocus; } GUITHREADINFO;
#define WH_CBT 5
#define HCBT_ACTIVATE 5
#define WM_NCACTIVATE 0x86
#define WM_ACTIVATE 6
#define WM_ACTIVATEAPP 0x1c
#define WM_QUERYNEWPALETTE 0x30f
#define WM_PALETTEISCHANGING 0x310
#define WM_PARENTNOTIFY 0x210
#define WA_INACTIVE 0
#define WA_ACTIVE 1
#define WA_CLICKACTIVE 2
#define HWND_BROADCAST ((HWND)0xffff)
#define SMTO_ABORTIFHUNG 2
#define MAKEWPARAM(low,high) ((WPARAM)(low)|((WPARAM)(high)<<16))
static BOOL is_window(HWND hwnd) { return find(hwnd)!=NULL; }
static BOOL is_iconic(HWND hwnd) { return !!(get_window_long(hwnd,GWL_STYLE)&WS_MINIMIZE); }
static BOOL call_hooks(int hook,int code,WPARAM wp,LPARAM lp,unsigned size) { (void)hook;(void)code;(void)wp;(void)lp;(void)size;return hook_veto; }
static int send_message(HWND hwnd,unsigned msg,WPARAM wp,LPARAM lp) {
    (void)wp;(void)lp;
    if (hwnd==windows[2].hwnd && msg==WM_QUERYNEWPALETTE && destroy_on_activate) {
        destroy_on_activate=0; windows[2].style&=~WS_VISIBLE;
        activate_other_window(hwnd); windows[2].alive=0;
    }
    if (hwnd==windows[2].hwnd && msg==WM_ACTIVATE && switch_on_activate) {
        switch_on_activate=0; set_foreground_window(windows[1].hwnd,TRUE,FALSE);
    }
    return 0;
}
static void send_message_timeout(HWND hwnd,unsigned msg,WPARAM wp,LPARAM lp,unsigned flags,unsigned timeout,BOOL ansi) {
    (void)hwnd;(void)msg;(void)wp;(void)lp;(void)flags;(void)timeout;(void)ansi;
}
static HWND *list_window_children(HWND hwnd) {
    (void)hwnd; HWND *list=calloc(7,sizeof(HWND));assert(list);
    for (unsigned i=0;i<6;i++) list[i]=windows[i].hwnd;
    return list;
}
static void NtUserPostMessage(HWND hwnd,unsigned msg,WPARAM wp,LPARAM lp) { (void)hwnd;(void)msg;(void)wp;(void)lp; }
static BOOL NtUserGetGUIThreadInfo(DWORD thread,GUITHREADINFO *info) { info->hwndActive=info->hwndFocus=thread_active[thread];return TRUE; }
static void set_focus_window(HWND hwnd,BOOL force) { (void)hwnd;(void)force; }
static void activate_driver(HWND hwnd,HWND previous) { (void)hwnd;(void)previous; }
static struct { void (*pActivateWindow)(HWND,HWND); } driver={activate_driver},*user_driver=&driver;
static void clip_fullscreen_window(HWND hwnd,BOOL reset) { (void)hwnd;(void)reset; }
'''

MAIN = r'''
static void reset(void) {
    memset(windows,0,sizeof(windows)); memset(thread_active,0,sizeof(thread_active));
    server_fail=foreground_calls=active_fail=hook_veto=destroy_on_activate=switch_on_activate=0; current_thread=foreground_thread=1;
    windows[0]=(struct window){.hwnd=0x501a4,.style=WS_VISIBLE|WS_POPUP,.thread=1,.rect={0,0,800,600},.alive=1,.rect_ok=1};
    windows[1]=(struct window){.hwnd=0x200c6,.next=0x501a4,.style=WS_VISIBLE,.thread=2,.rect={0,0,500,400},.alive=1,.rect_ok=1};
    windows[2]=(struct window){.hwnd=0x201bc,.next=0x200c6,.style=WS_VISIBLE|WS_POPUP,.thread=1,.rect={0,0,800,600},.alive=1,.rect_ok=1};
    windows[3]=(struct window){.hwnd=0x301c0,.next=0x200c6,.style=WS_VISIBLE|WS_POPUP,.thread=1,.rect={0,0,800,600},.alive=1,.rect_ok=1};
    foreground=active=windows[0].hwnd; monitor_rect=(RECT){0,0,800,600};
    thread_active[1]=foreground; thread_active[2]=windows[1].hwnd;
}
static void enter_popup(void) {
    assert(set_foreground_window(windows[2].hwnd,FALSE,FALSE));
#ifndef ACTIVATION_BASELINE
    assert(windows[2].history==windows[0].hwnd);
#endif
}
int main(int argc, char **argv) {
    assert(argc==2); const char *mode=argv[1]; reset();
    HWND game=windows[0].hwnd, explorer=windows[1].hwnd, popup=windows[2].hwnd;
    if (!strcmp(mode,"transient")) {
        enter_popup(); activate_other_window(popup);
        assert(foreground==game && !windows[2].history && !windows[0].history);
    } else if (!strcmp(mode,"mode-change")) {
        enter_popup(); monitor_rect=(RECT){0,0,1200,800};
        activate_other_window(popup); assert(foreground==game);
    } else if (!strcmp(mode,"user-switch")) {
        enter_popup(); set_foreground_window(explorer,TRUE,FALSE);
        assert(!windows[2].history); activate_other_window(popup); assert(foreground==explorer);
    } else if (!strcmp(mode,"mouse-internal")) {
        set_foreground_window(popup,TRUE,FALSE); assert(!windows[2].history);
        reset(); set_foreground_window(popup,FALSE,TRUE); assert(!windows[2].history);
    } else if (!strcmp(mode,"owned")) {
        windows[2].owner=game; set_foreground_window(popup,FALSE,FALSE); assert(!windows[2].history);
        activate_other_window(popup); assert(foreground==game);
    } else if (!strcmp(mode,"windowed")) {
        windows[0].rect.right=600; set_foreground_window(popup,FALSE,FALSE); assert(!windows[2].history);
        activate_other_window(popup); assert(foreground==explorer);
    } else if (!strcmp(mode,"invalid-target")) {
        enter_popup(); windows[0].style|=WS_DISABLED; activate_other_window(popup); assert(foreground==explorer);
        reset(); enter_popup(); windows[0].style|=WS_MINIMIZE; activate_other_window(popup); assert(foreground==explorer);
        reset(); enter_popup(); windows[0].style&=~WS_VISIBLE; activate_other_window(popup); assert(foreground==explorer);
    } else if (!strcmp(mode,"generation")) {
        enter_popup(); windows[0].hwnd=0x601a4; activate_other_window(popup); assert(foreground==explorer);
    } else if (!strcmp(mode,"different-thread")) {
        windows[2].thread=3; set_foreground_window(popup,FALSE,FALSE); assert(!windows[2].history);
        activate_other_window(popup); assert(foreground==explorer);
    } else if (!strcmp(mode,"failed-transition")) {
        enter_popup(); server_fail=1; assert(!set_foreground_window(explorer,TRUE,FALSE));
        assert(foreground==popup && windows[2].history==game);
        server_fail=0; activate_other_window(popup); assert(foreground==game);
    } else if (!strcmp(mode,"nested")) {
        enter_popup(); set_foreground_window(windows[3].hwnd,FALSE,FALSE);
        assert(windows[3].history==popup && windows[2].history==game);
        activate_other_window(windows[3].hwnd); assert(foreground==popup && windows[2].history==game);
        activate_other_window(popup); assert(foreground==game);
    } else if (!strcmp(mode,"stale-return")) {
        enter_popup(); foreground=explorer; foreground_thread=2; activate_other_window(popup);
        assert(foreground==explorer && !windows[2].history);
    } else if (!strcmp(mode,"rect-query")) {
        windows[0].rect_ok=0; set_foreground_window(popup,FALSE,FALSE); assert(!windows[2].history);
    } else if (!strcmp(mode,"active-failure")) {
        active_fail=1; assert(!set_foreground_window(popup,FALSE,FALSE));
        assert(foreground==game && !windows[2].history);
        reset(); hook_veto=1; assert(!set_foreground_window(popup,FALSE,FALSE));
        assert(foreground==game && !windows[2].history);
        reset(); enter_popup(); active_fail=1;
        assert(!set_foreground_window(game,FALSE,FALSE));
        assert(foreground==popup && windows[2].history==game);
    } else if (!strcmp(mode,"activation-destroy")) {
        destroy_on_activate=1; set_foreground_window(popup,FALSE,FALSE);
        assert(!windows[2].alive && foreground==game && !windows[0].history);
    } else if (!strcmp(mode,"activation-user-switch")) {
        switch_on_activate=1; set_foreground_window(popup,FALSE,FALSE);
        assert(foreground==explorer && !windows[2].history);
    } else assert(0);
    puts(mode); return 0;
}
'''

FOCUS_STUBS = r'''
#define WS_DISABLED 0x08000000u
#define SW_RESTORE 9
#define WM_WAYLAND_INIT_DISPLAY_DEVICES 0x80001001u
#define WM_WAYLAND_CONFIGURE 0x80001002u
#define WM_WAYLAND_SET_FOREGROUND 0x80001003u
#define NtUserCallNoParam_DisplayModeChanged 1
#define FIXME(...) ((void)0)
typedef intptr_t LRESULT, WPARAM, LPARAM;
static int activation_restores, activations, restore_behavior;
static void NtUserCallNoParam(unsigned code) { assert(code==1); }
static BOOL NtUserShowWindow(HWND hwnd, INT cmd) {
    assert(hwnd==wd.hwnd && cmd==SW_RESTORE && !lock_held); activation_restores++;
    if (restore_behavior==1) style=0;
    else if (restore_behavior!=2) {
        style&=~WS_MINIMIZE;
        struct wayland_win_data *d=wayland_win_data_get(hwnd);
        wayland_win_data_get_config(d,&d->wayland_surface->window);
        wayland_surface_update_state_toplevel(d->wayland_surface);
        wayland_win_data_release(d);
    }
    return TRUE;
}
static BOOL NtUserSetForegroundWindowInternal(HWND hwnd) {
    assert(hwnd==wd.hwnd && !lock_held && (style&(WS_VISIBLE|WS_DISABLED|WS_MINIMIZE))==WS_VISIBLE);
    activations++; return TRUE;
}
'''
FOCUS_MAIN = r'''
int main(void) {
    struct wayland_surface surface={0};
    surface.hwnd=(HWND)1; surface.role=WAYLAND_SURFACE_ROLE_TOPLEVEL;
    surface.xdg_surface=&xdg; surface.xdg_toplevel=&toplevel; surface.wl_surface=&wl;
    wd=(struct wayland_win_data){.hwnd=surface.hwnd,.wayland_surface=&surface,.managed=TRUE};
    style=WS_VISIBLE|WS_MINIMIZE;
    WAYLAND_WindowMessage(wd.hwnd,WM_WAYLAND_SET_FOREGROUND,0,0);
    assert(activation_restores==1 && activations==1 && !lock_held && !(style&WS_MINIMIZE));
    WAYLAND_WindowMessage(wd.hwnd,WM_WAYLAND_SET_FOREGROUND,0,0);
    assert(activation_restores==1 && activations==2);
    style=WS_VISIBLE|WS_DISABLED|WS_MINIMIZE;
    WAYLAND_WindowMessage(wd.hwnd,WM_WAYLAND_SET_FOREGROUND,0,0);
    style=0; WAYLAND_WindowMessage(wd.hwnd,WM_WAYLAND_SET_FOREGROUND,0,0);
    assert(activation_restores==1 && activations==2);
    style=WS_VISIBLE|WS_MINIMIZE; restore_behavior=1;
    WAYLAND_WindowMessage(wd.hwnd,WM_WAYLAND_SET_FOREGROUND,0,0);
    assert(activation_restores==2 && activations==2);
    style=WS_VISIBLE|WS_MINIMIZE; restore_behavior=2;
    WAYLAND_WindowMessage(wd.hwnd,WM_WAYLAND_SET_FOREGROUND,0,0);
    assert(activation_restores==3 && activations==2);
    puts("explicit activation restore, reentry, destroyed or refused HWND PASS");
}
'''

class WineActivationReturnTest(base.WineShmStateCacheTest):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        window=cls.first_replay['dlls/win32u/window.c'].decode()
        input_source=cls.first_replay['dlls/win32u/input.c'].decode()
        history=window[window.index('static const WCHAR winehua_activation_return_prop[]'):window.index('\n#endif',window.index('static const WCHAR winehua_activation_return_prop[]'))]
        if baseline:
            window=subprocess.check_output(['git','show','HEAD:dlls/win32u/window.c'],cwd=base.options.wine_src).decode()
            input_source=subprocess.check_output(['git','show','HEAD:dlls/win32u/input.c'],cwd=base.options.wine_src).decode()
        code=("#define ACTIVATION_BASELINE\n" if baseline else "")+TYPES+base.function(window,'static BOOL is_fullscreen(')+base.function(window,'static BOOL can_activate_window(')+history
        if 'static BOOL set_active_window_with_history(' in input_source:
            code+=base.function(input_source,'static BOOL set_active_window_with_history(')
        code+=base.function(input_source,'BOOL set_active_window(')
        code+=base.function(input_source,'BOOL set_foreground_window(')+base.function(window,'static void activate_other_window(')+MAIN
        cls.activation_binary=cls.compile('activation-return',code)
        prefix='dlls/winewayland.drv/'
        header=cls.first_replay[prefix+'waylanddrv.h'].decode()
        driver=cls.first_replay[prefix+'window.c'].decode()
        structs=''
        for name in ('wayland_surface_config','wayland_window_config','wayland_surface'):
            i=header.index('struct '+name+'\n{'); structs+=header[i:header.index('\n};',i)+4]+'\n'
        functions=''.join(base.function(driver,sig) for sig in ('static void wayland_win_data_get_config(','static void wayland_surface_update_state_toplevel(','static void wayland_configure_window(','static void wayland_set_foreground_window(','LRESULT WAYLAND_WindowMessage('))
        cls.focus_binary=cls.compile('explicit-activation',base.STUB_TYPES+structs+base.STUB_CALLS+minimize.STUBS+FOCUS_STUBS+functions+FOCUS_MAIN)

    @classmethod
    def compile(cls,name,code):
        path=cls.folder/(name+'.c'); path.write_text(code)
        binary=cls.folder/name
        r=subprocess.run(['gcc','-D_GNU_SOURCE','-D__OHOS__','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-variable','-Wno-unused-parameter','-Wno-unused-function','-fsanitize=address,undefined','-fno-pie','-no-pie','-g',str(path),'-lm','-pthread','-o',str(binary)],capture_output=True,text=True)
        if r.returncode: raise RuntimeError(r.stdout+r.stderr)
        return binary

    def run_activation_case(self,binary,*args):
        r=subprocess.run([str(binary),*args],capture_output=True,text=True,timeout=10,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:halt_on_error=1'))
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_foreground_transient_returns_to_recorded_fullscreen_window(self):
        self.run_activation_case(self.activation_binary,'transient')

    def test_cancellation_identity_owner_and_nested_history(self):
        for case in ('mode-change','user-switch','mouse-internal','owned','windowed','invalid-target','generation','different-thread','failed-transition','nested','stale-return','rect-query'):
            with self.subTest(case=case): self.run_activation_case(self.activation_binary,case)

    def test_explicit_wayland_activation_runs_restore_before_foreground(self):
        self.run_activation_case(self.focus_binary)

    def test_active_commit_failure_and_synchronous_callbacks(self):
        for case in ('active-failure','activation-destroy','activation-user-switch'):
            with self.subTest(case=case): self.run_activation_case(self.activation_binary,case)

if __name__=='__main__':
    unittest.main(argv=[__file__]+base.remaining)
