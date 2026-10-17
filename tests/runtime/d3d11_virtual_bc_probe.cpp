// End-to-end D3D11 qualification. Uses the actual DXBC compiler, descriptor
// bindings, resource uploads, SRV mip ranges and GPU readback, not a GLSL clone.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#define BCDEC_STATIC
#define WINE_UNUSED 1
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

struct F4 { float v[4] = {}; };
struct Case { F4 a; };
static FILE* logFile;
static ID3D11Device* dev;
static ID3D11DeviceContext* ctx;
static ID3D11ComputeShader* shader;
static ID3D11VertexShader* vertexShader;
static ID3D11PixelShader* pixelShader;
static FILE* captureFile;
static bool nativeCapture;
static bool transferOnly;
static void check(HRESULT hr, const char* what) {
  if (FAILED(hr)) { fprintf(logFile, "FAIL %s %08lx\n", what, (unsigned long)hr); fflush(logFile); throw std::runtime_error(what); }
}
template<typename T> static void release(T*& p) { if (p) { p->Release(); p = nullptr; } }
static void memorySnapshot(unsigned kind,bool srgb,const char* phase) {
  IDXGIDevice* device=nullptr;IDXGIAdapter* adapter=nullptr;IDXGIAdapter3* adapter3=nullptr;
  if(SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice),(void**)&device))
   && SUCCEEDED(device->GetAdapter(&adapter))
   && SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3),(void**)&adapter3))) {
    for(auto segment:{DXGI_MEMORY_SEGMENT_GROUP_LOCAL,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL}) {
      DXGI_QUERY_VIDEO_MEMORY_INFO info={};
      if(SUCCEEDED(adapter3->QueryVideoMemoryInfo(0,segment,&info)))
        fprintf(logFile,"MEMORY BC%u srgb%d phase=%s segment=%u allocated=%llu budget=%llu\n",kind,int(srgb),phase,unsigned(segment),info.CurrentUsage,info.Budget);
    }
  }
  release(adapter3);release(adapter);release(device);fflush(logFile);
}
static const char hlsl[] = R"(
Texture2D<float4> tex : register(t0);
StructuredBuffer<float4> cases : register(t1);
RWStructuredBuffer<float4> output : register(u0);
SamplerState state : register(s0);
[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID) {
  if (id.x == 0) { uint w,h,n; tex.GetDimensions(0,w,h,n); output[0]=float4(w,h,n,1); return; }
  float4 c=cases[id.x];
  if (c.w == 0) output[id.x]=tex.Load(int3(c.xyz));
  else if(c.w==1) output[id.x]=tex.SampleLevel(state,c.xy,c.z);
  else if(c.w==2) { uint w,h;tex.GetDimensions(w,h);float q=exp2(c.z);output[id.x]=tex.SampleGrad(state,c.xy,float2(q/w,0),float2(0,q/h)); }
  else if(c.w==3) output[id.x]=tex.GatherRed(state,c.xy,int2(1,-1));
  else if(c.w==4) output[id.x]=tex.SampleLevel(state,c.xy,c.z,int2(1,-1));
  else if(c.w==5) output[id.x]=tex.Load(int3(c.xyz),int2(1,-1));
  else {uint w,h;tex.GetDimensions(w,h);float q=exp2(c.z);output[id.x]=tex.SampleGrad(state,c.xy,float2(8*q/w,0),float2(0,q/h));}
})";
static const char rasterHlsl[] = R"(
Texture2D<float4> tex : register(t0);
SamplerState state : register(s0);
cbuffer Params : register(b0) { float4 originStep; float4 controls; };
float4 vertex(uint id : SV_VertexID) : SV_Position {
  return float4(id==2?3:-1,id==1?3:-1,0,1);
}
float4 pixel(float4 pos : SV_Position) : SV_Target {
  float2 uv=originStep.xy+pos.xy*originStep.zw;
  if(controls.x==0)return tex.Sample(state,uv);
  if(controls.x==1)return tex.SampleBias(state,uv,1.25);
  return float4(tex.CalculateLevelOfDetail(state,uv),tex.CalculateLevelOfDetailUnclamped(state,uv),0,1);
})";

static F4 reference(const Case&,const std::vector<std::vector<uint8_t>>&,unsigned,
                    unsigned,unsigned,bool,int,int,bool,bool,D3D11_TEXTURE_ADDRESS_MODE,const F4&);
