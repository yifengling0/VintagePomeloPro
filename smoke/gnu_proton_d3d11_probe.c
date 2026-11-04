// SPDX-License-Identifier: LGPL-2.1-or-later
// Bounded diagnostic fixture. Intentional staging readback verifies clear output;
// this is NOT a zero-readback workload or a game-performance benchmark.
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define VK_NO_PROTOTYPES
#include <vulkan_core.h>

// DXVK's public IDXGIVkInteropAdapter ABI, identical in both pinned forks.
typedef struct HslVkAdapter HslVkAdapter;
typedef struct HslVkAdapterVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(HslVkAdapter*,REFIID,void**);
    ULONG (STDMETHODCALLTYPE *AddRef)(HslVkAdapter*);
    ULONG (STDMETHODCALLTYPE *Release)(HslVkAdapter*);
    void (STDMETHODCALLTYPE *GetVulkanHandles)(HslVkAdapter*,VkInstance*,VkPhysicalDevice*);
} HslVkAdapterVtbl;
struct HslVkAdapter { const HslVkAdapterVtbl* lpVtbl; };
static const GUID hsl_vk_adapter_iid={0x3a6d8f2c,0xb0e8,0x4ab4,{0xb4,0xdc,0x4f,0xd2,0x48,0x91,0xbf,0xa5}};

