"""Replay actual MF texture allocation block with a failing D3D device."""
from pathlib import Path
import argparse,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,required=True);a=p.parse_args()
s=(a.source/'dlls/mfplat/sample.c').read_text()
start=s.index('            hr = ID3D11Device_CreateTexture2D(')
end=s.index('\n        }\n        else',start)
code=r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FAILED(hr) ((hr)<0)
#define SUCCEEDED(hr) ((hr)>=0)
#define TRACE(...) ((void)0)
#define D3D11_RESOURCE_MISC_SHARED 1
#define D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX 2
#define FALSE 0
typedef void IUnknown;
typedef void ID3D11Texture2D;
static int IID_ID3D11Texture2D;
static unsigned calls,releases,surfaces,flags[2],retryOk=1;
struct desc {unsigned MiscFlags;unsigned width;};
static int ID3D11Device_CreateTexture2D(void *d,struct desc *v,void *init,ID3D11Texture2D **out){
 assert(!d&&!init&&v->width==123);flags[calls++]=v->MiscFlags;
 if ((v->MiscFlags&3)||!retryOk)return -1;
 *out=(void*)1;return 0;
}
static int MFCreateDXGISurfaceBuffer(int *iid,IUnknown *t,int n,int b,void **out){
 assert(iid==&IID_ID3D11Texture2D&&t==(void*)1&&!n&&!b);++surfaces;*out=(void*)2;return 0;
}
static void ID3D11Texture2D_Release(ID3D11Texture2D *t){assert(t==(void*)1);++releases;}
static void test(const char *value,unsigned misc,unsigned expectedCalls,unsigned expectedSurfaces){
 if(value)setenv("WINEHUA_MF_FALLBACK_NONSHARED",value,1);else unsetenv("WINEHUA_MF_FALLBACK_NONSHARED");
 calls=releases=surfaces=0;struct desc desc={misc,123};ID3D11Texture2D *texture;void *buffer=0;
 struct {void *d3d11_device;} record={0},*service=&record;int hr;
"""+s[start:end]+r"""
 assert(calls==expectedCalls&&surfaces==expectedSurfaces&&releases==expectedSurfaces);
 assert((surfaces!=0)==SUCCEEDED(hr));if(calls==2)assert(flags[1]==(misc&~3));
}
int main(void){
 for(unsigned misc=1;misc<=3;++misc){
  for(unsigned i=0;i<6;++i){const char *v[]={NULL,"","0","true","01","1x"};test(v[i],misc|8,1,0);}
  test("1",misc|8,2,1);retryOk=0;test("1",misc|8,2,0);retryOk=1;
 }
 test("1",8,1,1);test(NULL,8,1,1);test("0",8,1,1);
 puts("MF allocation: 27 fault scenarios PASS (default, exact opt-in, flags, retry failure, release)");
}
"""
with tempfile.TemporaryDirectory(prefix='mf-retry-') as d:
 d=Path(d);(d/'test.c').write_text(code)
 subprocess.run(['cc','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror',str(d/'test.c'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
