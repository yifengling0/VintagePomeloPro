#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include <stdint.h>
static volatile LONG captureStop;
typedef WCHAR* (__cdecl *dos_fn)(const char*);
static DWORD WINAPI CaptureLogs(void*) {
 char path[MAX_PATH], dest[MAX_PATH], wine[160];
 GetModuleFileNameA(NULL,path,sizeof(path)); char* end=strrchr(path,'\\');
 if(!end)return 0; *(end+1)=0;
 SYSTEMTIME time;GetLocalTime(&time);
 snprintf(wine,sizeof(wine),"/data/storage/el2/base/files/wine-logs/wine_stderr_%04u%02u%02u.log",time.wYear,time.wMonth,time.wDay);
 const char* inputs[]={"/data/storage/el2/base/cache/winehua_virgl_host.log",wine};
 const char* outputs[]={"backend-host-trace.txt","backend-wine-trace.txt"};
 dos_fn dos=(dos_fn)GetProcAddress(GetModuleHandleA("kernel32.dll"),"wine_get_dos_file_name");
 long long initial[2]={-1,-1};
 for(unsigned step=0;step<300&&!captureStop;step++) {
  for(unsigned i=0;i<2;i++) {
   WCHAR* name=dos?dos(inputs[i]):NULL;FILE* input=name?_wfopen(name,L"rb"):NULL;
   if(name)HeapFree(GetProcessHeap(),0,name);
   if(!input)continue;
   _fseeki64(input,0,SEEK_END);long long size=_ftelli64(input);
   if(initial[i]<0)initial[i]=size;
   long long begin=initial[i];if(size<begin)begin=0;
   if(size-begin>2*1024*1024)begin=size-2*1024*1024;
   _fseeki64(input,begin,SEEK_SET);
   snprintf(dest,sizeof(dest),"%s%s",path,outputs[i]);FILE* output=fopen(dest,"wb");
   if(output){fprintf(output,"capture pid=%lu step=%u input=%s initial=%lld begin=%lld size=%lld\n",GetCurrentProcessId(),step,inputs[i],initial[i],begin,size);
    char buffer[8192];long long left=size-begin;
    while(left>0){size_t n=fread(buffer,1,left>8192?8192:(size_t)left,input);if(!n)break;fwrite(buffer,1,n,output);left-=n;}
    fclose(output);
   }fclose(input);
  }Sleep(40);
 }return 0;
}

