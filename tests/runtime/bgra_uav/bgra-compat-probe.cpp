// End-to-end BGRA UAV semantics probe; no game assets or saves are accessed.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cmath>
#include <stdexcept>
#include <initializer_list>
#include "bgra_probe_shaders.h"
static FILE* resultFile;
static ID3D11Device* dev;
static ID3D11DeviceContext* ctx;
static unsigned passes=0;
static void ok(HRESULT hr,const char* operation) { if(FAILED(hr)) {fprintf(resultFile,"FAIL %s hr=%08lx\n",operation,(unsigned long)hr);fflush(resultFile);throw std::runtime_error(operation);} }
static void check(bool value,const char* operation) {ok(value?S_OK:E_FAIL,operation);fprintf(resultFile,"PASS %s\n",operation);fflush(resultFile);++passes;}
static ID3D11Texture2D* texture(DXGI_FORMAT format=DXGI_FORMAT_B8G8R8A8_TYPELESS,UINT bind=168,UINT size=4,UINT array=1,UINT mips=1) {
 D3D11_TEXTURE2D_DESC d={};d.Width=size;d.Height=size;d.MipLevels=mips;d.ArraySize=array;d.Format=format;d.SampleDesc.Count=1;d.BindFlags=bind;
 ID3D11Texture2D*t=nullptr;ok(dev->CreateTexture2D(&d,nullptr,&t),"CreateTexture2D");return t;
}
static ID3D11UnorderedAccessView* uav(ID3D11Texture2D*t,DXGI_FORMAT format=DXGI_FORMAT_B8G8R8A8_UNORM) {
 D3D11_UNORDERED_ACCESS_VIEW_DESC d={};d.Format=format;d.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D;
 ID3D11UnorderedAccessView*v=nullptr;ok(dev->CreateUnorderedAccessView(t,&d,&v),"CreateUAV");return v;
}
static void expectBytes(ID3D11Texture2D*t,const unsigned char expected[4],const char* label,UINT subresource=0) {
 D3D11_TEXTURE2D_DESC d={};t->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;
 ID3D11Texture2D*s=nullptr;ok(dev->CreateTexture2D(&d,nullptr,&s),"Create staging");ctx->CopyResource(s,t);
 D3D11_MAPPED_SUBRESOURCE map={};ok(ctx->Map(s,subresource,D3D11_MAP_READ,0,&map),"Map staging");bool same=true;
 for(UINT y=0;y<(d.Height>>(subresource%d.MipLevels));y++)for(UINT x=0;x<(d.Width>>(subresource%d.MipLevels));x++)for(int c=0;c<4;c++)same&=std::abs(int(((unsigned char*)map.pData)[y*map.RowPitch+4*x+c])-expected[c])<=1;
 auto b=(unsigned char*)map.pData;fprintf(resultFile,"BYTES %s actual=%u,%u,%u,%u expected=%u,%u,%u,%u\n",label,b[0],b[1],b[2],b[3],expected[0],expected[1],expected[2],expected[3]);
 ctx->Unmap(s,subresource);s->Release();check(same,label);
}
static void expectFloat(ID3D11Texture2D*t,bool srgb,bool useUav,const char* label) {
 ID3D11ComputeShader* shader=nullptr;ok(dev->CreateComputeShader(useUav?uavreader_dxbc:reader_dxbc,useUav?sizeof(uavreader_dxbc):sizeof(reader_dxbc),nullptr,&shader),"reader shader");
 D3D11_BUFFER_DESC bd={};bd.ByteWidth=16*16;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=16;
 ID3D11Buffer* output=nullptr;ok(dev->CreateBuffer(&bd,nullptr,&output),"output buffer");
 D3D11_UNORDERED_ACCESS_VIEW_DESC ud={};ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=16;
 ID3D11UnorderedAccessView* ov=nullptr;ok(dev->CreateUnorderedAccessView(output,&ud,&ov),"output UAV");
 ID3D11ShaderResourceView* sv=nullptr;ID3D11UnorderedAccessView* uv=nullptr;
 if(useUav) {uv=uav(t);ID3D11UnorderedAccessView* views[]={uv,ov};ctx->CSSetUnorderedAccessViews(0,2,views,nullptr);}
 else {
  D3D11_SHADER_RESOURCE_VIEW_DESC sd={};sd.Format=srgb?DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:DXGI_FORMAT_B8G8R8A8_UNORM;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sd.Texture2D.MipLevels=1;
  ok(dev->CreateShaderResourceView(t,&sd,&sv),"reader SRV");ctx->CSSetShaderResources(0,1,&sv);ctx->CSSetUnorderedAccessViews(0,1,&ov,nullptr);
 }
 ctx->CSSetShader(shader,nullptr,0);ctx->Dispatch(4,4,1);ctx->ClearState();
 bd.Usage=D3D11_USAGE_STAGING;bd.BindFlags=0;bd.MiscFlags=0;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 ID3D11Buffer* staging=nullptr;ok(dev->CreateBuffer(&bd,nullptr,&staging),"float staging");ctx->CopyResource(staging,output);
 D3D11_MAPPED_SUBRESOURCE map={};ok(ctx->Map(staging,0,D3D11_MAP_READ,0,&map),"float readback");
 const float expected[]={.8f,.4f,.2f,1.f};bool same=true;auto f=(float*)map.pData;
 for(int i=0;i<16;i++)for(int k=0;k<4;k++) {float e=expected[k];if(srgb&&k<3)e=std::pow((e+.055f)/1.055f,2.4f);same&=std::fabs(f[4*i+k]-e)<.015f;}
 fprintf(resultFile,"FLOAT %s actual=%.5f,%.5f,%.5f,%.5f\n",label,f[0],f[1],f[2],f[3]);ctx->Unmap(staging,0);
 staging->Release();output->Release();ov->Release();shader->Release();if(sv)sv->Release();if(uv)uv->Release();check(same,label);
}
int main() {
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);resultFile=fopen("C:\\bgra-compat-result.txt","wb");if(!resultFile)return 10;setvbuf(resultFile,nullptr,_IONBF,0);
 try {
 for(const char* name:{"d3d11.dll","dxgi.dll"}) {char path[MAX_PATH]={};GetModuleFileNameA(GetModuleHandleA(name),path,MAX_PATH);fprintf(resultFile,"MODULE %s %s\n",name,path);}
 char levelEnv[64]={};GetEnvironmentVariableA("DXVK_LOG_LEVEL",levelEnv,sizeof(levelEnv));fprintf(resultFile,"ENV log=%s\n",levelEnv);
 D3D_FEATURE_LEVEL fl;ok(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,&fl,&ctx),"D3D11CreateDevice");fprintf(resultFile,"featureLevel=%x\n",fl);
 for(auto format:{DXGI_FORMAT_B8G8R8A8_TYPELESS,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_TYPELESS,DXGI_FORMAT_R8G8B8A8_UNORM})for(UINT bind:{168u,40u,136u}) {auto t=texture(format,bind,815);t->Release();check(true,"815x815 allocation");}
 auto t=texture();auto v=uav(t);const float color[]={.8f,.4f,.2f,1},zero[]={0,0,0,0};const unsigned char bgra[]={51,102,204,255},rgba[]={204,102,51,255};
 ctx->ClearUnorderedAccessViewFloat(v,color);expectBytes(t,bgra,"ClearUAVFloat BGRA bytes");
 const UINT uints[]={204,102,51,255};ctx->ClearUnorderedAccessViewUint(v,uints);expectBytes(t,bgra,"ClearUAVUint BGRA bytes");
 unsigned char upload[64];for(int i=0;i<16;i++)memcpy(upload+4*i,bgra,4);ctx->UpdateSubresource(t,0,nullptr,upload,16,64);expectBytes(t,bgra,"UpdateSubresource bytes");
 auto ordinary=texture(DXGI_FORMAT_B8G8R8A8_UNORM,40);ctx->CopyResource(ordinary,t);expectBytes(ordinary,bgra,"copy emulated to native BGRA");
 const float other[]={.2f,.6f,.8f,1};D3D11_RENDER_TARGET_VIEW_DESC rd={};rd.Format=DXGI_FORMAT_B8G8R8A8_UNORM;rd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;ID3D11RenderTargetView* rtv=nullptr;
 ok(dev->CreateRenderTargetView(ordinary,&rd,&rtv),"native RTV");ctx->ClearRenderTargetView(rtv,other);ctx->CopyResource(t,ordinary);const unsigned char opposite[]={204,153,51,255};expectBytes(t,opposite,"copy native to emulated BGRA");rtv->Release();
 ID3D11ComputeShader* writer=nullptr;ok(dev->CreateComputeShader(writer_dxbc,sizeof(writer_dxbc),nullptr,&writer),"writer shader");
 ctx->CSSetShader(writer,nullptr,0);ctx->CSSetUnorderedAccessViews(0,1,&v,nullptr);ctx->Dispatch(4,4,1);ctx->ClearState();expectBytes(t,bgra,"CS BGRA UAV store");
 auto rt=texture(DXGI_FORMAT_R8G8B8A8_UNORM);auto rv=uav(rt,DXGI_FORMAT_R8G8B8A8_UNORM);ctx->CSSetShader(writer,nullptr,0);ctx->CSSetUnorderedAccessViews(0,1,&rv,nullptr);ctx->Dispatch(4,4,1);ctx->ClearState();expectBytes(rt,rgba,"same shader RGBA unmodified");
 ctx->ClearUnorderedAccessViewFloat(v,zero);ctx->CSSetShader(writer,nullptr,0);ctx->CSSetUnorderedAccessViews(0,1,&v,nullptr);ctx->Dispatch(4,4,1);ctx->ClearState();expectBytes(t,bgra,"same shader rebound BGRA");
 ID3D11RenderTargetView* emulatedRtv=nullptr;ok(dev->CreateRenderTargetView(t,&rd,&emulatedRtv),"emulated RTV");ctx->ClearRenderTargetView(emulatedRtv,color);expectBytes(t,bgra,"emulated RTV clear BGRA bytes");emulatedRtv->Release();
 auto multi=texture(DXGI_FORMAT_B8G8R8A8_TYPELESS,168,8,3,3);D3D11_UNORDERED_ACCESS_VIEW_DESC multiDesc={};multiDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;multiDesc.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2DARRAY;multiDesc.Texture2DArray.MipSlice=1;multiDesc.Texture2DArray.FirstArraySlice=2;multiDesc.Texture2DArray.ArraySize=1;
 ID3D11UnorderedAccessView* multiUav=nullptr;ok(dev->CreateUnorderedAccessView(multi,&multiDesc,&multiUav),"array mip UAV");ctx->ClearUnorderedAccessViewFloat(multiUav,color);expectBytes(multi,bgra,"mip1 array slice2 clear",D3D11CalcSubresource(1,2,3));multiUav->Release();multi->Release();
 expectFloat(t,false,false,"linear SRV after UAV store");expectFloat(t,true,false,"sRGB SRV after UAV store");
 D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support={DXGI_FORMAT_R8G8B8A8_UNORM,0};dev->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2,&support,sizeof(support));
 fprintf(resultFile,"RGBA support2=%x\n",support.OutFormatSupport2);if(support.OutFormatSupport2&D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD)expectFloat(t,false,true,"typed BGRA UAV load");
 ctx->ClearUnorderedAccessViewFloat(v,zero);ID3D11DeviceContext* deferred=nullptr;ok(dev->CreateDeferredContext(0,&deferred),"deferred context");deferred->CSSetShader(writer,nullptr,0);deferred->CSSetUnorderedAccessViews(0,1,&v,nullptr);deferred->Dispatch(4,4,1);
 ID3D11CommandList* list=nullptr;ok(deferred->FinishCommandList(FALSE,&list),"command list");ctx->ExecuteCommandList(list,TRUE);expectBytes(t,bgra,"deferred UAV store and restore");list->Release();deferred->Release();
 ctx->ClearUnorderedAccessViewFloat(v,zero);ID3D11ComputeShader* high=nullptr;ok(dev->CreateComputeShader(writer_high_dxbc,sizeof(writer_high_dxbc),nullptr,&high),"high-slot shader");ctx->CSSetShader(high,nullptr,0);ctx->CSSetUnorderedAccessViews(33,1,&v,nullptr);ctx->Dispatch(4,4,1);ctx->ClearState();expectBytes(t,bgra,"UAV slot 33");high->Release();
 ctx->ClearUnorderedAccessViewFloat(v,zero);ID3D11VertexShader* vs=nullptr;ID3D11PixelShader* ps=nullptr;ok(dev->CreateVertexShader(vs_dxbc,sizeof(vs_dxbc),nullptr,&vs),"vertex shader");ok(dev->CreatePixelShader(ps_dxbc,sizeof(ps_dxbc),nullptr,&ps),"pixel shader");
 ctx->VSSetShader(vs,nullptr,0);ctx->PSSetShader(ps,nullptr,0);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);D3D11_VIEWPORT vp={0,0,4,4,0,1};ctx->RSSetViewports(1,&vp);ctx->OMSetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,1,&v,nullptr);ctx->Draw(3,0);ctx->ClearState();expectBytes(t,bgra,"pixel shader UAV store");
 ps->Release();vs->Release();rv->Release();rt->Release();writer->Release();ordinary->Release();v->Release();t->Release();ctx->Release();dev->Release();fprintf(resultFile,"PASS total=%u\n",passes);fclose(resultFile);return 0;
 }catch(const std::exception&e) {fprintf(resultFile,"FAILED after=%u operation=%s\n",passes,e.what());fclose(resultFile);return 1;}
}
