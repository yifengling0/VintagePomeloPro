/* Explicit GNU Wine/FEX diagnostics. Never touches a user's prefix or saves. */
#define COBJMACROS
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <dsound.h>
#include <mmsystem.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <xinput.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static LRESULT CALLBACK window_proc(HWND w,UINT m,WPARAM a,LPARAM b) {
    return DefWindowProcA(w,m,a,b);
}
static int graphics_framebuffer(void) {
    PFNGLGENFRAMEBUFFERSPROC gen=(PFNGLGENFRAMEBUFFERSPROC)(void*)wglGetProcAddress("glGenFramebuffers");
    PFNGLBINDFRAMEBUFFERPROC bind=(PFNGLBINDFRAMEBUFFERPROC)(void*)wglGetProcAddress("glBindFramebuffer");
    PFNGLFRAMEBUFFERTEXTURE2DPROC attach=(PFNGLFRAMEBUFFERTEXTURE2DPROC)(void*)wglGetProcAddress("glFramebufferTexture2D");
    PFNGLCHECKFRAMEBUFFERSTATUSPROC check=(PFNGLCHECKFRAMEBUFFERSTATUSPROC)(void*)wglGetProcAddress("glCheckFramebufferStatus");
    PFNGLDELETEFRAMEBUFFERSPROC remove=(PFNGLDELETEFRAMEBUFFERSPROC)(void*)wglGetProcAddress("glDeleteFramebuffers");
    PFNGLDRAWBUFFERSPROC buffers=(PFNGLDRAWBUFFERSPROC)(void*)wglGetProcAddress("glDrawBuffers");
    if(!gen||!bind||!attach||!check||!remove||!buffers)return 0;
    GLuint fbo=0,texture=0;gen(1,&fbo);glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,16,16,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    bind(GL_FRAMEBUFFER,fbo);attach(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    GLenum target=GL_COLOR_ATTACHMENT0;buffers(1,&target);glReadBuffer(target);
    GLint selected=0;glGetIntegerv(GL_DRAW_BUFFER0,&selected);
    int good=check(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE&&selected==GL_COLOR_ATTACHMENT0&&glGetError()==GL_NO_ERROR;
    glClearColor(0.2f,0.4f,0.6f,1);glClear(GL_COLOR_BUFFER_BIT);good=good&&glGetError()==GL_NO_ERROR;
    // An invalid target must still produce the real GL error. The adapter
    // must never turn a failed framebuffer operation into apparent success.
    target=0xdead;buffers(1,&target);good=good&&glGetError()==GL_INVALID_ENUM;
    bind(GL_FRAMEBUFFER,0);remove(1,&fbo);glDeleteTextures(1,&texture);
    good=good&&glGetError()==GL_NO_ERROR;
    printf("HMOSDECK_GNU_GPU_FBO verified=%d pixel_readbacks=0\n",good);return good;
}
static int graphics(int shared) {
    WNDCLASSA c={0};c.lpfnWndProc=window_proc;c.hInstance=GetModuleHandleA(NULL);c.lpszClassName="HmosDeckProtonGPU";c.style=CS_OWNDC;
    if(!RegisterClassA(&c))return 2;
    HWND w=CreateWindowA(c.lpszClassName,"GNU Proton GPU verification",WS_OVERLAPPEDWINDOW|WS_VISIBLE,80,80,640,400,NULL,NULL,c.hInstance,NULL);
    if(!w)return 3;
    HDC dc=GetDC(w);PIXELFORMATDESCRIPTOR p={0};p.nSize=sizeof(p);p.nVersion=1;p.dwFlags=PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER;p.iPixelType=PFD_TYPE_RGBA;p.cColorBits=24;
    int format=ChoosePixelFormat(dc,&p);if(!format||!SetPixelFormat(dc,format,&p)){printf("GPU pixel_format_failed error=%lu\n",GetLastError());return 4;}
    HGLRC context=wglCreateContext(dc);if(!context||!wglMakeCurrent(dc,context)){printf("GPU context_failed error=%lu\n",GetLastError());return 5;}
    const char* renderer=(const char*)glGetString(GL_RENDERER);const char* version=(const char*)glGetString(GL_VERSION);
    printf("HMOSDECK_GNU_GL renderer=%s version=%s\n",renderer?renderer:"null",version?version:"null");fflush(stdout);
    // Wine's optional fullscreen gamma shader can leave an error during
    // context creation on GL 3.1. Report it separately from this fixture's
    // rendering errors; every operation below is still checked per frame.
    for(unsigned n=0;n<16;n++){GLenum error=glGetError();if(error==GL_NO_ERROR)break;printf("HMOSDECK_GNU_GL_STARTUP_ERROR code=0x%x\n",error);if(n==15)return 7;}
    if(shared&&!graphics_framebuffer())return 9;
    unsigned good=0;const unsigned expected=shared?480:120;DWORD start=GetTickCount();
    for(unsigned n=0;n<expected;n++) {
        if(shared&&n==240) {
            if(!SetWindowPos(w,NULL,0,0,480,300,SWP_NOMOVE|SWP_NOZORDER))return 8;
            puts("HMOSDECK_GNU_GPU_RESIZE width=480 height=300");
        }
        MSG message;while(PeekMessageA(&message,NULL,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageA(&message);}
        glViewport(0,0,500,300);
        int frameGood=0;
        if(shared) {
            // Slow-moving shapes make a screenshot useful without flashing
            // the entire screen or forcing GPU readbacks for validation.
            glClearColor(0.04f,0.08f,0.18f,1);glClear(GL_COLOR_BUFFER_BIT);
            glEnable(GL_SCISSOR_TEST);glScissor(20+(n%120),40,180,110);
            glClearColor(0.1f,0.8f,0.4f,1);glClear(GL_COLOR_BUFFER_BIT);
            glScissor(40,190,300,25);glClearColor(1,0.7f,0.1f,1);glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);frameGood=glGetError()==GL_NO_ERROR;
        }
        else {
            glClearColor(n%3==0,n%3==1,n%3==2,1);glClear(GL_COLOR_BUFFER_BIT);
            unsigned char pixel[4]={0};glReadPixels(10,10,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
            frameGood=pixel[n%3]>240&&pixel[(n+1)%3]<15&&pixel[(n+2)%3]<15&&glGetError()==GL_NO_ERROR;
        }
        if(!SwapBuffers(dc))break;
        GLenum swapError=glGetError();
        if(shared&&n<3)printf("HMOSDECK_GNU_GPU_FRAME n=%u draw_ok=%d swap_error=0x%x\n",n,frameGood,swapError);
        if(frameGood&&swapError==GL_NO_ERROR)good++;
        Sleep(shared?20:10);
    }
    DWORD elapsed=GetTickCount()-start;
    const int accelerated=renderer&&strstr(renderer,"virgl")!=NULL;
    printf("HMOSDECK_GNU_GPU_RESULT frames=%u expected=%u accelerated=%d elapsed_ms=%lu readback_samples=%u\n",good,expected,accelerated,elapsed,shared?0:good);
    wglMakeCurrent(NULL,NULL);wglDeleteContext(context);ReleaseDC(w,dc);DestroyWindow(w);
    if(good==expected&&accelerated){puts(shared?"HMOSDECK_GNU_GPU_SHARED_SUBMITTED":"HMOSDECK_GNU_GPU_OK");return 0;}return 6;
}
static int directsound(void) {
    typedef HRESULT(WINAPI*Create)(LPCGUID,LPDIRECTSOUND8*,LPUNKNOWN);
    HMODULE library=LoadLibraryA("dsound.dll");
    Create create=library?(Create)(void*)GetProcAddress(library,"DirectSoundCreate8"):NULL;
    LPDIRECTSOUND8 device=NULL;
    HRESULT result=create?create(NULL,&device,NULL):E_NOINTERFACE;
    if(SUCCEEDED(result))result=IDirectSound8_SetCooperativeLevel(device,GetDesktopWindow(),DSSCL_PRIORITY);
    for(unsigned n=0;n<2&&SUCCEEDED(result);n++) {
        unsigned rate=n?48000:44100;
        WAVEFORMATEX format={WAVE_FORMAT_PCM,2,rate,rate*4,4,16,0};
        DSBUFFERDESC description={0};description.dwSize=sizeof(description);
        description.dwFlags=DSBCAPS_GLOBALFOCUS|DSBCAPS_GETCURRENTPOSITION2;
        description.dwBufferBytes=rate*4;description.lpwfxFormat=&format;
        LPDIRECTSOUNDBUFFER buffer=NULL;
        result=IDirectSound8_CreateSoundBuffer(device,&description,&buffer,NULL);
        void *a=NULL,*b=NULL;DWORD as=0,bs=0,first=0,last=0,write=0;
        if(SUCCEEDED(result))result=IDirectSoundBuffer_Lock(buffer,0,description.dwBufferBytes,&a,&as,&b,&bs,0);
        if(SUCCEEDED(result)) {
            int16_t *samples=a;
            for(unsigned i=0;i<as/4;i++)samples[i*2]=samples[i*2+1]=(int16_t)(2000*sin(6.283185307179586*330*i/rate));
            if(b)memset(b,0,bs);
            result=IDirectSoundBuffer_Unlock(buffer,a,as,b,bs);
        }
        if(SUCCEEDED(result))result=IDirectSoundBuffer_GetCurrentPosition(buffer,&first,&write);
        if(SUCCEEDED(result))result=IDirectSoundBuffer_Play(buffer,0,0,DSBPLAY_LOOPING);
        if(SUCCEEDED(result)){Sleep(600);result=IDirectSoundBuffer_GetCurrentPosition(buffer,&last,&write);}
        printf("HMOSDECK_GNU_DSOUND rate=%u result=0x%lx cursor_first=%lu cursor_last=%lu\n",rate,(unsigned long)result,first,last);
        if(SUCCEEDED(result)&&last==first)result=E_FAIL;
        if(buffer){IDirectSoundBuffer_Stop(buffer);IDirectSoundBuffer_Release(buffer);}
    }
    if(device)IDirectSound8_Release(device);
    if(library)FreeLibrary(library);
    if(FAILED(result))return 13;
    puts("HMOSDECK_GNU_DSOUND_OK");return 0;
}
static int audio_clock(void) {
    IMMDeviceEnumerator *enumerator=NULL;
    IMMDevice *device=NULL;
    IAudioClient *client=NULL;
    IAudioClock *clock=NULL;
    WAVEFORMATEX *format=NULL;
    HRESULT init=CoInitializeEx(NULL,COINIT_MULTITHREADED),hr=init;
    if(SUCCEEDED(hr))hr=CoCreateInstance(&CLSID_MMDeviceEnumerator,NULL,CLSCTX_INPROC_SERVER,
        &IID_IMMDeviceEnumerator,(void**)&enumerator);
    if(SUCCEEDED(hr))hr=IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator,eRender,eConsole,&device);
    if(SUCCEEDED(hr))hr=IMMDevice_Activate(device,&IID_IAudioClient,CLSCTX_INPROC_SERVER,NULL,(void**)&client);
    if(SUCCEEDED(hr))hr=IAudioClient_GetMixFormat(client,&format);
    if(SUCCEEDED(hr))hr=IAudioClient_Initialize(client,AUDCLNT_SHAREMODE_SHARED,0,1000000,0,format,NULL);
    if(SUCCEEDED(hr))hr=IAudioClient_GetService(client,&IID_IAudioClock,(void**)&clock);
    UINT64 position=0,stamp=0;
    LARGE_INTEGER before,after,frequency;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&before);
    if(SUCCEEDED(hr))hr=IAudioClock_GetPosition(clock,&position,&stamp);
    QueryPerformanceCounter(&after);
    UINT64 first=(UINT64)(((__uint128_t)before.QuadPart*10000000)/frequency.QuadPart);
    UINT64 last=(UINT64)(((__uint128_t)after.QuadPart*10000000)/frequency.QuadPart);
    /* Allow scheduling jitter but reject a different epoch after days of uptime. */
    int valid=SUCCEEDED(hr)&&stamp+2000000>=first&&stamp<=last+2000000;
    printf("HMOSDECK_GNU_AUDIO_CLOCK valid=%d hr=0x%lx qpc_before=%llu audio_qpc=%llu qpc_after=%llu tick_ms=%llu\n",
        valid,(unsigned long)hr,(unsigned long long)first,(unsigned long long)stamp,
        (unsigned long long)last,(unsigned long long)GetTickCount64());
    if(clock)IAudioClock_Release(clock);
    if(format)CoTaskMemFree(format);
    if(client)IAudioClient_Release(client);
    if(device)IMMDevice_Release(device);
    if(enumerator)IMMDeviceEnumerator_Release(enumerator);
    if(SUCCEEDED(init))CoUninitialize();
    return valid?0:14;
}
static int audio(void) {
    int clock_result=audio_clock();if(clock_result)return clock_result;
    WAVEOUTCAPSA caps={0};UINT count=waveOutGetNumDevs();printf("HMOSDECK_GNU_AUDIO devices=%u\n",count);
    if(!count)return 10;
    WAVEFORMATEX format={WAVE_FORMAT_PCM,2,48000,192000,4,16,0};HWAVEOUT device=NULL;
    MMRESULT result=waveOutOpen(&device,WAVE_MAPPER,&format,0,0,CALLBACK_NULL);
    if(result){printf("AUDIO open_error=%u\n",result);return 11;}
    waveOutGetDevCapsA((UINT_PTR)device,&caps,sizeof(caps));printf("HMOSDECK_GNU_AUDIO device=%s\n",caps.szPname);
    int16_t samples[48000*2];
    for(unsigned n=0;n<48000;n++)samples[n*2]=samples[n*2+1]=(int16_t)(3000*sin(6.283185307179586*440*n/48000));
    WAVEHDR header={0};header.lpData=(LPSTR)samples;header.dwBufferLength=sizeof(samples);
    result=waveOutPrepareHeader(device,&header,sizeof(header));if(!result)result=waveOutWrite(device,&header,sizeof(header));
    DWORD start=GetTickCount();while(!result&&!(header.dwFlags&WHDR_DONE)&&GetTickCount()-start<5000)Sleep(10);
    const int done=!result&&(header.dwFlags&WHDR_DONE);waveOutReset(device);waveOutUnprepareHeader(device,&header,sizeof(header));waveOutClose(device);
    printf("HMOSDECK_GNU_AUDIO_RESULT done=%d error=%u elapsed_ms=%lu\n",done,result,GetTickCount()-start);
    if(!done)return 12;
    int ds=directsound();if(ds)return ds;
    puts("HMOSDECK_GNU_AUDIO_OK");return 0;
}
static int input(int actions) {
    HMODULE library=LoadLibraryA("xinput1_3.dll");if(!library)return 20;
    typedef DWORD(WINAPI*GetState)(DWORD,XINPUT_STATE*);
    GetState get=(GetState)(void*)GetProcAddress(library,"XInputGetState");if(!get)return 21;
    unsigned seen=0,changed=0,coverage=0;DWORD packet=0;const DWORD start=GetTickCount();
    while(GetTickCount()-start<10000){
        XINPUT_STATE state={0};
        if(get(0,&state)==ERROR_SUCCESS){
            seen++;
            const XINPUT_GAMEPAD *g=&state.Gamepad;
            // Signed SDL axes map to Wine's opposite Y range with a one-unit
            // rounding offset. This is neutral, not a held direction.
            if(!g->wButtons&&abs(g->sThumbLX)<=1&&abs(g->sThumbLY)<=1&&abs(g->sThumbRX)<=1&&abs(g->sThumbRY)<=1&&!g->bLeftTrigger&&!g->bRightTrigger)coverage|=1;
            if((g->wButtons&(XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_DPAD_RIGHT))==(XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_DPAD_RIGHT)&&g->sThumbLX>16000&&g->sThumbLY>16000&&g->sThumbRX< -16000&&g->sThumbRY< -16000&&g->bLeftTrigger>240)coverage|=2;
            if((g->wButtons&(XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_DPAD_DOWN|XINPUT_GAMEPAD_DPAD_LEFT))==(XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_DPAD_DOWN|XINPUT_GAMEPAD_DPAD_LEFT)&&g->sThumbLX< -16000&&g->sThumbLY< -16000&&g->sThumbRX>16000&&g->sThumbRY>16000&&g->bRightTrigger>240)coverage|=4;
            if(state.dwPacketNumber!=packet){changed++;packet=state.dwPacketNumber;printf("HMOSDECK_GNU_INPUT buttons=0x%x lx=%d ly=%d rx=%d ry=%d lt=%u rt=%u packet=%lu\n",g->wButtons,g->sThumbLX,g->sThumbLY,g->sThumbRX,g->sThumbRY,g->bLeftTrigger,g->bRightTrigger,state.dwPacketNumber);fflush(stdout);}
        }
        Sleep(50);
    }
    printf("HMOSDECK_GNU_INPUT_RESULT seen=%u changed=%u coverage=0x%x synthetic=%d\n",seen,changed,coverage,actions);FreeLibrary(library);
    if(actions){if(coverage!=7)return 23;puts("HMOSDECK_GNU_INPUT_ACTIONS_OK");}
    if(seen){puts("HMOSDECK_GNU_INPUT_ENUMERATED");return 0;}return 22;
}
int main(int argc,char**argv){setvbuf(stdout,NULL,_IONBF,0);if(argc!=2)return 1;if(!strcmp(argv[1],"gl"))return graphics(0);if(!strcmp(argv[1],"gl-shared"))return graphics(1);if(!strcmp(argv[1],"audio"))return audio();if(!strcmp(argv[1],"input"))return input(0);if(!strcmp(argv[1],"input-actions"))return input(1);return 1;}
