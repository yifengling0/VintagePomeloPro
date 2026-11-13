#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>

static FILE *out;
static int failures;
static ID3D11Device *device;
static ID3D11DeviceContext *context;
static IDXGIFactory *factory;

static void check(HWND window, const char *name)
{
    IDXGISwapChain *swap = NULL;
    DXGI_SWAP_CHAIN_DESC desc = {0};
    desc.BufferDesc.Width = 320;
    desc.BufferDesc.Height = 240;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ULONGLONG began = GetTickCount64();
    HRESULT hr = IDXGIFactory_CreateSwapChain(factory, (IUnknown *)device, &desc, &swap);
    fprintf(out, "%s\tvisible=%d\thr=%08lx\tms=%llu\n", name,
            IsWindowVisible(window), (unsigned long)hr, GetTickCount64() - began);
    if (FAILED(hr)) ++failures;
    if (swap) {
        ID3D11Texture2D *buffer = NULL;
        ID3D11RenderTargetView *view = NULL;
        HRESULT render = IDXGISwapChain_GetBuffer(swap, 0, &IID_ID3D11Texture2D, (void **)&buffer);
        if (SUCCEEDED(render)) render = ID3D11Device_CreateRenderTargetView(device,
                (ID3D11Resource *)buffer, NULL, &view);
        if (SUCCEEDED(render)) {
            const float color[4] = {0.1f, 0.45f, 0.9f, 1.0f};
            ID3D11DeviceContext_ClearRenderTargetView(context, view, color);
            render = IDXGISwapChain_Present(swap, 0, 0);
        }
        fprintf(out, "%s\tpresent=%08lx\n", name, (unsigned long)render);
        if (FAILED(render)) ++failures;
        if (!IsWindowVisible(window) && GetAncestor(window, GA_ROOT) == window) {
            fprintf(out, "hidden-hold\n");
            Sleep(8000);
        }
        if (view) ID3D11RenderTargetView_Release(view);
        if (buffer) ID3D11Texture2D_Release(buffer);
        IDXGISwapChain_Release(swap);
    }
}

int main(int argc, char **argv)
{
    out = fopen(argc > 1 ? argv[1] : "C:\\vp-d3d11-early-window.tsv", "wb");
    if (!out) return 1;
    setvbuf(out, NULL, _IONBF, 0);
    SYSTEMTIME wall; GetLocalTime(&wall);
    fprintf(out, "start\t%04u-%02u-%02u %02u:%02u:%02u\n",
            wall.wYear, wall.wMonth, wall.wDay, wall.wHour, wall.wMinute, wall.wSecond);
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
            D3D11_SDK_VERSION, &device, &level, &context);
    fprintf(out, "device\thr=%08lx\tlevel=%x\n", (unsigned long)hr, level);
    if (FAILED(hr)) return 2;
    IDXGIDevice *dxgi = NULL;
    IDXGIAdapter *adapter = NULL;
    ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi);
    IDXGIDevice_GetAdapter(dxgi, &adapter);
    IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void **)&factory);
    WNDCLASSA cls = {0}; cls.lpfnWndProc = DefWindowProcA;
    cls.hInstance = GetModuleHandleA(NULL); cls.lpszClassName = "vp-early-window";
    RegisterClassA(&cls);
    HWND top = CreateWindowA(cls.lpszClassName, "vp-hidden-before-swapchain",
            WS_OVERLAPPEDWINDOW, 80, 60, 336, 279, NULL, NULL, cls.hInstance, NULL);
    check(top, "hidden-toplevel");
    ShowWindow(top, SW_SHOWNOACTIVATE);
    Sleep(800);
    check(top, "visible-toplevel");
    HWND child = CreateWindowA(cls.lpszClassName, "vp-hidden-child-before-swapchain",
            WS_CHILD, 20, 30, 200, 120, top, NULL, cls.hInstance, NULL);
    check(child, "hidden-child");
    ShowWindow(child, SW_SHOWNOACTIVATE);
    check(child, "visible-child");
    Sleep(1000);
    DestroyWindow(child); DestroyWindow(top);
    IDXGIFactory_Release(factory); IDXGIAdapter_Release(adapter); IDXGIDevice_Release(dxgi);
    ID3D11DeviceContext_Release(context); ID3D11Device_Release(device);
    fprintf(out, "done\tfailures=%d\n", failures); fclose(out); return failures ? 3 : 0;
}
