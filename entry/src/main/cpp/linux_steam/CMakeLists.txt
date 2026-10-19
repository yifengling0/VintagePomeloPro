cmake_minimum_required(VERSION 3.16)

# Opt-in host diagnostics; no GNU ELF or Linux execution backend is included.
add_library(steamohos_host STATIC
    host_v1/wayland_probe.cpp
    host_v1/winehua_host.cpp
    surface_probe.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../protocols/xdg-shell-protocol.c)
target_compile_features(steamohos_host PUBLIC cxx_std_17)
target_include_directories(steamohos_host PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/host_v1
    ${CMAKE_CURRENT_LIST_DIR}/..)
set_target_properties(steamohos_host PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(steamohos_host PRIVATE -Wall -Wextra -Werror)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(steamohos_host PRIVATE -Wno-unused-command-line-argument)
endif()
target_link_libraries(steamohos_host PUBLIC ${STEAMOHOS_WAYLAND_CLIENT_LIBRARY})