static DWORD pid;
static void json_string(const char* value) {
    putchar('"');for(const unsigned char* p=(const unsigned char*)value;*p;++p){
        if(*p=='"'||*p=='\\'){putchar('\\');putchar(*p);}else if(*p<32)printf("\\u%04x",*p);else putchar(*p);
    }putchar('"');
}
static void hex_string(const BYTE* bytes,unsigned count) {
    putchar('"');for(unsigned i=0;i<count;++i)printf("%02x",bytes[i]);putchar('"');
}
static int adapter_vulkan_identity(IDXGIAdapter* adapter) {
    HslVkAdapter* interop=NULL;VkInstance instance=VK_NULL_HANDLE;VkPhysicalDevice physical=VK_NULL_HANDLE;
    if(FAILED(IDXGIAdapter_QueryInterface(adapter,&hsl_vk_adapter_iid,(void**)&interop))||!interop)return 0;
    interop->lpVtbl->GetVulkanHandles(interop,&instance,&physical);
    interop->lpVtbl->Release(interop);
    HMODULE library=GetModuleHandleW(L"vulkan-1.dll");
    if(!library)library=GetModuleHandleW(L"winevulkan.dll");
    PFN_vkGetInstanceProcAddr address=library?(PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(library,"vkGetInstanceProcAddr"):NULL;
    PFN_vkGetPhysicalDeviceProperties2 properties=address&&instance?(PFN_vkGetPhysicalDeviceProperties2)address(instance,"vkGetPhysicalDeviceProperties2"):NULL;
    if(!properties||!physical)return 0;
    VkPhysicalDeviceIDProperties id={0};id.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 data={0};data.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;data.pNext=&id;
    properties(physical,&data);
    printf(",\"vulkanIdentity\":{\"source\":\"dxgi-vulkan-interop\",\"apiVersion\":%u,\"driverVersion\":%u,\"vendorId\":%u,\"deviceId\":%u,\"deviceUuid\":",data.properties.apiVersion,data.properties.driverVersion,data.properties.vendorID,data.properties.deviceID);
    hex_string(id.deviceUUID,VK_UUID_SIZE);printf(",\"driverUuid\":");hex_string(id.driverUUID,VK_UUID_SIZE);
    printf(",\"deviceLUIDValid\":%s,\"deviceLUID\":",id.deviceLUIDValid?"true":"false");hex_string(id.deviceLUID,VK_LUID_SIZE);putchar('}');
    return 1;
}
static int module_record(const char* name,HMODULE module) {
    wchar_t path[2048],backing[2048];char utf8[8192],backing_utf8[8192];
    DWORD n=GetModuleFileNameW(module,path,2048);
    if(!n||n>=2048||!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path,-1,utf8,sizeof(utf8),NULL,NULL))return 0;
    // WoW64 can report a logical system32 module name. Ask the public file
    // API for the actual redirected backing, rather than guessing from ABI.
    HANDLE file=CreateFileW(path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    if(file==INVALID_HANDLE_VALUE)return 0;
    n=GetFinalPathNameByHandleW(file,backing,2048,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    CloseHandle(file);
    if(!n||n>=2048)return 0;
    const wchar_t* normalized=backing;
    if(!wcsncmp(backing,L"\\\\?\\",4))normalized=backing+4;
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,normalized,-1,backing_utf8,sizeof(backing_utf8),NULL,NULL))return 0;
    const IMAGE_DOS_HEADER* dos=(const IMAGE_DOS_HEADER*)module;
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<64||dos->e_lfanew>1024*1024)return 0;
    const IMAGE_NT_HEADERS* pe=(const IMAGE_NT_HEADERS*)((const BYTE*)module+dos->e_lfanew);
    if(pe->Signature!=IMAGE_NT_SIGNATURE)return 0;
    printf("{\"schema\":1,\"event\":\"module\",\"pid\":%lu,\"name\":",(unsigned long)pid);json_string(name);printf(",\"path\":");json_string(utf8);
    printf(",\"backingPath\":");json_string(backing_utf8);
    DWORD load_rva=pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG].VirtualAddress;
    DWORD load_size=pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG].Size;
    DWORD chpe_rva=0,chpe_version=0;
    if(pe->OptionalHeader.Magic==0x20b&&load_rva&&load_size>=208&&load_rva<pe->OptionalHeader.SizeOfImage&&load_size<=pe->OptionalHeader.SizeOfImage-load_rva){
        const BYTE* config=(const BYTE*)module+load_rva;DWORD declared=0;memcpy(&declared,config,4);
        if(declared>=208){uint64_t pointer=0;memcpy(&pointer,config+200,8);uintptr_t base=(uintptr_t)module;
            if(pointer>=base&&pointer-base<=pe->OptionalHeader.SizeOfImage-4){chpe_rva=(DWORD)(pointer-base);memcpy(&chpe_version,(const BYTE*)module+chpe_rva,4);}}
    }
    printf(",\"machine\":%u,\"optionalMagic\":%u,\"timeDateStamp\":%lu,\"sizeOfImage\":%lu,\"imageBase\":%llu,\"peView\":{",pe->FileHeader.Machine,pe->OptionalHeader.Magic,(unsigned long)pe->FileHeader.TimeDateStamp,(unsigned long)pe->OptionalHeader.SizeOfImage,(unsigned long long)(uintptr_t)module);
    printf("\"machine\":%u,\"optionalMagic\":%u,\"timeDateStamp\":%lu,\"sizeOfImage\":%lu,\"loadConfigRva\":%lu,\"loadConfigSize\":%lu,\"chpeMetadataRva\":%lu,\"chpeMetadataVersion\":%lu,",pe->FileHeader.Machine,pe->OptionalHeader.Magic,(unsigned long)pe->FileHeader.TimeDateStamp,(unsigned long)pe->OptionalHeader.SizeOfImage,(unsigned long)load_rva,(unsigned long)load_size,(unsigned long)chpe_rva,(unsigned long)chpe_version);
    printf("\"exportDirectoryRva\":%lu,\"exportDirectorySize\":%lu,\"importDirectoryRva\":%lu,\"importDirectorySize\":%lu},\"fileSha256\":null}\n",(unsigned long)pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress,(unsigned long)pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size,(unsigned long)pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress,(unsigned long)pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size);
    return 1;
}
// Real upload/decode and mapped visibility checks, not capability spoofing.
static void profile_record(void) {
    const char* keys[]={"VPP_GNU_DXVK_PROFILE","WINEHUA_DXVK_RELAXED_FEATURES","DXVK_WINEHUA_COMMAND_QUERY_RESET","DXVK_WINEHUA_FLUSH_DYNAMIC_MAPPED","DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT","DXVK_WINEHUA_PRECISE_SHADOW","DXVK_WINEHUA_BATCH_MAPPED_FLUSH","VN_OHOS_PRECISE_SHADOW","VN_WINEHUA_PERSISTENT_MAP_SYNC"};
    printf("{\"schema\":1,\"event\":\"profile\",\"pid\":%lu,\"environment\":{",(unsigned long)pid);
    for(unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);++i){if(i)putchar(',');json_string(keys[i]);putchar(':');const char* value=getenv(keys[i]);json_string(value?value:"");}
    puts("}}");
}
static void block_bits(BYTE* data,unsigned* position,unsigned value,unsigned count) {
    for(unsigned i=0;i<count;++i,++*position)if((value>>i)&1)data[*position/8]|=(BYTE)(1u<<(*position%8));
}
static unsigned bc_block(unsigned format,BYTE data[16],float expected[4]) {
    memset(data,0,16);expected[0]=1.0f;expected[1]=0.0f;expected[2]=0.0f;expected[3]=1.0f;
    switch(format){
        case 1:data[0]=0;data[1]=0xf8;return 8; // RGB565 red, all selectors 0.
        case 2:memset(data,0xff,8);data[9]=0xf8;return 16;
        case 3:data[0]=255;data[1]=255;data[9]=0xf8;return 16;
        case 4:data[0]=255;data[1]=255;return 8;
        case 5:data[0]=255;data[1]=255;return 16;
        case 6:{ // Valid BC6H UF16 mode 11, identical explicit red endpoints.
            // Quantized 495 -> half 0x3c00 (1.0); all selectors zero.
            // Checked against pinned WineHua bcdec, all 16 pixels. Nonzero
            // output deliberately rejects an always-black BC6H decoder.
            unsigned bit=0;block_bits(data,&bit,3,5);
            const unsigned endpoint[6]={495,0,0,495,0,0};
            for(unsigned i=0;i<6;++i)block_bits(data,&bit,endpoint[i],10);
            return 16;
        }
        case 7:{ // Valid BC7 mode 6, equal RGBA endpoints, zero selectors.
            unsigned bit=0;block_bits(data,&bit,1u<<6,7);
            const unsigned endpoint[8]={127,127,0,0,0,0,127,127};
            for(unsigned i=0;i<8;++i)block_bits(data,&bit,endpoint[i],7);
            block_bits(data,&bit,0,2);expected[0]=254.0f/255.0f;expected[3]=254.0f/255.0f;return 16;
        }
        default:return 0;
    }
}
typedef HRESULT (WINAPI *CompileShader)(const void*,SIZE_T,const char*,const D3D_SHADER_MACRO*,ID3DInclude*,const char*,const char*,UINT,UINT,ID3DBlob**,ID3DBlob**);
static int draw_readback(ID3D11DeviceContext* context,ID3D11RenderTargetView* view,ID3D11Texture2D* target,ID3D11Texture2D* staging,
                         ID3D11ShaderResourceView* input,ID3D11Buffer* constants,const float expected[4]) {
    D3D11_VIEWPORT viewport={0,0,4,4,0,1};ID3D11DeviceContext_RSSetViewports(context,1,&viewport);
    ID3D11DeviceContext_OMSetRenderTargets(context,1,&view,NULL);
    ID3D11DeviceContext_PSSetShaderResources(context,0,1,&input);
    ID3D11DeviceContext_PSSetConstantBuffers(context,0,1,&constants);
    const float sentinel[4]={17,19,23,29};ID3D11DeviceContext_ClearRenderTargetView(context,view,sentinel);
    ID3D11DeviceContext_Draw(context,3,0);
    ID3D11DeviceContext_CopyResource(context,(ID3D11Resource*)staging,(ID3D11Resource*)target);
    D3D11_MAPPED_SUBRESOURCE mapped;memset(&mapped,0,sizeof(mapped));
    if(FAILED(ID3D11DeviceContext_Map(context,(ID3D11Resource*)staging,0,D3D11_MAP_READ,0,&mapped)))return 0;
    int valid=mapped.pData&&mapped.RowPitch>=64&&mapped.RowPitch<=65536;
    for(unsigned y=0;valid&&y<4;++y)for(unsigned x=0;valid&&x<4;++x){
        const float* pixel=(const float*)((const BYTE*)mapped.pData+(size_t)y*mapped.RowPitch+x*16);
        for(unsigned c=0;c<4;++c){float difference=pixel[c]-expected[c];
    if(!(difference>=-0.0001f&&difference<=0.0001f)){valid=0;break;}}
    }
    ID3D11DeviceContext_Unmap(context,(ID3D11Resource*)staging,0);
    ID3D11ShaderResourceView* empty=NULL;ID3D11DeviceContext_PSSetShaderResources(context,0,1,&empty);
    return valid;
}
static int mapped_color(ID3D11DeviceContext* context,ID3D11Buffer* buffer,D3D11_MAP mode,const float color[4]) {
    D3D11_MAPPED_SUBRESOURCE mapped;memset(&mapped,0,sizeof(mapped));
    if(FAILED(ID3D11DeviceContext_Map(context,(ID3D11Resource*)buffer,0,mode,0,&mapped)))return 0;
    if(!mapped.pData){ID3D11DeviceContext_Unmap(context,(ID3D11Resource*)buffer,0);return 0;}
    memcpy(mapped.pData,color,16);ID3D11DeviceContext_Unmap(context,(ID3D11Resource*)buffer,0);return 1;
}
static int fork_tests(ID3D11Device* device,ID3D11DeviceContext* context) {
    int result=0,discard_ok=0,no_overwrite_ok=0;unsigned bc_count=0;
    HMODULE compiler=LoadLibraryW(L"d3dcompiler_47.dll");
    ID3DBlob *vs_code=NULL,*ps_code=NULL,*error=NULL;
    ID3D11VertexShader* vertex=NULL;ID3D11PixelShader* pixel=NULL;ID3D11RasterizerState* raster=NULL;
    ID3D11Texture2D *target=NULL,*staging=NULL,*texture=NULL;ID3D11RenderTargetView* view=NULL;
    ID3D11ShaderResourceView* input=NULL;ID3D11Buffer* constants=NULL;
    if(!compiler||!module_record("d3dcompiler_47.dll",compiler))goto done;
    CompileShader compile=(CompileShader)(void*)GetProcAddress(compiler,"D3DCompile");
    if(!compile)goto done;
    const char shader[]="cbuffer P:register(b0){float4 tint;} Texture2D<float4> image:register(t0);"
      "float4 vs(uint n:SV_VertexID):SV_Position{float2 p=float2((n==2)?3:-1,(n==1)?3:-1);return float4(p,0,1);}"
      "float4 ps(float4 p:SV_Position):SV_Target{return image.Load(int3(int2(p.xy),0))*tint;}";
    HRESULT hr=compile(shader,sizeof(shader)-1,"hsl-controlled-fixture",NULL,NULL,"vs","vs_5_0",0,0,&vs_code,&error);
    if(error){error->lpVtbl->Release(error);error=NULL;}if(FAILED(hr)||!vs_code)goto done;
    hr=compile(shader,sizeof(shader)-1,"hsl-controlled-fixture",NULL,NULL,"ps","ps_5_0",0,0,&ps_code,&error);
    if(error){error->lpVtbl->Release(error);error=NULL;}if(FAILED(hr)||!ps_code)goto done;
    if(FAILED(ID3D11Device_CreateVertexShader(device,vs_code->lpVtbl->GetBufferPointer(vs_code),vs_code->lpVtbl->GetBufferSize(vs_code),NULL,&vertex))||
       FAILED(ID3D11Device_CreatePixelShader(device,ps_code->lpVtbl->GetBufferPointer(ps_code),ps_code->lpVtbl->GetBufferSize(ps_code),NULL,&pixel)))goto done;
    D3D11_RASTERIZER_DESC rd;memset(&rd,0,sizeof(rd));rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    if(FAILED(ID3D11Device_CreateRasterizerState(device,&rd,&raster)))goto done;
    ID3D11DeviceContext_RSSetState(context,raster);ID3D11DeviceContext_VSSetShader(context,vertex,NULL,0);ID3D11DeviceContext_PSSetShader(context,pixel,NULL,0);
    ID3D11DeviceContext_IASetPrimitiveTopology(context,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_BUFFER_DESC bd;memset(&bd,0,sizeof(bd));bd.ByteWidth=16;bd.Usage=D3D11_USAGE_DYNAMIC;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    if(FAILED(ID3D11Device_CreateBuffer(device,&bd,NULL,&constants)))goto done;
    const float white[4]={1,1,1,1};
    if(!mapped_color(context,constants,D3D11_MAP_WRITE_DISCARD,white))goto done;
    D3D11_TEXTURE2D_DESC td;memset(&td,0,sizeof(td));td.Width=4;td.Height=4;td.MipLevels=1;td.ArraySize=1;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    if(FAILED(ID3D11Device_CreateTexture2D(device,&td,NULL,&target))||FAILED(ID3D11Device_CreateRenderTargetView(device,(ID3D11Resource*)target,NULL,&view)))goto done;
    td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if(FAILED(ID3D11Device_CreateTexture2D(device,&td,NULL,&staging)))goto done;
    const DXGI_FORMAT formats[7]={DXGI_FORMAT_BC1_UNORM,DXGI_FORMAT_BC2_UNORM,DXGI_FORMAT_BC3_UNORM,DXGI_FORMAT_BC4_UNORM,DXGI_FORMAT_BC5_UNORM,DXGI_FORMAT_BC6H_UF16,DXGI_FORMAT_BC7_UNORM};
    for(unsigned bc=1;bc<=7;++bc){
        BYTE block[16];float expected[4];unsigned bytes=bc_block(bc,block,expected);
        td.Format=formats[bc-1];td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;td.CPUAccessFlags=0;
        D3D11_SUBRESOURCE_DATA source;memset(&source,0,sizeof(source));source.pSysMem=block;source.SysMemPitch=bytes;source.SysMemSlicePitch=bytes;
        if(FAILED(ID3D11Device_CreateTexture2D(device,&td,&source,&texture))||FAILED(ID3D11Device_CreateShaderResourceView(device,(ID3D11Resource*)texture,NULL,&input)))goto done;
        if(!draw_readback(context,view,target,staging,input,constants,expected))goto done;
        ID3D11ShaderResourceView_Release(input);input=NULL;ID3D11Texture2D_Release(texture);texture=NULL;++bc_count;
    }
    // Probe the advertised dynamic constant-buffer contract, never use
    // NO_OVERWRITE if the device says it is unsupported. Every readback waits
    // for prior GPU consumption before the next write, so no in-flight range
    // is illegally overwritten merely to make a test appear to pass.
    D3D11_FEATURE_DATA_D3D11_OPTIONS options;memset(&options,0,sizeof(options));
    if(FAILED(ID3D11Device_CheckFeatureSupport(device,D3D11_FEATURE_D3D11_OPTIONS,&options,sizeof(options)))||!options.MapNoOverwriteOnDynamicConstantBuffer)goto done;
    BYTE pixels[64];memset(pixels,255,sizeof(pixels));td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D11_SUBRESOURCE_DATA source;memset(&source,0,sizeof(source));source.pSysMem=pixels;source.SysMemPitch=16;source.SysMemSlicePitch=64;
    if(FAILED(ID3D11Device_CreateTexture2D(device,&td,&source,&texture))||FAILED(ID3D11Device_CreateShaderResourceView(device,(ID3D11Resource*)texture,NULL,&input)))goto done;
    const float red[4]={1,0,0,1},green[4]={0,1,0,1};
    discard_ok=mapped_color(context,constants,D3D11_MAP_WRITE_DISCARD,red)&&draw_readback(context,view,target,staging,input,constants,red);
    if(!discard_ok)goto done;
    no_overwrite_ok=mapped_color(context,constants,D3D11_MAP_WRITE_NO_OVERWRITE,green)&&draw_readback(context,view,target,staging,input,constants,green);
    result=no_overwrite_ok;
done:
    printf("{\"schema\":1,\"event\":\"fork-tests\",\"pid\":%lu,\"bcVerified\":[",(unsigned long)pid);
    for(unsigned i=1;i<=bc_count;++i)printf("%s%u",i>1?",":"",i);
    printf("],\"mappedDiscardVerified\":%s,\"mappedNoOverwriteVerified\":%s,\"scope\":\"controlled-pixels-and-mapped-CB-not-complete-game-feature-coverage\"}\n",discard_ok?"true":"false",no_overwrite_ok?"true":"false");
    ID3D11DeviceContext_ClearState(context);
    if(input)ID3D11ShaderResourceView_Release(input);
    if(texture)ID3D11Texture2D_Release(texture);
    if(view)ID3D11RenderTargetView_Release(view);
    if(target)ID3D11Texture2D_Release(target);
    if(staging)ID3D11Texture2D_Release(staging);
    if(constants)ID3D11Buffer_Release(constants);
    if(vertex)ID3D11VertexShader_Release(vertex);
    if(pixel)ID3D11PixelShader_Release(pixel);
    if(raster)ID3D11RasterizerState_Release(raster);
    if(vs_code)vs_code->lpVtbl->Release(vs_code);
    if(ps_code)ps_code->lpVtbl->Release(ps_code);
    if(error)error->lpVtbl->Release(error);
    if(compiler)FreeLibrary(compiler);
    return result;
}

static LRESULT CALLBACK window_proc(HWND h,UINT msg,WPARAM w,LPARAM l){if(msg==WM_DESTROY){PostQuitMessage(0);return 0;}return DefWindowProcW(h,msg,w,l);}
static DWORD WINAPI timeout_thread(void* stop){if(WaitForSingleObject((HANDLE)stop,25000)==WAIT_OBJECT_0)return 0;fputs("{\"schema\":1,\"event\":\"timeout\",\"exitCode\":124}\n",stdout);fflush(stdout);ExitProcess(124);return 0;}
static HMODULE load_d3d11(int app_local) {
    if(!app_local)return LoadLibraryW(L"d3d11.dll");
    wchar_t path[2048];DWORD n=GetModuleFileNameW(NULL,path,2048);
    if(!n||n>=2048)return NULL;
    wchar_t* slash=wcsrchr(path,L'\\');
    if(!slash||slash-path>2000)return NULL;
    wcscpy(slash+1,L"d3d11.dll");return LoadLibraryExW(path,NULL,LOAD_WITH_ALTERED_SEARCH_PATH);
}
typedef HRESULT (WINAPI *CreateDevice)(IDXGIAdapter*,D3D_DRIVER_TYPE,HMODULE,UINT,const D3D_FEATURE_LEVEL*,UINT,UINT,const DXGI_SWAP_CHAIN_DESC*,IDXGISwapChain**,ID3D11Device**,D3D_FEATURE_LEVEL*,ID3D11DeviceContext**);
static int run(unsigned frames,int app_local,D3D_FEATURE_LEVEL requested) {
    int exit_code=1;unsigned accepted=0,verified=0,attempted=0,read_width=0,read_height=0;HMODULE library=load_d3d11(app_local);
    IDXGISwapChain* swap=NULL;ID3D11Device* device=NULL;ID3D11DeviceContext* context=NULL;ID3D11Texture2D* back=NULL;ID3D11Texture2D* readback=NULL;ID3D11RenderTargetView* view=NULL;
    HWND window=NULL;
    if(!library){printf("{\"schema\":1,\"event\":\"load-error\",\"pid\":%lu,\"win32Error\":%lu}\n",(unsigned long)pid,(unsigned long)GetLastError());goto end;}
    CreateDevice create=(CreateDevice)(void*)GetProcAddress(library,"D3D11CreateDeviceAndSwapChain");
    if(!create||!module_record("d3d11.dll",library)){exit_code=11;goto end;}
    WNDCLASSW cls;memset(&cls,0,sizeof(cls));cls.lpfnWndProc=window_proc;cls.hInstance=GetModuleHandleW(NULL);cls.lpszClassName=L"HslD3d11Fixture";
    if(!RegisterClassW(&cls)){exit_code=12;goto end;}
    window=CreateWindowExW(0,cls.lpszClassName,L"VPP GNU D3D11 diagnostic",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,256,192,NULL,NULL,cls.hInstance,NULL);
    if(!window){exit_code=13;goto end;}ShowWindow(window,SW_SHOW);UpdateWindow(window);
    DXGI_SWAP_CHAIN_DESC desc;memset(&desc,0,sizeof(desc));desc.BufferDesc.Width=64;desc.BufferDesc.Height=64;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.OutputWindow=window;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL chosen=0;HRESULT hr=create(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,0,&requested,1,D3D11_SDK_VERSION,&desc,&swap,&device,&chosen,&context);
    printf("{\"schema\":1,\"event\":\"device\",\"pid\":%lu,\"hresult\":%ld,\"featureLevelRequested\":%u,\"featureLevel\":%u}\n",(unsigned long)pid,(long)hr,requested,chosen);
    HMODULE dxgi=GetModuleHandleW(L"dxgi.dll");
    if(dxgi)module_record("dxgi.dll",dxgi);
    HMODULE vk=GetModuleHandleW(L"vulkan-1.dll");
    if(vk)module_record("vulkan-1.dll",vk);
    vk=GetModuleHandleW(L"winevulkan.dll");
    if(vk)module_record("winevulkan.dll",vk);
    if(FAILED(hr)||!swap||!device||!context||!dxgi){exit_code=14;goto end;}
    IDXGIDevice* dxgi_device=NULL;IDXGIAdapter* adapter=NULL;DXGI_ADAPTER_DESC adapter_desc;
    memset(&adapter_desc,0,sizeof(adapter_desc));
    HRESULT identity=ID3D11Device_QueryInterface(device,&IID_IDXGIDevice,(void**)&dxgi_device);
    if(SUCCEEDED(identity))identity=IDXGIDevice_GetAdapter(dxgi_device,&adapter);
    if(SUCCEEDED(identity))identity=IDXGIAdapter_GetDesc(adapter,&adapter_desc);
    if(SUCCEEDED(identity)){
        char description[1024];adapter_desc.Description[127]=0;
        if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,adapter_desc.Description,-1,description,sizeof(description),NULL,NULL))strcpy(description,"encoding-unavailable");
        printf("{\"schema\":1,\"event\":\"selected-adapter\",\"pid\":%lu,\"description\":",(unsigned long)pid);json_string(description);
        char luid[17];const BYTE* luid_bytes=(const BYTE*)&adapter_desc.AdapterLuid;for(unsigned i=0;i<8;++i)snprintf(luid+i*2,3,"%02x",luid_bytes[i]);
        printf(",\"vendorId\":%u,\"deviceId\":%u,\"subSysId\":%u,\"revision\":%u,\"luid\":\"%s\"",adapter_desc.VendorId,adapter_desc.DeviceId,adapter_desc.SubSysId,adapter_desc.Revision,luid);
        if(!adapter_vulkan_identity(adapter))identity=E_FAIL;
        printf(",\"scope\":\"D3D11-selected-DXGI-adapter-with-Vulkan-interop-identity\"}\n");
    }
    if(adapter)IDXGIAdapter_Release(adapter);
    if(dxgi_device)IDXGIDevice_Release(dxgi_device);
    if(FAILED(identity)){exit_code=23;goto end;}
    if(!fork_tests(device,context)){exit_code=24;goto end;}
    for(unsigned frame=0;frame<frames;++frame){
        MSG msg;while(PeekMessageW(&msg,NULL,0,0,PM_REMOVE)){if(msg.message==WM_QUIT){exit_code=15;goto end;}TranslateMessage(&msg);DispatchMessageW(&msg);}
        if(frame==frames/2&&frame){
            if(view){ID3D11RenderTargetView_Release(view);view=NULL;}if(back){ID3D11Texture2D_Release(back);back=NULL;}if(readback){ID3D11Texture2D_Release(readback);readback=NULL;}
            hr=IDXGISwapChain_ResizeBuffers(swap,2,96,64,DXGI_FORMAT_R8G8B8A8_UNORM,0);
            printf("{\"schema\":1,\"event\":\"resize\",\"pid\":%lu,\"hresult\":%ld,\"width\":96,\"height\":64}\n",(unsigned long)pid,(long)hr);
            if(FAILED(hr)){exit_code=16;goto end;}
        }
        if(!back){
            hr=IDXGISwapChain_GetBuffer(swap,0,&IID_ID3D11Texture2D,(void**)&back);
    if(FAILED(hr)){exit_code=17;goto end;}
            hr=ID3D11Device_CreateRenderTargetView(device,(ID3D11Resource*)back,NULL,&view);
    if(FAILED(hr)){exit_code=18;goto end;}
            D3D11_TEXTURE2D_DESC texture;ID3D11Texture2D_GetDesc(back,&texture);read_width=texture.Width;read_height=texture.Height;texture.Usage=D3D11_USAGE_STAGING;texture.BindFlags=0;texture.CPUAccessFlags=D3D11_CPU_ACCESS_READ;texture.MiscFlags=0;
            hr=ID3D11Device_CreateTexture2D(device,&texture,NULL,&readback);
    if(FAILED(hr)){exit_code=19;goto end;}
        }
        const float color[4]={frame&1?0.0f:1.0f,frame&1?1.0f:0.0f,0.0f,1.0f};
        ID3D11DeviceContext_ClearRenderTargetView(context,view,color);
        ID3D11DeviceContext_CopyResource(context,(ID3D11Resource*)readback,(ID3D11Resource*)back);
        D3D11_MAPPED_SUBRESOURCE map;hr=ID3D11DeviceContext_Map(context,(ID3D11Resource*)readback,0,D3D11_MAP_READ,0,&map);
        if(FAILED(hr)){exit_code=20;goto end;}
        int correct=map.pData&&read_width<=96&&read_height<=64&&map.RowPitch>=read_width*4&&map.RowPitch<=65536;
        for(unsigned y=0;correct&&y<read_height;++y)for(unsigned x=0;x<read_width;++x){
            const BYTE* pixel=(const BYTE*)map.pData+(size_t)y*map.RowPitch+x*4;
            if(pixel[0]!=(frame&1?0:255)||pixel[1]!=(frame&1?255:0)||pixel[2]!=0||pixel[3]!=255){correct=0;break;}
        }
        ID3D11DeviceContext_Unmap(context,(ID3D11Resource*)readback,0);
    if(!correct){exit_code=21;goto end;}++verified;
        ++attempted;hr=IDXGISwapChain_Present(swap,0,0);
        // DXGI_STATUS_OCCLUDED is not an accepted present. Do not count SUCCEEDED alone.
        if(hr!=S_OK){printf("{\"schema\":1,\"event\":\"present-error\",\"pid\":%lu,\"hresult\":%ld}\n",(unsigned long)pid,(long)hr);exit_code=22;goto end;}++accepted;
        Sleep(8);
    }
    exit_code=0;
