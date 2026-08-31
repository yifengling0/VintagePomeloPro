cmake_minimum_required(VERSION 3.5.0)
project(WineHua)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(
    ${NATIVERENDER_ROOT_PATH}
    ${NATIVERENDER_ROOT_PATH}/include
)

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

# 预编译 ARM64 wayland-server (放在 libs/ 下，hvigor 自动打包)
set(WAYLAND_LIBS_DIR ${NATIVERENDER_ROOT_PATH}/../../../libs/${OHOS_ARCH})

add_library(entry SHARED
    app_log.cpp
    audio_broker.cpp
    audio_ipc_server.cpp
    audio_stream.cpp
    graphics_broker.cpp
    graphics_profile.cpp
    game_controller_bridge.cpp
    controller/controller_hub.cpp
    controller/controller_napi.cpp
    controller/controller_runtime.cpp
    controller/gamepad_bridge.cpp
    controller/physical_gamepad.cpp
    virgl_host_config.cpp
    napi_init.cpp
    experiment_payload.cpp
    wine_process.cpp
    wine_env.cpp
    env_spec.cpp
    env_profiles.cpp
    spawner.cpp
    wine_launch.cpp
    wine_exe.cpp
    font_zip.cpp
    wine_mmap_test.cpp
    host_vulkan_probe.cpp
    broker.cpp
    ncp_dispatch.cpp
    perf_utils.cpp
    performance_monitor.cpp
    performance_monitor_napi.cpp
    phone_adapter/phone_process.cpp
    phone_adapter/phone_virgl_relay.cpp
    wayland_server.cpp
    wl_core.cpp
    compositor/compositor_blit.cpp
    compositor/toplevel_manager.cpp
    compositor/move_grab.cpp
    compositor/compositor_utils.cpp
    compositor/geometry.cpp
    compositor/desktop_compositor.cpp
    compositor/zc_bridge.cpp
    compositor/frame_composer.cpp
    compositor/frame_pipeline.cpp
    compositor/input_resolver.cpp
    compositor/input_space_mapper.cpp
    compositor/input_queue.cpp
    compositor/input_state_tracker.cpp
    compositor/input_injector.cpp
    compositor/desktop_root_manager.cpp
    egl_renderer.cpp
    fs_utils.cpp
    plugin_manager.cpp
    process_utils.cpp
    ring_buffer.cpp
    seat.cpp
    shader_utils.cpp
    string_utils.cpp
    input_manager.cpp
    pointer_extras.cpp
    text_input.cpp
    text-input-unstable-v3-protocol.c
    xdg_shell.cpp
    xdg-shell-protocol.c
    viewporter-server-protocol.c
    pointer-warp-v1-protocol.c
    pointer-constraints-unstable-v1-protocol.c
    relative-pointer-unstable-v1-protocol.c
)

# 应用日志文件 sink：force-include app_log.h，让所有 OH_LOG_* 调用在保持
# hilog 行为不变的同时追加写入 Download/.../logs/native-YYYYMMDD.log。
target_compile_options(entry PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/app_log.h)
# BlitScaled 热路径单独 -O2，其余保持 -O0 便于调试。
set_source_files_properties(compositor/compositor_blit.cpp PROPERTIES COMPILE_OPTIONS "-O2")

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
    wine_child.cpp
    wine_mmap_child.cpp
)
target_link_libraries(wine_child PUBLIC libhilog_ndk.z.so libchild_process.so)

add_library(virgl_child SHARED
    virgl_child.cpp
    virgl_host_config.cpp
    virgl_surface_presenter.cpp
    venus_surface_presenter.cpp
    native_window_vk_target.cpp
    native_window_gles_target.cpp
    shader_utils.cpp
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
