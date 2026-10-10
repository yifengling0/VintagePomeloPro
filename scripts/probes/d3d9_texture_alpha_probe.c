#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *out;
static unsigned failed;
static unsigned checks;
static D3DCMPFUNC comparison = D3DCMP_GREATER;
static DWORD reference = 127;
static BOOL separate_alpha;
struct vertex { float x, y, z, w; DWORD color; float u, v; };

static void check_pixel(IDirect3DDevice9 *dev, IDirect3DSurface9 *surface,
        const char *name, DWORD expected, BOOL check_alpha)
{
    IDirect3DSurface9 *copy = NULL;
    D3DLOCKED_RECT map;
    D3DSURFACE_DESC desc;
    IDirect3DSurface9_GetDesc(surface, &desc);
    HRESULT hr = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 64, 64,
            desc.Format, D3DPOOL_SYSTEMMEM, &copy, NULL);
    DWORD pixel = 0;
    BOOL ok = FALSE;
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_GetRenderTargetData(dev, surface, copy);
    if (SUCCEEDED(hr)) hr = IDirect3DSurface9_LockRect(copy, &map, NULL, D3DLOCK_READONLY);
    if (SUCCEEDED(hr))
    {
        pixel = *(DWORD *)((char *)map.pBits + 32 * map.Pitch + 32 * 4);
        IDirect3DSurface9_UnlockRect(copy);
        ok = TRUE;
        for (unsigned shift = 0; shift < (check_alpha && desc.Format == D3DFMT_A8R8G8B8 ? 32 : 24); shift += 8)
            ok &= abs((int)((pixel >> shift) & 255) - (int)((expected >> shift) & 255)) <= 3;
    }
    fprintf(out, "pixel\t%s\t%08lx\t%08lx\t%d\thr=%08lx\n", name,
            (unsigned long)pixel, (unsigned long)expected, ok, (unsigned long)hr);
    fflush(out);
    failed += !ok;
    ++checks;
    if (copy) IDirect3DSurface9_Release(copy);
}

static HRESULT draw(IDirect3DDevice9 *dev, IDirect3DTexture9 *texture, BOOL blend, BOOL alpha_test)
{
    struct vertex v[] = {
        {-.5f,-.5f,0,1,0xffffffff,0,0}, {63.5f,-.5f,0,1,0xffffffff,1,0},
        {-.5f,63.5f,0,1,0xffffffff,0,1}, {63.5f,63.5f,0,1,0xffffffff,1,1}
    };
    HRESULT hr;
    IDirect3DDevice9_SetRenderState(dev,D3DRS_LIGHTING,FALSE);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_ZENABLE,FALSE);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_CULLMODE,D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_SRGBWRITEENABLE,FALSE);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_ALPHABLENDENABLE,blend);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_SEPARATEALPHABLENDENABLE,separate_alpha);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_SRCBLENDALPHA,D3DBLEND_ONE);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_DESTBLENDALPHA,D3DBLEND_ZERO);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_ALPHATESTENABLE,alpha_test);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_ALPHAFUNC,comparison);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_ALPHAREF,reference);
    IDirect3DDevice9_SetTexture(dev,0,(IDirect3DBaseTexture9 *)texture);
    IDirect3DDevice9_SetSamplerState(dev,0,D3DSAMP_SRGBTEXTURE,FALSE);
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
    hr = IDirect3DDevice9_BeginScene(dev);
    if (SUCCEEDED(hr))
    {
        hr = IDirect3DDevice9_DrawPrimitiveUP(dev,D3DPT_TRIANGLESTRIP,2,v,sizeof(v[0]));
        IDirect3DDevice9_EndScene(dev);
    }
    IDirect3DDevice9_SetTexture(dev,0,NULL);
    return hr;
}

