/* Real GPU/window lifetime probe: two windows, resize, destroy/recreate.
 * No render-target readback in the frame loop. Inspect the coloured corners
 * on the device and correlate Wine readback / host native-queue counters. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

struct target { HWND window; IDirect3DDevice9 *device; unsigned width, height, frames; };
static unsigned errors;
static FILE *out;
static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_PAINT) { ValidateRect(window,NULL); return 0; }
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcA(window,message,wparam,lparam);
}

static int create(IDirect3D9 *d3d, struct target *target, int x, int y, int width, int height)
{
    RECT rect = {0, 0, width, height};
    D3DPRESENT_PARAMETERS params = {0};
    HRESULT hr;
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    fprintf(out,"create_begin\t%d,%d\n",x,y); fflush(out);
    target->window = CreateWindowA("GpuPresentProbe", "GPU present probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        x, y, rect.right-rect.left, rect.bottom-rect.top, NULL, NULL, GetModuleHandleA(NULL), NULL);
    target->width = width; target->height = height;
    params.Windowed = TRUE; params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.BackBufferWidth = width; params.BackBufferHeight = height;
    params.BackBufferFormat = D3DFMT_X8R8G8B8;
    params.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, target->window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &params, &target->device);
    fprintf(out,"create_device\t%08lx\n",(unsigned long)hr); fflush(out);
    if (FAILED(hr)) { fprintf(out,"create_failed\t%08lx\n",(unsigned long)hr); ++errors; }
    return SUCCEEDED(hr);
}
static void destroy(struct target *target)
{
    if (target->device) IDirect3DDevice9_Release(target->device);
    if (target->window) DestroyWindow(target->window);
    target->window = NULL; target->device = NULL;
}
static int resize(struct target *target)
{
    D3DPRESENT_PARAMETERS params = {0};
    RECT rect = {0,0,480,320};
    HRESULT hr;
    AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE);
    SetWindowPos(target->window,NULL,70,70,rect.right-rect.left,rect.bottom-rect.top,SWP_NOZORDER);
    params.Windowed=TRUE; params.SwapEffect=D3DSWAPEFFECT_DISCARD;
    params.BackBufferWidth=480; params.BackBufferHeight=320;
    params.BackBufferFormat=D3DFMT_X8R8G8B8;
    params.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
    hr=IDirect3DDevice9_Reset(target->device,&params);
    fprintf(out,"resize\t%08lx\n",(unsigned long)hr); fflush(out);
    if (FAILED(hr)) { ++errors; return 0; }
    target->width=480; target->height=320; return 1;
}
static int present(struct target *target, unsigned index)
{
    const unsigned w=target->width,h=target->height;
    const DWORD colors[2][4]={{0xff336699,0xff993366,0xff669933,0xff333333},
                            {0xffcc4422,0xff22cc44,0xff4422cc,0xff999999}};
    D3DRECT rect[4]={{0,0,w/2,h/2},{w/2,0,w,h/2},{0,h/2,w/2,h},{w/2,h/2,w,h}};
    HRESULT hr=S_OK;
    if (target->frames<10) { fprintf(out,"frame\t%u\t%u\tclear\n",index,target->frames); fflush(out); }
    for (unsigned i=0;i<4 && SUCCEEDED(hr);++i)
        hr=IDirect3DDevice9_Clear(target->device,1,&rect[i],D3DCLEAR_TARGET,colors[index][i],1,0);
    if (target->frames<10) { fprintf(out,"frame\t%u\t%u\tpresent\n",index,target->frames); fflush(out); }
    if (SUCCEEDED(hr)) hr=IDirect3DDevice9_Present(target->device,NULL,NULL,NULL,NULL);
    if (FAILED(hr)) { fprintf(out,"present_failed\t%u\t%08lx\n",index,(unsigned long)hr); ++errors; return 0; }
    ++target->frames; return 1;
}
int main(int argc,char **argv)
{
    IDirect3D9 *d3d;
    struct target targets[2]={{0},{0}};
    MSG msg;
    DWORD started;
    int resized=0,recreated=0,ok=1;
    WNDCLASSA wc={0};
    out=fopen(argc>1?argv[1]:"C:\\vp-gpu-present.tsv","w");
    if (!out) return 2;
    d3d=Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { fclose(out); return 3; }
    wc.style=CS_OWNDC; wc.lpfnWndProc=window_proc;
    wc.hInstance=GetModuleHandleA(NULL); wc.lpszClassName="GpuPresentProbe";
    if (!RegisterClassA(&wc)) { IDirect3D9_Release(d3d); fclose(out); return 3; }
    ok=create(d3d,&targets[0],70,70,320,240) && create(d3d,&targets[1],600,70,320,240);
    started=GetTickCount();
    while (ok && GetTickCount()-started<20000) {
        DWORD elapsed=GetTickCount()-started;
        if (targets[0].frames<10) { fprintf(out,"loop\t%u\tpump\n",targets[0].frames); fflush(out); }
        for (unsigned n=0;n<128 && PeekMessageA(&msg,NULL,0,0,PM_REMOVE);++n)
            { TranslateMessage(&msg); DispatchMessageA(&msg); }
        if (targets[0].frames<10) { fprintf(out,"loop\t%u\tpresent\n",targets[0].frames); fflush(out); }
        if (!resized && elapsed>=5000) { resized=1; ok=resize(&targets[0]); }
        if (!recreated && elapsed>=11000) {
            recreated=1; destroy(&targets[1]);
            ok=create(d3d,&targets[1],600,70,320,240);
            fprintf(out,"recreate\t%d\n",ok); fflush(out);
        }
        if (ok) ok=present(&targets[0],0) && present(&targets[1],1);
        if (targets[0].frames == 1) { fprintf(out,"first_present\t%d\n",ok); fflush(out); }
        if (targets[0].frames<11) { fprintf(out,"loop\t%u\tsleep\n",targets[0].frames); fflush(out); }
        Sleep(16);
    }
    fprintf(out,"summary\terrors=%u\tframes_a=%u\tframes_b=%u\tresized=%d\trecreated=%d\n",
        errors,targets[0].frames,targets[1].frames,resized,recreated);
    fclose(out);
    destroy(&targets[0]); destroy(&targets[1]); IDirect3D9_Release(d3d);
    return ok && !errors && resized && recreated ? 0 : 4;
}
