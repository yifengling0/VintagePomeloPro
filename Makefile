# Makefile — Wine for HarmonyOS 构建编排
#
# 用法:
#   make                                          # 默认: x86_64 全量构建
#   make NATIVE_ARCH=x86_64
#   make NATIVE_ARCH=arm64-v8a
#   make NATIVE_ARCH=all                          # 双架构 HAP
#
#   单个模块: make deps | wine | box64 | native | assemble | hap
#   清理:     make clean

ROOT := $(realpath $(dir $(lastword $(MAKEFILE_LIST))))
.DEFAULT_GOAL := all

# ── 配置 ──
NATIVE_ARCH ?= x86_64
GUEST_ARCH ?= x86_64
BUILD_GUEST_GFX ?= 1
BUILD_GUEST_VULKAN ?= 1
TARGET_SDK_VERSION ?= 6.1.0(23)
COMPATIBLE_SDK_VERSION ?= 6.1.0(23)
export NATIVE_ARCH
export GUEST_ARCH
export BUILD_GUEST_GFX
export BUILD_GUEST_VULKAN
export TARGET_SDK_VERSION
export COMPATIBLE_SDK_VERSION

CONFIG    := $(NATIVE_ARCH)
BUILD_DIR := $(ROOT)/build
STAMPS    := $(BUILD_DIR)/.stamps
SCRIPTS   := $(ROOT)/scripts
WINE_PATCH_DIR := $(ROOT)/patches/wine
DXVK_ARTIFACTS := \
	$(BUILD_DIR)/dxvk/legacy/x64/bin/d3d9.dll \
	$(BUILD_DIR)/dxvk/legacy/x64/bin/d3d10core.dll \
	$(BUILD_DIR)/dxvk/legacy/x64/bin/d3d10.dll \
	$(BUILD_DIR)/dxvk/legacy/x64/bin/d3d10_1.dll \
	$(BUILD_DIR)/dxvk/legacy/x64/bin/d3d11.dll \
	$(BUILD_DIR)/dxvk/legacy/x64/bin/dxgi.dll \
	$(BUILD_DIR)/dxvk/legacy/x86/bin/d3d9.dll \
	$(BUILD_DIR)/dxvk/legacy/x86/bin/d3d10core.dll \
	$(BUILD_DIR)/dxvk/legacy/x86/bin/d3d10.dll \
	$(BUILD_DIR)/dxvk/legacy/x86/bin/d3d10_1.dll \
	$(BUILD_DIR)/dxvk/legacy/x86/bin/d3d11.dll \
	$(BUILD_DIR)/dxvk/legacy/x86/bin/dxgi.dll
DXVK_MODERN_ARTIFACTS := \
	$(BUILD_DIR)/dxvk/modern-2.6/x64/bin/d3d11.dll \
	$(BUILD_DIR)/dxvk/modern-2.6/x64/bin/dxgi.dll \
	$(BUILD_DIR)/dxvk/modern-2.6/x86/bin/d3d11.dll \
	$(BUILD_DIR)/dxvk/modern-2.6/x86/bin/dxgi.dll
VKD3D_PROTON_ARTIFACTS := \
	$(BUILD_DIR)/vkd3d-proton/limited-500k/x64/d3d12.dll \
	$(BUILD_DIR)/vkd3d-proton/limited-500k/x64/winehua-d3d12-smoke.exe \
	$(BUILD_DIR)/vkd3d-proton/limited-500k/x64/triangle.exe \
	$(BUILD_DIR)/vkd3d-proton/limited-500k/x64/gears.exe \
	$(BUILD_DIR)/vkd3d-proton/limited-500k/manifest.json

# 架构列表 (NATIVE_ARCH=all 时展开为两个)
ifeq ($(NATIVE_ARCH),all)
ARCHES := arm64-v8a x86_64
else
ARCHES := $(NATIVE_ARCH)
endif

# ── 关键产物 (用于验证构建是否完成) ──
DEPS_SENTINEL   := $(BUILD_DIR)/sysroot-ext/usr/lib/x86_64-linux-ohos/libfreetype.so.6
WINE_SENTINEL   := $(BUILD_DIR)/wine-native/tools/winegcc/winegcc
GUEST_GFX_SENTINEL := $(BUILD_DIR)/guest_gfx/$(GUEST_ARCH)/winehua-guest-gfx.env
GUEST_VULKAN_SENTINEL := $(BUILD_DIR)/guest_vulkan/$(GUEST_ARCH)/manifest.json
HOST_VULKAN_SOURCE := $(ROOT)/smoke/venus_heaven_material_replay.c

