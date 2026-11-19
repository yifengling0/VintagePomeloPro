#include "EmbeddedContract.h"
#include <cassert>
#include <limits>

int main() {
    {
        VppGamescopeLayer scene[4];
        const uint32_t pixel=0xffffffff;
        scene[0].pixels=&pixel; scene[0].w=1920; scene[0].h=1200;
        scene[1].native=reinterpret_cast<void*>(1); scene[1].upscale=1;
        scene[1].width=800; scene[1].height=600; scene[1].w=1600; scene[1].h=1200;
        scene[2]=scene[1]; scene[2].w=320; scene[2].h=200;
        scene[3]=scene[0]; // SHM menu above the client must remain an overlay.
        VppGamescopeFrame frame; frame.width=1920; frame.height=1200;
        frame.layers=scene; frame.count=4;
        assert(VppGamescopeFrameGenerationLayer(frame)==1);
        scene[1].x=-1700; // wholly off-screen client cannot steal interpolation.
        assert(VppGamescopeFrameGenerationLayer(frame)==2);
        scene[2].opaque=0; // genuinely transparent GPU content stays unchanged.
        assert(VppGamescopeFrameGenerationLayer(frame)==4);
        scene[1].x=0; scene[1].upscale=0;
        assert(VppGamescopeFrameGenerationLayer(frame)==4);
    }
    VppGamescopeLayer layers[2];
    layers[0].native=reinterpret_cast<void*>(1);
    layers[0].width=800; layers[0].height=600; layers[0].w=1600; layers[0].h=1200;
    VppGamescopeFrame f; f.width=1920; f.height=1200; f.layers=layers; f.count=1; f.filter=3;
    assert(VppGamescopeValidFrame(f));
    layers[0].v0=1; layers[0].v2=-1; assert(VppGamescopeValidFrame(f)); // GLES
    layers[0].u0=0.05f; layers[0].u1=0.9f; assert(VppGamescopeValidFrame(f)); // client crop
    layers[0].u1=1; assert(!VppGamescopeValidFrame(f));
    layers[0].u0=0; layers[0].x=std::numeric_limits<float>::quiet_NaN(); assert(!VppGamescopeValidFrame(f));
    layers[0].x=0; layers[0].w=std::numeric_limits<float>::infinity(); assert(!VppGamescopeValidFrame(f));
    layers[0].w=1600; layers[1]=layers[0]; f.count=2; assert(!VppGamescopeValidFrame(f)); // duplicate ownership
    const uint32_t popup=0xffffffff;
    layers[1].native=nullptr; layers[1].pixels=&popup; layers[1].width=layers[1].height=1;
    assert(!VppGamescopeValidFrame(f)); // truncated SHM before GPU commands
    layers[1].pixelBytes=4; assert(VppGamescopeValidFrame(f));
    layers[1].width=2400; layers[1].height=1600; layers[1].pixelBytes=2400u*1600*4;
    assert(VppGamescopeValidFrame(f)); // Mini desktop exceeds the old 1080p arena
    layers[1].width=4096; layers[1].height=2048; layers[1].pixelBytes=32u*1024*1024;
    assert(VppGamescopeValidFrame(f));
    layers[1].height=2049; layers[1].pixelBytes+=4096u*4;
    assert(!VppGamescopeValidFrame(f));
    layers[1].height=2048; layers[1].pixelBytes=32u*1024*1024;
    f.sharpness=21; assert(!VppGamescopeValidFrame(f));
    f.sharpness=0; f.size=0; assert(!VppGamescopeValidFrame(f));
}
