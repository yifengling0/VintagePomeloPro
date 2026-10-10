#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>

static FILE *out;
static unsigned failures;
static const DWORD rgb = D3DCOLOR_ARGB(255,51,102,153);

static int sample(IDirect3DDevice9 *dev, IDirect3DSurface9 *target,
        const char *name, unsigned r, unsigned g, unsigned b)
{
    IDirect3DSurface9 *copy = NULL;
    D3DLOCKED_RECT map;
    D3DSURFACE_DESC desc;
    HRESULT hr;
    DWORD pixel = 0;
    int ok = 0;
    IDirect3DSurface9_GetDesc(target, &desc);
    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, desc.Width, desc.Height,
            desc.Format, D3DPOOL_SYSTEMMEM, &copy, NULL);
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_GetRenderTargetData(dev, target, copy);
    if (SUCCEEDED(hr)) hr = IDirect3DSurface9_LockRect(copy, &map, NULL, D3DLOCK_READONLY);
    if (SUCCEEDED(hr))
    {
        pixel = *(DWORD *)((char *)map.pBits + (desc.Height / 2) * map.Pitch + (desc.Width / 2) * 4);
        IDirect3DSurface9_UnlockRect(copy);
        ok = abs((int)((pixel >> 16) & 255) - (int)r) <= 2
                && abs((int)((pixel >> 8) & 255) - (int)g) <= 2
                && abs((int)(pixel & 255) - (int)b) <= 2;
    }
    fprintf(out,"sample\t%s\t%lu,%lu,%lu\t%u,%u,%u\t%d\t%08lx\n",name,
            (pixel>>16)&255,(pixel>>8)&255,pixel&255,r,g,b,ok,(unsigned long)hr);
    fflush(out);
    if (copy) IDirect3DSurface9_Release(copy);
    failures += !ok;
    return ok;
}

