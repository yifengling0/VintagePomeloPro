#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cmath>
#include <initializer_list>
#include "bgra_probe_shaders.h"
#include "bc_volume_shader.h"
static FILE* f;
static unsigned failures=0;
static bool hr(HRESULT value,const char* op) {
 if(FAILED(value)){fprintf(f,"FAIL %s hr=%08lx\n",op,(unsigned long)value);fflush(f);++failures;return false;}return true;
}
int main(){
 f=fopen("C:\\bc-probe-result.txt","w");if(!f)return 2;
 ID3D11Device* d=nullptr;ID3D11DeviceContext* c=nullptr;
 if(!hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&c),"device")){fclose(f);return 1;}
 for(auto fmt:{DXGI_FORMAT_BC1_UNORM,DXGI_FORMAT_BC2_UNORM,DXGI_FORMAT_BC3_UNORM,DXGI_FORMAT_BC4_UNORM,DXGI_FORMAT_BC4_SNORM,DXGI_FORMAT_BC5_UNORM,DXGI_FORMAT_BC5_SNORM,DXGI_FORMAT_BC6H_UF16,DXGI_FORMAT_BC6H_SF16,DXGI_FORMAT_BC7_UNORM}){
  D3D11_TEXTURE2D_DESC t={};t.Width=t.Height=64;t.MipLevels=7;t.ArraySize=1;t.Format=fmt;t.SampleDesc.Count=1;t.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture2D* texture=nullptr;HRESULT v=d->CreateTexture2D(&t,nullptr,&texture);
  fprintf(f,"CREATE format=%u hr=%08lx\n",unsigned(fmt),(unsigned long)v);fflush(f);
  if(!hr(v,"create 64x64 seven-mip BC"))continue;
  texture->Release();
 }
 for(auto fmt:{DXGI_FORMAT_BC1_TYPELESS,DXGI_FORMAT_BC4_TYPELESS,DXGI_FORMAT_BC5_TYPELESS,DXGI_FORMAT_BC6H_TYPELESS,DXGI_FORMAT_BC7_TYPELESS}){
  D3D11_TEXTURE3D_DESC t={};t.Width=t.Height=t.Depth=32;t.MipLevels=1;t.Format=fmt;t.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture3D* texture=nullptr;HRESULT v=d->CreateTexture3D(&t,nullptr,&texture);
  fprintf(f,"CREATE3D format=%u hr=%08lx\n",unsigned(fmt),(unsigned long)v);fflush(f);
  if(hr(v,"create 32-cubed typeless BC volume"))texture->Release();
 }
 {
  const unsigned short colors[]={0xf800,0x07e0,0x001f,0xffff};unsigned char data[32]={};
  for(unsigned z=0;z<4;++z){data[z*8]=data[z*8+2]=colors[z]&255;data[z*8+1]=data[z*8+3]=colors[z]>>8;}
  D3D11_TEXTURE3D_DESC td={};td.Width=td.Height=td.Depth=4;td.MipLevels=1;td.Format=DXGI_FORMAT_BC1_TYPELESS;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init={data,8,8};ID3D11Texture3D* tex=nullptr;
  if(hr(d->CreateTexture3D(&td,&init,&tex),"BC1 volume upload")){
   ID3D11ShaderResourceView* sv=nullptr;ID3D11ComputeShader* shader=nullptr;ID3D11Buffer *output=nullptr,*staging=nullptr;ID3D11UnorderedAccessView* uv=nullptr;
   D3D11_SHADER_RESOURCE_VIEW_DESC sd={};sd.Format=DXGI_FORMAT_BC1_UNORM;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE3D;sd.Texture3D.MipLevels=1;
   if(!hr(d->CreateShaderResourceView(tex,&sd,&sv),"volume SRV"))return 1;
   if(!hr(d->CreateComputeShader(bc_volume_dxbc,sizeof(bc_volume_dxbc),nullptr,&shader),"volume read shader"))return 1;
   D3D11_BUFFER_DESC bd={};bd.ByteWidth=256;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=16;
   if(!hr(d->CreateBuffer(&bd,nullptr,&output),"volume output"))return 1;
   D3D11_UNORDERED_ACCESS_VIEW_DESC ud={};ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=16;
   if(!hr(d->CreateUnorderedAccessView(output,&ud,&uv),"volume output UAV"))return 1;
   c->CSSetShader(shader,nullptr,0);c->CSSetShaderResources(0,1,&sv);c->CSSetUnorderedAccessViews(0,1,&uv,nullptr);c->Dispatch(4,4,1);c->ClearState();
   bd.Usage=D3D11_USAGE_STAGING;bd.BindFlags=0;bd.MiscFlags=0;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
   if(!hr(d->CreateBuffer(&bd,nullptr,&staging),"volume staging"))return 1;
   c->CopyResource(staging,output);D3D11_MAPPED_SUBRESOURCE map={};
   if(!hr(c->Map(staging,0,D3D11_MAP_READ,0,&map),"volume readback"))return 1;
   const float expected[4][4]={{1,0,0,1},{0,1,0,1},{0,0,1,1},{1,1,1,1}};auto actual=(float*)map.pData;unsigned errors=0;
   for(unsigned i=0;i<64;++i)if(!std::isfinite(actual[i])||std::fabs(actual[i]-expected[(i/4)%4][i%4])>.009f)++errors;
   fprintf(f,"VOLUME_SAMPLE errors=%u\n",errors);fflush(f);if(errors)++failures;
   c->Unmap(staging,0);staging->Release();uv->Release();output->Release();shader->Release();sv->Release();tex->Release();
  }
 }
 // Two channels with all selectors zero: R=64,G=192; negative G for SNORM.
 for(auto fmt:{DXGI_FORMAT_BC5_UNORM,DXGI_FORMAT_BC5_SNORM}){
  unsigned char data[16]={64,0,0,0,0,0,0,0,192,0,0,0,0,0,0,0};
  D3D11_TEXTURE2D_DESC t={};t.Width=t.Height=4;t.MipLevels=t.ArraySize=1;t.Format=fmt;t.SampleDesc.Count=1;t.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init={data,16,16};ID3D11Texture2D* tex=nullptr;
  if(!hr(d->CreateTexture2D(&t,&init,&tex),"BC5 upload"))continue;
  ID3D11ShaderResourceView* sv=nullptr;ID3D11ComputeShader* shader=nullptr;ID3D11Buffer *output=nullptr,*staging=nullptr;ID3D11UnorderedAccessView* uv=nullptr;
  if(!hr(d->CreateShaderResourceView(tex,nullptr,&sv),"BC5 SRV"))break;
  if(!hr(d->CreateComputeShader(reader_dxbc,sizeof(reader_dxbc),nullptr,&shader),"read shader"))break;
  D3D11_BUFFER_DESC bd={};bd.ByteWidth=256;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=16;
  if(!hr(d->CreateBuffer(&bd,nullptr,&output),"output"))break;
  D3D11_UNORDERED_ACCESS_VIEW_DESC ud={};ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=16;
  if(!hr(d->CreateUnorderedAccessView(output,&ud,&uv),"output UAV"))break;
  c->CSSetShader(shader,nullptr,0);c->CSSetShaderResources(0,1,&sv);c->CSSetUnorderedAccessViews(0,1,&uv,nullptr);c->Dispatch(4,4,1);c->ClearState();
  bd.Usage=D3D11_USAGE_STAGING;bd.BindFlags=0;bd.MiscFlags=0;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  if(!hr(d->CreateBuffer(&bd,nullptr,&staging),"staging"))break;
  c->CopyResource(staging,output);D3D11_MAPPED_SUBRESOURCE map={};
  if(!hr(c->Map(staging,0,D3D11_MAP_READ,0,&map),"readback"))break;
  const bool sn=fmt==DXGI_FORMAT_BC5_SNORM;float expected[4]={64.f/(sn?127.f:255.f),(sn?-64.f/127.f:192.f/255.f),0,1};float* actual=(float*)map.pData;unsigned errors=0;
  for(unsigned i=0;i<64;++i)if(!std::isfinite(actual[i])||std::fabs(actual[i]-expected[i%4])>.009f)++errors;
  fprintf(f,"SAMPLE format=%u errors=%u first=%f,%f,%f,%f\n",unsigned(fmt),errors,actual[0],actual[1],actual[2],actual[3]);fflush(f);if(errors)++failures;
  c->Unmap(staging,0);staging->Release();uv->Release();output->Release();shader->Release();sv->Release();tex->Release();
 }
 c->Release();d->Release();fprintf(f,"complete=1 failures=%u\n",failures);fclose(f);return failures?1:0;
}
