#pragma once
#include <stdint.h>

// Host ABI only. No host address or function pointer is exposed to the guest CPU.
#ifdef __cplusplus
extern "C" {
#endif
enum SteamOhosGraphicsStatusV1 {
    STEAMOHOS_GRAPHICS_VERIFIED = 0,
    STEAMOHOS_GRAPHICS_ABI = 1,
    STEAMOHOS_GRAPHICS_CONNECT = 2,
    STEAMOHOS_GRAPHICS_TIMEOUT = 3,
    STEAMOHOS_GRAPHICS_TRANSPORT = 4,
    STEAMOHOS_GRAPHICS_PROTOCOL = 5,
    STEAMOHOS_GRAPHICS_MISSING_GLOBALS = 6
};
enum SteamOhosWaylandGlobalV1 {
    STEAMOHOS_WL_COMPOSITOR = 0, STEAMOHOS_WL_SHM, STEAMOHOS_XDG_WM_BASE,
    STEAMOHOS_WL_SEAT, STEAMOHOS_WL_OUTPUT, STEAMOHOS_WL_SUBCOMPOSITOR,
    STEAMOHOS_WP_VIEWPORTER, STEAMOHOS_XWAYLAND_SHELL, STEAMOHOS_WL_GLOBAL_COUNT
};
typedef struct SteamOhosGraphicsHostV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    void* context;
    // Return a fresh connected AF_UNIX/SOCK_STREAM fd, or -1. The probe owns
    // and closes a successful fd. Bound the entire callback by timeout_ms.
    int (*open_wayland)(void* context, uint32_t timeout_ms);
} SteamOhosGraphicsHostV1;
typedef struct SteamOhosGraphicsProbeV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t status;
    uint32_t globals_seen;
    uint32_t versions[STEAMOHOS_WL_GLOBAL_COUNT];
    uint32_t sync_received;
    // Registry discovery proves none of these later stages.
    uint32_t window_configured;
    uint32_t frame_presented;
    uint32_t input_verified;
    uint32_t steam_client_started;
} SteamOhosGraphicsProbeV1;
int steamohos_probe_graphics_host_v1(const SteamOhosGraphicsHostV1* host,
                                    SteamOhosGraphicsProbeV1* result, uint32_t timeout_ms);
#ifdef __cplusplus
}
#endif
