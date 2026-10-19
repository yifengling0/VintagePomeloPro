#pragma once
#include "host.h"
#ifdef __cplusplus
extern "C" {
#endif
// Call on a host worker after WaylandServer::Start, using the host's gSockPath.
// Discovery opens a fresh connection; it does not start Wine or Steam.
int steamohos_winehua_probe_graphics_v1(const char* socket_path,SteamOhosGraphicsProbeV1* result,uint32_t timeout_ms);
#ifdef __cplusplus
}
#endif
