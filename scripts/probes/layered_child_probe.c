#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static void pump(void) { MSG m; while (PeekMessageA(&m,0,0,0,PM_REMOVE)) {TranslateMessage(&m); DispatchMessageA(&m);} }
static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l) {return DefWindowProcA(h,m,w,l);}
int main(void)
{
    FILE *f=fopen("C:\\vp-layered-child-result.tsv","wb"); if (!f) return 1;
    setvbuf(f,0,_IONBF,0);
    WNDCLASSA cls={0}; cls.lpfnWndProc=proc; cls.lpszClassName="VpLayeredChildProbe";
    cls.hbrBackground=(HBRUSH)(COLOR_WINDOW+1); RegisterClassA(&cls);
    HWND parent=CreateWindowExA(0,cls.lpszClassName,"vp:parent",WS_POPUP|WS_VISIBLE,40,80,400,260,0,0,0,0);
    HWND child=CreateWindowExA(WS_EX_LAYERED,cls.lpszClassName,"vp:visible-layered-child",WS_CHILD|WS_VISIBLE,30,40,96,72,parent,0,0,0);
    HWND hidden=CreateWindowExA(0,cls.lpszClassName,"vp:hidden-parent",WS_POPUP,600,200,180,150,0,0,0,0);
    HWND hidden_child=CreateWindowExA(WS_EX_LAYERED,cls.lpszClassName,"vp:hidden-layered-child",WS_CHILD|WS_VISIBLE,12,18,96,72,hidden,0,0,0);
    HDC dc=CreateCompatibleDC(0); BITMAPINFO bi={0}; bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=96; bi.bmiHeader.biHeight=-72; bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32;
    void *bits=0; HBITMAP bitmap=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&bits,0,0); HGDIOBJ old=SelectObject(dc,bitmap);
    for (unsigned i=0;i<96*72;i++) ((DWORD *)bits)[i]=0xff2080e0;
    SIZE size={96,72}; POINT source={0,0}; BLENDFUNCTION blend={AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    int failures=0;
    for (int n=0;n<5;n++) {
        RECT r; GetWindowRect(child,&r); POINT at={r.left,r.top};
        BOOL ok=UpdateLayeredWindow(child,0,&at,&size,dc,&source,0,&blend,ULW_ALPHA);
        GetWindowRect(child,&r);
        fprintf(f,"update\t%d\tok=%d\tchild=%ld,%ld,%ld,%ld\tvisible=%d\n",n,ok,r.left,r.top,r.right-r.left,r.bottom-r.top,IsWindowVisible(child));
        if (!ok || r.left!=70 || r.top!=120) failures++;
        GetWindowRect(hidden_child,&r); at=(POINT){r.left,r.top};
        ok=UpdateLayeredWindow(hidden_child,0,&at,&size,dc,&source,0,&blend,ULW_ALPHA);
        GetWindowRect(hidden_child,&r);
        fprintf(f,"hidden\t%d\tok=%d\tchild=%ld,%ld,%ld,%ld\tvisible=%d\n",n,ok,r.left,r.top,r.right-r.left,r.bottom-r.top,IsWindowVisible(hidden_child));
        if (!ok || r.left!=612 || r.top!=218 || IsWindowVisible(hidden_child)) failures++;
        pump(); Sleep(500);
    }
    for(int n=0;n<80;n++) {pump(); Sleep(100);}
    fprintf(f,"result\tfailures=%d\n",failures);
    DestroyWindow(parent); DestroyWindow(hidden); SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc); fclose(f);
    return failures?2:0;
}