struct vertex { float x,y,z,w; DWORD color; float u,v; };
static HRESULT draw(IDirect3DDevice9 *dev, IDirect3DSurface9 *dst,
        IDirect3DTexture9 *src, BOOL decode, BOOL blend)
{
    struct vertex v[] = {
        {-.5f,-.5f,0,1,0xffffffff,0,0}, {255.5f,-.5f,0,1,0xffffffff,1,0},
        {-.5f,255.5f,0,1,0xffffffff,0,1}, {255.5f,255.5f,0,1,0xffffffff,1,1}
    };
    HRESULT hr;
    IDirect3DDevice9_SetRenderTarget(dev,0,dst);
    IDirect3DDevice9_SetDepthStencilSurface(dev,NULL);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_SRGBWRITEENABLE,FALSE);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_ZENABLE,FALSE);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_LIGHTING,FALSE);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_CULLMODE,D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_ALPHABLENDENABLE,blend);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
    if (blend) for (unsigned i=0;i<4;i++) v[i].color=0x80ffffff;
    IDirect3DDevice9_SetTexture(dev,0,(IDirect3DBaseTexture9 *)src);
    IDirect3DDevice9_SetSamplerState(dev,0,D3DSAMP_SRGBTEXTURE,decode);
    IDirect3DDevice9_SetSamplerState(dev,0,D3DSAMP_MINFILTER,D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev,0,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
    IDirect3DDevice9_SetTextureStageState(dev,0,D3DTSS_COLOROP,D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(dev,0,D3DTSS_COLORARG1,D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(dev,0,D3DTSS_COLORARG2,D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(dev,0,D3DTSS_ALPHAOP,D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(dev,0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(dev,0,D3DTSS_ALPHAARG2,D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(dev,1,D3DTSS_COLOROP,D3DTOP_DISABLE);
    IDirect3DDevice9_SetFVF(dev,D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1);
    hr=IDirect3DDevice9_BeginScene(dev);
    if (SUCCEEDED(hr))
    {
        hr=IDirect3DDevice9_DrawPrimitiveUP(dev,D3DPT_TRIANGLESTRIP,2,v,sizeof(v[0]));
        IDirect3DDevice9_EndScene(dev);
    }
    IDirect3DDevice9_SetTexture(dev,0,NULL);
    return hr;
}

int main(int argc,char **argv)
{
    IDirect3D9 *d3d = NULL;
    IDirect3DDevice9 *dev = NULL;
    IDirect3DSurface9 *back = NULL, *surface[2] = {NULL,NULL};
    IDirect3DTexture9 *texture[2] = {NULL,NULL}, *upload = NULL;
    D3DPRESENT_PARAMETERS pp = {0};
    D3DLOCKED_RECT map;
    HRESULT hr;
    HWND hwnd;
    out=fopen(argc>1?argv[1]:"C:\\vp-d3d9-srgb-probe.tsv","w");
    if (!out) return 2;
    hwnd=CreateWindowA("STATIC","D3D9 linear render target regression",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
            0,0,320,320,NULL,NULL,GetModuleHandleA(NULL),NULL);
    d3d=Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d || !hwnd) goto fail;
    pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=hwnd;
    pp.BackBufferWidth=256;pp.BackBufferHeight=256;pp.BackBufferFormat=D3DFMT_A8R8G8B8;
    hr=IDirect3D9_CreateDevice(d3d,D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&dev);
    if (FAILED(hr)) { fprintf(out,"createDevice\t%08lx\n",(unsigned long)hr); goto fail; }
    IDirect3DDevice9_GetRenderTarget(dev,0,&back);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_SRGBWRITEENABLE,FALSE);
    IDirect3DDevice9_Clear(dev,0,NULL,D3DCLEAR_TARGET,rgb,1,0);
    sample(dev,back,"backbuffer_clear",51,102,153);
    for (unsigned i=0;i<2;i++)
    {
        hr=IDirect3DDevice9_CreateTexture(dev,256,256,1,D3DUSAGE_RENDERTARGET,
                D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&texture[i],NULL);
        if (FAILED(hr)) goto fail;
        IDirect3DTexture9_GetSurfaceLevel(texture[i],0,&surface[i]);
    }
    IDirect3DDevice9_SetRenderTarget(dev,0,surface[0]);
    IDirect3DDevice9_Clear(dev,0,NULL,D3DCLEAR_TARGET,rgb,1,0);
    sample(dev,surface[0],"linear_rt_clear",51,102,153);
    if (FAILED(draw(dev,back,texture[0],FALSE,FALSE))) goto fail;
    sample(dev,back,"linear_rt_sample",51,102,153);
    for (unsigned i=0;i<4;i++)
    {
        char name[48];
        unsigned dst=(i+1)%2,src=i%2;
        if (FAILED(draw(dev,surface[dst],texture[src],FALSE,FALSE))) goto fail;
        snprintf(name,sizeof(name),"linear_pingpong_%u",i+1);
        sample(dev,surface[dst],name,51,102,153);
    }
    IDirect3DDevice9_SetRenderTarget(dev,0,back);
    IDirect3DDevice9_Clear(dev,0,NULL,D3DCLEAR_TARGET,0xff000000,1,0);
    if (FAILED(draw(dev,back,texture[0],FALSE,TRUE))) goto fail;
    sample(dev,back,"linear_blend_black",26,51,77);
    hr=IDirect3DDevice9_CreateTexture(dev,1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&upload,NULL);
    if (FAILED(hr)) goto fail;
    if (FAILED(IDirect3DTexture9_LockRect(upload,0,&map,NULL,0))) goto fail;
    *(DWORD *)map.pBits=rgb;
    IDirect3DTexture9_UnlockRect(upload,0);
    if (FAILED(draw(dev,back,upload,TRUE,FALSE))) goto fail;
    sample(dev,back,"explicit_srgb_read",8,34,81);
    if (FAILED(draw(dev,back,upload,FALSE,FALSE))) goto fail;
    sample(dev,back,"linear_read_after_srgb",51,102,153);
    fprintf(out,"summary\tfailures=%u\n",failures);
    if (upload) IDirect3DTexture9_Release(upload);
    for(unsigned i=0;i<2;i++) { IDirect3DSurface9_Release(surface[i]); IDirect3DTexture9_Release(texture[i]); }
    IDirect3DSurface9_Release(back);IDirect3DDevice9_Release(dev);IDirect3D9_Release(d3d);
    DestroyWindow(hwnd);fclose(out);
    return failures?1:0;
fail:
    fprintf(out,"fatal\tinitialization_or_draw_failed\n");fclose(out);return 3;
}
