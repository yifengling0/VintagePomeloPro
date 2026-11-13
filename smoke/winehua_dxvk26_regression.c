#define COBJMACROS
#include "programs/common/winehua_t_check.h"
#include <d3d11.h>
#include <d3d10.h>
#include <d3dcompiler.h>
#include <vulkan/vulkan.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

static void transport_identity(void) {
 VkApplicationInfo app={0};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;app.apiVersion=VK_API_VERSION_1_3;
 VkInstanceCreateInfo ci={0};ci.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;ci.pApplicationInfo=&app;
 VkInstance instance=VK_NULL_HANDLE;VkResult r=vkCreateInstance(&ci,NULL,&instance);
 t_check("vulkan-identity-instance",r==VK_SUCCESS,"result=%d",r);if(r)return;
 VkPhysicalDevice physical;uint32_t count=1;r=vkEnumeratePhysicalDevices(instance,&count,&physical);
 if(r==VK_SUCCESS&&count) {
  VkPhysicalDeviceVulkan12Properties p12={0};p12.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
  VkPhysicalDeviceProperties2 p={0};p.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;p.pNext=&p12;
  vkGetPhysicalDeviceProperties2(physical,&p);
  t_metric("vulkan-device","%s",p.properties.deviceName);
  t_metric("vulkan-driver-id","%u",p12.driverID);
  t_metric("vulkan-driver-name","%s",p12.driverName);
 }else t_check("vulkan-identity-device",0,"result=%d count=%u",r,count);
 vkDestroyInstance(instance,NULL);
}
static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11Texture2D *rt, *readback;
static ID3D11RenderTargetView *rtv;
static ID3D11VertexShader *vs;
static ID3D11PixelShader *ps;
static int checked_hr(const char *name, HRESULT hr) {
 t_check(name,SUCCEEDED(hr),"hr=%08lx",(unsigned long)hr);
 return SUCCEEDED(hr);
}
static const char *vs_text =
 "float4 main(uint id:SV_VertexID):SV_Position {"
 "return float4(id==2?3:-1,id==1?3:-1,0,1);}";
static const char *ps_text = "float4 main():SV_Target { return float4(1,0,0,0); }";

