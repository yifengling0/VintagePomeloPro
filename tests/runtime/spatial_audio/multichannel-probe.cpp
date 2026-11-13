#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <cstdio>
#include <cmath>
#include <stdexcept>
#include <algorithm>
static FILE *logfile;
static void check(HRESULT hr, const char *label)
{
    fprintf(logfile, "%s hr=%08lx\n", label, (unsigned long)hr);
    if (FAILED(hr)) throw std::runtime_error(label);
}
int main()
{
    logfile = fopen("C:\\multichannel-result.txt", "wb");
    if (!logfile) return 10;
    setvbuf(logfile, nullptr, _IONBF, 0);
    try
    {
        check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM");
        IMMDeviceEnumerator *enumerator = nullptr; IMMDevice *device = nullptr;
        IAudioClient *client = nullptr; IAudioRenderClient *render = nullptr;
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), (void**)&enumerator), "enumerator");
        check(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device), "endpoint");
        check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client), "client");
        WAVEFORMATEXTENSIBLE format = {};
        format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        format.Format.nChannels = 12; format.Format.nSamplesPerSec = 48000;
        format.Format.wBitsPerSample = 32; format.Format.nBlockAlign = 48;
        format.Format.nAvgBytesPerSec = 2304000; format.Format.cbSize = 22;
        format.Samples.wValidBitsPerSample = 32; format.dwChannelMask = 0x2d63f;
        format.SubFormat = {3, 0, 0x10, {0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
        check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
            300000, 0, &format.Format, nullptr), "Initialize 12-channel shared Float32");
        UINT32 capacity; check(client->GetBufferSize(&capacity), "capacity");
        check(client->GetService(__uuidof(IAudioRenderClient), (void**)&render), "render");
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        check(client->SetEventHandle(event), "event");
        UINT64 frames = 0; bool started = false; DWORD began = GetTickCount();
        const UINT64 limit = 12 * 12000;
        while (frames < limit)
        {
            if (GetTickCount() - began > 10000) throw std::runtime_error("render deadline");
            UINT32 padding; check(client->GetCurrentPadding(&padding), "padding");
            if (padding > capacity) throw std::runtime_error("padding exceeds capacity");
            UINT32 count = (UINT32)std::min<UINT64>(capacity-padding, limit-frames);
            if (!count) { WaitForSingleObject(event, 100); continue; }
            BYTE *data; check(render->GetBuffer(count, &data), "buffer");
            float *samples = (float*)data;
            for (UINT32 i = 0; i < count; ++i)
            {
                UINT64 frame = frames + i;
                unsigned int channel = (unsigned int)(frame / 12000);
                float tone = 0.2f * std::sin(float(frame % 48000) * 6.28318530718f * 440.0f / 48000.0f);
                for (unsigned int c = 0; c < 12; ++c) samples[i*12+c] = c == channel ? tone : 0;
            }
            check(render->ReleaseBuffer(count, 0), "release"); frames += count;
            if (!started) { check(client->Start(), "start"); started = true; }
            WaitForSingleObject(event, 100);
        }
        Sleep(250); check(client->Stop(), "stop"); CloseHandle(event);
        render->Release(); client->Release(); device->Release(); enumerator->Release(); CoUninitialize();
        fprintf(logfile, "PASS 12-channel stream, 144000 submitted frames, all source channels exercised\n");
        fclose(logfile); return 0;
    }
    catch (const std::exception& e) { fprintf(logfile, "FAIL %s\n", e.what()); fclose(logfile); return 1; }
}
