/* Matched PE32/PE64 device check. This measures a clear/present workload,
 * not game performance. Compile with the matching MinGW gcc and
 * -ld3d11 -ldxgi -ldxguid -luser32 -lgdi32 -static-libgcc. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static double now_ms(void)
{
    LARGE_INTEGER n, f;
    QueryPerformanceCounter(&n);
    QueryPerformanceFrequency(&f);
    return 1000.0 * n.QuadPart / f.QuadPart;
}

int main(int argc, char **argv)
{
    char output[160];
    snprintf(output, sizeof(output), "C:\\vp-gumu10-evidence\\direct-dxvk%u-smoke.tsv", (unsigned)(8*sizeof(void*)));
    FILE *out = fopen(output, "w");
    if (!out) return 10;
    setvbuf(out, NULL, _IONBF, 0);
    fprintf(out, "pointer_bits\t%u\n", (unsigned)(8*sizeof(void*)));
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WineHuaDirectDxvkSmoke";
    RegisterClassA(&wc);
    HWND window = CreateWindowA(wc.lpszClassName, sizeof(void*)==4 ? "Direct DXVK PE32 smoke" : "Direct DXVK PE64 smoke",
        WS_OVERLAPPEDWINDOW, 40, 40, 660, 520, NULL, NULL, wc.hInstance, NULL);
    if (!window) { fprintf(out, "window_error\t%lu\n", GetLastError()); fclose(out); return 11; }
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferDesc.Width = 640;
    sd.BufferDesc.Height = 480;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = window;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    IDXGISwapChain *swapchain = NULL;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
        D3D11_SDK_VERSION, &sd, &swapchain, &device, &level, &context);
    fprintf(out, "device\t0x%08lx\tlevel=0x%x\n", (unsigned long)hr, level);
    if (FAILED(hr)) { fclose(out); return 12; }
    IDXGIDevice *dxgi_device = NULL;
    IDXGIAdapter *adapter = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void**)&dxgi_device)) &&
        SUCCEEDED(IDXGIDevice_GetAdapter(dxgi_device, &adapter))) {
        DXGI_ADAPTER_DESC desc;
        if (SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &desc))) fprintf(out, "adapter\t%ls\n", desc.Description);
        IDXGIAdapter_Release(adapter);
    }
    if (dxgi_device) IDXGIDevice_Release(dxgi_device);
    ID3D11Texture2D *backbuffer = NULL;
    ID3D11RenderTargetView *view = NULL;
    hr = IDXGISwapChain_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D, (void**)&backbuffer);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource*)backbuffer, NULL, &view);
    fprintf(out, "render_target\t0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) { fclose(out); return 13; }

    D3D11_BUFFER_DESC bd = {0};
    bd.ByteWidth = 256*1024;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer *dynamic = NULL, *staging = NULL;
    hr = ID3D11Device_CreateBuffer(device, &bd, NULL, &dynamic);
    bd.Usage = D3D11_USAGE_STAGING;
    bd.BindFlags = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateBuffer(device, &bd, NULL, &staging);
    fprintf(out, "buffers\t0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) { fclose(out); return 14; }
    unsigned mismatches = 0;
    for (unsigned round = 0; round < 4; ++round) {
        D3D11_MAPPED_SUBRESOURCE mapped = {0};
        hr = ID3D11DeviceContext_Map(context, (ID3D11Resource*)dynamic, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        fprintf(out, "upload_map\t%u\t0x%08lx\t%p\n", round, (unsigned long)hr, mapped.pData);
        if (FAILED(hr)) { fclose(out); return 15; }
        unsigned char value = 0x31+round;
        memset(mapped.pData, value, bd.ByteWidth);
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource*)dynamic, 0);
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource*)staging, (ID3D11Resource*)dynamic);
        hr = ID3D11DeviceContext_Map(context, (ID3D11Resource*)staging, 0, D3D11_MAP_READ, 0, &mapped);
        fprintf(out, "readback_map\t%u\t0x%08lx\t%p\n", round, (unsigned long)hr, mapped.pData);
        if (FAILED(hr)) { fclose(out); return 16; }
        unsigned char *bytes = mapped.pData;
        unsigned bad = 0;
        for (unsigned i=0; i<bd.ByteWidth; ++i) if (bytes[i] != value) ++bad;
        mismatches += bad;
        fprintf(out, "upload_readback\t%u\tbad=%u\n", round, bad);
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource*)staging, 0);
    }
    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = 128;
    td.Height = 96;
    td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Texture2D *texture = NULL, *readback = NULL;
    hr = ID3D11Device_CreateTexture2D(device, &td, NULL, &texture);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateTexture2D(device, &td, NULL, &readback);
    fprintf(out, "textures\t0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) { fclose(out); return 18; }
    for (unsigned round = 0; round < 4; ++round) {
        D3D11_MAPPED_SUBRESOURCE mapped = {0};
        hr = ID3D11DeviceContext_Map(context, (ID3D11Resource*)texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) { fprintf(out, "texture_upload_error\t0x%08lx\n", (unsigned long)hr); fclose(out); return 19; }
        for (unsigned y=0; y<td.Height; ++y) {
            unsigned char *row = (unsigned char*)mapped.pData + y*mapped.RowPitch;
            for (unsigned x=0; x<td.Width*4; ++x) row[x] = (unsigned char)(x + 3*y + 17*round);
        }
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource*)texture, 0);
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource*)readback, (ID3D11Resource*)texture);
        hr = ID3D11DeviceContext_Map(context, (ID3D11Resource*)readback, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) { fprintf(out, "texture_readback_error\t0x%08lx\n", (unsigned long)hr); fclose(out); return 20; }
        unsigned bad = 0;
        for (unsigned y=0; y<td.Height; ++y) {
            unsigned char *row = (unsigned char*)mapped.pData + y*mapped.RowPitch;
            for (unsigned x=0; x<td.Width*4; ++x)
                if (row[x] != (unsigned char)(x + 3*y + 17*round)) ++bad;
        }
        mismatches += bad;
        fprintf(out, "texture_upload_readback\t%u\tbad=%u\n", round, bad);
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource*)readback, 0);
    }
    float color[4] = {0.125f, 0.5f, 0.875f, 1.0f};
    DWORD hold_ms = 30000;
    for (int arg = 1; arg < argc; ++arg) {
        if (!strcmp(argv[arg], "--hold")) hold_ms = 60000;
        if (!strcmp(argv[arg], "--alpha-zero")) color[3] = 0.0f;
        if (!strcmp(argv[arg], "--alpha-half")) color[3] = 0.5f;
    }
    fprintf(out, "opaque_swapchain_clear_alpha\t%.1f\n", color[3]);
    unsigned frames = 0;
    double start = now_ms();
    MSG msg;
    for (unsigned i=0; i<120; ++i) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        color[0] = (i&1) ? 0.125f : 0.375f;
        ID3D11DeviceContext_ClearRenderTargetView(context, view, color);
        hr = IDXGISwapChain_Present(swapchain, 0, 0);
        if (FAILED(hr)) break;
        ++frames;
    }
    double elapsed = now_ms()-start;
    fprintf(out, "present\tframes=%u\thr=0x%08lx\tms=%.3f\tfps=%.3f\n", frames, (unsigned long)hr, elapsed, 1000*frames/elapsed);
    fprintf(out, "RESULT\t%s\n", !mismatches && frames==120 && SUCCEEDED(hr) ? "PASS" : "FAIL");
    fprintf(out, "hold_ms\t%lu\n", hold_ms);
    Sleep(hold_ms);
    ID3D11Texture2D_Release(readback);
    ID3D11Texture2D_Release(texture);
    ID3D11Buffer_Release(staging);
    ID3D11Buffer_Release(dynamic);
    ID3D11RenderTargetView_Release(view);
    ID3D11Texture2D_Release(backbuffer);
    IDXGISwapChain_Release(swapchain);
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
    DestroyWindow(window);
    fclose(out);
    return mismatches || frames!=120 ? 17 : 0;
}