end:
    printf("{\"schema\":1,\"event\":\"present\",\"pid\":%lu,\"attempted\":%u,\"accepted\":%u,\"readbackVerified\":%u}\n",(unsigned long)pid,attempted,accepted,verified);
    if(context)ID3D11DeviceContext_ClearState(context);
    if(view)ID3D11RenderTargetView_Release(view);
    if(back)ID3D11Texture2D_Release(back);
    if(readback)ID3D11Texture2D_Release(readback);
    if(swap)IDXGISwapChain_Release(swap);
    if(context)ID3D11DeviceContext_Release(context);
    if(device)ID3D11Device_Release(device);
    if(window)DestroyWindow(window);
    if(library)FreeLibrary(library);
    printf("{\"schema\":1,\"event\":\"end\",\"pid\":%lu,\"exitCode\":%d,\"frames\":%u,\"renderVerified\":%s,\"displayFpsMeasured\":false,\"diagnosticReadback\":true}\n",(unsigned long)pid,exit_code,accepted,exit_code?"false":"true");
    return exit_code;
}
int main(int argc,char** argv){
    unsigned frames=60;int app_local=0,spawn=0,child=0;D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_10_0;
    for(int i=1;i<argc;++i){
        if(!strcmp(argv[i],"--frames")&&i+1<argc){char* end=NULL;unsigned long n=strtoul(argv[++i],&end,10);
    if(!*argv[i]||*end||n<2||n>240)return 2;frames=(unsigned)n;}
        else if(!strcmp(argv[i],"--loader")&&i+1<argc){const char* v=argv[++i];
    if(!strcmp(v,"app-local"))app_local=1;else if(strcmp(v,"prefix"))return 2;}
        else if(!strcmp(argv[i],"--feature-level")&&i+1<argc){const char* v=argv[++i];
    if(!strcmp(v,"11_0"))level=D3D_FEATURE_LEVEL_11_0;else if(strcmp(v,"10_0"))return 2;}
        else if(!strcmp(argv[i],"--spawn-child"))spawn=1;else if(!strcmp(argv[i],"--child"))child=1;else return 2;
    }
    setvbuf(stdout,NULL,_IONBF,0);pid=GetCurrentProcessId();HANDLE stop=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!stop)return 3;HANDLE timeout=CreateThread(NULL,0,timeout_thread,stop,0,NULL);
    if(!timeout){CloseHandle(stop);return 3;}
    printf("{\"schema\":1,\"event\":\"begin\",\"contractVersion\":2,\"pid\":%lu,\"arch\":\"%s\",\"loaderMode\":\"%s\",\"child\":%s}\n",(unsigned long)pid,sizeof(void*)==8?"x86_64":"i386",app_local?"app-local":"prefix",child?"true":"false");
    profile_record();
    // Qualify dynamic loads independently of the statically imported closure.
    const char* dynamic[]={"concrt140.dll","mfplat.dll","winhttp.dll","mfreadwrite.dll"};
    int complete=1;
    for(unsigned i=0;i<sizeof(dynamic)/sizeof(dynamic[0]);++i){
        HMODULE module=LoadLibraryA(dynamic[i]);
        if(!module||!module_record(dynamic[i],module))complete=0;
        if(module)FreeLibrary(module);
    }
    int result=complete?run(frames,app_local,level):40;
    SetEvent(stop);WaitForSingleObject(timeout,INFINITE);CloseHandle(timeout);CloseHandle(stop);
    if(!result&&spawn&&!child){
        wchar_t path[2048],command[8192];DWORD n=GetModuleFileNameW(NULL,path,2048);
    if(!n||n>=2048)return 30;
        int count=swprintf(command,8192,L"\"%ls\" --frames %u --loader %ls --feature-level %ls --child",path,frames,app_local?L"app-local":L"prefix",level==D3D_FEATURE_LEVEL_11_0?L"11_0":L"10_0");
    if(count<0||count>=8192)return 31;
        STARTUPINFOW startup;PROCESS_INFORMATION process;memset(&startup,0,sizeof(startup));memset(&process,0,sizeof(process));startup.cb=sizeof(startup);
        if(!CreateProcessW(path,command,NULL,NULL,TRUE,0,NULL,NULL,&startup,&process)){printf("{\"schema\":1,\"event\":\"child-error\",\"win32Error\":%lu}\n",(unsigned long)GetLastError());return 32;}
        DWORD wait=WaitForSingleObject(process.hProcess,20000),code=125;
        if(wait==WAIT_OBJECT_0)GetExitCodeProcess(process.hProcess,&code);else code=124;
        if(wait==WAIT_OBJECT_0)printf("{\"schema\":1,\"event\":\"child\",\"pid\":%lu,\"childPid\":%lu,\"exitCode\":%lu,\"waitResult\":%lu,\"childStillRunning\":false}\n",(unsigned long)pid,(unsigned long)process.dwProcessId,(unsigned long)code,(unsigned long)wait);
        else printf("{\"schema\":1,\"event\":\"child\",\"pid\":%lu,\"childPid\":%lu,\"exitCode\":null,\"waitResult\":%lu,\"childStillRunning\":true}\n",(unsigned long)pid,(unsigned long)process.dwProcessId,(unsigned long)wait);
        CloseHandle(process.hThread);CloseHandle(process.hProcess);result=(int)code;
    }
    printf("{\"schema\":1,\"event\":\"process-exit\",\"pid\":%lu,\"exitCode\":%d}\n",(unsigned long)pid,result);
    return result;
}