# Guest runtime build scripts can also be invoked directly while iterating on
# Mesa/Venus. Track their manifests as assemble inputs so a subsequent
# `make hap` cannot silently reuse an older staged wine-data.zip.
ASSEMBLE_GUEST_INPUTS :=
ifeq ($(BUILD_GUEST_GFX),1)
ASSEMBLE_GUEST_INPUTS += $(wildcard $(GUEST_GFX_SENTINEL))
endif

# ============================================================
# dxvk — managed WineHua DXVK Legacy fork (x64 + x86)
# ============================================================
.PHONY: dxvk dxvk-cache-check
dxvk: dxvk-cache-check

dxvk-cache-check: $(SCRIPTS)/build_dxvk.sh $(SCRIPTS)/build_cache.sh $(SCRIPTS)/env.sh | $(STAMPS)
	@echo "=== dxvk legacy ==="
	bash $(SCRIPTS)/build_dxvk.sh

# dxvk-modern — WineHua DXVK 2.6.2 compatibility profile (x64 + x86)
# ============================================================
.PHONY: dxvk-modern dxvk-modern-cache-check
dxvk-modern: dxvk-modern-cache-check

dxvk-modern-cache-check: $(SCRIPTS)/build_dxvk_modern.sh $(SCRIPTS)/build_cache.sh $(SCRIPTS)/env.sh | $(STAMPS)
	@echo "=== dxvk modern 2.6 ==="
	bash $(SCRIPTS)/build_dxvk_modern.sh

$(DXVK_MODERN_ARTIFACTS): dxvk-modern-cache-check
	@test -s "$@" || { echo "ERROR: DXVK Modern artifact missing after build: $@" >&2; exit 1; }

# vkd3d-proton — x64-only, explicit, default-off 2.6 limited-500K profile
# ============================================================
.PHONY: vkd3d-proton vkd3d-proton-cache-check
vkd3d-proton: vkd3d-proton-cache-check

vkd3d-proton-cache-check: $(SCRIPTS)/build_vkd3d_proton.sh $(SCRIPTS)/build_cache.sh $(SCRIPTS)/env.sh | $(STAMPS)
	@echo "=== vkd3d-proton 2.6 limited-500K ==="
	bash $(SCRIPTS)/build_vkd3d_proton.sh

$(VKD3D_PROTON_ARTIFACTS): vkd3d-proton-cache-check
	@test -s "$@" || { echo "ERROR: VKD3D-Proton artifact missing after build: $@" >&2; exit 1; }

# DXVK is produced as a versioned multi-DLL side effect of one cache-verified
# build. Give every packaged DLL an explicit rule so a clean checkout can
# resolve assemble dependencies. The size check also prevents packaging a
# partial or truncated install.
$(DXVK_ARTIFACTS): dxvk-cache-check
	@test -s "$@" || { echo "ERROR: DXVK artifact missing after build: $@" >&2; exit 1; }
ifeq ($(BUILD_GUEST_VULKAN),1)
ASSEMBLE_GUEST_INPUTS += $(wildcard $(GUEST_VULKAN_SENTINEL))
endif

# ============================================================
# 默认目标
# ============================================================
.PHONY: all
all: hap

# FORCE: 伪目标，永远"过期"，让 make 总是进入 recipe
# recipe 内部的 find -newer 才是真正的增量判断
.PHONY: FORCE
FORCE:

# 确保 stamps 目录存在
$(STAMPS):
	mkdir -p $(STAMPS)

# 确保架构子目录存在
$(STAMPS)/arm64-v8a $(STAMPS)/x86_64:
	mkdir -p $@

# ============================================================
# host-vulkan — native Host Vulkan exact replay diagnostic
# ============================================================
.PHONY: host-vulkan
host-vulkan: $(foreach a,$(ARCHES),$(STAMPS)/$(a)/host-vulkan)

define host_vulkan_rule
.PHONY: host-vulkan-$(1)
host-vulkan-$(1): $$(STAMPS)/$(1)/host-vulkan