static FILE *out;
static void result(const char* stage,HRESULT hr){fprintf(out,"%s=0x%08lx\n",stage,(unsigned long)hr);fflush(out);}
static LONG CALLBACK fault(EXCEPTION_POINTERS* p) {
 fprintf(out,"exception=%08lx eip=%08lx esp=%08lx address=%p\n",
  p->ExceptionRecord->ExceptionCode,p->ContextRecord->Eip,p->ContextRecord->Esp,
  p->ExceptionRecord->ExceptionAddress);fflush(out);return EXCEPTION_CONTINUE_SEARCH;
}
int main(int argc,char** argv){
 char path[MAX_PATH];GetModuleFileNameA(nullptr,path,sizeof(path));char* end=strrchr(path,'\\');if(!end)return 1;strcpy(end+1,"backend-draw-probe.txt");out=fopen(path,"wb");if(!out)return 2;
 if(GetEnvironmentVariableA("WINEHUA_PROBE_DUMP_SPIRV",NULL,0)) {
  char folder[MAX_PATH];GetModuleFileNameA(NULL,folder,sizeof(folder));char* sep=strrchr(folder,'\\');
  if(!sep)return 20;strcpy(sep+1,"backend-draw-shaders");
  if(!CreateDirectoryA(folder,NULL)&&GetLastError()!=ERROR_ALREADY_EXISTS){result("dumpDirectory",GetLastError());return 21;}
  SetEnvironmentVariableA("DXVK_SHADER_DUMP_PATH",folder);
  SetEnvironmentVariableA("DXVK_WINEHUA_DUMP_REMAPPED_SPIRV","1");
  fprintf(out,"dumpDirectory=%s\n",folder);fflush(out);
 }
 HANDLE capture=CreateThread(NULL,0,CaptureLogs,NULL,0,NULL);Sleep(80);
 AddVectoredExceptionHandler(1,fault);
 HMODULE dll=LoadLibraryA("d3d9.dll");if(!dll){result("load",GetLastError());return 3;}
 char mod[MAX_PATH];GetModuleFileNameA(dll,mod,sizeof(mod));fprintf(out,"module=%s\n",mod);fflush(out);
 auto create=(IDirect3D9* (WINAPI*)(UINT))GetProcAddress(dll,"Direct3DCreate9");IDirect3D9* api=create(D3D_SDK_VERSION);if(!api){result("createApi",E_FAIL);return 4;}
 HWND win=CreateWindowA("STATIC","Backend D3D9 probe",WS_OVERLAPPEDWINDOW|WS_VISIBLE,0,0,128,128,nullptr,nullptr,GetModuleHandleA(nullptr),nullptr);
 D3DPRESENT_PARAMETERS pp={};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=win;pp.BackBufferWidth=64;pp.BackBufferHeight=64;pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
 IDirect3DDevice9* dev=nullptr;HRESULT hr=api->CreateDevice(0,D3DDEVTYPE_HAL,win,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&dev);result("createDevice",hr);if(FAILED(hr))return 5;


 const D3DFORMAT ati1=(D3DFORMAT)MAKEFOURCC('A','T','I','1'),ati2=(D3DFORMAT)MAKEFOURCC('A','T','I','2');
 bool bottom=argc>1&&!strcmp(argv[1],"bottom");
 for(D3DFORMAT fmt:{ati1,ati2,D3DFMT_DXT1,D3DFMT_A8R8G8B8}){
  IDirect3DTexture9 *src=nullptr,*tex=nullptr;
  hr=dev->CreateTexture(16,16,5,0,fmt,D3DPOOL_SYSTEMMEM,&src,nullptr);result("createSource",hr);if(FAILED(hr))return 6;
  unsigned dim=bottom?4:16;
  hr=dev->CreateTexture(dim,dim,3,0,fmt,D3DPOOL_DEFAULT,&tex,nullptr);result("createDestination",hr);if(FAILED(hr))return 7;
  fprintf(out,"CASE format=%lu bottom=%u source=16/5 destination=%u/3\n",(unsigned long)fmt,bottom,dim);fflush(out);
  const unsigned colors[]={0xffff0000,0xff00ff00,0xff0000ff,0xffffffff,0xffff0000};
  for(unsigned mip=0;mip<5;mip++){
   D3DLOCKED_RECT mapped={};hr=src->LockRect(mip,&mapped,nullptr,0);result("sourceLock",hr);if(FAILED(hr))return 8;
   unsigned size=16>>mip;
   if(fmt==D3DFMT_A8R8G8B8){for(unsigned y=0;y<size;y++)for(unsigned x=0;x<size;x++)*(unsigned*)((char*)mapped.pBits+y*mapped.Pitch+4*x)=colors[mip];}
   else{
    unsigned blocks=(size+3)/4,bytes=fmt==ati2?16:8;
    // ATI formats expose the legacy driver-hack pitch. The data remains BC packed.
    unsigned pitch=blocks*bytes;
    for(unsigned y=0;y<blocks;y++)for(unsigned x=0;x<blocks;x++){
     auto b=(unsigned char*)mapped.pBits+y*pitch+x*bytes;memset(b,0,bytes);
     if(fmt==D3DFMT_DXT1){unsigned short c=mip%4==0?0xf800:(mip%4==1?0x07e0:(mip%4==2?0x001f:0xffff));memcpy(b,&c,2);}
     else {b[0]=b[1]=(mip+1)*32;if(fmt==ati2)b[8]=b[9]=255-(mip+1)*32;}
    }
   }
   hr=src->UnlockRect(mip);result("sourceUnlock",hr);if(FAILED(hr))return 9;
  }
  hr=dev->UpdateTexture(src,tex);result("updateTexture",hr);
  if(FAILED(hr)){fprintf(out,"UPDATE_FAILED\n");fflush(out);return 10;}
  dev->SetTexture(0,tex);dev->SetRenderState(D3DRS_LIGHTING,FALSE);dev->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
  dev->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT);dev->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT);dev->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_POINT);
  float bias=-16;DWORD biasBits;memcpy(&biasBits,&bias,4);dev->SetSamplerState(0,D3DSAMP_MIPMAPLODBIAS,biasBits);
  dev->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);dev->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);
  dev->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);dev->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
  struct V{float x,y,z,w,u,v;};V vertices[]={{-.5f,-.5f,0,1,0,0},{63.5f,-.5f,0,1,1,0},{-.5f,63.5f,0,1,0,1},{63.5f,63.5f,0,1,1,1}};
  dev->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);
  for(unsigned mip=0;mip<3;mip++){
   dev->SetSamplerState(0,D3DSAMP_MAXMIPLEVEL,mip);
   unsigned sourceMip=mip+(bottom?2:0),value=(sourceMip+1)*32;
   unsigned expected=fmt==ati1?value<<16:(fmt==ati2?((255-value)<<16)|(value<<8)|255:colors[sourceMip]&0xffffff);
   dev->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0);dev->BeginScene();hr=dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(V));dev->EndScene();result("draw",hr);if(FAILED(hr))return 11;
   IDirect3DSurface9 *back=nullptr,*read=nullptr;
   hr=dev->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back);if(FAILED(hr))return 12;
   hr=dev->CreateOffscreenPlainSurface(64,64,D3DFMT_X8R8G8B8,D3DPOOL_SYSTEMMEM,&read,nullptr);if(FAILED(hr))return 13;
   hr=dev->GetRenderTargetData(back,read);result("readback",hr);if(FAILED(hr))return 14;
   D3DLOCKED_RECT mapped={};hr=read->LockRect(&mapped,nullptr,D3DLOCK_READONLY);result("readLock",hr);if(FAILED(hr))return 15;
   unsigned pixel=*(unsigned*)((char*)mapped.pBits+32*mapped.Pitch+128);read->UnlockRect();
   bool pass=(pixel&0xffffff)==expected;
   fprintf(out,"SAMPLE mip=%u sourceMip=%u pixel=%08x expected=%06x pass=%u\n",mip,sourceMip,pixel,expected,pass);fflush(out);
   read->Release();back->Release();if(!pass){fprintf(out,"PIXEL_FAIL\n");fflush(out);return 16;}
   hr=dev->Present(nullptr,nullptr,nullptr,nullptr);result("present",hr);if(FAILED(hr))return 17;
  }
  dev->SetTexture(0,nullptr);src->Release();tex->Release();
 }
 fprintf(out,"PASS\n");captureStop=1;if(capture){WaitForSingleObject(capture,3000);CloseHandle(capture);}fclose(out);dev->Release();api->Release();DestroyWindow(win);return 0;
}
