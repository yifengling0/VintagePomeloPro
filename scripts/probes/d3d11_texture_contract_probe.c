
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

int main(int argc, char **argv) {
    FILE *out = fopen(argc > 1 ? argv[1] : "texture-result.txt", "wb");
    if (!out) return 1;
    setvbuf(out, NULL, _IONBF, 0);
    int failures = 0;
    WNDCLASSA cls = {0}; cls.lpfnWndProc = DefWindowProcA;
    cls.hInstance = GetModuleHandleA(NULL); cls.lpszClassName = "vp-texture-contract";
    RegisterClassA(&cls);
    HWND window = CreateWindowA(cls.lpszClassName, "Texture: red green / blue yellow",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 120, 80, 656, 519,
        NULL, NULL, cls.hInstance, NULL);
    DXGI_SWAP_CHAIN_DESC desc = {0};
    desc.BufferDesc.Width = 640; desc.BufferDesc.Height = 480;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BufferCount = 2;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window; desc.Windowed = TRUE;
    ID3D11Device *device = NULL; ID3D11DeviceContext *context = NULL;
    IDXGISwapChain *swap = NULL; D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
        0, NULL, 0, D3D11_SDK_VERSION, &desc, &swap, &device, &level, &context);
    fprintf(out, "device\thr=%08lx\tlevel=%x\n", (unsigned long)hr, level);
    if (FAILED(hr)) { fprintf(out,"done\tfailures=1\n"); return 2; }
    ID3D11Texture2D *buffer = NULL, *upload = NULL, *staging = NULL;
    hr = IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void**)&buffer);
    D3D11_TEXTURE2D_DESC texture = {0};
    if (SUCCEEDED(hr)) ID3D11Texture2D_GetDesc(buffer, &texture);
    texture.Usage = D3D11_USAGE_DEFAULT; texture.BindFlags = 0;
    texture.CPUAccessFlags = 0; texture.MiscFlags = 0;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateTexture2D(device, &texture, NULL, &upload);
    texture.Usage = D3D11_USAGE_STAGING; texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateTexture2D(device, &texture, NULL, &staging);
    fprintf(out, "textures\thr=%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) { fprintf(out,"done\tfailures=1\n"); return 3; }
    static uint32_t pixels[640 * 480];
    const uint32_t colors[] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00};
    for (int y = 0; y < 480; ++y) for (int x = 0; x < 640; ++x)
        pixels[y * 640 + x] = colors[(y >= 240 ? 2 : 0) + (x >= 320 ? 1 : 0)];
    for (int pass = 0; pass < 2; ++pass) {
        // A second different upload catches stale copies/cache invalidation.
        if (pass) for (int i = 0; i < 640 * 480; ++i) pixels[i] ^= 0x000f0f0f;
        ID3D11DeviceContext_UpdateSubresource(context, (ID3D11Resource*)upload, 0,
            NULL, pixels, 640 * 4, 0);
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource*)buffer, (ID3D11Resource*)upload);
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource*)staging, (ID3D11Resource*)buffer);
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = ID3D11DeviceContext_Map(context, (ID3D11Resource*)staging, 0,
            D3D11_MAP_READ, 0, &mapped);
        int badrows = 0;
        if (SUCCEEDED(hr)) {
            for (int y = 0; y < 480; ++y)
                if (memcmp((char*)mapped.pData + y * mapped.RowPitch, pixels + y * 640, 640 * 4)) ++badrows;
            ID3D11DeviceContext_Unmap(context, (ID3D11Resource*)staging, 0);
        }
        if (FAILED(hr) || badrows) ++failures;
        fprintf(out, "readback\tpass=%d\thr=%08lx\tbadrows=%d\n", pass, (unsigned long)hr, badrows);
        hr = IDXGISwapChain_Present(swap, 0, 0);
        if (FAILED(hr)) ++failures;
        fprintf(out,"present\tpass=%d\thr=%08lx\n",pass,(unsigned long)hr);
        for (int i = 0; i < 160; ++i) {
            MSG msg; while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
            Sleep(100);
        }
    }
    ID3D11Texture2D_Release(staging); ID3D11Texture2D_Release(upload); ID3D11Texture2D_Release(buffer);
    IDXGISwapChain_Release(swap); ID3D11DeviceContext_Release(context); ID3D11Device_Release(device);
    DestroyWindow(window); fprintf(out,"done\tfailures=%d\n",failures); fclose(out);
    return failures ? 4 : 0;
}