static ID3DBlob *compile(const char *text, const char *target) {
 ID3DBlob *blob=NULL,*error=NULL;
 HRESULT hr=D3DCompile(text,strlen(text),NULL,NULL,NULL,"main",target,0,0,&blob,&error);
 t_check(target,SUCCEEDED(hr),"compile hr=%08lx %s",(unsigned long)hr,
   error?(const char*)ID3D10Blob_GetBufferPointer(error):"");
 if(error) ID3D10Blob_Release(error);
 return blob;
}
static ID3D11Texture2D *texture(DXGI_FORMAT format, UINT usage, UINT bind, UINT cpu,
 UINT layers, UINT misc, const D3D11_SUBRESOURCE_DATA *data) {
 D3D11_TEXTURE2D_DESC d={0}; ID3D11Texture2D *t=NULL;
 d.Width=d.Height=4; d.MipLevels=1; d.ArraySize=layers; d.Format=format;
 d.SampleDesc.Count=1; d.Usage=usage; d.BindFlags=bind; d.CPUAccessFlags=cpu; d.MiscFlags=misc;
 HRESULT hr=ID3D11Device_CreateTexture2D(dev,&d,data,&t);
 t_check("create-texture",SUCCEEDED(hr),"fmt=%u bind=%u cpu=%u hr=%08lx",format,bind,cpu,(unsigned long)hr);
 return t;
}
static float pixel(ID3D11Texture2D *source) {
 D3D11_MAPPED_SUBRESOURCE m; float f=NAN;
 ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource*)readback,(ID3D11Resource*)source);
 HRESULT hr=ID3D11DeviceContext_Map(ctx,(ID3D11Resource*)readback,0,D3D11_MAP_READ,0,&m);
 if(SUCCEEDED(hr)) {f=((float*)m.pData)[0]; ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)readback,0);}
 else t_check("readback-map",0,"hr=%08lx",(unsigned long)hr);
 return f;
}
static void check_float(const char *name,float f,float expected) {
 t_check(name,isfinite(f)&&fabsf(f-expected)<0.003f,"actual=%.7f expected=%.7f",f,expected);
}
static void test_memory(void) {
 float values[64]; for(int i=0;i<64;i++) values[i]=0.37f;
 D3D11_SUBRESOURCE_DATA data={values,64,256};
 ID3D11Texture2D *st=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_USAGE_STAGING,0,
   D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE,1,0,&data);
 if(!st) return;
 D3D11_MAPPED_SUBRESOURCE m;
 HRESULT hr=ID3D11DeviceContext_Map(ctx,(ID3D11Resource*)st,0,D3D11_MAP_READ,0,&m);
 if(checked_hr("initial-staging-map",hr)) {check_float("initial-staging-immediate-read",((float*)m.pData)[0],0.37f);
   ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)st,0);}
 for(int n=0;n<8;n++) {
   float expected=0.1f+n*0.07f;
   hr=ID3D11DeviceContext_Map(ctx,(ID3D11Resource*)st,0,D3D11_MAP_WRITE,0,&m);
   if(FAILED(hr)) {t_check("staging-write-map",0,"hr=%08lx",(unsigned long)hr);break;}
   for(int y=0;y<4;y++) for(int x=0;x<16;x++) ((float*)((char*)m.pData+y*m.RowPitch))[x]=expected;
   ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)st,0);
   ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource*)rt,(ID3D11Resource*)st);
   check_float("staging-write-gpu-readback",pixel(rt),expected);
 }
 ID3D11Texture2D_Release(st);
}
static void sample_to_rt(ID3D11ShaderResourceView *srv,ID3D11SamplerState *sampler,const char *text) {
 ID3DBlob *b=compile(text,"cs_5_0"); if(!b) return;
 ID3D11ComputeShader *cs=NULL; ID3D11UnorderedAccessView *uav=NULL;
 HRESULT hr=ID3D11Device_CreateComputeShader(dev,ID3D10Blob_GetBufferPointer(b),ID3D10Blob_GetBufferSize(b),NULL,&cs);
 ID3D10Blob_Release(b);
 if(FAILED(hr)) {t_check("create-cs",0,"hr=%08lx",(unsigned long)hr);return;}
 hr=ID3D11Device_CreateUnorderedAccessView(dev,(ID3D11Resource*)rt,NULL,&uav);
 if(FAILED(hr)) {t_check("create-uav",0,"hr=%08lx",(unsigned long)hr);ID3D11ComputeShader_Release(cs);return;}
 ID3D11DeviceContext_OMSetRenderTargets(ctx,0,NULL,NULL);
 ID3D11DeviceContext_CSSetShader(ctx,cs,NULL,0);
 ID3D11DeviceContext_CSSetShaderResources(ctx,0,1,&srv);
 ID3D11DeviceContext_CSSetSamplers(ctx,0,1,&sampler);
 ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx,0,1,&uav,NULL);
 ID3D11DeviceContext_Dispatch(ctx,1,1,1);
 ID3D11UnorderedAccessView *none=NULL; ID3D11ShaderResourceView *no_srv=NULL;
 ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx,0,1,&none,NULL);
 ID3D11DeviceContext_CSSetShaderResources(ctx,0,1,&no_srv);
 ID3D11UnorderedAccessView_Release(uav); ID3D11ComputeShader_Release(cs);
}
static void test_border_and_dynamic(void) {
 ID3D11Texture2D *input=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_USAGE_DYNAMIC,
   D3D11_BIND_SHADER_RESOURCE,D3D11_CPU_ACCESS_WRITE,1,0,NULL);
 ID3D11ShaderResourceView *srv=NULL; ID3D11SamplerState *sampler=NULL;
 if(!input) return;
 HRESULT hr=ID3D11Device_CreateShaderResourceView(dev,(ID3D11Resource*)input,NULL,&srv);
 if(!checked_hr("dynamic-srv",hr)) return;
 D3D11_SAMPLER_DESC d={0}; d.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
 d.AddressU=d.AddressV=d.AddressW=D3D11_TEXTURE_ADDRESS_BORDER; d.MaxLOD=1000;
 d.BorderColor[0]=d.BorderColor[1]=d.BorderColor[2]=0.9f; d.BorderColor[3]=1;
 hr=ID3D11Device_CreateSamplerState(dev,&d,&sampler); if(!checked_hr("border-sampler",hr)) return;
 for(int n=0;n<4;n++) {
   D3D11_MAPPED_SUBRESOURCE m; float expected=0.2f+n*0.1f;
   hr=ID3D11DeviceContext_Map(ctx,(ID3D11Resource*)input,0,D3D11_MAP_WRITE_DISCARD,0,&m);
   if(FAILED(hr)) {t_check("dynamic-map",0,"hr=%08lx",(unsigned long)hr);break;}
   for(int y=0;y<4;y++) for(int x=0;x<16;x++) ((float*)((char*)m.pData+y*m.RowPitch))[x]=expected;
   ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)input,0);
   sample_to_rt(srv,sampler,"Texture2D<float4> t:register(t0);SamplerState s:register(s0);"
     "RWTexture2D<float4> o:register(u0);[numthreads(1,1,1)]void main(){o[uint2(0,0)]=t.SampleLevel(s,float2(.5,.5),0);}");
   check_float("dynamic-texture-discard-visible",pixel(rt),expected);
 }
 sample_to_rt(srv,sampler,"Texture2D<float4> t:register(t0);SamplerState s:register(s0);"
   "RWTexture2D<float4> o:register(u0);[numthreads(1,1,1)]void main(){o[uint2(0,0)]=t.SampleLevel(s,float2(-2,-2),0);}");
 check_float("border-white-fallback-correction",pixel(rt),0.9f);
 ID3D11SamplerState_Release(sampler);ID3D11ShaderResourceView_Release(srv);ID3D11Texture2D_Release(input);
}
static void test_event(void) {
 D3D11_QUERY_DESC d={D3D11_QUERY_EVENT,0}; ID3D11Query *q=NULL;
 HRESULT hr=ID3D11Device_CreateQuery(dev,&d,&q); if(!checked_hr("event-query",hr)) return;
 int failed=0;
 for(int n=0;n<32;n++) {
   ID3D11DeviceContext_End(ctx,(ID3D11Asynchronous*)q); ID3D11DeviceContext_Flush(ctx);
   float color[4]={0.01f*n,0,0,1};
   ID3D11DeviceContext_ClearRenderTargetView(ctx,rtv,color);
   ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource*)readback,(ID3D11Resource*)rt);
   ID3D11DeviceContext_End(ctx,(ID3D11Asynchronous*)q); ID3D11DeviceContext_Flush(ctx);
   BOOL done=FALSE; DWORD start=GetTickCount();
   while((hr=ID3D11DeviceContext_GetData(ctx,(ID3D11Asynchronous*)q,&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH))==S_FALSE
      && GetTickCount()-start<10000) Sleep(1);
   if(hr!=S_OK||!done) {failed++;break;}
   D3D11_MAPPED_SUBRESOURCE m;
   hr=ID3D11DeviceContext_Map(ctx,(ID3D11Resource*)readback,0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&m);
   if(FAILED(hr)) {failed++;break;}
   if(fabsf(((float*)m.pData)[0]-color[0])>0.003f) failed++;
   ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)readback,0);
 }
 t_check("repeated-event-latest-gpu-work",!failed,"iterations=32 failures=%d hr=%08lx",failed,(unsigned long)hr);
 ID3D11DeviceContext *deferred=NULL; ID3D11CommandList *list=NULL;
 hr=ID3D11Device_CreateDeferredContext(dev,0,&deferred);
 if(checked_hr("event-deferred-context",hr)) {
   ID3D11DeviceContext_End(deferred,(ID3D11Asynchronous*)q);
   hr=ID3D11DeviceContext_FinishCommandList(deferred,FALSE,&list);
   if(checked_hr("event-command-list",hr)) {
     for(int i=0;i<32;i++) ID3D11DeviceContext_ExecuteCommandList(ctx,list,TRUE);
     ID3D11DeviceContext_Flush(ctx); BOOL done=FALSE;DWORD start=GetTickCount();
     while((hr=ID3D11DeviceContext_GetData(ctx,(ID3D11Asynchronous*)q,&done,sizeof(done),0))==S_FALSE&&GetTickCount()-start<10000)Sleep(1);
     t_check("deferred-event-replay",hr==S_OK&&done,"replays=32 hr=%08lx",(unsigned long)hr);
     ID3D11CommandList_Release(list);
   }
   ID3D11DeviceContext_Release(deferred);
 }
 ID3D11Query_Release(q);
}
static void test_a2c_and_divisor(void) {
 ID3D11DeviceContext_ClearState(ctx);
 D3D11_RASTERIZER_DESC raster={0};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
 ID3D11RasterizerState *rs=NULL;HRESULT raster_hr=ID3D11Device_CreateRasterizerState(dev,&raster,&rs);
 if(!checked_hr("raster-state",raster_hr))return;
 ID3D11DeviceContext_RSSetState(ctx,rs);ID3D11RasterizerState_Release(rs);
 D3D11_VIEWPORT vp={0,0,4,4,0,1};
 ID3D11DeviceContext_RSSetViewports(ctx,1,&vp);
 ID3D11DeviceContext_OMSetRenderTargets(ctx,1,&rtv,NULL);
 ID3D11DeviceContext_IASetPrimitiveTopology(ctx,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 ID3D11DeviceContext_VSSetShader(ctx,vs,NULL,0); ID3D11DeviceContext_PSSetShader(ctx,ps,NULL,0);
 D3D11_BLEND_DESC bd={0}; bd.AlphaToCoverageEnable=TRUE;bd.RenderTarget[0].RenderTargetWriteMask=15;
 ID3D11BlendState *blend=NULL;HRESULT hr=ID3D11Device_CreateBlendState(dev,&bd,&blend);
 if(checked_hr("a2c-blend-state",hr)) {
   float control[4]={0.23f,0,0,1};ID3D11DeviceContext_ClearRenderTargetView(ctx,rtv,control);
   ID3D11DeviceContext_Draw(ctx,3,0);
    float control_result=pixel(rt);check_float("a2c-control-draw-visible",control_result,1.0f);
    int draw_visible=isfinite(control_result)&&fabsf(control_result-1.0f)<0.003f;
   float color[4]={0.23f,0,0,1}; ID3D11DeviceContext_ClearRenderTargetView(ctx,rtv,color);
   ID3D11DeviceContext_OMSetBlendState(ctx,blend,NULL,~0u); ID3D11DeviceContext_Draw(ctx,3,0);
   if(draw_visible)check_float("single-sample-a2c-alpha-zero",pixel(rt),0.23f);
    else t_metric("unverified","single-sample-a2c-alpha-zero: draw control failed");
   ID3D11DeviceContext_OMSetBlendState(ctx,NULL,NULL,~0u);ID3D11BlendState_Release(blend);
 }
 const char *v="struct V{float4 p:SV_Position;float4 c:COLOR;};V main(uint id:SV_VertexID,float c:DATA){"
   "V o;o.p=float4(id==2?3:-1,id==1?3:-1,0,1);o.c=float4(c,0,0,1);return o;}";
 ID3DBlob *b=compile(v,"vs_5_0");if(!b)return;
 ID3D11VertexShader *ivs=NULL; ID3D11InputLayout *layout=NULL;
 D3D11_INPUT_ELEMENT_DESC e={"DATA",0,DXGI_FORMAT_R32_FLOAT,0,0,D3D11_INPUT_PER_INSTANCE_DATA,2};
 hr=ID3D11Device_CreateInputLayout(dev,&e,1,ID3D10Blob_GetBufferPointer(b),ID3D10Blob_GetBufferSize(b),&layout);
 if(SUCCEEDED(hr))hr=ID3D11Device_CreateVertexShader(dev,ID3D10Blob_GetBufferPointer(b),ID3D10Blob_GetBufferSize(b),NULL,&ivs);
 ID3D10Blob_Release(b);if(FAILED(hr)) {t_check("instance-layout",0,"hr=%08lx",(unsigned long)hr);return;}
 b=compile("float4 main(float4 p:SV_Position,float4 c:COLOR):SV_Target{return c;}","ps_5_0");if(!b)return;
 ID3D11PixelShader *ips=NULL;hr=ID3D11Device_CreatePixelShader(dev,ID3D10Blob_GetBufferPointer(b),ID3D10Blob_GetBufferSize(b),NULL,&ips);ID3D10Blob_Release(b);checked_hr("instance-pixel-shader",hr);
 float records[]={0.1f,0.2f,0.3f,0.4f,0.5f,0.6f,0.7f,0.8f};
 D3D11_BUFFER_DESC desc={0};desc.ByteWidth=sizeof(records);desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
 D3D11_SUBRESOURCE_DATA data={records,0,0};ID3D11Buffer *buffer=NULL;
 hr=ID3D11Device_CreateBuffer(dev,&desc,&data,&buffer);
 if(checked_hr("instance-buffer",hr)&&ips) {
   UINT stride=4,offset=0;ID3D11DeviceContext_IASetVertexBuffers(ctx,0,1,&buffer,&stride,&offset);
   ID3D11DeviceContext_IASetInputLayout(ctx,layout);ID3D11DeviceContext_VSSetShader(ctx,ivs,NULL,0);ID3D11DeviceContext_PSSetShader(ctx,ips,NULL,0);
   float sentinel[4]={-1,0,0,1};ID3D11DeviceContext_ClearRenderTargetView(ctx,rtv,sentinel);
   ID3D11DeviceContext_DrawInstanced(ctx,3,4,0,2);
   check_float("divisor-two-base-instance-two-gpu-buffer",pixel(rt),0.4f);
   ID3D11Buffer_Release(buffer);
 }
 if(ips) ID3D11PixelShader_Release(ips);ID3D11VertexShader_Release(ivs);ID3D11InputLayout_Release(layout);
}

static void test_cube_array(void) {
 float depth[12][16]; D3D11_SUBRESOURCE_DATA data[12];
 for(int face=0;face<12;face++) {
   for(int i=0;i<16;i++)depth[face][i]=(face&1)?0.75f:0.25f;
   data[face].pSysMem=depth[face];data[face].SysMemPitch=16;data[face].SysMemSlicePitch=64;
 }
 ID3D11Texture2D *cube=texture(DXGI_FORMAT_R32_TYPELESS,D3D11_USAGE_DEFAULT,
   D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE,0,12,D3D11_RESOURCE_MISC_TEXTURECUBE,data);
 if(!cube)return;
 D3D11_SHADER_RESOURCE_VIEW_DESC view={0};view.Format=DXGI_FORMAT_R32_FLOAT;
 view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;view.TextureCubeArray.MipLevels=1;view.TextureCubeArray.NumCubes=2;
 ID3D11ShaderResourceView *srv=NULL;HRESULT hr=ID3D11Device_CreateShaderResourceView(dev,(ID3D11Resource*)cube,&view,&srv);
 t_check("cube-array-srv",SUCCEEDED(hr),"hr=%08lx",(unsigned long)hr);if(FAILED(hr))return;
 D3D11_SAMPLER_DESC d={0};d.Filter=D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
 d.AddressU=d.AddressV=d.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;d.MaxLOD=1000;d.ComparisonFunc=D3D11_COMPARISON_LESS_EQUAL;
 ID3D11SamplerState *sampler=NULL;hr=ID3D11Device_CreateSamplerState(dev,&d,&sampler);if(!checked_hr("cube-comparison-sampler",hr))return;
 const char *shader="TextureCubeArray<float> t:register(t0);SamplerComparisonState s:register(s0);"
   "RWTexture2D<float4> o:register(u0);[numthreads(1,1,1)]void main(){"
   "uint w,h,c;t.GetDimensions(w,h,c);"
   "o[uint2(0,0)]=float4(t.SampleCmpLevelZero(s,float4(1,0,0,0),.5),"
   "t.SampleCmpLevelZero(s,float4(-1,0,0,1),.5),c,w);}";
 sample_to_rt(srv,sampler,shader);
 ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource*)readback,(ID3D11Resource*)rt);
 D3D11_MAPPED_SUBRESOURCE m;hr=ID3D11DeviceContext_Map(ctx,(ID3D11Resource*)readback,0,D3D11_MAP_READ,0,&m);
 if(checked_hr("cube-readback-map",hr)) {
   float *v=m.pData;t_check("cube-array-dref-and-dimensions",fabsf(v[0])<.001f&&fabsf(v[1]-1)<.001f&&v[2]==2&&v[3]==4,
     "values=%.3f,%.3f,%.3f,%.3f",v[0],v[1],v[2],v[3]);ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)readback,0);
 }
 ID3D11SamplerState_Release(sampler);ID3D11ShaderResourceView_Release(srv);ID3D11Texture2D_Release(cube);
}

