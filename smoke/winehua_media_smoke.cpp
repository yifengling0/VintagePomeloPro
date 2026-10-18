#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dshow.h>
#include <mfplay.h>
#include <string>
#include <sstream>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>

namespace {

constexpr DWORD kDefaultTimeoutMs = 30000;
constexpr DWORD kStatusIntervalMs = 500;

void LogHresult(const wchar_t* action, HRESULT hr)
{
    std::fwprintf(stderr, L"MEDIA_SMOKE action=%ls hr=0x%08lx\n",
                  action, static_cast<unsigned long>(hr));
    std::fflush(stderr);
}

uint64_t ReferenceTimeToMilliseconds(LONGLONG value)
{
    return value > 0 ? static_cast<uint64_t>(value / 10000) : 0;
}

void PumpWindowMessages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

} // namespace

static int DirectShowPlayback(int argc, wchar_t** argv)
{
    if (argc < 2 || !argv[1][0]) {
        std::fwprintf(stderr,
                      L"MEDIA_SMOKE result=FAIL reason=usage "
                      L"usage=winehua_media_smoke.exe_media_path_[timeout_ms]\n");
        return 2;
    }

    DWORD timeoutMs = kDefaultTimeoutMs;
    if (argc >= 3) {
        wchar_t* end = nullptr;
        const unsigned long parsed = std::wcstoul(argv[2], &end, 10);
        if (end && !*end && parsed >= 1000 && parsed <= 300000)
            timeoutMs = static_cast<DWORD>(parsed);
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        LogHresult(L"CoInitializeEx", hr);
        return 3;
    }

    IGraphBuilder* graph = nullptr;
    IMediaControl* control = nullptr;
    IMediaEvent* events = nullptr;
    IMediaSeeking* seeking = nullptr;
    IVideoWindow* videoWindow = nullptr;
    int exitCode = 1;
    LONGLONG duration = 0;
    ULONGLONG startedAt = 0;
    ULONGLONG nextStatusAt = 0;
    bool completed = false;
    long completionCode = 0;

    hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IGraphBuilder,
                          reinterpret_cast<void**>(&graph));
    if (FAILED(hr)) {
        LogHresult(L"CoCreateInstance_FilterGraph", hr);
        goto cleanup;
    }

    hr = graph->QueryInterface(IID_IMediaControl,
                               reinterpret_cast<void**>(&control));
    if (FAILED(hr)) {
        LogHresult(L"QueryInterface_IMediaControl", hr);
        goto cleanup;
    }
    hr = graph->QueryInterface(IID_IMediaEvent,
                               reinterpret_cast<void**>(&events));
    if (FAILED(hr)) {
        LogHresult(L"QueryInterface_IMediaEvent", hr);
        goto cleanup;
    }
    graph->QueryInterface(IID_IMediaSeeking,
                          reinterpret_cast<void**>(&seeking));
    graph->QueryInterface(IID_IVideoWindow,
                          reinterpret_cast<void**>(&videoWindow));

    std::fwprintf(stderr,
                  L"MEDIA_SMOKE action=RenderFile path=%ls timeout_ms=%lu\n",
                  argv[1], static_cast<unsigned long>(timeoutMs));
    std::fflush(stderr);
    hr = graph->RenderFile(argv[1], nullptr);
    if (hr != S_OK) {
        LogHresult(L"RenderFile_all_streams", hr);
        goto cleanup;
    }

    {
        IBasicVideo* video = nullptr;
        IBasicAudio* audio = nullptr;
        long width = 0, height = 0, volume = 0;
        graph->QueryInterface(IID_IBasicVideo, reinterpret_cast<void**>(&video));
        graph->QueryInterface(IID_IBasicAudio, reinterpret_cast<void**>(&audio));
        const bool hasVideo = video && SUCCEEDED(video->GetVideoSize(&width, &height)) && width > 0 && height > 0;
        const bool hasAudio = audio && SUCCEEDED(audio->get_Volume(&volume));
        std::fprintf(stderr, "MEDIA_SMOKE route=directshow video=%d audio=%d size=%ldx%ld volume=%ld\n",
                     hasVideo, hasAudio, width, height, volume);
        if (video) video->Release();
        if (audio) audio->Release();
        if (!hasVideo || !hasAudio) goto cleanup;
    }

    if (seeking && SUCCEEDED(seeking->GetDuration(&duration))) {
        std::fprintf(stderr, "MEDIA_SMOKE duration_ms=%llu\n",
                     static_cast<unsigned long long>(
                         ReferenceTimeToMilliseconds(duration)));
    }
    if (videoWindow) {
        videoWindow->put_AutoShow(OATRUE);
        videoWindow->put_Visible(OATRUE);
    }

    hr = control->Run();
    if (FAILED(hr)) {
        LogHresult(L"Run", hr);
        goto cleanup;
    }

    startedAt = GetTickCount64();
    nextStatusAt = startedAt;
    while (GetTickCount64() - startedAt < timeoutMs) {
        PumpWindowMessages();

        long eventCode = 0;
        LONG_PTR parameter1 = 0;
        LONG_PTR parameter2 = 0;
        while (events->GetEvent(&eventCode, &parameter1, &parameter2, 0) == S_OK) {
            events->FreeEventParams(eventCode, parameter1, parameter2);
            if (eventCode == EC_COMPLETE || eventCode == EC_ERRORABORT ||
                eventCode == EC_USERABORT) {
                completed = true;
                completionCode = eventCode;
                break;
            }
        }
        if (completed) break;

        const ULONGLONG now = GetTickCount64();
        if (now >= nextStatusAt) {
            LONGLONG position = 0;
            const HRESULT positionHr = seeking ?
                seeking->GetCurrentPosition(&position) : E_NOINTERFACE;
            std::fprintf(stderr,
                         "MEDIA_SMOKE progress wall_ms=%llu pts_ms=%llu "
                         "position_hr=0x%08lx\n",
                         static_cast<unsigned long long>(now - startedAt),
                         static_cast<unsigned long long>(
                             ReferenceTimeToMilliseconds(position)),
                         static_cast<unsigned long>(positionHr));
            std::fflush(stderr);
            nextStatusAt = now + kStatusIntervalMs;
        }
        Sleep(10);
    }

    control->Stop();
    if (completed && completionCode == EC_COMPLETE) {
        LONGLONG finalPosition = 0;
        if (seeking) seeking->GetCurrentPosition(&finalPosition);
        std::fprintf(stderr,
                     "MEDIA_SMOKE result=PASS event=EC_COMPLETE pts_ms=%llu\n",
                     static_cast<unsigned long long>(
                         ReferenceTimeToMilliseconds(finalPosition)));
        exitCode = 0;
    } else if (completed) {
        std::fprintf(stderr,
                     "MEDIA_SMOKE result=FAIL reason=media_event event=0x%lx\n",
                     completionCode);
        exitCode = 4;
    } else {
        LONGLONG finalPosition = 0;
        if (seeking) seeking->GetCurrentPosition(&finalPosition);
        std::fprintf(stderr,
                     "MEDIA_SMOKE result=FAIL reason=timeout pts_ms=%llu\n",
                     static_cast<unsigned long long>(
                         ReferenceTimeToMilliseconds(finalPosition)));
        exitCode = 5;
    }