$$(STAMPS)/$(1)/host-vulkan: $(SCRIPTS)/build_ohos_host_vulkan.sh $(SCRIPTS)/env.sh \
	$(HOST_VULKAN_SOURCE) FORCE | $$(STAMPS)/$(1)
	@manifest="$(BUILD_DIR)/host_vulkan/$(1)/manifest.json"; \
	module="$(BUILD_DIR)/host_vulkan/$(1)/lib/libwinehua_host_heaven_replay.so"; \
	if [ -f $$@ ] && [ -f "$$$$manifest" ] && [ -f "$$$$module" ] && \
	    ! [ "$(SCRIPTS)/build_ohos_host_vulkan.sh" -nt $$@ ] && \
	    ! [ "$(HOST_VULKAN_SOURCE)" -nt $$@ ]; then \
	    echo "  [host-vulkan/$(1)] up to date"; \
	else \
	    NATIVE_ARCH=$(1) bash $(SCRIPTS)/build_ohos_host_vulkan.sh && touch $$@; \
	fi
endef
$(foreach a,arm64-v8a x86_64,$(eval $(call host_vulkan_rule,$(a))))

# ============================================================
# deps — 交叉编译依赖 → build/sysroot-ext/ (架构无关)
# ============================================================
.PHONY: deps
deps: $(STAMPS)/deps

$(STAMPS)/deps: $(SCRIPTS)/build_deps.sh $(SCRIPTS)/build_gnutls.sh $(SCRIPTS)/build_gstreamer.sh \
	$(SCRIPTS)/build_ohos_guest_gfx.sh \
	$(SCRIPTS)/build_ohos_guest_vulkan.sh $(ROOT)/smoke/guest_vulkan_smoke.c \
	$(ROOT)/smoke/venus_sampled_image_probe.c \
	$(ROOT)/smoke/venus_depth_cube_probe.inc \
	$(ROOT)/smoke/venus_depth_cube_graphics_replay.inc \
	$(ROOT)/smoke/venus_fullscreen_triangle.vert \
	$(ROOT)/smoke/venus_heaven_material.vert \
	$(ROOT)/smoke/venus_heaven_material_replay.c \
	$(ROOT)/smoke/venus_depth_cube_golden.frag \
	$(ROOT)/smoke/venus_depth_cube_fail.spvasm \
	$(ROOT)/smoke/venus_storage_write.comp \
	$(ROOT)/smoke/venus_storage_read.comp \
	$(ROOT)/smoke/venus_image_fetch.comp \
	$(ROOT)/smoke/venus_combined_sample.comp \
	$(ROOT)/smoke/venus_dxvk_contract_sample.comp \
	$(ROOT)/smoke/venus_dxvk_contract_unknown_sample.comp \
	$(ROOT)/smoke/venus_dxvk_contract_spec_sample.comp \
	$(ROOT)/smoke/venus_dxvk_contract_vector_spec_sample.comp \
	$(ROOT)/smoke/venus_depth_array_compare.comp \
	$(ROOT)/smoke/venus_depth_cube_sample.comp \
	$(ROOT)/smoke/venus_depth_cube_compare.comp \
	$(ROOT)/smoke/venus_depth_cube_separated_compare.comp \
	$(ROOT)/smoke/venus_depth_cube_dxvk_contract_compare.spvasm \
	$(ROOT)/smoke/venus_depth_cube_array_sample.comp \
	$(ROOT)/smoke/venus_depth_cube_array_2d_compare.comp \
	$(ROOT)/smoke/venus_depth_cube_array_compare.comp \
	$(ROOT)/smoke/venus_spirv_replay.c \
	$(wildcard $(ROOT)/replay_spv/CS_*.remapped.spv) \
	$(ROOT)/smoke/venus_separated_sample.comp \
	$(SCRIPTS)/env.sh FORCE | $(STAMPS)
	@guest_gfx_ready=1; \
	if [ "$(BUILD_GUEST_GFX)" = "1" ] && [ ! -f "$(GUEST_GFX_SENTINEL)" ]; then \
	    guest_gfx_ready=0; \
	fi; \
	guest_vulkan_ready=1; \
	if [ "$(BUILD_GUEST_VULKAN)" = "1" ] && [ ! -f "$(GUEST_VULKAN_SENTINEL)" ]; then \
	    guest_vulkan_ready=0; \
	fi; \
	if [ -f $@ ] && [ -f $(DEPS_SENTINEL) ] && [ "$$guest_gfx_ready" = "1" ] && \
	    [ "$$guest_vulkan_ready" = "1" ] && \
	    ! [ "$(SCRIPTS)/build_ohos_guest_gfx.sh" -nt $@ ] && \
	    ! [ "$(SCRIPTS)/build_ohos_guest_vulkan.sh" -nt $@ ] && \
	    ! [ "$(SCRIPTS)/build_gnutls.sh" -nt $@ ] && \
	    ! [ "$(SCRIPTS)/build_gstreamer.sh" -nt $@ ] && \
	    ! [ "$(SCRIPTS)/build_deps.sh" -nt $@ ] && \
	    ! [ "$(SCRIPTS)/build_libffi.sh" -nt $@ ] && \
	    ! [ "$(SCRIPTS)/build_freetype.sh" -nt $@ ] && \
	    ! [ "$(SCRIPTS)/build_wayland.sh" -nt $@ ] && \
	    ! [ "$(SCRIPTS)/build_xkbcommon.sh" -nt $@ ] && \
	    ! [ "$(SCRIPTS)/build_xkbconfig.sh" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/guest_vulkan_smoke.c" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_sampled_image_probe.c" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_probe.inc" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_graphics_replay.inc" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_fullscreen_triangle.vert" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_heaven_material.vert" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_heaven_material_replay.c" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_golden.frag" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_fail.spvasm" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_storage_write.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_storage_read.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_image_fetch.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_combined_sample.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_dxvk_contract_sample.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_array_compare.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_sample.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_compare.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_separated_compare.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_dxvk_contract_compare.spvasm" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_array_sample.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_array_2d_compare.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_depth_cube_array_compare.comp" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_spirv_replay.c" -nt $@ ] && \
	    ! [ "$(ROOT)/smoke/venus_separated_sample.comp" -nt $@ ] && \
	    ! find $(ROOT)/thirdparty/freetype \
	           $(ROOT)/thirdparty/libffi \
	           $(ROOT)/thirdparty/wayland \
	           $(ROOT)/thirdparty/wayland-protocols \
	           $(ROOT)/thirdparty/libxml2 \
	           $(ROOT)/thirdparty/libxkbcommon \
	           $(ROOT)/thirdparty/xkeyboard-config \
	           $(ROOT)/thirdparty/mesa \
	           $(ROOT)/thirdparty/libdrm \
	           -newer $@ -type f \
	           \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.cc' \
	              -o -name 'meson.build' -o -name 'CMakeLists.txt' \
	              -o -name 'configure' -o -name '*.py' -o -name '*.xml' \
	              -o -name '*.ac' -o -name 'Makefile.am' -o -name '*.m4' \) \
	           2>/dev/null | grep -q .; then \
	    echo "  [deps] up to date"; \
	else \
	    echo "=== deps ==="; \
	    bash $(SCRIPTS)/build_deps.sh && touch $@; \
	fi

