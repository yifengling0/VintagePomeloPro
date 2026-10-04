#pragma once
// Only resource identity/user-data are mocked. Tests compile the production
// compositor, input resolver, toplevel manager and pixel blitter unchanged.
#include <cstdint>
struct wl_client;
struct wl_resource { void* userData = nullptr; };
inline void* wl_resource_get_user_data(wl_resource* resource) { return resource->userData; }
#include <sys/types.h>
void wl_client_get_credentials(wl_client*, pid_t*, uid_t*, gid_t*);
