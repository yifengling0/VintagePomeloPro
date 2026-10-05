#pragma once
#include "compositor/input/input_resolver.h"
// Platform window movement and raising are outside the focus test. Input
// resolution, queuing, dispatch and injection use production implementations.
class WaylandServer {
public:
    using InputTarget = ::InputTarget;
    static WaylandServer* GetInstance() { static WaylandServer server; return &server; }
    void RaiseToplevel(uint32_t, bool) {}
    bool ProcessMoveGrabMotion(int32_t, int32_t) { return false; }
    void EndMoveGrab() {}
};
