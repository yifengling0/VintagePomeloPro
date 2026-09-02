cmake_minimum_required(VERSION 3.5.0)
project(WineHua)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(
    ${NATIVERENDER_ROOT_PATH}
    ${NATIVERENDER_ROOT_PATH}/protocols
    ${NATIVERENDER_ROOT_PATH}/compositor/frame
    ${NATIVERENDER_ROOT_PATH}/compositor/toplevel
    ${NATIVERENDER_ROOT_PATH}/compositor/input
)

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

# 预编译 ARM64 wayland-server (放在 libs/ 下，hvigor 自动打包)
set(WAYLAND_LIBS_DIR ${NATIVERENDER_ROOT_PATH}/../../../libs/${OHOS_ARCH})

add_library(entry SHARED
    common/app_log.cpp
    audio/audio_broker.cpp
    audio/audio_ipc_server.cpp
    audio/audio_pcm_metrics.cpp
    audio/audio_stream.cpp
    graphics/graphics_broker.cpp
    graphics/graphics_profile.cpp
    input/game_controller_bridge.cpp
    input/controller/controller_hub.cpp
    input/controller/controller_napi.cpp
    input/controller/controller_runtime.cpp
    input/controller/gamepad_bridge.cpp
    input/controller/physical_gamepad.cpp
    graphics/virgl_host_config.cpp
    bridge/napi_init.cpp
    wine/experiment_payload.cpp
    proc/wine_process.cpp
    wine/wine_env.cpp
    wine/env_spec.cpp
    wine/env_profiles.cpp
    proc/spawner.cpp
    wine/wine_launch.cpp
    wine/wine_exe.cpp
    common/font_zip.cpp
    wine/wine_mmap_test.cpp
    graphics/host_vulkan_probe.cpp
    proc/broker.cpp
    proc/ncp_dispatch.cpp
    common/perf_utils.cpp
    common/performance_monitor.cpp
    bridge/performance_monitor_napi.cpp
    phone_adapter/phone_process.cpp
    phone_adapter/phone_virgl_relay.cpp
    compositor/wayland_server.cpp
    compositor/wl_core.cpp
    compositor/frame/compositor_blit.cpp
    compositor/toplevel/toplevel_manager.cpp
    compositor/toplevel/popup_manager.cpp
    compositor/toplevel/move_grab.cpp
    compositor/frame/compositor_utils.cpp
    compositor/frame/geometry.cpp
    compositor/toplevel/desktop_compositor.cpp
    compositor/frame/zc_bridge.cpp
    compositor/frame/frame_composer.cpp
    compositor/frame/frame_pipeline.cpp
    compositor/frame/shm_frame_source.cpp
    compositor/input/input_resolver.cpp
    compositor/input/input_space_mapper.cpp
    compositor/input/input_queue.cpp
    compositor/input/input_state_tracker.cpp
    compositor/input/input_injector.cpp
    compositor/toplevel/desktop_root_manager.cpp
    compositor/toplevel/toplevel_event_bus.cpp
    graphics/egl_renderer.cpp
    common/fs_utils.cpp
    bridge/plugin_manager.cpp
    common/process_utils.cpp
    common/ring_buffer.cpp
    input/seat.cpp
    graphics/shader_utils.cpp
    common/string_utils.cpp
    input/input_manager.cpp
    input/pointer_extras.cpp
    input/text_input.cpp
    protocols/text-input-unstable-v3-protocol.c
    compositor/xdg_shell.cpp
    protocols/xdg-shell-protocol.c
    protocols/viewporter-server-protocol.c
    protocols/pointer-warp-v1-protocol.c
    protocols/pointer-constraints-unstable-v1-protocol.c
    protocols/relative-pointer-unstable-v1-protocol.c
)

# 应用日志文件 sink：force-include app_log.h，让所有 OH_LOG_* 调用在保持
# hilog 行为不变的同时追加写入 Download/.../logs/native-YYYYMMDD.log。
target_compile_options(entry PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/common/app_log.h)
execute_process(
    COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR}/../../../.. rev-parse --short=12 HEAD
    OUTPUT_VARIABLE WINEHUA_AUDIO_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT WINEHUA_AUDIO_GIT_COMMIT)
    set(WINEHUA_AUDIO_GIT_COMMIT "unknown")
endif()
target_compile_definitions(entry PRIVATE WINEHUA_AUDIO_GIT_COMMIT="${WINEHUA_AUDIO_GIT_COMMIT}")
# BlitScaled 热路径单独 -O2，其余保持 -O0 便于调试。
set_source_files_properties(compositor/frame/compositor_blit.cpp PROPERTIES COMPILE_OPTIONS "-O2")

target_link_libraries(entry PUBLIC
    libohhidebug.so
    libace_napi.z.so
    libace_ndk.z.so
    libnative_window.so
    libnative_image.so
    libnative_vsync.so
    libEGL.so
    libGLESv3.so
    libvulkan.so
    libhilog_ndk.z.so
    libchild_process.so
    libipc_capi.so
    libohaudio.so
    libz.so
    libnative_window_manager.so
    ${WAYLAND_LIBS_DIR}/libwayland-server.so.0
)

add_library(wine_child SHARED
    proc/wine_child.cpp
    proc/wine_mmap_child.cpp
)
target_link_libraries(wine_child PUBLIC libhilog_ndk.z.so libchild_process.so)

add_library(virgl_child SHARED
    graphics/virgl_child.cpp
    graphics/virgl_host_config.cpp
    graphics/virgl_surface_presenter.cpp
    graphics/venus_surface_presenter.cpp
    graphics/native_window_vk_target.cpp
    graphics/native_window_gles_target.cpp
    graphics/shader_utils.cpp
    phone_adapter/phone_virgl_dispatch.cpp
)
target_link_libraries(virgl_child PUBLIC
    libEGL.so
    libGLESv3.so
    libvulkan.so
    libnative_window.so
    libnative_buffer.so
    libhilog_ndk.z.so
    libchild_process.so
    libipc_capi.so
)
