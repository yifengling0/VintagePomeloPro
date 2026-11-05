#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <stdexcept>
#include "query_shaders.h"
static void check(HRESULT hr) {if(FAILED(hr))throw std::runtime_error("D3D call failed");}
int main() {
 FILE*f=fopen("C:\\occlusion-result.txt","wb");if(!f)return 10;setvbuf(f,nullptr,_IONBF,0);
 try {
 ID3D11Device*d=nullptr;ID3D11DeviceContext*c=nullptr;D3D_FEATURE_LEVEL fl;
 check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,&fl,&c));
 ID3D11VertexShader*vs;ID3D11PixelShader*ps;
 check(d->CreateVertexShader(query_vs,sizeof(query_vs),nullptr,&vs));check(d->CreatePixelShader(query_ps,sizeof(query_ps),nullptr,&ps));
 c->VSSetShader(vs,nullptr,0);c->PSSetShader(ps,nullptr,0);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 D3D11_TEXTURE2D_DESC td={};td.Width=64;td.Height=64;td.MipLevels=td.ArraySize=1;td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;
 ID3D11Texture2D*rt;ID3D11RenderTargetView*rtv;check(d->CreateTexture2D(&td,nullptr,&rt));check(d->CreateRenderTargetView(rt,nullptr,&rtv));
 td.Format=DXGI_FORMAT_D32_FLOAT;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;ID3D11Texture2D*depth;ID3D11DepthStencilView*dsv;check(d->CreateTexture2D(&td,nullptr,&depth));check(d->CreateDepthStencilView(depth,nullptr,&dsv));
 c->OMSetRenderTargets(1,&rtv,dsv);D3D11_VIEWPORT viewport={0,0,64,64,0,1};c->RSSetViewports(1,&viewport);
 D3D11_RASTERIZER_DESC rd={};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;ID3D11RasterizerState*rs;check(d->CreateRasterizerState(&rd,&rs));c->RSSetState(rs);
 D3D11_DEPTH_STENCIL_DESC ds={};ds.DepthEnable=TRUE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ds.DepthFunc=D3D11_COMPARISON_LESS;ID3D11DepthStencilState*dss;check(d->CreateDepthStencilState(&ds,&dss));c->OMSetDepthStencilState(dss,0);
 D3D11_BUFFER_DESC bd={};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ID3D11Buffer*cb;check(d->CreateBuffer(&bd,nullptr,&cb));c->VSSetConstantBuffers(0,1,&cb);
 ID3D11Buffer*staticCb[3];for(unsigned i=0;i<3;i++){float z[4]={i==0?.5f:i==1?.75f:.25f,0,0,0};D3D11_SUBRESOURCE_DATA init={z,0,0};check(d->CreateBuffer(&bd,&init,&staticCb[i]));}
 ID3D11Query*q[3];D3D11_QUERY_DESC qd={D3D11_QUERY_OCCLUSION,0};for(auto&v:q)check(d->CreateQuery(&qd,&v));
 ID3D11Query*done;D3D11_QUERY_DESC eq={D3D11_QUERY_EVENT,0};check(d->CreateQuery(&eq,&done));
 unsigned failures=0, pending=0, total=0;
 for(unsigned wait=0;wait<4;wait++) { unsigned startFailures=failures;
 for(unsigned frame=0;frame<128;frame++) {
   c->ClearDepthStencilView(dsv,D3D11_CLEAR_DEPTH,1,0);
   for(unsigned i=0;i<3;i++) {float z[4]={i==0?.5f:i==1?.75f:.25f,0,0,0};if(wait==3)c->VSSetConstantBuffers(0,1,&staticCb[i]);else {c->VSSetConstantBuffers(0,1,&cb);c->UpdateSubresource(cb,0,nullptr,z,0,0);}c->Begin(q[i]);c->Draw(3,0);c->End(q[i]);
    if(wait==2) {c->End(done);c->Flush();BOOL ready=FALSE;DWORD start=GetTickCount();while(c->GetData(done,&ready,sizeof(ready),0)==S_FALSE) {if(GetTickCount()-start>5000)throw std::runtime_error("phase timeout");Sleep(1);}}
   }
   c->End(done);c->Flush();
   if(wait) {BOOL ready=FALSE;DWORD start=GetTickCount();while(c->GetData(done,&ready,sizeof(ready),0)==S_FALSE) {if(GetTickCount()-start>5000)throw std::runtime_error("event timeout");Sleep(1);}}
   for(unsigned i=0;i<3;i++) {UINT64 value=~UINT64(0);HRESULT hr;DWORD start=GetTickCount();
     while((hr=c->GetData(q[i],&value,sizeof(value),0))==S_FALSE) {pending++;if(GetTickCount()-start>5000)throw std::runtime_error("query timeout");Sleep(1);}
     check(hr);UINT64 expected=i==1?0:4096;total++;
     if(value!=expected) {if(failures<20)fprintf(f,"FAIL frame=%u phase=%u expected=%llu actual=%llu\n",frame,i,expected,value);failures++;}
   }
 }
 fprintf(f,"eventWait=%u failures=%u\n",wait,failures-startFailures);}
 fprintf(f,"complete=1 queries=%u failures=%u pending=%u\n",total,failures,pending);fclose(f);return failures?1:0;
 }catch(const std::exception&e){fprintf(f,"FAIL %s\n",e.what());fclose(f);return 2;}
}