# ============================================================
# wine — Wine 交叉编译 + wineserver
# ============================================================
.PHONY: wine
wine: $(STAMPS)/wine-$(CONFIG)

$(STAMPS)/wine-$(CONFIG): $(SCRIPTS)/build_wine.sh $(SCRIPTS)/env.sh $(STAMPS)/deps FORCE | $(STAMPS)
	@if [ -f $@ ] && [ -f $(WINE_SENTINEL) ] && \
	    ! [ "$(SCRIPTS)/build_wine.sh" -nt $@ ] && \
	    ! find $(WINE_PATCH_DIR) -newer $@ -type f -name '*.patch' \
	           2>/dev/null | grep -q . && \
	    ! find $(ROOT)/thirdparty/wine \
	           -newer $@ -type f \
	           \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.cc' \
	              -o -name 'meson.build' -o -name 'CMakeLists.txt' \
	              -o -name 'configure' -o -name '*.ac' -o -name 'Makefile.am' \
	              -o -name '*.m4' -o -name '*.in' -o -name '*.rc' -o -name '*.spec' \) \
	           2>/dev/null | grep -q .; then \
	    echo "  [wine] up to date"; \
	else \
	    echo "=== wine ($(CONFIG)) ==="; \
	    bash $(SCRIPTS)/build_wine.sh && touch $@; \
	fi

# ============================================================
# box64 — ARM64 翻译器 (始终 arm64-v8a 架构, 编译为 box64.so dlopen 加载)
# ============================================================
.PHONY: box64
box64: $(STAMPS)/box64-arm64-v8a

$(STAMPS)/box64-arm64-v8a: $(SCRIPTS)/build_box64.sh $(SCRIPTS)/env.sh FORCE | $(STAMPS)
	@if [ "$(NATIVE_ARCH)" = "x86_64" ]; then \
	    echo "  [box64] skip (x86_64)"; \
	    mkdir -p $(dir $@) && touch $@; \
	elif [ -f $@ ] && \
	    ! find $(ROOT)/thirdparty/box64 \
	           -newer $@ -type f \
	           \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.S' \
	              -o -name 'CMakeLists.txt' -o -name '*.cmake' \) \
	           2>/dev/null | grep -q .; then \
	    echo "  [box64] up to date"; \
	else \
	    echo "=== box64 ==="; \
	    NATIVE_ARCH=arm64-v8a bash $(SCRIPTS)/build_box64.sh && touch $@; \
	fi

