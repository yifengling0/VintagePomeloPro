#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>
static FILE *out;
static void result(const char* stage,HRESULT hr){fprintf(out,"%s=0x%08lx\n",stage,(unsigned long)hr);fflush(out);}
static LONG CALLBACK fault(EXCEPTION_POINTERS* p) {
 fprintf(out,"exception=%08lx eip=%08lx esp=%08lx address=%p\n",
  p->ExceptionRecord->ExceptionCode,p->ContextRecord->Eip,p->ContextRecord->Esp,
  p->ExceptionRecord->ExceptionAddress);fflush(out);return EXCEPTION_CONTINUE_SEARCH;
}
int main(int argc,char** argv){
 char path[MAX_PATH];GetModuleFileNameA(nullptr,path,sizeof(path));char* end=strrchr(path,'\\');if(!end)return 1;strcpy(end+1,"backend-d3d9-probe.txt");out=fopen(path,"wb");if(!out)return 2;
 AddVectoredExceptionHandler(1,fault);
 HMODULE dll=LoadLibraryA("d3d9.dll");if(!dll){result("load",GetLastError());return 3;}
 char mod[MAX_PATH];GetModuleFileNameA(dll,mod,sizeof(mod));fprintf(out,"module=%s\n",mod);fflush(out);
 auto create=(IDirect3D9* (WINAPI*)(UINT))GetProcAddress(dll,"Direct3DCreate9");IDirect3D9* api=create(D3D_SDK_VERSION);if(!api){result("createApi",E_FAIL);return 4;}
 HWND win=CreateWindowA("STATIC","Backend D3D9 probe",WS_OVERLAPPEDWINDOW|WS_VISIBLE,0,0,128,128,nullptr,nullptr,GetModuleHandleA(nullptr),nullptr);
 D3DPRESENT_PARAMETERS pp={};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=win;pp.BackBufferWidth=64;pp.BackBufferHeight=64;pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
 IDirect3DDevice9* dev=nullptr;HRESULT hr=api->CreateDevice(0,D3DDEVTYPE_HAL,win,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&dev);result("createDevice",hr);if(FAILED(hr))return 5;
 bool rgba=argc>1&&!strcmp(argv[1],"rgba");
 if(argc>1&&!rgba){
  hr=dev->Clear(0,nullptr,D3DCLEAR_TARGET,0xffff0000,1,0);result("clear",hr);
  if(!strcmp(argv[1],"present")) {
   for(unsigned i=0;i<3;++i){hr=dev->Present(nullptr,nullptr,nullptr,nullptr);result("present",hr);Sleep(100);}
   fprintf(out,"PRESENT_COMPLETE (no pixel validation)\n");fflush(out);return FAILED(hr)?12:0;
  }
  IDirect3DSurface9 *back=nullptr,*read=nullptr;
  hr=dev->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back);result("back",hr);if(FAILED(hr))return 13;
  hr=dev->CreateOffscreenPlainSurface(64,64,D3DFMT_X8R8G8B8,D3DPOOL_SYSTEMMEM,&read,nullptr);result("surface",hr);if(FAILED(hr))return 14;
  hr=dev->GetRenderTargetData(back,read);result("readback",hr);if(FAILED(hr))return 15;
  fprintf(out,"enter_readLock\n");fflush(out);
  D3DLOCKED_RECT mapped={};hr=read->LockRect(&mapped,nullptr,D3DLOCK_READONLY);result("readLock",hr);
  if(FAILED(hr)||!mapped.pBits)return 16;
  unsigned pixel=*(unsigned*)((unsigned char*)mapped.pBits+32*mapped.Pitch+128);
  fprintf(out,"pixel=%08x pass=%u\n",pixel,(pixel&0xffffff)==0xff0000);fflush(out);
  read->UnlockRect();fprintf(out,"%s\n",(pixel&0xffffff)==0xff0000?"PASS":"PIXEL_FAIL");fclose(out);return 0;
 }
 std::initializer_list<D3DFORMAT> formats=rgba?std::initializer_list<D3DFORMAT>{D3DFMT_A8R8G8B8}:std::initializer_list<D3DFORMAT>{D3DFMT_DXT1,D3DFMT_DXT3,D3DFMT_DXT5};
 for(D3DFORMAT fmt:formats){
  fprintf(out,"format=%lu\n",(unsigned long)fmt);fflush(out);
  IDirect3DTexture9* tex=nullptr;hr=dev->CreateTexture(8,8,4,0,fmt,D3DPOOL_MANAGED,&tex,nullptr);result("createTexture",hr);if(FAILED(hr))return 6;
  unsigned blockSize=fmt==D3DFMT_DXT1?8:16;
  for(unsigned mip=0;mip<4;mip++){
   D3DLOCKED_RECT lock={};hr=tex->LockRect(mip,&lock,nullptr,0);result("lock",hr);if(FAILED(hr))return 7;
   unsigned size=8>>mip;if(!size)size=1;
   if(rgba){for(unsigned y=0;y<size;y++)for(unsigned x=0;x<size;x++)*(unsigned*)((unsigned char*)lock.pBits+y*lock.Pitch+x*4)=0xffff0000;}
   else for(unsigned y=0;y<(size+3)/4;y++)for(unsigned x=0;x<(size+3)/4;x++){
    unsigned char* block=(unsigned char*)lock.pBits+y*lock.Pitch+x*blockSize;memset(block,0,blockSize);unsigned char* color=block+(blockSize-8);color[0]=0x00;color[1]=0xf8;
    if(fmt==D3DFMT_DXT3)memset(block,0xff,8);if(fmt==D3DFMT_DXT5){block[0]=255;block[1]=255;}
   }tex->UnlockRect(mip);
  }
  dev->SetTexture(0,tex);dev->SetRenderState(D3DRS_LIGHTING,FALSE);dev->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);dev->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT);dev->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
  dev->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);dev->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);dev->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);dev->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
  struct V{float x,y,z,w,u,v;};V vertices[]={{-.5f,-.5f,0,1,0,0},{63.5f,-.5f,0,1,1,0},{-.5f,63.5f,0,1,0,1},{63.5f,63.5f,0,1,1,1}};
  dev->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);dev->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0);dev->BeginScene();hr=dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(V));dev->EndScene();result("draw",hr);
  IDirect3DSurface9 *back=nullptr,*read=nullptr;dev->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back);dev->CreateOffscreenPlainSurface(64,64,D3DFMT_X8R8G8B8,D3DPOOL_SYSTEMMEM,&read,nullptr);hr=dev->GetRenderTargetData(back,read);result("readback",hr);if(FAILED(hr))return 8;
  D3DLOCKED_RECT lock={};hr=read->LockRect(&lock,nullptr,D3DLOCK_READONLY);result("readLock",hr);fprintf(out,"bits=%p pitch=%d\n",lock.pBits,(int)lock.Pitch);fflush(out);if(FAILED(hr)||!lock.pBits)return 9;unsigned pixel=*(unsigned*)((unsigned char*)lock.pBits+32*lock.Pitch+32*4);read->UnlockRect();fprintf(out,"pixel=%08x pass=%u\n",pixel,(pixel&0xffffff)==0xff0000);fflush(out);if((pixel&0xffffff)!=0xff0000)return 10;
  read->Release();back->Release();hr=dev->Present(nullptr,nullptr,nullptr,nullptr);result("present",hr);tex->Release();if(FAILED(hr))return 11;
 }
 fprintf(out,"PASS\n");fclose(out);dev->Release();api->Release();DestroyWindow(win);return 0;
}
