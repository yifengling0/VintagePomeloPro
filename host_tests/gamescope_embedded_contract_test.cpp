#include "EmbeddedContract.h"
#include <cassert>
#include <limits>

int main() {
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
    f.sharpness=21; assert(!VppGamescopeValidFrame(f));
    f.sharpness=0; f.size=0; assert(!VppGamescopeValidFrame(f));
}