static int pixelTest(ID3D11ShaderResourceView* view, ID3D11SamplerState* sampler,
  const std::vector<std::vector<uint8_t>>& data,unsigned kind,bool srgb,int first,int mode,int address,const F4& border) {
  constexpr unsigned rw=32,rh=16,w=14360,h=4724;
  D3D11_TEXTURE2D_DESC td={};td.Width=rw;td.Height=rh;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
  td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D* target=nullptr;check(dev->CreateTexture2D(&td,nullptr,&target),"pixel target");
  ID3D11RenderTargetView* rtv=nullptr;check(dev->CreateRenderTargetView(target,nullptr,&rtv),"pixel RTV");
  td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* readback=nullptr;check(dev->CreateTexture2D(&td,nullptr,&readback),"pixel readback");
  D3D11_BUFFER_DESC bd={};bd.ByteWidth=32;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
  ID3D11Buffer* params=nullptr;check(dev->CreateBuffer(&bd,nullptr,&params),"pixel parameters");
  D3D11_RASTERIZER_DESC rd={};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
  ID3D11RasterizerState* raster=nullptr;check(dev->CreateRasterizerState(&rd,&raster),"raster state");
  D3D11_VIEWPORT vp={0,0,float(rw),float(rh),0,1};
  ctx->RSSetState(raster);ctx->RSSetViewports(1,&vp);ctx->OMSetRenderTargets(1,&rtv,nullptr);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vertexShader,nullptr,0);ctx->PSSetShader(pixelShader,nullptr,0);
  ctx->PSSetShaderResources(0,1,&view);ctx->PSSetSamplers(0,1,&sampler);ctx->PSSetConstantBuffers(0,1,&params);
  int failures=0;float maxError=0;
  if(first==0 && mode==0 && address==1) {
    // Exercise a regular Texture2D on the same shader and SRV slot, then
    // return to the virtual BC resource without clearing the state.
    F4 constant={{0.125f,0.375f,0.625f,0.875f}};std::array<F4,16> pixels;pixels.fill(constant);
    auto small=td;small.Width=small.Height=4;small.Usage=D3D11_USAGE_IMMUTABLE;
    small.BindFlags=D3D11_BIND_SHADER_RESOURCE;small.CPUAccessFlags=0;
    D3D11_SUBRESOURCE_DATA data={pixels.data(),4*sizeof(F4),0};
    ID3D11Texture2D* normal=nullptr;check(dev->CreateTexture2D(&small,&data,&normal),"native switch texture");
    ID3D11ShaderResourceView* nativeView=nullptr;check(dev->CreateShaderResourceView(normal,nullptr,&nativeView),"native switch SRV");
    float values[8]={0,0,1/1024.f,1/512.f,0,0,0,0};ctx->UpdateSubresource(params,0,nullptr,values,0,0);
    ctx->PSSetShaderResources(0,1,&nativeView);ctx->Draw(3,0);ctx->CopyResource(readback,target);
    D3D11_MAPPED_SUBRESOURCE map={};check(ctx->Map(readback,0,D3D11_MAP_READ,0,&map),"native switch readback");
    if(memcmp(map.pData,&constant,sizeof(F4))){fprintf(logFile,"NATIVE SWITCH FAIL\n");failures++;}
    ctx->Unmap(readback,0);ctx->PSSetShaderResources(0,1,&view);release(nativeView);release(normal);
  }
  for(int op=0;op<3;op++) {
    float values[8]={-0.03125f,0.96875f,1/1024.f,1/512.f,float(op),0,0,0};
    ctx->UpdateSubresource(params,0,nullptr,values,0,0);
    if(op==0 && first==0 && mode==0 && address==1) {
      ID3D11DeviceContext* deferred=nullptr;check(dev->CreateDeferredContext(0,&deferred),"deferred context");
      deferred->RSSetState(raster);deferred->RSSetViewports(1,&vp);deferred->OMSetRenderTargets(1,&rtv,nullptr);
      deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      deferred->VSSetShader(vertexShader,nullptr,0);deferred->PSSetShader(pixelShader,nullptr,0);
      deferred->PSSetShaderResources(0,1,&view);deferred->PSSetSamplers(0,1,&sampler);
      deferred->PSSetConstantBuffers(0,1,&params);deferred->Draw(3,0);
      ID3D11CommandList* list=nullptr;check(deferred->FinishCommandList(FALSE,&list),"deferred finish");
      ctx->ExecuteCommandList(list,TRUE);release(list);release(deferred);
    } else ctx->Draw(3,0);
    ctx->CopyResource(readback,target);
    D3D11_MAPPED_SUBRESOURCE map={};check(ctx->Map(readback,0,D3D11_MAP_READ,0,&map),"pixel GPU result");
    float lod=log2f(std::max(float(w>>first)/1024,float(h>>first)/512));
    for(unsigned y=0;y<rh;y++)for(unsigned x=0;x<rw;x++) {
      Case c={{{values[0]+(x+0.5f)*values[2],values[1]+(y+0.5f)*values[3],lod+(op==1?1.25f:0),1}}};
      F4 expected={};
      if(op==2)expected={{std::clamp(lod,0.f,float(13-first)),lod,0,1}};
      else if(mode==3) {
        c.a.v[2]-=1; // Two taps along the major derivative axis.
        for(float offset : {-0.25f,0.25f}) {
          auto tap=c;tap.a.v[0]+=values[2]*offset;
          auto color=reference(tap,data,kind,w,h,srgb,first,14-first,true,true,D3D11_TEXTURE_ADDRESS_MODE(address),border);
          for(int ch=0;ch<4;ch++)expected.v[ch]+=color.v[ch]*0.5f;
        }
      } else expected=reference(c,data,kind,w,h,srgb,first,14-first,mode!=0,mode==2,D3D11_TEXTURE_ADDRESS_MODE(address),border);
      auto got=((F4*)((uint8_t*)map.pData+y*map.RowPitch))[x];
      for(int ch=0;ch<4;ch++) {
        float error=fabsf(got.v[ch]-expected.v[ch]);maxError=std::max(maxError,error);
        if(!std::isfinite(got.v[ch])||error>0.0003f) {
          if(failures++<2)fprintf(logFile,"RASTER FAIL BC%u srgb%d first%d mode%d addr%d op%d xy%u,%u c%d got%.7f expected%.7f\n",kind,srgb,first,mode,address,op,x,y,ch,got.v[ch],expected.v[ch]);
        }
      }
    }
    ctx->Unmap(readback,0);
  }
  if(failures)fprintf(logFile,"RASTER failures%d maxError%.8f\n",failures,maxError);
  ctx->ClearState();release(raster);release(params);release(readback);release(rtv);release(target);return failures;
}