# ============================================================
# native — Native compositor 依赖 → entry/libs/ (架构相关)
# ============================================================
.PHONY: native
native: $(foreach a,$(ARCHES),$(STAMPS)/$(a)/native)

# Compile the application Native layer without repackaging the Wine payload.
# Run inside the same SDK-equipped Docker container as `hap`.
.PHONY: check-native
check-native: native
	@for arch in $(ARCHES); do \
	    cmake -S $(ROOT)/entry/src/main/cpp -B $(BUILD_DIR)/native-check/$$arch -GNinja \
	        -DCMAKE_TOOLCHAIN_FILE=$(OHOS_SDK)/native/build/cmake/ohos.toolchain.cmake \
	        -DOHOS_ARCH=$$arch -DOHOS_PLATFORM=OHOS -DOHOS_STL=c++_shared \
	        -DCMAKE_BUILD_TYPE=Debug && \
	    cmake --build $(BUILD_DIR)/native-check/$$arch --parallel 4 || exit $$?; \
	done

NATIVE_SENTINEL_arm64_v8a := $(ROOT)/entry/libs/arm64-v8a/libvirglrenderer.so.1
NATIVE_SENTINEL_x86_64    := $(ROOT)/entry/libs/x86_64/libvirglrenderer.so.1

define native_rule
.PHONY: native-$(1)
native-$(1): $$(STAMPS)/$(1)/native

$$(STAMPS)/$(1)/native: $(SCRIPTS)/build_native.sh $(SCRIPTS)/env.sh FORCE | $$(STAMPS)/$(1)
	@sentinel="$(NATIVE_SENTINEL_$(subst -,_,$(1)))"; \
	libs_dir="$(ROOT)/entry/libs/$(1)"; \
		if [ -f $$@ ] && [ -f "$$$$sentinel" ] && \
		    [ -f "$$$$libs_dir/libfreetype.so.6" ] && \
		    [ -f "$$$$libs_dir/libxkbcommon.so.0" ] && \
		    [ -f "$$$$libs_dir/libxml2.so.2" ] && \
		    [ -f "$$$$libs_dir/libffi.so.8" ] && \
		    [ -f "$$$$libs_dir/libwinehua_vtest_server.so" ] && \
	    ! [ "$(SCRIPTS)/build_native.sh" -nt $$@ ] && \
	    ! find $(ROOT)/thirdparty/wayland \
	           $(ROOT)/thirdparty/libffi \
	           $(ROOT)/thirdparty/libepoxy \
	           $(ROOT)/thirdparty/virglrenderer \
	           -newer $$@ -type f \
	           \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.cc' \
	              -o -name 'meson.build' -o -name 'CMakeLists.txt' \
	              -o -name 'configure' -o -name '*.ac' -o -name 'Makefile.am' \) \
	           2>/dev/null | grep -q .; then \
	    echo "  [native/$(1)] up to date"; \
	else \
	    echo "=== native ($(1)) ==="; \
	    NATIVE_ARCH=$(1) bash $(SCRIPTS)/build_native.sh && touch $$@; \
	fi
endef
$(foreach a,arm64-v8a x86_64,$(eval $(call native_rule,$(a))))

# ============================================================
# assemble — 组装布局 (架构 + 设备类型相关)
# ============================================================
.PHONY: assemble
assemble: $(foreach a,$(ARCHES),$(STAMPS)/$(a)/assemble)

define assemble_rule
.PHONY: assemble-$(1)

assemble-$(1): $$(STAMPS)/$(1)/assemble

$$(STAMPS)/$(1)/assemble: $(SCRIPTS)/assemble.sh $(SCRIPTS)/env.sh $(DXVK_ARTIFACTS) $(DXVK_MODERN_ARTIFACTS) \
	$(VKD3D_PROTON_ARTIFACTS) \
	$(ROOT)/smoke/winehua_d3d8_smoke.c \
	$(ROOT)/smoke/winehua_d3d_switch_cube.c \
	$(ROOT)/smoke/winehua_media_smoke.cpp \
	$(ROOT)/smoke/winehua_dxvk26_requirements.c \
	$(ROOT)/smoke/winehua_win32_driver.c \
	$(ROOT)/smoke/winehua_network_probe.c \
	$(ROOT)/smoke/winehua_network_wininet.c \
	$$(STAMPS)/deps $$(STAMPS)/wine-$(1) $$(STAMPS)/$(1)/native \
	$$(STAMPS)/$(1)/host-vulkan \
	$$(ASSEMBLE_GUEST_INPUTS) | $$(STAMPS)/$(1)
	@echo "=== assemble ($(1)) ==="
	NATIVE_ARCH=$(1) GUEST_ARCH=$(GUEST_ARCH) BUILD_GUEST_GFX=$(BUILD_GUEST_GFX) bash $(SCRIPTS)/assemble.sh
	@touch $$@
