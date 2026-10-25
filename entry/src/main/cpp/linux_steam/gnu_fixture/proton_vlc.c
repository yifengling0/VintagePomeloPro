/* Private runtime diagnostic: load an installed game's unmodified VLC.
 * No media copies, game edits, registration or product DLL overrides. */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include "proton_dsound_observer.h"

static volatile LONG logged;
static void __cdecl log_message(void *opaque, int level, const void *context,
                                const char *format, va_list arguments)
{
    (void)opaque; (void)context;
    char line[2048];
    vsnprintf(line, sizeof(line), format, arguments);
    /* Ignore routine decoder messages; retain clock/underrun errors with a
       fixed line limit so this diagnostic cannot create unbounded logs. */
    if (level < 3 && !strstr(line,"late") && !strstr(line,"drift") &&
        !strstr(line,"resampl") && !strstr(line,"synchron") &&
        !strstr(line,"starv") && !strstr(line,"buffer") &&
        !strstr(line,"time") && !strstr(line,"clock")) return;
    if (InterlockedIncrement(&logged) > 1200) return;
    fprintf(stderr,"HMOSDECK_VLC_LOG tick=%llu level=%d text=%s\n",
            (unsigned long long)GetTickCount64(),level,line);
}

int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0); setvbuf(stderr,NULL,_IONBF,0);
    printf("HMOSDECK_DS_OBSERVER installed=%d\n",observe_install());
    SetDllDirectoryA("G:\\");
    HMODULE library=LoadLibraryA("G:\\libvlc.dll");
    if(!library){printf("HMOSDECK_VLC_ERROR load=%lu\n",GetLastError());return 1;}
    typedef void *(__cdecl *New)(int,const char *const *);
    typedef void (__cdecl *Release)(void *);
    typedef void (__cdecl *Log)(void *,void (__cdecl *)(void *,int,const void *,const char *,va_list),void *);
    typedef void *(__cdecl *Media)(void *,const char *);
    typedef void *(__cdecl *Player)(void *);
    typedef int (__cdecl *Play)(void *);
    typedef int64_t (__cdecl *Time)(void *);
    New create=(New)(void*)GetProcAddress(library,"libvlc_new");
    Release release=(Release)(void*)GetProcAddress(library,"libvlc_release");
    Release release_media=(Release)(void*)GetProcAddress(library,"libvlc_media_release");
    Release release_player=(Release)(void*)GetProcAddress(library,"libvlc_media_player_release");
    Release stop=(Release)(void*)GetProcAddress(library,"libvlc_media_player_stop");
    Log log=(Log)(void*)GetProcAddress(library,"libvlc_log_set");
    Media media=(Media)(void*)GetProcAddress(library,"libvlc_media_new_path");
    Player player=(Player)(void*)GetProcAddress(library,"libvlc_media_player_new_from_media");
    Play play=(Play)(void*)GetProcAddress(library,"libvlc_media_player_play");
    Time get_time=(Time)(void*)GetProcAddress(library,"libvlc_media_player_get_time");
    if(!create||!release||!release_media||!release_player||!stop||!log||!media||!player||!play||!get_time)return 2;
    int perf=GetFileAttributesA("C:\\proton-vlc-perf")!=INVALID_FILE_ATTRIBUTES;
    int dummy=GetFileAttributesA("C:\\proton-vlc-dummy")!=INVALID_FILE_ATTRIBUTES;
    DWORD duration=GetFileAttributesA("C:\\proton-vlc-short")!=INVALID_FILE_ATTRIBUTES?30000:65000;
    char x87[32]={0};GetEnvironmentVariableA("FEX_X87REDUCEDPRECISION",x87,sizeof(x87));
    const char *options[]={"--no-video",dummy?"--aout=dummy":"--aout=directsound","--verbose=2","--clock-source=perf"};
    void *instance=create(perf?4:3,options);
    if(!instance)return 3;
    log(instance,log_message,NULL);
    void *item=media(instance,"G:\\Music\\ending.mp3");
    void *output=item?player(item):NULL;
    if(!output){if(item)release_media(item);release(instance);return 4;}
    if(play(output)){release_player(output);release_media(item);release(instance);return 5;}
    LARGE_INTEGER origin,now,freq;QueryPerformanceCounter(&origin);QueryPerformanceFrequency(&freq);
    DWORD start=GetTickCount(),mm_start=timeGetTime();
    printf("HMOSDECK_VLC_START word_bits=%u qpc_frequency=%llu duration_ms=%lu perf_clock=%d dummy=%d x87=%s\n",
        (unsigned)(8*sizeof(void*)),(unsigned long long)freq.QuadPart,duration,perf,dummy,x87);
    while(GetTickCount()-start<duration){
        Sleep(2000);QueryPerformanceCounter(&now);
        observe_report();
        printf("HMOSDECK_VLC_POSITION tick_ms=%lu multimedia_ms=%lu qpc_ms=%llu media_ms=%lld\n",
            GetTickCount()-start,timeGetTime()-mm_start,(unsigned long long)((now.QuadPart-origin.QuadPart)*1000/freq.QuadPart),
            (long long)get_time(output));
    }
    stop(output);release_player(output);release_media(item);release(instance);
    puts("HMOSDECK_VLC_DONE");return 0;
}