int main(int argc, char **argv)
{
    HWND window;
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev = NULL;
    IDirect3DSurface9 *back = NULL;
    D3DPRESENT_PARAMETERS pp = {0};
    DWORD sources[] = {0x00336699, 0x80336699, 0xff336699};
    D3DFORMAT texture_format = D3DFMT_A8R8G8B8;
    const DWORD background = 0xff204060;
    out = fopen(argc > 1 ? argv[1] : "C:\\vp-alpha-probe.tsv", "w");
    if (!out) return 2;
    fprintf(out,"pointer_bits\t%u\n",(unsigned)(sizeof(void *)*8));
    window = CreateWindowA("STATIC","D3D9 texture-alpha probe",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
            0,0,160,160,NULL,NULL,GetModuleHandleA(NULL),NULL);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window; pp.BackBufferWidth = 64; pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    for (int i=2; i<argc; ++i)
    {
        if (!strcmp(argv[i], "--xrgb")) pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        if (!strcmp(argv[i], "--argb4444")) {texture_format = D3DFMT_A4R4G4B4;sources[1]=0x88336699;}
    }
    fprintf(out,"backbuffer_format\t%u\n",(unsigned)pp.BackBufferFormat);
    fprintf(out,"texture_format\t%u\n",(unsigned)texture_format);
    if (!d3d || FAILED(IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&dev))) goto fatal;
    IDirect3DDevice9_GetRenderTarget(dev,0,&back);
    for (unsigned pool = 0; pool < 2; ++pool)
    for (unsigned s = 0; s < 3; ++s)
    {
        IDirect3DTexture9 *texture = NULL;
        D3DLOCKED_RECT map;
        char name[80];
        if (FAILED(IDirect3DDevice9_CreateTexture(dev,1,1,1,pool?D3DUSAGE_DYNAMIC:0,
                texture_format,pool?D3DPOOL_DEFAULT:D3DPOOL_MANAGED,&texture,NULL))) goto fatal;
        if (FAILED(IDirect3DTexture9_LockRect(texture,0,&map,NULL,pool?D3DLOCK_DISCARD:0))) goto fatal;
        if (texture_format == D3DFMT_A4R4G4B4)
            *(WORD *)map.pBits = (WORD)(((sources[s]>>16)&0xf000)|((sources[s]>>12)&0x0f00)|((sources[s]>>8)&0x00f0)|((sources[s]>>4)&0x000f));
        else *(DWORD *)map.pBits = sources[s];
        IDirect3DTexture9_UnlockRect(texture,0);
        for (unsigned mode = 0; mode < 4; ++mode)
        {
            DWORD expected = sources[s];
            IDirect3DDevice9_Clear(dev,0,NULL,D3DCLEAR_TARGET,background,1,0);
            separate_alpha = mode == 3;
            if (FAILED(draw(dev,texture,mode==1||mode==3,mode==2))) goto fatal;
            if ((mode==1||mode==3) && s==0) expected = mode==3 ? background&0xffffff : background;
            if ((mode==1||mode==3) && s==1) expected = texture_format == D3DFMT_A4R4G4B4 ? 0xc02a547e : 0xbf2a537d;
            if (mode==3 && s==1) expected = (expected&0xffffff) | (sources[s]&0xff000000);
            if (mode==2 && s==0) expected = background;
            snprintf(name,sizeof(name),"pool%u_alpha%u_mode%u",pool,(unsigned)(sources[s]>>24),mode);
            check_pixel(dev,back,name,expected,TRUE);
        }
        separate_alpha = FALSE;
        for (unsigned cmp = D3DCMP_NEVER; cmp <= D3DCMP_ALWAYS; ++cmp)
        {
            const unsigned alpha=sources[s]>>24;
            BOOL pass=FALSE;
            comparison=(D3DCMPFUNC)cmp;reference=127;
            switch(cmp) {
                case D3DCMP_NEVER:break;case D3DCMP_LESS:pass=alpha<127;break;
                case D3DCMP_EQUAL:pass=alpha==127;break;case D3DCMP_LESSEQUAL:pass=alpha<=127;break;
                case D3DCMP_GREATER:pass=alpha>127;break;case D3DCMP_NOTEQUAL:pass=alpha!=127;break;
                case D3DCMP_GREATEREQUAL:pass=alpha>=127;break;case D3DCMP_ALWAYS:pass=TRUE;break;
            }
            IDirect3DDevice9_Clear(dev,0,NULL,D3DCLEAR_TARGET,background,1,0);
            if (FAILED(draw(dev,texture,FALSE,TRUE))) goto fatal;
            snprintf(name,sizeof(name),"pool%u_alpha%u_cmp%u",pool,alpha,cmp);
            check_pixel(dev,back,name,pass?sources[s]:background,TRUE);
        }
        comparison=D3DCMP_GREATER;reference=127;
        IDirect3DTexture9_Release(texture);
    }
    fprintf(out,"summary\tchecks=%u\tfailures=%u\n",checks,failed);
    fclose(out);
    IDirect3DSurface9_Release(back); IDirect3DDevice9_Release(dev); IDirect3D9_Release(d3d);
    DestroyWindow(window);
    return failed?1:0;
fatal:
    fprintf(out,"fatal\tinitialization_or_draw\n"); fclose(out); return 3;
}
