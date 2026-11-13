#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ks.h>
#include <ksmedia.h>
#include <stdio.h>
#include <math.h>

static FILE *report;
static int failures;
static const GUID floatSubtype = {3,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
static void check(const char *name, int ok, HRESULT hr) {
    fprintf(report, "%s\t%s\t%08lx\n", name, ok ? "PASS" : "FAIL", (unsigned long)hr);
    fflush(report);
    if (!ok) ++failures;
}
int main(int argc, char **argv) {
    IMMDeviceEnumerator *enumerator=NULL;
    IMMDevice *device=NULL;
    IAudioClient *client[2]={NULL,NULL};
    IAudioRenderClient *render[2]={NULL,NULL};
    IAudioClock *clock[2]={NULL,NULL};
    WAVEFORMATEX *mix=NULL, pcm={WAVE_FORMAT_PCM,2,44100,176400,4,16,0};
    UINT32 size[2]={0,0};
    UINT64 before[2]={0,0}, after[2]={0,0}, frames[2]={0,0};
    HRESULT hr;
    report = fopen(argc>1 ? argv[1] : "C:\\vp-audio-shared-format.txt", "wb");
    if (!report) return 2;
    hr=CoInitializeEx(NULL, COINIT_MULTITHREADED);
    check("com", SUCCEEDED(hr), hr);
    hr=CoCreateInstance(&CLSID_MMDeviceEnumerator,NULL,CLSCTX_ALL,&IID_IMMDeviceEnumerator,(void**)&enumerator);
    check("enumerate", SUCCEEDED(hr), hr); if (FAILED(hr)) goto done;
    hr=IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator,eRender,eConsole,&device);
    check("endpoint", SUCCEEDED(hr), hr); if (FAILED(hr)) goto done;
    for (int i=0;i<2;i++) {
        hr=IMMDevice_Activate(device,&IID_IAudioClient,CLSCTX_ALL,NULL,(void**)&client[i]);
        check("activate",SUCCEEDED(hr),hr); if (FAILED(hr)) goto done;
    }
    hr=IAudioClient_GetMixFormat(client[0],&mix);
    check("get-mix-format",SUCCEEDED(hr),hr); if (FAILED(hr)) goto done;
    fprintf(report,"mix\trate=%lu channels=%u bits=%u align=%u bytes=%lu\n",mix->nSamplesPerSec,mix->nChannels,mix->wBitsPerSample,mix->nBlockAlign,mix->nAvgBytesPerSec);
    check("float32-mix",mix->wFormatTag==WAVE_FORMAT_EXTENSIBLE && mix->cbSize>=22 && mix->wBitsPerSample==32 && IsEqualGUID(&((WAVEFORMATEXTENSIBLE*)mix)->SubFormat,&floatSubtype),S_OK);
    check("mix-consistent",mix->nBlockAlign==mix->nChannels*4 && mix->nAvgBytesPerSec==mix->nSamplesPerSec*mix->nBlockAlign,S_OK);
    for (int i=0;i<2;i++) {
        WAVEFORMATEX *fmt=i ? &pcm : mix;
        hr=IAudioClient_Initialize(client[i],AUDCLNT_SHAREMODE_SHARED,0,2000000,0,fmt,NULL);
        check(i ? "pcm44100-initialize" : "float48000-initialize",SUCCEEDED(hr),hr); if(FAILED(hr)) goto done;
        hr=IAudioClient_GetBufferSize(client[i],&size[i]); check("buffer-size",SUCCEEDED(hr)&&size[i]>0,hr); if(FAILED(hr)||!size[i]) goto done;
        hr=IAudioClient_GetService(client[i],&IID_IAudioRenderClient,(void**)&render[i]); check("render-service",SUCCEEDED(hr),hr); if(FAILED(hr)) goto done;
        hr=IAudioClient_GetService(client[i],&IID_IAudioClock,(void**)&clock[i]); check("clock-service",SUCCEEDED(hr),hr); if(FAILED(hr)) goto done;
    }
    for (int tick=0;tick<260;tick++) {
        for (int i=0;i<2;i++) {
            UINT32 padding=0,n;
            BYTE *data=NULL;
            hr=IAudioClient_GetCurrentPadding(client[i],&padding); if(FAILED(hr)){check("padding",0,hr);goto done;}
            n=size[i]-padding;
            if(n){
                hr=IAudioRenderClient_GetBuffer(render[i],n,&data); if(FAILED(hr)){check("get-buffer",0,hr);goto done;}
                for(UINT32 j=0;j<n;j++) {
                    double v=sin(2.0*3.141592653589793*(i?660.0:440.0)*(frames[i]+j)/(i?44100.0:48000.0))*0.35;
                    if(i){((short*)data)[2*j]=((short*)data)[2*j+1]=(short)(v*32767);}
                    else {((float*)data)[2*j]=((float*)data)[2*j+1]=(float)v;}
                }
                frames[i]+=n;
                hr=IAudioRenderClient_ReleaseBuffer(render[i],n,0); if(FAILED(hr)){check("release-buffer",0,hr);goto done;}
            }
            if(tick==0 || tick==131){hr=IAudioClient_Start(client[i]);check(tick?"restart":"start",SUCCEEDED(hr),hr);if(FAILED(hr))goto done;}
            if(tick==20) IAudioClock_GetPosition(clock[i],&before[i],NULL);
            if(tick==100){hr=IAudioClock_GetPosition(clock[i],&after[i],NULL);check("clock-advances",SUCCEEDED(hr)&&after[i]>before[i],hr);}
            if(tick==130){hr=IAudioClient_Stop(client[i]);check("stop",SUCCEEDED(hr),hr); hr=IAudioClient_Reset(client[i]);check("reset",SUCCEEDED(hr),hr);}
        }
        Sleep(10);
    }
done:
    for(int i=0;i<2;i++){if(client[i])IAudioClient_Stop(client[i]);if(clock[i])IAudioClock_Release(clock[i]);if(render[i])IAudioRenderClient_Release(render[i]);if(client[i])IAudioClient_Release(client[i]);}
    if(mix)CoTaskMemFree(mix);if(device)IMMDevice_Release(device);if(enumerator)IMMDeviceEnumerator_Release(enumerator);CoUninitialize();
    fprintf(report,"failures\t%d\n",failures);fclose(report);
    return failures ? 1 : 0;
}