cleanup:
    if (videoWindow) {
        videoWindow->put_Visible(OAFALSE);
        videoWindow->Release();
    }
    if (seeking) seeking->Release();
    if (events) events->Release();
    if (control) control->Release();
    if (graph) graph->Release();
    CoUninitialize();
    std::fflush(stderr);
    return exitCode;
}

// MFPlay drives a real Media Foundation session with video and audio renderers.
class PlayerCallback final : public IMFPMediaPlayerCallback {
public:
    LONG refs = 1;
    LONG ended = 0;
    LONG failed = 0;
    LONG audioVideo = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(IMFPMediaPlayerCallback)) return E_NOINTERFACE;
        *out = static_cast<IMFPMediaPlayerCallback*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG count = InterlockedDecrement(&refs); if (!count) delete this; return count; }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* event) override {
        std::fprintf(stderr, "MEDIA_SMOKE route=mf event=%u hr=0x%08lx\n",
                     event->eEventType, static_cast<unsigned long>(event->hrEvent));
        if (FAILED(event->hrEvent) || event->eEventType == MFP_EVENT_TYPE_ERROR)
            InterlockedExchange(&failed, 1);
        if (event->eEventType == MFP_EVENT_TYPE_MEDIAITEM_SET && SUCCEEDED(event->hrEvent)) {
            IMFPMediaItem* item = reinterpret_cast<MFP_MEDIAITEM_SET_EVENT*>(event)->pMediaItem;
            BOOL audio = FALSE, video = FALSE, selectedAudio = FALSE, selectedVideo = FALSE;
            if (item) { item->HasAudio(&audio, &selectedAudio); item->HasVideo(&video, &selectedVideo); }
            if (audio && video && selectedAudio && selectedVideo) InterlockedExchange(&audioVideo, 1);
        }
        if (event->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED) InterlockedExchange(&ended, 1);
        std::fflush(stderr);
    }
};

static LRESULT CALLBACK MediaWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* player = reinterpret_cast<IMFPMediaPlayer*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_PAINT) {
        PAINTSTRUCT paint; BeginPaint(window, &paint);
        if (player) player->UpdateVideo();
        EndPaint(window, &paint); return 0;
    }
    if (message == WM_SIZE && player) player->UpdateVideo();
    return DefWindowProcW(window, message, wparam, lparam);
}