endef
$(foreach a,arm64-v8a x86_64,$(eval $(call assemble_rule,$(a))))

# arm64 assemble 额外依赖 box64 (32-bit PE DLL 已由 wine 主构建 --enable-archs=i386 提供)
$(STAMPS)/arm64-v8a/assemble: $(STAMPS)/box64-arm64-v8a

# ============================================================
# hap — HAP 构建 + 签名 (统一 rawfile zip)
# ============================================================
.PHONY: hap
hap: assemble
	@echo "=== hap ($(CONFIG)) ==="
	bash $(SCRIPTS)/package.sh hap
	@echo ""
	@echo "HAP: $(ROOT)/entry/build/default/outputs/default/entry-default-signed.hap"
	@ls -lh $(ROOT)/entry/build/default/outputs/default/entry-default-signed.hap 2>/dev/null || true

# Public CI has no device signing material. Never disguise unsigned output.
.PHONY: hap-unsigned test-ci-release
hap-unsigned: assemble
	bash $(SCRIPTS)/package.sh hap-unsigned

test-ci-release:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/host_tests/ci_release_test.py

# ============================================================
# test: 宿主机单元测试 (纯函数, 不依赖 OHOS SDK, 用宿主 g++ 编译)
# ============================================================
HOST_TEST_DIR := $(BUILD_DIR)/host_tests

.PHONY: graphics-contract-check
graphics-contract-check:
	bash $(SCRIPTS)/verify_graphics_contract.sh

.PHONY: test
test: graphics-contract-check
	@mkdir -p $(HOST_TEST_DIR)
	g++ -std=c++17 -Wall -Wextra -Werror -I $(ROOT)/entry/src/main/cpp \
	    -o $(HOST_TEST_DIR)/gles_direct_policy_test $(ROOT)/host_tests/gles_direct_policy_test.cpp
	$(HOST_TEST_DIR)/gles_direct_policy_test
	g++ -std=c++17 -Wall -Wextra -Werror -I $(ROOT)/entry/src/main/cpp \
	    -o $(HOST_TEST_DIR)/present_timing_test $(ROOT)/host_tests/present_timing_test.cpp
	$(HOST_TEST_DIR)/present_timing_test
	g++ -std=c++17 -Wall -Wextra -I $(ROOT)/entry/src/main/cpp \
	    -o $(HOST_TEST_DIR)/geometry_test \
	    $(ROOT)/host_tests/geometry_test.cpp \
	    $(ROOT)/entry/src/main/cpp/compositor/geometry.cpp
	$(HOST_TEST_DIR)/geometry_test
	g++ -std=c++17 -Wall -Wextra -Werror -DWINEHUA_DEBUG_ASSERT \
	    -I $(ROOT)/host_tests/stubs -I $(ROOT)/entry/src/main/cpp \
	    -o $(HOST_TEST_DIR)/compositor_state_test \
	    $(ROOT)/host_tests/compositor_state_test.cpp \
	    $(ROOT)/entry/src/main/cpp/compositor/desktop_compositor.cpp \
	    $(ROOT)/entry/src/main/cpp/compositor/frame_pipeline.cpp \
	    $(ROOT)/entry/src/main/cpp/compositor/input_resolver.cpp \
	    $(ROOT)/entry/src/main/cpp/compositor/toplevel_manager.cpp \
	    $(ROOT)/entry/src/main/cpp/compositor/geometry.cpp \
	    $(ROOT)/entry/src/main/cpp/compositor/compositor_blit.cpp
	$(HOST_TEST_DIR)/compositor_state_test
	g++ -std=c++17 -Wall -Wextra -I $(ROOT)/entry/src/main/cpp \
	    -o $(HOST_TEST_DIR)/blit_scaled_test \
	    $(ROOT)/host_tests/blit_scaled_test.cpp \
	    $(ROOT)/entry/src/main/cpp/compositor/compositor_blit.cpp
	$(HOST_TEST_DIR)/blit_scaled_test
	g++ -std=c++17 -Wall -Wextra -I $(ROOT)/entry/src/main/cpp \
	    -o $(HOST_TEST_DIR)/blit_clip_test \
	    $(ROOT)/host_tests/blit_clip_test.cpp
	$(HOST_TEST_DIR)/blit_clip_test
	g++ -std=c++17 -Wall -Wextra -I $(ROOT)/entry/src/main/cpp \
	    -o $(HOST_TEST_DIR)/env_spec_test \
	    $(ROOT)/host_tests/env_spec_test.cpp \
	    $(ROOT)/entry/src/main/cpp/env_spec.cpp
	$(HOST_TEST_DIR)/env_spec_test
	g++ -std=c++17 -Wall -Wextra -I $(ROOT)/entry/src/main/cpp \
	    -o $(HOST_TEST_DIR)/env_baseline_test \
	    $(ROOT)/host_tests/env_baseline_test.cpp
	$(HOST_TEST_DIR)/env_baseline_test
	g++ -std=c++17 -Wall -Wextra -I $(ROOT)/entry/src/main/cpp \
	    -o $(HOST_TEST_DIR)/controller_merge_test \
	    $(ROOT)/host_tests/controller_merge_test.cpp \
	    $(ROOT)/entry/src/main/cpp/controller/controller_hub.cpp
	$(HOST_TEST_DIR)/controller_merge_test
	g++ -std=c++17 -Wall -Wextra -I $(ROOT)/entry/src/main/cpp \
	    -o $(HOST_TEST_DIR)/graphics_policy_test \
	    $(ROOT)/host_tests/graphics_policy_test.cpp \
	    $(ROOT)/entry/src/main/cpp/graphics_profile.cpp \
	    $(ROOT)/entry/src/main/cpp/virgl_host_config.cpp
	$(HOST_TEST_DIR)/graphics_policy_test
	g++ -std=c++17 -Wall -Wextra -Werror \
	    -I $(ROOT)/thirdparty/dxvk-modern/src/dxvk \
	    -o $(HOST_TEST_DIR)/dxvk_mapped_range_test \
	    $(ROOT)/host_tests/dxvk_mapped_range_test.cpp
	$(HOST_TEST_DIR)/dxvk_mapped_range_test
