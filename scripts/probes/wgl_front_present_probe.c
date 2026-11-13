/* Default-framebuffer presentation, deliberately without pixel readback.
 * --preflush reproduces applications that flush before SwapBuffers. The
 * display must track red -> green -> blue -> white in each window. */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>

static LRESULT CALLBACK proc(HWND window, UINT msg, WPARAM wp, LPARAM lp)
{ return DefWindowProcA(window, msg, wp, lp); }

int main(int argc, char **argv)
{
    int preflush = 0, single = 0, rgb16 = 0;
    for (int i=2; i<argc; ++i) {
        preflush |= !strcmp(argv[i], "--preflush");
        single |= !strcmp(argv[i], "--singlebuffer");
        rgb16 |= !strcmp(argv[i], "--rgb16");
    }
    FILE *out = fopen(argc > 1 ? argv[1] : "C:\\vp-front-present.tsv", "w");
    if (!out) return 1;
    setvbuf(out, NULL, _IONBF, 0);
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WineHuaFrontPresentProbe";
    RegisterClassA(&wc);
    HWND windows[2]; HDC dcs[2]; HGLRC contexts[2];
    for (int i=0; i<2; ++i) {
        windows[i] = CreateWindowA(wc.lpszClassName, i ? "Front probe B" : "Front probe A",
            WS_OVERLAPPEDWINDOW, 30+360*i, 70, 340, 300, NULL, NULL, wc.hInstance, NULL);
        dcs[i] = GetDC(windows[i]);
        PIXELFORMATDESCRIPTOR pf = {0}; pf.nSize = sizeof(pf); pf.nVersion = 1;
        pf.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | (single ? 0 : PFD_DOUBLEBUFFER);
        pf.iPixelType = PFD_TYPE_RGBA; pf.cColorBits = rgb16 ? 16 : 24; pf.cAlphaBits = rgb16 ? 0 : 8;
        int format = ChoosePixelFormat(dcs[i], &pf);
        if (!format || !SetPixelFormat(dcs[i], format, &pf) ||
            !(contexts[i]=wglCreateContext(dcs[i]))) return 2;
        DescribePixelFormat(dcs[i], format, sizeof(pf), &pf);
        fprintf(out, "format\twindow=%d\tbits=%u\tdouble=%d\n",i,pf.cColorBits,!!(pf.dwFlags&PFD_DOUBLEBUFFER));
        ShowWindow(windows[i], SW_SHOW);
    }
    float colors[4][4] = {{1,0,0,1},{0,1,0,1},{0,0,1,1},{1,1,1,1}};
    fprintf(out, "pointer_bits\t%u\npreflush\t%d\n", (unsigned)(sizeof(void*)*8), preflush);
    for (int phase=0; phase<4; ++phase) {
        fprintf(out, "phase\t%d\tstart_ms=%lu\n", phase, GetTickCount());
        for (int frame=0; frame<180; ++frame) {
            MSG msg;
            while (PeekMessageA(&msg,NULL,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
            for (int i=0; i<2; ++i) {
                if (!frame) fprintf(out,"stage\tphase=%d\twindow=%d\tmake_current\n",phase,i);
                if (!wglMakeCurrent(dcs[i],contexts[i])) return 3;
                if (!phase && !frame) fprintf(out,"gl\twindow=%d\tversion=%s\trenderer=%s\n",i,glGetString(GL_VERSION),glGetString(GL_RENDERER));
                glClearColor(colors[(phase+i)%4][0],colors[(phase+i)%4][1],colors[(phase+i)%4][2],1);
                glClear(GL_COLOR_BUFFER_BIT);
                if (!frame) fprintf(out,"stage\tphase=%d\twindow=%d\tclear_done\n",phase,i);
                if (preflush) glFlush();
                if (!frame) fprintf(out,"stage\tphase=%d\twindow=%d\tflush_done\n",phase,i);
                if (!SwapBuffers(dcs[i])) return 4;
                if (!frame) fprintf(out,"stage\tphase=%d\twindow=%d\tswap_done\n",phase,i);
            }
            Sleep(16);
        }
        fprintf(out, "phase_done\t%d\n", phase);
    }
    fprintf(out, "RESULT\tPASS_API_ONLY\n");
    Sleep(5000);
    for (int i=0; i<2; ++i) { wglMakeCurrent(NULL,NULL); wglDeleteContext(contexts[i]); ReleaseDC(windows[i],dcs[i]); DestroyWindow(windows[i]); }
    fclose(out);
    return 0;
}
