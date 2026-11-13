#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* D3D9 reference arithmetic for filtered, tinted, translucent sprites.
 * Each quad samples a constant coordinate, avoiding rasterizer rounding in
 * the reference. The four texels include RGB in fully transparent pixels. */
struct vertex { float x, y, z, w; DWORD color; float u, v; };
static unsigned failures, checks;
static FILE *out;

static float channel(DWORD pixel, unsigned c) { return ((pixel >> (c * 8)) & 255) / 255.f; }
static float decode(float x) { return x <= .04045f ? x / 12.92f : powf((x + .055f) / 1.055f, 2.4f); }
static DWORD expected(const DWORD *texels, unsigned sample, DWORD tint, unsigned op,
        unsigned blend, BOOL srgb)
{
    const DWORD background = 0xff203040;
    float color[4] = {0};
    for (unsigned c = 0; c < 4; ++c)
    {
        if (sample < 4) color[c] = srgb && c < 3 ? decode(channel(texels[sample], c)) : channel(texels[sample], c);
        else for (unsigned t = 0; t < 4; ++t)
            color[c] += (srgb && c < 3 ? decode(channel(texels[t], c)) : channel(texels[t], c)) * .25f;
        if (op != D3DTOP_SELECTARG1) color[c] *= channel(tint, c) * (op == D3DTOP_MODULATE2X && c < 3 ? 2 : 1);
        color[c] = fminf(color[c], 1.f);
    }
    DWORD result = 0;
    for (unsigned c = 0; c < 4; ++c)
    {
        float value = color[c];
        if (blend && c < 3)
            value = color[c] * color[3] + channel(background, c) * (blend == 1 ? 1.f - color[3] : 1.f);
        /* Separate alpha blending is ONE,ZERO, as used by Touhou 18. */
        value = fmaxf(0, fminf(1, value));
        result |= (DWORD)floorf(value * 255.f + .5f) << (c * 8);
    }
    return result;
}

