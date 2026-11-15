#!/usr/bin/env python3
"""Exercise real UpdateTexture mip selection and validation with recorded uploads."""
from pathlib import Path
import argparse,os,subprocess,tempfile

def function(text,marker):
    begin=text.index(marker);brace=text.index('{',begin);end=brace+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[begin:end]

def run(source):
    text=(source/'src/d3d9/d3d9_device.cpp').read_text()
    method=function(text,'  HRESULT STDMETHODCALLTYPE D3D9DeviceEx::UpdateTexture(')
    utils=(source/'src/dxvk/dxvk_util.h').read_text()
    extent=function(utils,'  inline VkExtent3D computeMipLevelExtent(VkExtent3D size, uint32_t level)')
    offset=function(utils,'  inline VkOffset3D computeMipLevelOffset(VkOffset3D offset, uint32_t level)')
    cpp=r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#define STDMETHODCALLTYPE
#define unlikely(x) (x)
using HRESULT=int;constexpr int D3DERR_INVALIDCALL=-1,D3D_OK=0;
constexpr int D3DPOOL_DEFAULT=0,D3DPOOL_SYSTEMMEM=2;
struct VkExtent3D {uint32_t width,height,depth;};
bool operator==(VkExtent3D a,VkExtent3D b){return a.width==b.width&&a.height==b.height&&a.depth==b.depth;}
bool operator!=(VkExtent3D a,VkExtent3D b){return !(a==b);}
struct VkOffset3D {int32_t x,y,z;};
struct D3DBOX {uint32_t Left,Top,Right,Bottom,Front,Back;};
namespace env {std::string getEnvVar(const char* name){auto p=std::getenv(name);return p?p:"";}}
namespace util {EXTENT
OFFSET
}
struct Desc {uint32_t MipLevels=8,ArraySize=1,Format=1;int Pool=D3DPOOL_SYSTEMMEM;};
struct DxvkImage{};template<class T>using Rc=std::shared_ptr<T>;
struct D3D9CommonTexture {
 ::Desc desc;VkExtent3D extent{128,128,1};bool automatic=false,cleared=false;
 std::vector<D3DBOX> boxes=std::vector<D3DBOX>(6,{0,0,128,128,0,1});
 std::vector<uint32_t> readbacks;
 const ::Desc* Desc()const{return &desc;}bool IsAutomaticMip()const{return automatic;}
 Rc<DxvkImage> GetImage()const{return {};}
 VkExtent3D GetExtent()const{return extent;}
 const D3DBOX& GetDirtyBox(uint32_t a)const{return boxes[a];}
 uint32_t CalcSubresource(uint32_t a,uint32_t mip)const{return a*desc.MipLevels+mip;}
 void SetNeedsReadback(uint32_t sub,bool v){assert(v);readbacks.push_back(sub);}
 void ClearDirtyBoxes(){cleared=true;}
};
using IDirect3DBaseTexture9=D3D9CommonTexture;
struct D3D9DeviceLock{};
struct Upload {uint32_t dst,src;VkOffset3D offset;VkExtent3D extent;};
struct D3D9DeviceEx {
 std::vector<Upload> uploads;int flushes=0,generated=0;
 D3D9DeviceLock LockDevice(){return {};}
 D3D9CommonTexture* GetCommonTexture(IDirect3DBaseTexture9* p){return p;}
 void UpdateTextureFromBuffer(D3D9CommonTexture*,D3D9CommonTexture*,uint32_t dst,uint32_t src,VkOffset3D offset,VkExtent3D extent,VkOffset3D dstOffset){
  assert(offset.x==dstOffset.x&&offset.y==dstOffset.y&&offset.z==dstOffset.z);uploads.push_back({dst,src,offset,extent});
 }
 void MarkTextureMipsDirty(D3D9CommonTexture*){generated++;}
 void FlushImplicit(bool strong){assert(!strong);flushes++;}
 HRESULT UpdateTexture(IDirect3DBaseTexture9*,IDirect3DBaseTexture9*);
};
METHOD
int main(){
 bool enabled=env::getEnvVar("WINEHUA_DXVK9_MATCH_TOP_MIPS")=="1";
 D3D9CommonTexture src,dst;dst.desc.Pool=D3DPOOL_DEFAULT;dst.desc.MipLevels=6;
 D3D9DeviceEx dev;
 auto hr=dev.UpdateTexture(&src,&dst);
 assert(hr==(enabled?D3D_OK:D3DERR_INVALIDCALL));
 assert(dev.uploads.size()==(enabled?6:0)&&src.cleared==enabled);
 if(enabled){for(unsigned i=0;i<6;i++)assert(dev.uploads[i].src==i&&dev.uploads[i].dst==i);}
 // Bottom-matching chains continue to use source mip 2, independent of the option.
 dev={};src.cleared=false;dst.extent={32,32,1};
 assert(dev.UpdateTexture(&src,&dst)==D3D_OK&&dev.uploads.size()==6);
 for(unsigned i=0;i<6;i++)assert(dev.uploads[i].src==i+2&&dev.uploads[i].dst==i);
 // Partial dirty regions scale from original source coordinates, including array slices.
 dev={};src.desc.ArraySize=dst.desc.ArraySize=2;src.boxes[0]={16,24,80,88,0,1};src.boxes[1]={0,0,0,0,0,1};
 assert(dev.UpdateTexture(&src,&dst)==D3D_OK&&dev.uploads.size()==6);
 assert(dev.uploads[0].offset.x==4&&dev.uploads[0].offset.y==6&&dev.uploads[0].extent.width==16);
 src.boxes[1]={0,0,128,128,0,1};dev={};assert(dev.UpdateTexture(&src,&dst)==D3D_OK&&dev.uploads.size()==12);
 assert(dev.uploads[6].src==10&&dev.uploads[6].dst==6);
 // Reject mismatches without clearing dirtiness or emitting uploads.
 src.desc.ArraySize=dst.desc.ArraySize=1;
 auto rejected=[&](){dev={};src.cleared=false;assert(dev.UpdateTexture(&src,&dst)==D3DERR_INVALIDCALL&&dev.uploads.empty()&&!src.cleared);};
 dst.extent={64,64,1};rejected();dst.extent={32,32,1};
 dst.desc.Format=2;rejected();dst.desc.Format=1;
 src.desc.Pool=0;rejected();src.desc.Pool=2;
 dst.desc.Pool=2;rejected();dst.desc.Pool=0;
 src.desc.MipLevels=5;rejected();src.desc.MipLevels=8;
 src.automatic=true;rejected();src.automatic=false;
 assert(dev.UpdateTexture(nullptr,&dst)==D3DERR_INVALIDCALL&&dev.UpdateTexture(&src,nullptr)==D3DERR_INVALIDCALL);
 // Equal chains remain valid, and autogen destination updates just one matched top level.
 dst.extent={128,128,1};dst.desc.MipLevels=8;dev={};assert(dev.UpdateTexture(&src,&dst)==D3D_OK&&dev.uploads.size()==8);
 dst.automatic=true;dev={};hr=dev.UpdateTexture(&src,&dst);
 assert(hr==(enabled?D3D_OK:D3DERR_INVALIDCALL));
 if(enabled)assert(dev.uploads.size()==1&&dev.uploads[0].src==0&&dev.generated==1&&dev.flushes==1);
}
""".replace('\nMETHOD\n','\n'+method+'\n').replace('EXTENT',extent).replace('OFFSET',offset)
    with tempfile.TemporaryDirectory(prefix='dxvk-mip-contract-') as folder:
        p=Path(folder);file=p/'test.cpp';file.write_text(cpp)
        command=['g++','-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-but-set-variable','-fsanitize=address,undefined',str(file),'-o',str(p/'test')]
        subprocess.run(command,check=True)
        for value in ('0','1'):
            subprocess.run([str(p/'test')],env={**os.environ,'WINEHUA_DXVK9_MATCH_TOP_MIPS':value},check=True)
        # The actual old bottom-only selector must fail the top-matching assertion.
        mutant=cpp.replace('matchTopMips && srcTexInfo->GetExtent() == dstFirstMipExtent','matchTopMips && false')
        assert mutant!=cpp;file.write_text(mutant);subprocess.run(command,check=True)
        failure=subprocess.run([str(p/'test')],env={**os.environ,'WINEHUA_DXVK9_MATCH_TOP_MIPS':'1'},capture_output=True)
        assert failure.returncode!=0 and b'Assertion' in failure.stderr,failure.stderr
    print('PASS: production UpdateTexture and mip helpers; default-off, top/bottom/equal/autogen chains, partial dirty boxes, array slices, pool/format/count/extent rejection; ASAN/UBSAN; bottom-only mutation rejected')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]/'thirdparty/dxvk');run(parser.parse_args().source)
