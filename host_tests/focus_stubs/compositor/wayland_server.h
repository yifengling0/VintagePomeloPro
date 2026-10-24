#pragma once
#include "compositor/input/input_resolver.h"
// Platform window movement and raising are outside the focus test. Input
// resolution, queuing, dispatch and injection use production implementations.
class WaylandServer {
public:
    using InputTarget = ::InputTarget;
    static WaylandServer* GetInstance() { static WaylandServer server; return &server; }
    void RaiseToplevel(uint32_t, bool) {}
    void SetToplevelRestored(uint32_t) {}
    int32_t moveX=0,moveY=0;
    bool ProcessMoveGrabMotion(int32_t x, int32_t y) {
        if (!movedHost) return false;
        moveX=x;moveY=y;return true;
    }
    void EndMoveGrab() { movedHost=0; }
    uint32_t movedHost = 0;
    void StartMoveGrabAt(uint32_t host, uint32_t, int32_t x,int32_t y) {
        movedHost=host;moveX=x;moveY=y;
    }
    uint32_t resizeEdges = 0;
    void StartExternalResizeGrabAt(uint32_t host, uint32_t serial, int32_t x, int32_t y, uint32_t edges) {
        StartMoveGrabAt(host,serial,x,y);resizeEdges=edges;
    }
};
