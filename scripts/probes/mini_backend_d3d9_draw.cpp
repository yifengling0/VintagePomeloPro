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

 bool vsConstant=argc>1&&!strcmp(argv[1],"vs-constant");
 bool psConstant=argc>1&&!strcmp(argv[1],"ps-constant");
 bool fog=argc>1&&!strcmp(argv[1],"fog");
 bool alpha=argc>1&&(!strcmp(argv[1],"alpha")||!strcmp(argv[1],"alpha-pass"));
 unsigned expected=fog?0x00ff00:(alpha&&!strcmp(argv[1],"alpha")?0:0xff0000);
 bool shaderBC=argc>1&&!strcmp(argv[1],"shader-bc");
 bool customVS=argc>1&&(!strcmp(argv[1],"shader")||!strcmp(argv[1],"vertex")||shaderBC||fog||alpha||vsConstant||psConstant);
 bool customPS=argc>1&&(!strcmp(argv[1],"shader")||!strcmp(argv[1],"pixel")||shaderBC||fog||alpha||vsConstant||psConstant);
 bool rgba=!shaderBC&&((argc>1&&!strcmp(argv[1],"rgba"))||customVS||customPS);
 if(customVS||customPS){
  HMODULE compiler=LoadLibraryA("d3dcompiler_47.dll");
  typedef HRESULT(WINAPI* Assemble)(LPCVOID,SIZE_T,LPCSTR,const D3D_SHADER_MACRO*,ID3DInclude*,UINT,ID3DBlob**,ID3DBlob**);
  Assemble assemble=compiler?(Assemble)GetProcAddress(compiler,"D3DAssemble"):NULL;
  if(!assemble){result("assembler",GetLastError());return 22;}
  const char* vs="vs_2_0\ndcl_position v0\ndcl_texcoord v1\nmov oPos, v0\nmov oT0, v1\n";
  const char* ps="ps_2_0\ndcl t0.xy\ndcl_2d s0\ntexld r0, t0, s0\nmov oC0, r0\n";
  if(vsConstant)vs="vs_2_0\ndcl_position v0\ndcl_texcoord v1\nmul oPos, v0, c0\nmov oT0, v1\n";
  if(psConstant)ps="ps_2_0\ndcl t0.xy\ndcl_2d s0\ntexld r0, t0, s0\nmul oC0, r0, c0\n";
  const float ones[]={1,1,1,1};
  if(vsConstant){hr=dev->SetVertexShaderConstantF(0,ones,1);result("setVSConstant",hr);if(FAILED(hr))return 26;}
  if(psConstant){hr=dev->SetPixelShaderConstantF(0,ones,1);result("setPSConstant",hr);if(FAILED(hr))return 27;}
  for(unsigned i=0;i<2;i++)if(i?customPS:customVS){
   const char* s=i?ps:vs;ID3DBlob *blob=NULL,*errors=NULL;
   hr=assemble(s,strlen(s),NULL,NULL,NULL,0,&blob,&errors);result(i?"assemblePS":"assembleVS",hr);
   if(errors){fprintf(out,"shaderError=%s\n",(char*)errors->GetBufferPointer());errors->Release();fflush(out);}
   if(FAILED(hr))return 23;
   if(i){IDirect3DPixelShader9* sh=NULL;hr=dev->CreatePixelShader((DWORD*)blob->GetBufferPointer(),&sh);result("createPS",hr);if(SUCCEEDED(hr)){dev->SetPixelShader(sh);sh->Release();}}
   else{IDirect3DVertexShader9* sh=NULL;hr=dev->CreateVertexShader((DWORD*)blob->GetBufferPointer(),&sh);result("createVS",hr);if(SUCCEEDED(hr)){dev->SetVertexShader(sh);sh->Release();}}
   blob->Release();if(FAILED(hr))return 24;
  }
 }

 if(argc>1&&!rgba&&!customVS&&!customPS){
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
  fprintf(out,"pixel=%08x pass=%u\n",pixel,(pixel&0xffffff)==expected);fflush(out);
  read->UnlockRect();fprintf(out,"%s\n",(pixel&0xffffff)==expected?"PASS":"PIXEL_FAIL");fclose(out);return 0;
 }
 std::initializer_list<D3DFORMAT> formats=rgba?std::initializer_list<D3DFORMAT>{D3DFMT_A8R8G8B8}:std::initializer_list<D3DFORMAT>{D3DFMT_DXT1,D3DFMT_DXT3,D3DFMT_DXT5};
 for(D3DFORMAT fmt:formats){
  fprintf(out,"format=%lu\n",(unsigned long)fmt);fflush(out);
  IDirect3DTexture9* tex=nullptr;hr=dev->CreateTexture(8,8,4,0,fmt,D3DPOOL_MANAGED,&tex,nullptr);result("createTexture",hr);if(FAILED(hr))return 6;
  unsigned blockSize=fmt==D3DFMT_DXT1?8:16;
  for(unsigned mip=0;mip<4;mip++){
   D3DLOCKED_RECT lock={};hr=tex->LockRect(mip,&lock,nullptr,0);result("lock",hr);if(FAILED(hr))return 7;
   unsigned size=8>>mip;if(!size)size=1;
   if(rgba){for(unsigned y=0;y<size;y++)for(unsigned x=0;x<size;x++)*(unsigned*)((unsigned char*)lock.pBits+y*lock.Pitch+x*4)=alpha?0x80ff0000:0xffff0000;}
   else for(unsigned y=0;y<(size+3)/4;y++)for(unsigned x=0;x<(size+3)/4;x++){
    unsigned char* block=(unsigned char*)lock.pBits+y*lock.Pitch+x*blockSize;memset(block,0,blockSize);unsigned char* color=block+(blockSize-8);color[0]=0x00;color[1]=0xf8;
    if(fmt==D3DFMT_DXT3)memset(block,0xff,8);if(fmt==D3DFMT_DXT5){block[0]=255;block[1]=255;}
   }tex->UnlockRect(mip);
  }
  dev->SetTexture(0,tex);dev->SetRenderState(D3DRS_LIGHTING,FALSE);dev->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);dev->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT);dev->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
  dev->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);dev->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);dev->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);dev->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
  struct V{float x,y,z,w,u,v;};V vertices[]={{-.5f,-.5f,0,1,0,0},{63.5f,-.5f,0,1,1,0},{-.5f,63.5f,0,1,0,1},{63.5f,63.5f,0,1,1,1}};
  dev->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);
  if(customVS){
   D3DVERTEXELEMENT9 elements[]={{0,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},{0,16,D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,0},D3DDECL_END()};
   IDirect3DVertexDeclaration9* declaration=NULL;hr=dev->CreateVertexDeclaration(elements,&declaration);result("declaration",hr);if(FAILED(hr))return 25;
   dev->SetVertexDeclaration(declaration);declaration->Release();
   for(unsigned i=0;i<4;i++){vertices[i].x=(i&1)?1.0f:-1.0f;vertices[i].y=(i&2)?-1.0f:1.0f;}
  }
  if(fog){float start=-1.0f,end=0.0f;DWORD a,b;memcpy(&a,&start,4);memcpy(&b,&end,4);
   dev->SetRenderState(D3DRS_FOGENABLE,TRUE);dev->SetRenderState(D3DRS_FOGCOLOR,0xff00ff00);
   dev->SetRenderState(D3DRS_FOGTABLEMODE,D3DFOG_LINEAR);dev->SetRenderState(D3DRS_FOGSTART,a);dev->SetRenderState(D3DRS_FOGEND,b);
  }
  if(alpha){dev->SetRenderState(D3DRS_ALPHATESTENABLE,TRUE);dev->SetRenderState(D3DRS_ALPHAFUNC,D3DCMP_GREATER);dev->SetRenderState(D3DRS_ALPHAREF,!strcmp(argv[1],"alpha")?192:64);}
  dev->Clear(0,nullptr,D3DCLEAR_TARGET,0xff000000,1,0);dev->BeginScene();hr=dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(V));dev->EndScene();result("draw",hr);
  IDirect3DSurface9 *back=nullptr,*read=nullptr;dev->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back);dev->CreateOffscreenPlainSurface(64,64,D3DFMT_X8R8G8B8,D3DPOOL_SYSTEMMEM,&read,nullptr);hr=dev->GetRenderTargetData(back,read);result("readback",hr);if(FAILED(hr))return 8;
  fprintf(out,"enter_readLock tick=%lu\n",GetTickCount());fflush(out);D3DLOCKED_RECT lock={};hr=read->LockRect(&lock,nullptr,D3DLOCK_READONLY);result("readLock",hr);fprintf(out,"bits=%p pitch=%d\n",lock.pBits,(int)lock.Pitch);fflush(out);if(FAILED(hr)||!lock.pBits)return 9;unsigned pixel=*(unsigned*)((unsigned char*)lock.pBits+32*lock.Pitch+32*4);read->UnlockRect();fprintf(out,"pixel=%08x pass=%u\n",pixel,(pixel&0xffffff)==expected);fflush(out);if((pixel&0xffffff)!=expected)return 10;
  read->Release();back->Release();hr=dev->Present(nullptr,nullptr,nullptr,nullptr);result("present",hr);tex->Release();if(FAILED(hr))return 11;
 }
 fprintf(out,"PASS\n");captureStop=1;if(capture){WaitForSingleObject(capture,3000);CloseHandle(capture);}fclose(out);dev->Release();api->Release();DestroyWindow(win);return 0;
}