# SDK-declaration/mock-call test; run inside the existing build container.
.PHONY: test-gles-direct
test-gles-direct:
	bash $(SCRIPTS)/test_gles_direct.sh

.PHONY: test-performance-hud
test-performance-hud:
	@mkdir -p $(HOST_TEST_DIR)
	g++ -std=c++17 -Wall -Wextra -Werror -I $(ROOT)/entry/src/main/cpp \
	    $(ROOT)/host_tests/performance_monitor_test.cpp $(ROOT)/entry/src/main/cpp/performance_monitor.cpp \
	    -o $(HOST_TEST_DIR)/performance_monitor_test
	$(HOST_TEST_DIR)/performance_monitor_test
	node $(SCRIPTS)/test_performance_hud.cjs

.PHONY: test-model
test-model:
	bash $(SCRIPTS)/run_model_unit_tests.sh

.PHONY: test-bottom-navigation
test-bottom-navigation:
	node $(SCRIPTS)/test_bottom_navigation.cjs

# Standalone diagnostic only: no runtime/HAP dependency or default packaging.
.PHONY: guest-inspect
guest-inspect: $(BUILD_DIR)/guest-inspect/winehua_guest_inspect.exe

$(BUILD_DIR)/guest-inspect/winehua_guest_inspect.exe: $(ROOT)/smoke/winehua_guest_inspect.c
	@mkdir -p "$(@D)"
	x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror \
	    -mwindows -static-libgcc -o "$@" "$<"

# Opt-in diagnostics: library target stays isolated; HAP target stages temporarily.
.PHONY: host-stage-timing host-stage-timing-hap test-host-stage-timing
host-stage-timing:
	python3 $(SCRIPTS)/build_host_stage_timing.py --arch $(NATIVE_ARCH)

host-stage-timing-hap: host-stage-timing test-host-stage-timing
	bash $(SCRIPTS)/package_host_stage_timing.sh