static void test_snorm(void) {
 ID3D11Texture2D *snorm=texture(DXGI_FORMAT_R8G8B8A8_SNORM,D3D11_USAGE_DEFAULT,
   D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,0,1,0,NULL);
 if(!snorm)return;
 ID3D11RenderTargetView *view=NULL;
 HRESULT hr=ID3D11Device_CreateRenderTargetView(dev,(ID3D11Resource*)snorm,NULL,&view);
 if(checked_hr("snorm-render-view",hr)) {
   ID3D11Texture2D *packed=texture(DXGI_FORMAT_R8G8B8A8_SNORM,D3D11_USAGE_STAGING,0,D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE,1,0,NULL);
   if(packed) {
     float color[4]={-0.5f,0.5f,-1,1}; ID3D11DeviceContext_ClearRenderTargetView(ctx,view,color);
     ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource*)packed,(ID3D11Resource*)snorm);
     D3D11_MAPPED_SUBRESOURCE m;hr=ID3D11DeviceContext_Map(ctx,(ID3D11Resource*)packed,0,D3D11_MAP_READ,0,&m);
     if(SUCCEEDED(hr)) {
       int8_t *p=m.pData;t_check("snorm-negative-clear-readback",p[0]<=-63&&p[0]>=-64&&p[1]>=63&&p[1]<=64&&p[2]==-127&&p[3]==127,
         "bytes=%d,%d,%d,%d",p[0],p[1],p[2],p[3]);ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)packed,0);
     } else t_check("snorm-read-map",0,"hr=%08lx",(unsigned long)hr);
     hr=ID3D11DeviceContext_Map(ctx,(ID3D11Resource*)packed,0,D3D11_MAP_WRITE,0,&m);
     if(checked_hr("snorm-write-map",hr)) {
       for(int y=0;y<4;y++)for(int x=0;x<16;x++)((int8_t*)((char*)m.pData+y*m.RowPitch))[x]=-32;
       ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)packed,0);
       ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource*)snorm,(ID3D11Resource*)packed);
       ID3D11DeviceContext_CopyResource(ctx,(ID3D11Resource*)packed,(ID3D11Resource*)snorm);
       hr=ID3D11DeviceContext_Map(ctx,(ID3D11Resource*)packed,0,D3D11_MAP_READ,0,&m);
       if(checked_hr("snorm-roundtrip-map",hr)) {t_check("snorm-staging-upload-readback",((int8_t*)m.pData)[0]==-32,"byte=%d",((int8_t*)m.pData)[0]);ID3D11DeviceContext_Unmap(ctx,(ID3D11Resource*)packed,0);}
     }
     ID3D11Texture2D_Release(packed);
   }
   ID3D11RenderTargetView_Release(view);
 }
 ID3D11Texture2D_Release(snorm);
}
int main(int argc,char **argv) {
 t_begin("dxvk26-regression",argc,argv);
 t_metric("backend","%s",getenv("WINEHUA_D3D_BACKEND"));
 const char *keys[]={"WINEHUA_PERF_PROFILE","VN_WINEHUA_REMOTE_MEMORY_SYNC","VN_WINEHUA_PERSISTENT_MAP_SYNC","VKR_WINEHUA_SHADOW_FROM_HOST","DXVK_WINEHUA_BATCH_MAPPED_FLUSH","DXVK_WINEHUA_EMULATE_RGBA8_SNORM_RT","VK_DRIVER_FILES","DXVK_WINEHUA_LEGACY_READBACK_COPY","DXVK_DEBUG","DXVK_WINEHUA_READBACK_DEVICE_IDLE","WINEHUA_VULKAN_BACKEND","DXVK_WINEHUA_OWN_CACHED_BUFFERS"};
 for(unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);i++)t_metric(keys[i],"%s",getenv(keys[i])?getenv(keys[i]):"unset");
 SetEnvironmentVariableA("DXVK_LOG_LEVEL","info");
 SetEnvironmentVariableA("DXVK_WINEHUA_TRACE_MAPPED","1");
 transport_identity();
 HRESULT hr=D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,0,NULL,0,D3D11_SDK_VERSION,&dev,NULL,&ctx);
 t_check("d3d11-create",SUCCEEDED(hr),"hr=%08lx",(unsigned long)hr);if(FAILED(hr))return t_finish();
 rt=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS,0,1,0,NULL);
 readback=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,D3D11_USAGE_STAGING,0,D3D11_CPU_ACCESS_READ,1,0,NULL);
 if(!rt||!readback)return t_finish();
 hr=ID3D11Device_CreateRenderTargetView(dev,(ID3D11Resource*)rt,NULL,&rtv);
 if(!checked_hr("render-view",hr))return t_finish();
 ID3DBlob *b=compile(vs_text,"vs_5_0");if(!b)return t_finish();hr=ID3D11Device_CreateVertexShader(dev,ID3D10Blob_GetBufferPointer(b),ID3D10Blob_GetBufferSize(b),NULL,&vs);ID3D10Blob_Release(b);if(!checked_hr("vertex-shader",hr))return t_finish();
 b=compile(ps_text,"ps_5_0");if(!b)return t_finish();hr=ID3D11Device_CreatePixelShader(dev,ID3D10Blob_GetBufferPointer(b),ID3D10Blob_GetBufferSize(b),NULL,&ps);ID3D10Blob_Release(b);if(!checked_hr("pixel-shader",hr))return t_finish();
  float baseline[4]={0.625f,0.25f,0.5f,1.0f};
  ID3D11DeviceContext_ClearRenderTargetView(ctx,rtv,baseline);
  check_float("clear-copy-readback-baseline",pixel(rt),baseline[0]);
  test_memory();
  if(getenv("DXVK26_MEMORY_ONLY")) goto cleanup;
  test_border_and_dynamic();test_event();test_a2c_and_divisor();test_snorm();test_cube_array();
 ID3D10Device *d10=NULL;
 hr=D3D10CreateDevice(NULL,D3D10_DRIVER_TYPE_HARDWARE,NULL,0,D3D10_SDK_VERSION,&d10);
 t_check("builtin-d3d10-modern-dxgi-bridge",SUCCEEDED(hr)&&d10,"hr=%08lx",(unsigned long)hr);if(d10)ID3D10Device_Release(d10);
cleanup:
  ID3D11DeviceContext_ClearState(ctx);ID3D11VertexShader_Release(vs);ID3D11PixelShader_Release(ps);
 ID3D11RenderTargetView_Release(rtv);ID3D11Texture2D_Release(rt);ID3D11Texture2D_Release(readback);ID3D11DeviceContext_Release(ctx);ID3D11Device_Release(dev);
 return t_finish();
}
