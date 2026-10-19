#include "compositor/subsurface_resource.h"
#include <wayland-server-protocol.h>
#include <cassert>
#include <cstdio>
#include <sys/socket.h>
#include <unistd.h>

int main() {
    for(int ordering=0;ordering<3;++ordering) {
        int sockets[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
        auto* display=wl_display_create();assert(display);
        auto* client=wl_client_create(display,sockets[0]);assert(client);
        auto* child=wl_resource_create(client,&wl_surface_interface,1,0);assert(child);
        auto* role=wl_resource_create(client,&wl_subsurface_interface,1,0);assert(role);
        wl_resource_set_implementation(role,nullptr,new SubsurfaceResource(child),[](wl_resource* r) {
            delete static_cast<SubsurfaceResource*>(wl_resource_get_user_data(r));
        });
        assert(SubsurfaceChild(role)==child);
        if(ordering==0) {
            wl_resource_destroy(child);
            assert(!SubsurfaceChild(role));
            wl_resource_destroy(role);
        } else if(ordering==1) {
            wl_resource_destroy(role);
            wl_resource_destroy(child);
        }
        wl_client_destroy(client);wl_display_destroy(display);close(sockets[1]);
    }
    std::puts("PASS real libwayland child-first, role-first and client-disconnect destruction");
}