static F4 pixel(const std::vector<std::vector<uint8_t>>& data, unsigned kind,
                unsigned width, unsigned height, int mip, int x, int y,
                bool srgb, D3D11_TEXTURE_ADDRESS_MODE address, const F4& border) {
  int w=std::max(width>>mip,1u), h=std::max(height>>mip,1u);
  int p[2]={x,y}, n[2]={w,h};
  for (int a=0;a<2;a++) {
    switch(address) {
      case D3D11_TEXTURE_ADDRESS_WRAP: p[a]=(p[a]%n[a]+n[a])%n[a]; break;
      case D3D11_TEXTURE_ADDRESS_MIRROR: {
        int r=(p[a]%(2*n[a])+2*n[a])%(2*n[a]); p[a]=std::min(r,2*n[a]-1-r); break;
      }
      case D3D11_TEXTURE_ADDRESS_BORDER: if(p[a]<0||p[a]>=n[a])return border; break;
      default: p[a]=std::clamp(p[a],0,n[a]-1); break;
    }
  }
  unsigned bytes=kind==1?8:16;
  const auto* block=data[mip].data()+((p[1]/4)*((w+3)/4)+p[0]/4)*bytes;
  alignas(16) uint8_t rgba[64];
  if(kind==1)bcdec_bc1(block,rgba,16);
  else if(kind==2)bcdec_bc2(block,rgba,16);
  else bcdec_bc3(block,rgba,16);
  F4 result;
  for(int c=0;c<4;c++) {
    float v=rgba[((p[1]%4)*4+p[0]%4)*4+c]/255.f;
    result.v[c]=srgb&&c<3?(v<=0.04045f?v/12.92f:powf((v+0.055f)/1.055f,2.4f)):v;
  }
  return result;
}
static F4 mix(const F4& a,const F4& b,float t) {
  F4 r; for(int c=0;c<4;c++)r.v[c]=a.v[c]+(b.v[c]-a.v[c])*t; return r;
}
static F4 reference(const Case& c,const std::vector<std::vector<uint8_t>>& data,unsigned kind,
                    unsigned w,unsigned h,bool srgb,int first,int levels,bool linear,bool mipLinear,
                    D3D11_TEXTURE_ADDRESS_MODE address,const F4& border) {
  if(c.a.v[3]==0 || c.a.v[3]==5) {
    int mip=(int)c.a.v[2];
    int x=int(c.a.v[0])+(c.a.v[3]==5?1:0),y=int(c.a.v[1])-(c.a.v[3]==5?1:0);
    if(mip<0||mip>=levels||x<0||y<0
     ||unsigned(x)>=std::max(w>>(first+mip),1u)||unsigned(y)>=std::max(h>>(first+mip),1u))return {};
    return pixel(data,kind,w,h,first+mip,x,y,srgb,D3D11_TEXTURE_ADDRESS_CLAMP,border);
  }
  float lod=std::clamp(c.a.v[2],0.f,float(levels-1));
  int lo=mipLinear?(int)floorf(lod):(int)floorf(lod+0.5f), hi=std::min(lo+1,levels-1);
  auto sample=[&](int mip){
    float uv[2]={c.a.v[0],c.a.v[1]};
    for(int a=0;a<2;a++) {
      if(address==D3D11_TEXTURE_ADDRESS_MIRROR_ONCE)uv[a]=fabsf(uv[a]);
      if(address==D3D11_TEXTURE_ADDRESS_WRAP)uv[a]-=floorf(uv[a]);
      if(address==D3D11_TEXTURE_ADDRESS_MIRROR){float t=uv[a]*0.5f; uv[a]=1-fabsf(2*(t-floorf(t))-1);}
      if(address!=D3D11_TEXTURE_ADDRESS_WRAP&&address!=D3D11_TEXTURE_ADDRESS_MIRROR)uv[a]=std::clamp(uv[a],-1.f,2.f);
    }
    int m=first+mip;
    float x=uv[0]*std::max(w>>m,1u)-(linear?0.5f:0), y=uv[1]*std::max(h>>m,1u)-(linear?0.5f:0);
    int ix=(int)floorf(x)+(c.a.v[3]==4?1:0), iy=(int)floorf(y)-(c.a.v[3]==4?1:0);
    if(c.a.v[3]==3) {
      // Gather uses a 2x2 footprint at mip zero, before filtering.
      ix=int(floorf(uv[0]*std::max(w>>first,1u)-0.5f))+1;
      iy=int(floorf(uv[1]*std::max(h>>first,1u)-0.5f))-1;
      const int offsets[4][2]={{0,1},{1,1},{1,0},{0,0}};F4 r;
      for(int k=0;k<4;k++)r.v[k]=pixel(data,kind,w,h,first,ix+offsets[k][0],iy+offsets[k][1],srgb,address,border).v[0];
      return r;
    }
    auto a=pixel(data,kind,w,h,m,ix,iy,srgb,address,border);
    if(!linear)return a;
    auto b=pixel(data,kind,w,h,m,ix+1,iy,srgb,address,border);
    auto cc=pixel(data,kind,w,h,m,ix,iy+1,srgb,address,border);
    auto d=pixel(data,kind,w,h,m,ix+1,iy+1,srgb,address,border);
    return mix(mix(a,b,x-floorf(x)),mix(cc,d,x-floorf(x)),y-floorf(y));
  };
  return mix(sample(lo),sample(hi),mipLinear?lod-floorf(lod):0);
}

