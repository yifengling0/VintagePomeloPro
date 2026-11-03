#!/usr/bin/env python3
"""Execute the production caption wrapper, including allocation/blit failure."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
BUILD=ROOT/'build/host_tests/x11-caption'
BUILD.mkdir(parents=True,exist_ok=True)
s=(ROOT/'thirdparty/wine-valve/dlls/win32u/defwnd.c').read_text()
a=s.index('static void draw_nc_caption( HDC'); b=s.index('\n/***********************************************************************',a)
source=r"""
#define __OHOS__ 1
#define ERR(...) ((void)0)
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
using BOOL=int;using DWORD=unsigned;using HDC=int;using HBITMAP=int;using HWND=int;
struct RECT{int left,top,right,bottom;};
struct XFORM{float a,b,c,d;float x,y;};
enum{FALSE=0,TRUE=1,NtGdiGetLayout=1,NtGdiSetGraphicsMode=2,GM_ADVANCED=2,MWT_SET=3,SRCCOPY=4};
int fail=0,dc=0,bitmap=0,selected=0,draws=0,direct=0,blits=0;DWORD layout=0;
RECT expected{7,11,967,43};bool transformed=false;
BOOL NtGdiGetDCDword(HDC h,int,DWORD* p){assert(h==1);*p=layout;return fail!=1;}
HDC NtGdiCreateCompatibleDC(HDC h){assert(h==1);if(fail==2)return 0;++dc;return 2;}
HBITMAP NtGdiCreateCompatibleBitmap(HDC h,int w,int z){assert(h==1&&w==960&&z==32);if(fail==3)return 0;++bitmap;return 3;}
HBITMAP NtGdiSelectBitmap(HDC h,HBITMAP b){assert(h==2);if(b==3){if(fail==4)return 0;++selected;return 4;}assert(b==4&&selected==1);--selected;return 3;}
BOOL NtGdiGetAndSetDCDword(HDC h,int,int,DWORD* previous){assert(h==2);if(!previous)return false;*previous=1;return fail!=5;}
BOOL NtGdiModifyWorldTransform(HDC h,XFORM* t,int m){assert(h==2&&m==MWT_SET);assert(t->x==-7&&t->y==-11&&t->a==1&&t->d==1);transformed=true;return fail!=6;}
void draw_nc_caption_contents(HDC h,RECT* r,HWND,DWORD,DWORD,BOOL){assert(r->left==7&&r->top==11);++draws;if(h==1)++direct;else assert(h==2&&selected&&transformed);}
BOOL NtGdiBitBlt(HDC h,int x,int y,int w,int z,HDC m,int sx,int sy,int,int,int){assert(h==1&&m==2&&x==7&&y==11&&sx==x&&sy==y&&w==960&&z==32);assert(draws==1&&direct==0);++blits;return fail!=7;}
void NtGdiDeleteObjectApp(int h){if(h==3){assert(!selected);--bitmap;}else{assert(h==2&&!selected);--dc;}}
"""
source+="\n#define draw_nc_caption_text draw_nc_caption_contents\n"
source+=s[a:b]
source+=r"""
void reset(){assert(!dc&&!bitmap&&!selected);draws=direct=blits=0;transformed=false;layout=0;}
int main(){
 for(int f=0;f<=7;++f){reset();fail=f;draw_nc_caption(1,&expected,9,0,0,1,0);assert(!dc&&!bitmap&&!selected);
  assert(direct==(f!=0));assert(blits==(f==0||f==7));assert(draws==1+(f==7));}
 reset();fail=0;layout=1;draw_nc_caption(1,&expected,9,0,0,1,0);assert(direct==1&&!blits);
 reset();setenv("WINEHUA_CSD_SINGLE_BLIT","0",1);draw_nc_caption(1,&expected,9,0,0,1,0);assert(direct==1&&!blits);unsetenv("WINEHUA_CSD_SINGLE_BLIT");
 reset();expected.bottom=expected.top;draw_nc_caption(1,&expected,9,0,0,1,0);assert(direct==1&&!blits);
 reset();expected.bottom=expected.top+513;draw_nc_caption(1,&expected,9,0,0,1,0);assert(direct==1&&!blits);
 reset();expected.bottom=43;expected.right=7+16385;draw_nc_caption(1,&expected,9,0,0,1,0);assert(direct==1&&!blits);
 std::cout<<"PASS caption transaction: offset coordinates, single destination write, 7 failure fallbacks, resource lifetime, mirrored DC, bounds and diagnostic off\n";
}
"""
p=BUILD/'probe.cpp';p.write_text(source)
subprocess.run(['g++','-std=c++17','-Wno-narrowing','-Wall','-Wextra','-Werror',str(p),'-o',str(BUILD/'probe')],check=True)
subprocess.run([str(BUILD/'probe')],check=True,timeout=10)
