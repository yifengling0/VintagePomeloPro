#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <cstddef>
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


static FILE* out;
static bool ok(const char* name,HRESULT hr){fprintf(out,"%s=0x%08lx\n",name,(unsigned long)hr);fflush(out);return SUCCEEDED(hr);}
struct V {float x,y,z,w,r,g,b,a;};
static void quad(V* v,float left,float right,unsigned color){
 for(unsigned i=0;i<4;i++)v[i]={(i&1)?right:left,(i&2)?-1.0f:1.0f,0,1,
  float((color>>16)&255)/255,float((color>>8)&255)/255,float(color&255)/255,1};
}
static bool sample(IDirect3DDevice9* dev,const char* stage,unsigned left,unsigned right){
 IDirect3DSurface9 *back=nullptr,*read=nullptr;
 if(!ok("getBack",dev->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back)))return false;
 if(!ok("createRead",dev->CreateOffscreenPlainSurface(64,64,D3DFMT_X8R8G8B8,D3DPOOL_SYSTEMMEM,&read,nullptr)))return false;
 if(!ok("readback",dev->GetRenderTargetData(back,read)))return false;
 D3DLOCKED_RECT m={};if(!ok("readLock",read->LockRect(&m,nullptr,D3DLOCK_READONLY))||!m.pBits)return false;
 unsigned a=*(unsigned*)((char*)m.pBits+32*m.Pitch+16*4),b=*(unsigned*)((char*)m.pBits+32*m.Pitch+48*4);
 bool pass=(a&0xffffff)==left&&(b&0xffffff)==right;
 fprintf(out,"stage=%s left=%08x right=%08x pass=%u\n",stage,a,b,pass);fflush(out);
 read->UnlockRect();read->Release();back->Release();
 return pass&&ok("present",dev->Present(nullptr,nullptr,nullptr,nullptr));
}
int main(int argc,char** argv){
 char path[MAX_PATH];GetModuleFileNameA(nullptr,path,sizeof(path));char* sep=strrchr(path,'\\');if(!sep)return 1;
 strcpy(sep+1,"backend-draw-probe.txt");out=fopen(path,"wb");if(!out)return 2;
 HANDLE capture=CreateThread(nullptr,0,CaptureLogs,nullptr,0,nullptr);Sleep(80);
 const char* mode=argc>1?argv[1]:"managed";
 const bool managed=!strcmp(mode,"managed"),staged=!strcmp(mode,"staged");
 const bool indexed=!strcmp(mode,"indexed"),up=!strcmp(mode,"indexed-up");
 const bool dynamic=!managed&&!staged&&!up;
 fprintf(out,"mode=%s managed=%u staged=%u indexed=%u dynamic=%u\n",mode,managed,staged,indexed,dynamic);fflush(out);
 HMODULE dll=LoadLibraryA("d3d9.dll");if(!dll)return 3;
 auto create=(IDirect3D9*(WINAPI*)(UINT))GetProcAddress(dll,"Direct3DCreate9");if(!create)return 4;
 IDirect3D9* api=create(D3D_SDK_VERSION);if(!api)return 5;
 HWND win=CreateWindowA("STATIC","Backend D3D9 buffer probe",WS_VISIBLE|WS_OVERLAPPEDWINDOW,0,0,128,128,nullptr,nullptr,GetModuleHandleA(nullptr),nullptr);
 D3DPRESENT_PARAMETERS pp={};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=win;
 pp.BackBufferWidth=64;pp.BackBufferHeight=64;pp.BackBufferFormat=D3DFMT_X8R8G8B8;pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
 IDirect3DDevice9* dev=nullptr;
 if(!ok("createDevice",api->CreateDevice(0,D3DDEVTYPE_HAL,win,D3DCREATE_HARDWARE_VERTEXPROCESSING,&pp,&dev)))return 6;
 HMODULE compiler=LoadLibraryA("d3dcompiler_47.dll");
 using Assemble=HRESULT(WINAPI*)(LPCVOID,SIZE_T,LPCSTR,const D3D_SHADER_MACRO*,ID3DInclude*,UINT,ID3DBlob**,ID3DBlob**);
 auto assemble=compiler?(Assemble)GetProcAddress(compiler,"D3DAssemble"):nullptr;if(!assemble)return 7;
 const char* programs[]={"vs_2_0\ndcl_position v0\ndcl_color v1\nmov oPos, v0\nmov oD0, v1\n","ps_2_0\ndcl v0\nmov oC0, v0\n"};
 for(unsigned i=0;i<2;i++){
  ID3DBlob *code=nullptr,*errors=nullptr;
  HRESULT hr=assemble(programs[i],strlen(programs[i]),nullptr,nullptr,nullptr,0,&code,&errors);
  if(errors){fprintf(out,"shaderError=%s\n",(char*)errors->GetBufferPointer());errors->Release();}
  if(!ok("assemble",hr))return 8;
  if(i){IDirect3DPixelShader9* sh=nullptr;hr=dev->CreatePixelShader((DWORD*)code->GetBufferPointer(),&sh);if(SUCCEEDED(hr)){hr=dev->SetPixelShader(sh);sh->Release();}}
  else {IDirect3DVertexShader9* sh=nullptr;hr=dev->CreateVertexShader((DWORD*)code->GetBufferPointer(),&sh);if(SUCCEEDED(hr)){hr=dev->SetVertexShader(sh);sh->Release();}}
  code->Release();if(!ok("shader",hr))return 9;
 }
 D3DVERTEXELEMENT9 elements[]={{0,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},{0,16,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_COLOR,0},D3DDECL_END()};
 IDirect3DVertexDeclaration9* decl=nullptr;if(!ok("declaration",dev->CreateVertexDeclaration(elements,&decl)))return 10;
 dev->SetVertexDeclaration(decl);decl->Release();dev->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);dev->SetRenderState(D3DRS_LIGHTING,FALSE);
 const WORD indices[]={0,1,2,2,1,3};V initial[8];quad(initial,-1,1,0xff0000);quad(initial+4,0,1,0x00ff00);
 if(up){
  dev->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0);dev->BeginScene();
  HRESULT hr=dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST,0,4,2,indices,D3DFMT_INDEX16,initial,sizeof(V));dev->EndScene();
  if(!ok("indexedUP",hr)||!sample(dev,"indexed-up",0xff0000,0xff0000))return 11;
 }else{
  const UINT prefix=64,total=prefix+sizeof(initial);void* mapped=nullptr;
  IDirect3DVertexBuffer9* vb=nullptr;IDirect3DIndexBuffer9* ib=nullptr;
  if(!ok("createVB",dev->CreateVertexBuffer(total,dynamic?(D3DUSAGE_DYNAMIC|D3DUSAGE_WRITEONLY):0,0,managed?D3DPOOL_MANAGED:D3DPOOL_DEFAULT,&vb,nullptr)))return 12;
  if(!ok("lockVB",vb->Lock(0,total,&mapped,dynamic?D3DLOCK_DISCARD:0)))return 13;
  memset(mapped,0,total);memcpy((char*)mapped+prefix,initial,sizeof(initial));if(!ok("unlockVB",vb->Unlock()))return 14;
  if(indexed){
   if(!ok("createIB",dev->CreateIndexBuffer(64+2*sizeof(indices),D3DUSAGE_DYNAMIC|D3DUSAGE_WRITEONLY,D3DFMT_INDEX16,D3DPOOL_DEFAULT,&ib,nullptr)))return 15;
   if(!ok("lockIB",ib->Lock(0,64+2*sizeof(indices),&mapped,D3DLOCK_DISCARD)))return 16;
   memset(mapped,0,64+2*sizeof(indices));memcpy((char*)mapped+64,indices,sizeof(indices));if(!ok("unlockIB",ib->Unlock()))return 17;
   dev->SetIndices(ib);
  }
  auto draw=[&](unsigned vbOffset,unsigned firstIndex=32){
   if(!ok("stream",dev->SetStreamSource(0,vb,vbOffset,sizeof(V))))return false;
   return ok("draw",indexed?dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,4,firstIndex,2):dev->DrawPrimitive(D3DPT_TRIANGLESTRIP,0,2));
  };
  auto clear=[&](){return ok("clear",dev->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0));};
  clear();dev->BeginScene();bool drawn=draw(prefix);dev->EndScene();
  if(!drawn||!sample(dev,"initial",0xff0000,0xff0000))return 18;
  // Partial non-DISCARD lock must preserve position data and the prefix.
  const UINT colorOffset=prefix+offsetof(V,r),colorBytes=sizeof(V)*3+sizeof(float)*4;
  if(!ok("partialLock",vb->Lock(colorOffset,colorBytes,&mapped,0)))return 19;
  for(unsigned i=0;i<4;i++){float* c=(float*)((char*)mapped+i*sizeof(V));c[0]=0;c[1]=1;c[2]=0;c[3]=1;}
  if(!ok("partialUnlock",vb->Unlock()))return 20;
  clear();dev->BeginScene();drawn=draw(prefix);dev->EndScene();
  if(!drawn||!sample(dev,"partial-preserve",0x00ff00,0x00ff00))return 21;
  if(dynamic){
   V pair[8];quad(pair,-1,0,0xff0000);quad(pair+4,0,1,0x0000ff);
   if(!ok("discardLock",vb->Lock(0,total,&mapped,D3DLOCK_DISCARD)))return 22;
   memset(mapped,0,total);memcpy((char*)mapped+prefix,pair,sizeof(pair));if(!ok("discardUnlock",vb->Unlock()))return 23;
   clear();dev->BeginScene();drawn=draw(prefix);
   if(!ok("appendLock",vb->Lock(prefix+4*sizeof(V),4*sizeof(V),&mapped,D3DLOCK_NOOVERWRITE)))return 24;
   V right[4];quad(right,0,1,0x00ff00);memcpy(mapped,right,sizeof(right));if(!ok("appendUnlock",vb->Unlock()))return 25;
   drawn=drawn&&draw(prefix+4*sizeof(V));dev->EndScene();
   if(!drawn||!sample(dev,"discard-nooverwrite",0xff0000,0x00ff00))return 26;
  }
  if(indexed){
   // Select an appended index range without overwriting the first draw's indices.
   V blue[4];quad(blue,-1,1,0x0000ff);
   if(!ok("blueLock",vb->Lock(0,total,&mapped,D3DLOCK_DISCARD)))return 27;
   memset(mapped,0,total);memcpy((char*)mapped+prefix,blue,sizeof(blue));vb->Unlock();
   if(!ok("appendIB",ib->Lock(64+sizeof(indices),sizeof(indices),&mapped,D3DLOCK_NOOVERWRITE)))return 28;
   memcpy(mapped,indices,sizeof(indices));if(!ok("appendIBUnlock",ib->Unlock()))return 29;
   clear();dev->BeginScene();drawn=draw(prefix,32+6);dev->EndScene();
   if(!drawn||!sample(dev,"indexed-append",0x0000ff,0x0000ff))return 30;
   dev->SetIndices(nullptr);ib->Release();
  }
  dev->SetStreamSource(0,nullptr,0,0);vb->Release();
 }
 fprintf(out,"PASS\n");fflush(out);captureStop=1;
 if(capture){WaitForSingleObject(capture,3000);CloseHandle(capture);}fclose(out);
 dev->Release();api->Release();DestroyWindow(win);return 0;
}