static int transferTest(ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC& td,
                        const std::vector<std::vector<uint8_t>>& original, unsigned bytes) {
  auto expected=original;
  // Cross both a logical BC block-row boundary and an atlas row boundary;
  // use padded input rows so incorrect pitch handling cannot pass.
  const unsigned bw=(td.Width+3)/4, rows=7, cols=23;
  std::vector<uint8_t> patch((cols+3)*bytes*rows,0x9b);
  for(unsigned y=0;y<rows;y++)for(unsigned x=0;x<cols*bytes;x++)patch[y*(cols+3)*bytes+x]=uint8_t(x*53+y*71);
  D3D11_BOX box={td.Width-4*cols,4,0,td.Width,4+4*rows,1};
  ctx->UpdateSubresource(texture,0,&box,patch.data(),(cols+3)*bytes,0);
  for(unsigned y=0;y<rows;y++)memcpy(expected[0].data()+((y+1)*bw+box.left/4)*bytes,
    patch.data()+y*(cols+3)*bytes,cols*bytes);
  ID3D11Texture2D* copy=nullptr;check(dev->CreateTexture2D(&td,nullptr,&copy),"copy destination");
  ctx->CopyResource(copy,texture);
  auto sd=td;sd.Usage=D3D11_USAGE_STAGING;sd.BindFlags=0;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE;
  ID3D11Texture2D* staging=nullptr;check(dev->CreateTexture2D(&sd,nullptr,&staging),"BC staging");
  ctx->CopyResource(staging,copy);
  int failures=0;
  for(unsigned m=0;m<td.MipLevels;m++) {
    D3D11_MAPPED_SUBRESOURCE map={};check(ctx->Map(staging,m,D3D11_MAP_READ_WRITE,0,&map),"BC staging readback");
    unsigned rowBytes=((std::max(td.Width>>m,1u)+3)/4)*bytes;
    unsigned rowCount=(std::max(td.Height>>m,1u)+3)/4;
    for(unsigned y=0;y<rowCount;y++)if(memcmp((uint8_t*)map.pData+y*map.RowPitch,expected[m].data()+y*rowBytes,rowBytes)) {
      if(failures++<4) {
        const auto* got=(const uint8_t*)map.pData+y*map.RowPitch;
        const auto* want=expected[m].data()+y*rowBytes;
        unsigned first=0;while(first<rowBytes&&got[first]==want[first])first++;
        fprintf(logFile,"TRANSFER FAIL mip%u row%u byte%u got%02x want%02x pitch%u\n",m,y,first,got[first],want[first],map.RowPitch);
      }
    }
    // Restore the original data through the mapped BC staging path.
    for(unsigned y=0;y<rowCount;y++)memcpy((uint8_t*)map.pData+y*map.RowPitch,original[m].data()+y*rowBytes,rowBytes);
    ctx->Unmap(staging,m);
  }
  ctx->CopyResource(texture,staging);
  // Same-resource subregion copy with disjoint blocks, then restore it.
  D3D11_BOX source={0,0,0,8,8,1};
  ctx->CopySubresourceRegion(texture,0,16,16,0,texture,0,&source);
  D3D11_BOX restore={16,16,0,24,24,1};
  ctx->CopySubresourceRegion(texture,0,16,16,0,staging,0,&restore);
  ctx->Flush();release(staging);release(copy);
  fprintf(logFile,"TRANSFER result failures%d\n",failures);fflush(logFile);
  return failures;
}