static int MediaFoundationPlayback(const wchar_t* path, DWORD timeout)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 3;
    WNDCLASSW cls{}; cls.lpfnWndProc = MediaWindowProc; cls.lpszClassName = L"WineHuaMediaProbe";
    cls.hInstance = GetModuleHandleW(nullptr); RegisterClassW(&cls);
    HWND window = CreateWindowW(cls.lpszClassName, L"Media Foundation video playback", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                40, 40, 672, 420, nullptr, nullptr, cls.hInstance, nullptr);
    IMFPMediaPlayer* player = nullptr;
    auto* callback = new PlayerCallback;
    hr = MFPCreateMediaPlayer(path, TRUE, 0, callback, window, &player);
    if (FAILED(hr)) LogHresult(L"MFPCreateMediaPlayer", hr);
    const ULONGLONG start = GetTickCount64();
    if (player) SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(player));
    while (SUCCEEDED(hr) && !InterlockedCompareExchange(&callback->ended, 0, 0) &&
           !InterlockedCompareExchange(&callback->failed, 0, 0) && GetTickCount64() - start < timeout) {
        PumpWindowMessages(); Sleep(10);
    }
    const bool passed = SUCCEEDED(hr) && callback->ended && !callback->failed && callback->audioVideo;
    std::fprintf(stderr, "MEDIA_SMOKE route=mf result=%s ended=%ld audio_video=%ld wall_ms=%llu\n",
                 passed ? "PASS" : "FAIL", callback->ended, callback->audioVideo, GetTickCount64() - start);
    if (player) { player->Shutdown(); SetWindowLongPtrW(window, GWLP_USERDATA, 0); player->Release(); }
    callback->Release(); DestroyWindow(window); CoUninitialize();
    return passed ? 0 : 6;
}

static std::string JsonString(const wchar_t* value)
{
    int count = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    std::string bytes(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, &bytes[0], count, nullptr, nullptr);
    std::string result = "\"";
    for (int i = 0; i + 1 < count; ++i) {
        unsigned char c = bytes[i];
        if (c == '\\' || c == '"') result += '\\';
        if (c < 32) result += '?'; else result += c;
    }
    return result + "\"";
}

int wmain(int argc, wchar_t** argv)
{
    if (argc >= 2 && argv[1][0] != L'-') return DirectShowPlayback(argc, argv);
    const wchar_t *path = L"", *route = L"directshow", *result = L"", *runId = L"", *testId = L"media";
    DWORD timeout = 30000;
    unsigned repeat = 2;
    for (int i = 1; i < argc; ++i) {
        if (i + 1 < argc) {
            const wchar_t* next = argv[i + 1];
            if (!wcscmp(argv[i], L"--file")) { path = next; ++i; }
            else if (!wcscmp(argv[i], L"--route")) { route = next; ++i; }
            else if (!wcscmp(argv[i], L"--result")) { result = next; ++i; }
            else if (!wcscmp(argv[i], L"--run-id")) { runId = next; ++i; }
            else if (!wcscmp(argv[i], L"--test-id")) { testId = next; ++i; }
            else if (!wcscmp(argv[i], L"--repeat")) { repeat = wcstoul(next, nullptr, 10); ++i; }
            else if (!wcscmp(argv[i], L"--timeout-ms")) { timeout = wcstoul(next, nullptr, 10); ++i; }
        }
    }
    int code = 2;
    unsigned completed = 0;
    if (*path && repeat >= 1 && repeat <= 10 && timeout >= 1000 && timeout <= 300000 &&
        (!wcscmp(route, L"directshow") || !wcscmp(route, L"mf"))) {
        for (unsigned i = 0; i < repeat; ++i) {
            std::fwprintf(stderr, L"MEDIA_SMOKE route=%ls iteration=%u/%u\n", route, i + 1, repeat);
            if (!wcscmp(route, L"mf")) code = MediaFoundationPlayback(path, timeout);
            else {
                wchar_t milliseconds[32]; swprintf(milliseconds, 32, L"%lu", timeout);
                wchar_t* legacy[] = {argv[0], const_cast<wchar_t*>(path), milliseconds};
                code = DirectShowPlayback(3, legacy);
            }
            if (code) break;
            ++completed;
        }
    }
    if (*result) {
        std::ostringstream json;
        json << "{\"schemaVersion\":1,\"runId\":" << JsonString(runId) << ",\"testId\":" << JsonString(testId)
             << ",\"status\":\"" << (code == 0 ? "PASS" : "FAIL") << "\",\"stage\":\"media-playback\",\"route\":"
             << JsonString(route) << ",\"completedPlays\":" << completed << ",\"exitCode\":" << code << "}\n";
        std::wstring temporary = std::wstring(result) + L".tmp";
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return 7;
        const std::string content = json.str(); DWORD written = 0;
        BOOL ok = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
        CloseHandle(file);
        if (!ok || written != content.size() || !MoveFileExW(temporary.c_str(), result, MOVEFILE_REPLACE_EXISTING)) return 7;
    }
    return code;
}