static HRESULT draw(IDirect3DDevice9 *dev, IDirect3DTexture9 *texture, DWORD tint,
        unsigned op, unsigned blend, BOOL factor, unsigned sample, BOOL srgb)
{
    float u = sample < 4 ? .25f + .5f * (sample & 1) : .5f;
    float v = sample < 4 ? .25f + .5f * (sample >> 1) : .5f;
    struct vertex vertices[] = {
        {-.5f,-.5f,0,1,tint,u,v}, {63.5f,-.5f,0,1,tint,u,v},
        {-.5f,63.5f,0,1,tint,u,v}, {63.5f,63.5f,0,1,tint,u,v},
    };
    IDirect3DDevice9_SetRenderState(dev,D3DRS_LIGHTING,FALSE);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_ZENABLE,FALSE);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_CULLMODE,D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_FOGENABLE,FALSE);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_SRGBWRITEENABLE,FALSE);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_ALPHATESTENABLE,FALSE);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_ALPHABLENDENABLE,blend != 0);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_DESTBLEND,blend == 2 ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_SEPARATEALPHABLENDENABLE,TRUE);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_SRCBLENDALPHA,D3DBLEND_ONE);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_DESTBLENDALPHA,D3DBLEND_ZERO);
    IDirect3DDevice9_SetRenderState(dev,D3DRS_TEXTUREFACTOR,tint);
    IDirect3DDevice9_SetTexture(dev,0,(IDirect3DBaseTexture9 *)texture);
    IDirect3DDevice9_SetSamplerState(dev,0,D3DSAMP_SRGBTEXTURE,srgb);
    IDirect3DDevice9_SetSamplerState(dev,0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(dev,0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(dev,0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(dev,0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetTextureStageState(dev,0,D3DTSS_COLOROP,op);
    IDirect3DDevice9_SetTextureStageState(dev,0,D3DTSS_COLORARG1,D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(dev,0,D3DTSS_COLORARG2,factor ? D3DTA_TFACTOR : D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(dev,0,D3DTSS_ALPHAOP,op == D3DTOP_SELECTARG1 ? op : D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(dev,0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(dev,0,D3DTSS_ALPHAARG2,factor ? D3DTA_TFACTOR : D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(dev,1,D3DTSS_COLOROP,D3DTOP_DISABLE);
    IDirect3DDevice9_SetFVF(dev,D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1);
    HRESULT hr = IDirect3DDevice9_BeginScene(dev);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9_DrawPrimitiveUP(dev,D3DPT_TRIANGLESTRIP,2,vertices,sizeof(vertices[0]));
        IDirect3DDevice9_EndScene(dev);
    }
    return hr;
}

int main(int argc, char **argv)
{
    IDirect3D9 *d3d = NULL;
    IDirect3DDevice9 *dev = NULL;
    IDirect3DSurface9 *back = NULL, *copy = NULL;
    HWND window = CreateWindowA("STATIC","D3D9 sprite color probe",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
            0,0,180,180,NULL,NULL,GetModuleHandleA(NULL),NULL);
    D3DPRESENT_PARAMETERS pp = {0};
    out = fopen(argc > 1 ? argv[1] : "C:\\vp-sprite-color.tsv", "w");
    if (!out) return 2;
    fprintf(out,"pointer_bits\t%u\n",(unsigned)(sizeof(void*)*8));
    pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow=window;
    pp.BackBufferWidth=64; pp.BackBufferHeight=64; pp.BackBufferFormat=D3DFMT_A8R8G8B8;
    if (argc > 2) pp.BackBufferFormat=D3DFMT_X8R8G8B8;
    d3d=Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d || FAILED(IDirect3D9_CreateDevice(d3d,0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&dev))) goto fatal;
    IDirect3DDevice9_GetRenderTarget(dev,0,&back);
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev,64,64,pp.BackBufferFormat,D3DPOOL_SYSTEMMEM,&copy,NULL))) goto fatal;
    DWORD tints[]={0xffffffff,0x804080c0,0x33886644};
    unsigned ops[]={D3DTOP_MODULATE,D3DTOP_SELECTARG1,D3DTOP_MODULATE2X};
    D3DFORMAT formats[]={D3DFMT_A8R8G8B8,D3DFMT_A4R4G4B4};
    for (unsigned fmt=0; fmt<2; ++fmt) for(unsigned pool=0; pool<2; ++pool)
    {
        IDirect3DTexture9 *texture=NULL;
        D3DLOCKED_RECT map;
        DWORD texels[]={0x00ff00ff,0x33557799,0x88884422,0xff336699};
        if(FAILED(IDirect3DDevice9_CreateTexture(dev,2,2,1,pool?D3DUSAGE_DYNAMIC:0,formats[fmt],pool?D3DPOOL_DEFAULT:D3DPOOL_MANAGED,&texture,NULL))) goto fatal;
        if(FAILED(IDirect3DTexture9_LockRect(texture,0,&map,NULL,pool?D3DLOCK_DISCARD:0))) goto fatal;
        for(unsigned i=0; i<4; ++i) {
            char *row=(char*)map.pBits+(i>>1)*map.Pitch;
            if(fmt) ((WORD*)row)[i&1]=(WORD)(((texels[i]>>16)&0xf000)|((texels[i]>>12)&0x0f00)|((texels[i]>>8)&0x00f0)|((texels[i]>>4)&0x000f));
            else ((DWORD*)row)[i&1]=texels[i];
        }
        IDirect3DTexture9_UnlockRect(texture,0);
        for(unsigned srgb=0; srgb<2; ++srgb) for(unsigned tint=0; tint<3; ++tint)
        for(unsigned op=0; op<3; ++op) for(unsigned factor=0; factor<2; ++factor)
        for(unsigned blend=0; blend<3; ++blend) for(unsigned sample=0; sample<5; ++sample)
        {
            IDirect3DDevice9_Clear(dev,0,NULL,D3DCLEAR_TARGET,0xff203040,1,0);
            if(FAILED(draw(dev,texture,tints[tint],ops[op],blend,factor,sample,srgb))) goto fatal;
            HRESULT hr=IDirect3DDevice9_GetRenderTargetData(dev,back,copy);
            if(FAILED(hr) || FAILED(IDirect3DSurface9_LockRect(copy,&map,NULL,D3DLOCK_READONLY))) goto fatal;
            DWORD actual=*(DWORD*)((char*)map.pBits+32*map.Pitch+32*4);
            IDirect3DSurface9_UnlockRect(copy);
            DWORD wanted=expected(texels,sample,tints[tint],ops[op],blend,srgb);
            BOOL ok=TRUE;
            for(unsigned c=0; c<(pp.BackBufferFormat==D3DFMT_A8R8G8B8?4:3); ++c) ok &= abs((int)((actual>>(c*8))&255)-(int)((wanted>>(c*8))&255))<=3;
            if(!ok) fprintf(out,"FAIL\tfmt=%u pool=%u srgb=%u tint=%u op=%u factor=%u blend=%u sample=%u\tactual=%08lx expected=%08lx\n",fmt,pool,srgb,tint,op,factor,blend,sample,(unsigned long)actual,(unsigned long)wanted);
            ++checks; failures+=!ok; fflush(out);
        }
        IDirect3DDevice9_SetTexture(dev,0,NULL);
        IDirect3DTexture9_Release(texture);
    }
    fprintf(out,"summary\tchecks=%u failures=%u\n",checks,failures);
    fclose(out);
    IDirect3DSurface9_Release(copy); IDirect3DSurface9_Release(back);
    IDirect3DDevice9_Release(dev); IDirect3D9_Release(d3d); DestroyWindow(window);
    return failures?1:0;
fatal:
    fprintf(out,"fatal\tinitialization_or_draw\n"); fclose(out); return 3;
}