static int test(unsigned kind,bool srgb) {
  memorySnapshot(kind,srgb,"begin");
  const unsigned w=14360,h=4724,levels=14,bytes=kind==1?8:16;
  std::vector<std::vector<uint8_t>> data(levels);
  std::vector<D3D11_SUBRESOURCE_DATA> initial(levels);
  for(unsigned m=0;m<levels;m++) {
    unsigned bw=(std::max(w>>m,1u)+3)/4,bh=(std::max(h>>m,1u)+3)/4;
    data[m].resize(size_t(bw)*bh*bytes);
    for(unsigned y=0;y<bh;y++)for(unsigned x=0;x<bw;x++) {
      // Vary endpoints, all color/alpha indices, both BC3 alpha modes.
      uint32_t state=0x96374a21u+x*193939u+y*10271u+m*7177u;
      for(unsigned b=0;b<bytes;b++){state=state*1664525u+1013904223u;data[m][(size_t(y)*bw+x)*bytes+b]=uint8_t(state>>24);}
    }
    initial[m]={data[m].data(),bw*bytes,bw*bh*bytes};
  }
  D3D11_TEXTURE2D_DESC td={}; td.Width=w;td.Height=h;td.MipLevels=levels;td.ArraySize=1;
  td.Format=kind==1?DXGI_FORMAT_BC1_TYPELESS:kind==2?DXGI_FORMAT_BC2_TYPELESS:DXGI_FORMAT_BC3_TYPELESS;
  td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture2D* texture=nullptr;check(dev->CreateTexture2D(&td,initial.data(),&texture),"CreateTexture2D oversized");
  D3D11_TEXTURE2D_DESC actual;texture->GetDesc(&actual);
  if(actual.Width!=w||actual.Height!=h)throw std::runtime_error("logical dimensions changed");
  int failures=nativeCapture?0:transferTest(texture,td,data,bytes);float maxError=0;
  if(transferOnly){release(texture);fprintf(logFile,"TRANSFER ONLY BC%u srgb%d failures%d\n",kind,int(srgb),failures);fflush(logFile);return failures;}
  unsigned opFailures[7]={};
  for(int first : {0,2}) for(int mode=0;mode<4;mode++) for(int address=1;address<=5;address++) {
    D3D11_SHADER_RESOURCE_VIEW_DESC vd={};
    vd.Format=DXGI_FORMAT((kind==1?DXGI_FORMAT_BC1_UNORM:kind==2?DXGI_FORMAT_BC2_UNORM:DXGI_FORMAT_BC3_UNORM)+(srgb?1:0));
    vd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;vd.Texture2D.MostDetailedMip=first;vd.Texture2D.MipLevels=levels-first;
    ID3D11ShaderResourceView* view=nullptr;check(dev->CreateShaderResourceView(texture,&vd,&view),"Create SRV mip view");
    D3D11_SAMPLER_DESC sd={};sd.Filter=mode==0?D3D11_FILTER_MIN_MAG_MIP_POINT:mode==1?D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT:mode==2?D3D11_FILTER_MIN_MAG_MIP_LINEAR:D3D11_FILTER_ANISOTROPIC;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_MODE(address);sd.MaxLOD=100;sd.MaxAnisotropy=8;
    F4 border={{0.2f,0.4f,0.6f,0.3f}};memcpy(sd.BorderColor,border.v,16);
    ID3D11SamplerState* sampler=nullptr;check(dev->CreateSamplerState(&sd,&sampler),"CreateSamplerState");
    constexpr int count=289;
    std::array<Case,count> cases={};
    for(int i=1;i<count;i++) {
      int mip=(i/16)%(levels-first);unsigned mw=std::max(w>>(mip+first),1u),mh=std::max(h>>(mip+first),1u);
      if(i<65)cases[i].a={{float((i*2137u)%mw),float((i*131u)%mh),float(mip),0}};
      else cases[i].a={{float((i*179u)%199u)/197.f-0.01f,float((i*151u)%197u)/193.f-0.01f,float(mip)+0.5f,1}};
    }
    for(int i=1;i<=16;i++)cases[i].a={{float(8190+(i%4)),float(3+(i/4)),0,0}};
    cases[17].a={{-1,0,0,0}};cases[18].a={{float(w>>first),0,0,0}};
    for(int j=0;j<4;j++) {
      cases[19+j].a={{float(j&1?(w>>first)-1:0),float(j&2?(h>>first)-1:0),0,0}};
    }
    cases[65].a={{0,0,0,1}};cases[66].a={{1,1,0,1}};
    cases[67].a={{-0.25f,1.25f,0,1}};cases[68].a={{2.25f,-1.25f,1,1}};
    for(int i=129;i<count;i++) {
      int base=65+(i-129)%32;cases[i]=cases[base];cases[i].a.v[3]=float(2+(i-129)/32);
      if(i>=225&&i<257){cases[i]=cases[1+(i-225)%32];cases[i].a.v[3]=5;}
      if(i>=257){cases[i]=cases[65+(i-257)%32];cases[i].a.v[2]+=0.125f;cases[i].a.v[3]=6;}
      if(i<161)cases[i].a.v[2]+=0.125f; // Avoid nearest-mip ties after log2 rounding.
      if(i>=161&&i<193)cases[i].a.v[2]=0;
    }
    D3D11_BUFFER_DESC bd={};bd.ByteWidth=sizeof(cases);bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=sizeof(Case);
    D3D11_SUBRESOURCE_DATA src={cases.data(),0,0};ID3D11Buffer* input=nullptr;check(dev->CreateBuffer(&bd,&src,&input),"input buffer");
    D3D11_SHADER_RESOURCE_VIEW_DESC ivd={};ivd.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;ivd.Buffer.NumElements=count;
    ID3D11ShaderResourceView* inputView=nullptr;check(dev->CreateShaderResourceView(input,&ivd,&inputView),"input SRV");
    bd.ByteWidth=count*sizeof(F4);bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;bd.StructureByteStride=sizeof(F4);
    ID3D11Buffer* output=nullptr;check(dev->CreateBuffer(&bd,nullptr,&output),"output buffer");
    D3D11_UNORDERED_ACCESS_VIEW_DESC uvd={};uvd.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;uvd.Buffer.NumElements=count;
    ID3D11UnorderedAccessView* uav=nullptr;check(dev->CreateUnorderedAccessView(output,&uvd,&uav),"output UAV");
    bd.Usage=D3D11_USAGE_STAGING;bd.BindFlags=0;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;bd.MiscFlags=0;
    ID3D11Buffer* readback=nullptr;check(dev->CreateBuffer(&bd,nullptr,&readback),"readback buffer");
    ID3D11ShaderResourceView* views[]={view,inputView};ctx->CSSetShaderResources(0,2,views);ctx->CSSetSamplers(0,1,&sampler);
    ctx->CSSetUnorderedAccessViews(0,1,&uav,nullptr);ctx->CSSetShader(shader,nullptr,0);ctx->Dispatch(count,1,1);
    ctx->CopyResource(readback,output);D3D11_MAPPED_SUBRESOURCE map={};check(ctx->Map(readback,0,D3D11_MAP_READ,0,&map),"GPU readback");
    auto* values=static_cast<const F4*>(map.pData);
    if(captureFile)fwrite(values,sizeof(F4),count,captureFile);
    if(!srgb && first==0 && mode==1 && address==1) {
      for(int i : {19,20,21,22,65}) {
        auto e=reference(cases[i],data,kind,w,h,srgb,first,levels-first,true,false,sd.AddressU,border);
        fprintf(logFile,"DIAG BC%u case%d gpu %.7f %.7f %.7f %.7f cpu %.7f %.7f %.7f %.7f\n",kind,i,
          values[i].v[0],values[i].v[1],values[i].v[2],values[i].v[3],e.v[0],e.v[1],e.v[2],e.v[3]);
      }
    }
    if(values[0].v[0]!=(w>>first)||values[0].v[1]!=(h>>first)||values[0].v[2]!=(levels-first)){
      fprintf(logFile,"DIM FAIL BC%u first%d got %.0f %.0f %.0f\n",kind,first,values[0].v[0],values[0].v[1],values[0].v[2]);failures++;
    }
    for(int i=1;i<count;i++) {
      F4 expected={};
      if(cases[i].a.v[3]==6 && mode==3) {
        for(int tap=0;tap<8;tap++) {
          auto c=cases[i];c.a.v[0]+=((tap+0.5f)/8-0.5f)*8*exp2f(c.a.v[2])/float(w>>first);c.a.v[3]=1;
          auto color=reference(c,data,kind,w,h,srgb,first,levels-first,true,true,sd.AddressU,border);
          for(int ch=0;ch<4;ch++)expected.v[ch]+=color.v[ch]/8;
        }
      } else {
        auto c=cases[i];if(c.a.v[3]==6){c.a.v[2]+=3;c.a.v[3]=1;}
        expected=reference(c,data,kind,w,h,srgb,first,levels-first,mode!=0,mode>=2,sd.AddressU,border);
      }
      for(int c=0;c<4;c++) {
        float error=fabsf(expected.v[c]-values[i].v[c]);maxError=std::max(maxError,error);
        if(!std::isfinite(values[i].v[c])||(!nativeCapture&&error>0.0003f)) {
          if(failures<12)fprintf(logFile,"PIXEL FAIL BC%u srgb%d first%d mode%d addr%d i%d c%d got%.7f expected%.7f\n",kind,srgb,first,mode,address,i,c,values[i].v[c],expected.v[c]);
          failures++;
          opFailures[int(cases[i].a.v[3])]++;
        }
      }
    }
    ctx->Unmap(readback,0);ctx->ClearState();ctx->Flush();
    if(!nativeCapture)failures+=pixelTest(view,sampler,data,kind,srgb,first,mode,address,border);
    release(readback);release(uav);release(output);release(inputView);release(input);release(sampler);release(view);
    fflush(logFile);
  }
  release(texture);
  memorySnapshot(kind,srgb,"released");
  fprintf(logFile,"RESULT BC%u srgb%d compute11520 raster%u failures%d %s%.8f\n",kind,srgb,nativeCapture?0u:61440u,failures,nativeCapture?"cpuDiagnosticError":"computeMaxError",maxError);fflush(logFile);
  fprintf(logFile,"OPS Load%u Level%u Grad%u Gather%u LevelOffset%u LoadOffset%u AnisoGrad%u\n",opFailures[0],opFailures[1],opFailures[2],opFailures[3],opFailures[4],opFailures[5],opFailures[6]);fflush(logFile);
  return failures;
}
int main(int argc,char** argv) {
  if(argc>1 && !strcmp(argv[1],"--compile")) {
    struct Program {const char* source;size_t length;const char* entry;const char* target;const char* file;};
    for(auto program : {Program{hlsl,sizeof(hlsl)-1,"main","cs_5_0","virtual_bc_test.dxbc"},
      Program{rasterHlsl,sizeof(rasterHlsl)-1,"vertex","vs_5_0","virtual_bc_vertex.dxbc"},
      Program{rasterHlsl,sizeof(rasterHlsl)-1,"pixel","ps_5_0","virtual_bc_pixel.dxbc"}}) {
    ID3DBlob *code=nullptr,*errors=nullptr;
    HRESULT hr=D3DCompile(program.source,program.length,"virtual_bc_test",nullptr,nullptr,program.entry,program.target,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
    if(errors){fprintf(stderr,"%s\n",(char*)errors->GetBufferPointer());release(errors);}
    if(FAILED(hr))return 1;
    FILE* file=fopen(program.file,"wb");if(!file)return 2;
    fwrite(code->GetBufferPointer(),1,code->GetBufferSize(),file);fclose(file);release(code);
    }return 0;
  }
  char exePath[MAX_PATH]={};GetModuleFileNameA(nullptr,exePath,MAX_PATH);
  char* slash=strrchr(exePath,'\\');if(!slash)return 2;slash[1]=0;
  std::string folder=exePath;
  logFile=fopen(argc>1?argv[1]:(folder+"result.log").c_str(),"w");if(!logFile)return 2;
  nativeCapture=argc>2 && !strcmp(argv[2],"--native-capture");
  transferOnly=argc>2 && !strcmp(argv[2],"--transfer-only");
  fprintf(logFile,"MODE %s\n",nativeCapture?"native-capture (compare binary with unchanged native path)":"CPU-reference conformance");
  if(nativeCapture)captureFile=fopen((std::string(argv[1])+".bin").c_str(),"wb");
  SetEnvironmentVariableA("DXVK_SHADER_DUMP_PATH",(folder+"shaders").c_str());
  CreateDirectoryA((folder+"shaders").c_str(),nullptr);
  // Auto mode uses queried Vulkan format limits. This program must exercise
  // the virtual path on 8K devices, and the native path on 16K devices.
  try {
    D3D_FEATURE_LEVEL level;D3D_FEATURE_LEVEL requested=D3D_FEATURE_LEVEL_11_0;
    check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,&requested,1,D3D11_SDK_VERSION,&dev,&level,&ctx),"D3D11CreateDevice FL11");
    char dllPath[MAX_PATH]={};GetModuleFileNameA(GetModuleHandleA("d3d11.dll"),dllPath,MAX_PATH);
    fprintf(logFile,"D3D11 module %s\n",dllPath);fflush(logFile);
    FILE* compiled=fopen((folder+"virtual_bc_test.dxbc").c_str(),"rb");
    if(compiled) {
      fseek(compiled,0,SEEK_END);long n=ftell(compiled);rewind(compiled);std::vector<uint8_t> code(n);
      if(fread(code.data(),1,n,compiled)!=size_t(n))throw std::runtime_error("short DXBC read");fclose(compiled);
      check(dev->CreateComputeShader(code.data(),code.size(),nullptr,&shader),"CreateComputeShader precompiled DXBC");
    } else {
      ID3DBlob *code=nullptr,*errors=nullptr;
      HRESULT hr=D3DCompile(hlsl,sizeof(hlsl)-1,"virtual_bc_test",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
      if(errors){fprintf(logFile,"HLSL: %s\n",(char*)errors->GetBufferPointer());release(errors);}check(hr,"D3DCompile");
      check(dev->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader),"CreateComputeShader");release(code);
    }
    for(int stage=0;stage<2;stage++) {
      std::string path=folder+(stage?"virtual_bc_pixel.dxbc":"virtual_bc_vertex.dxbc");
      FILE* file=fopen(path.c_str(),"rb");if(!file)throw std::runtime_error("missing precompiled raster DXBC");
      fseek(file,0,SEEK_END);long n=ftell(file);rewind(file);std::vector<uint8_t> code(n);
      if(fread(code.data(),1,n,file)!=size_t(n))throw std::runtime_error("short raster DXBC");fclose(file);
      if(stage)check(dev->CreatePixelShader(code.data(),code.size(),nullptr,&pixelShader),"raster pixel shader");
      else check(dev->CreateVertexShader(code.data(),code.size(),nullptr,&vertexShader),"raster vertex shader");
    }
    if(argc>2 && !strcmp(argv[2],"--dump")) {
      fprintf(logFile,"DXBC translation complete; no GPU dispatch\n");fclose(logFile);return 0;
    }
    for(auto format : {DXGI_FORMAT_BC1_UNORM,DXGI_FORMAT_BC2_UNORM,DXGI_FORMAT_BC3_UNORM}) {
      D3D11_TEXTURE2D_DESC wide={};wide.Width=16384;wide.Height=4;wide.MipLevels=15;
      wide.ArraySize=wide.SampleDesc.Count=1;wide.Format=format;wide.Usage=D3D11_USAGE_DEFAULT;wide.BindFlags=D3D11_BIND_SHADER_RESOURCE;
      ID3D11Texture2D* texture=nullptr;check(dev->CreateTexture2D(&wide,nullptr,&texture),"16384-wide full mip chain");
      D3D11_TEXTURE2D_DESC actual;texture->GetDesc(&actual);if(actual.Width!=16384||actual.MipLevels!=15)throw std::runtime_error("16K logical descriptor");
      release(texture);fprintf(logFile,"WIDE16K format%d mips15 PASS\n",int(format));
    }
    if(argc>2 && !strcmp(argv[2],"--wide-only")){fprintf(logFile,"FINAL failures=0 (16K creation only)\n");fclose(logFile);return 0;}
    unsigned repeats=1;
    for(int i=2;i<argc;i++)if(!strncmp(argv[i],"--repeat=",9)) {
      unsigned requested=0;
      if(sscanf(argv[i]+9,"%u",&requested)!=1||requested<1||requested>20)
        throw std::runtime_error("repeat count must be 1..20");
      repeats=requested;
    }
    int failures=0;
    for(unsigned run=0;run<repeats;run++) {
      if(repeats>1){fprintf(logFile,"RUN %u/%u\n",run+1,repeats);fflush(logFile);}
      for(unsigned kind=1;kind<=3;kind++)for(bool srgb:{false,true})failures+=test(kind,srgb);
    }
    fprintf(logFile,"FINAL failures=%d\n",failures);fflush(logFile);
    release(pixelShader);release(vertexShader);release(shader);release(ctx);release(dev);if(captureFile)fclose(captureFile);fclose(logFile);return failures?1:0;
  } catch(const std::exception& e){fprintf(logFile,"ABORT %s\n",e.what());fflush(logFile);fclose(logFile);return 1;}
}