test-host-stage-timing:
	@mkdir -p $(HOST_TEST_DIR)
	gcc -std=gnu11 -O2 -Wall -Wextra -Werror \
	    -I $(ROOT)/thirdparty/virglrenderer/vtest -I $(ROOT)/thirdparty/virglrenderer/src \
	    -I $(BUILD_DIR)/native_$(NATIVE_ARCH)/virglrenderer/src \
	    $(ROOT)/host_tests/host_stage_timing_test.c -o $(HOST_TEST_DIR)/host_stage_timing_test
	$(HOST_TEST_DIR)/host_stage_timing_test

# Isolated WineD3D PE diagnostics; do not stage into the runtime or HAP.
.PHONY: wined3d-readback test-wined3d-readback-build
wined3d-readback: test-wined3d-readback-build
	python3 $(SCRIPTS)/build_wined3d_readback.py

test-wined3d-readback-build:
	PYTHONDONTWRITEBYTECODE=1 python3 $(ROOT)/host_tests/wined3d_readback_build_test.py

# Explicit Guest timing bridge; no production staging or runtime archive changes.
.PHONY: guest-stage-timing test-guest-stage-timing
guest-stage-timing:
	python3 $(SCRIPTS)/build_guest_stage_timing.py

# Opt-in candidate output only: same synchronization, combined short I/O.
.PHONY: guest-busy-io test-vtest-busy-io
guest-busy-io: test-vtest-busy-io
	python3 $(SCRIPTS)/build_guest_stage_timing.py --busy-io

test-vtest-busy-io:
	@mkdir -p $(HOST_TEST_DIR)
	gcc -std=gnu11 -O2 -Wall -Wextra -Werror \
	    -I $(ROOT)/thirdparty/mesa/src/virtio \
	    $(ROOT)/host_tests/vtest_busy_io_test.c -o $(HOST_TEST_DIR)/vtest_busy_io_test
	$(HOST_TEST_DIR)/vtest_busy_io_test

test-guest-stage-timing:
	@mkdir -p $(HOST_TEST_DIR)
	gcc -std=gnu11 -O2 -Wall -Wextra -Werror \
	    -I $(ROOT)/thirdparty/mesa/src/virtio \
	    $(ROOT)/host_tests/guest_stage_timing_test.c -ldl -o $(HOST_TEST_DIR)/guest_stage_timing_test
	$(HOST_TEST_DIR)/guest_stage_timing_test

# ============================================================
# clean
# ============================================================
.PHONY: clean
clean:
	@echo "=== clean ==="
	rm -rf $(BUILD_DIR)
	rm -f $(ROOT)/entry/libs/arm64-v8a/*.so*
	rm -f $(ROOT)/entry/libs/arm64-v8a/virgl_test_server
	rm -f $(ROOT)/entry/libs/x86_64/*.so*
	rm -f $(ROOT)/entry/libs/x86_64/virgl_test_server
	rm -rf $(ROOT)/entry/build
	rm -f $(ROOT)/entry/src/main/resources/rawfile/wine-data.zip
	@echo "  已清理所有中间产物"

# ============================================================
# 帮助
# ============================================================
.PHONY: help
help:
	@echo "用法: make [target] [NATIVE_ARCH=x86_64|arm64-v8a|all]"
	@echo ""
	@echo "默认: NATIVE_ARCH=x86_64"
	@echo "SDK: target=$(TARGET_SDK_VERSION), compatible=$(COMPATIBLE_SDK_VERSION)"
	@echo ""
	@echo "全部构建:"
	@echo "  make                                          # 默认配置全量 → HAP"
	@echo "  make NATIVE_ARCH=arm64-v8a                    # ARM64"
	@echo "  make NATIVE_ARCH=all                          # 双架构 HAP"
	@echo ""
	@echo "单模块:"
	@echo "  make deps      # 交叉编译依赖 → sysroot-ext"
	@echo "  make wine      # Wine + wineserver"
	@echo "  make box64     # Box64 (仅 arm64)"
	@echo "  make native    # Native compositor 依赖"
	@echo "  make host-vulkan # Host Vulkan exact replay"
	@echo "  make assemble  # 组装布局"
	@echo "  make hap       # HAP 打包 + 签名"
	@echo "  make hap-unsigned # CI 无签名 HAP (安装前需自行签名)"
	@echo "  make graphics-contract-check # 校验图形协议、默认 profile 与 gitlink"
	@echo "  make test      # 图形契约 + 宿主机纯函数测试"
	@echo ""
	@echo "每个架构:"
	@echo "  make native-x86_64  make native-arm64-v8a"
	@echo ""
	@echo "清理:"
	@echo "  make clean     # 删除所有中间产物"
	@echo ""
	@echo "产物统一在 build/ 下"
