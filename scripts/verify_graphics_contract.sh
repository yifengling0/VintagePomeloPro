#!/usr/bin/env bash
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
LOCK="$ROOT/docs/graphics/graphics-stack.lock.yaml"
PROTOCOL_HEADER="$ROOT/entry/src/main/cpp/graphics/virgl_ipc_protocol.h"

fail() {
    printf 'graphics-contract: ERROR: %s\n' "$*" >&2
    exit 1
}

yaml_value() {
    local section="$1"
    local key="$2"
    awk -v wanted_section="$section" -v wanted_key="$key" '
        /^[^[:space:]#][^:]*:[[:space:]]*$/ {
            section = $0
            sub(/:[[:space:]]*$/, "", section)
            next
        }
        section == wanted_section && $0 ~ "^[[:space:]]+" wanted_key ":[[:space:]]*" {
            value = $0
            sub(/^[^:]+:[[:space:]]*/, "", value)
            sub(/[[:space:]]+#.*$/, "", value)
            gsub(/^"|"$/, "", value)
            print value
            exit
        }
    ' "$LOCK"
}

require_value() {
    local section="$1"
    local key="$2"
    local value
    value="$(yaml_value "$section" "$key")"
    [[ -n "$value" ]] || fail "missing $section.$key in lock"
    printf '%s' "$value"
}

assert_equal() {
    local label="$1"
    local expected="$2"
    local actual="$3"
    [[ "$actual" == "$expected" ]] || fail "$label expected=$expected actual=$actual"
    printf 'graphics-contract: OK: %s=%s\n' "$label" "$actual"
}

gitlink_sha() {
    local path="$1"
    local row
    row="$(git -C "$ROOT" ls-tree HEAD -- "$path")"
    [[ -n "$row" ]] || fail "missing gitlink $path"
    awk '{ print $3 }' <<<"$row"
}

require_literal() {
    local label="$1"
    local literal="$2"
    local path="$3"
    grep -Fq -- "$literal" "$ROOT/$path" || fail "$label missing from $path"
    printf 'graphics-contract: OK: %s in %s\n' "$label" "$path"
}

reject_literal() {
    local label="$1"
    local literal="$2"
    local path="$3"
    if grep -Fq -- "$literal" "$ROOT/$path"; then
        fail "$label unexpectedly present in $path"
    fi
    printf 'graphics-contract: OK: %s absent from %s\n' "$label" "$path"
}

require_literal_count() {
    local label="$1"
    local literal="$2"
    local expected="$3"
    local path="$4"
    local actual
    actual="$(grep -Fc -- "$literal" "$ROOT/$path" || true)"
    [[ "$actual" == "$expected" ]] ||
        fail "$label expected=$expected actual=$actual in $path"
    printf 'graphics-contract: OK: %s=%s in %s\n' "$label" "$actual" "$path"
}

normalized_sha256() {
    sed 's/\r$//' "$1" | sha256sum | awk '{ print $1 }'
}

[[ -f "$LOCK" ]] || fail "missing lock file: $LOCK"
[[ -f "$PROTOCOL_HEADER" ]] || fail "missing protocol header: $PROTOCOL_HEADER"

known_tag="$(require_value known_good product_tag)"
known_commit="$(require_value known_good product_commit)"
tag_commit="$(git -C "$ROOT" rev-parse "$known_tag^{commit}" 2>/dev/null)" ||
    fail "known-good tag is unavailable: $known_tag"
assert_equal "known-good tag" "$known_commit" "$tag_commit"

while IFS='|' read -r key path; do
    expected="$(require_value submodules "$key")"
    actual="$(gitlink_sha "$path")"
    assert_equal "gitlink $key" "$expected" "$actual"
done <<'EOF'
virglrenderer|thirdparty/virglrenderer
mesa|thirdparty/mesa
dxvk|thirdparty/dxvk
dxvk-modern|thirdparty/dxvk-modern
vkd3d-proton|thirdparty/vkd3d-proton
wine|thirdparty/wine
EOF

expected_whip="$(require_value protocol whip_version)"
actual_whip="$(sed -n 's/.*kProtocolVersion[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$PROTOCOL_HEADER" | head -n 1)"
[[ -n "$actual_whip" ]] || fail "cannot parse kProtocolVersion"
assert_equal "WHIP protocol" "$expected_whip" "$actual_whip"

expected_strings="$(require_value protocol host_config_strings)"
actual_strings="$(sed -n 's/.*kHostConfigStringCount[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$PROTOCOL_HEADER" | head -n 1)"
[[ -n "$actual_strings" ]] || fail "cannot parse kHostConfigStringCount"
assert_equal "HostConfig strings" "$expected_strings" "$actual_strings"

virgl_route="$(require_value defaults virgl_route)"
vulkan_route="$(require_value defaults vulkan_route)"
require_literal "VirGL product route" \
    "kProductVirglRoute = \"$virgl_route\";" \
    entry/src/main/cpp/graphics/graphics_profile.h
require_literal "Vulkan product route" \
    "kProductVulkanRoute = \"$vulkan_route\";" \
    entry/src/main/cpp/graphics/graphics_profile.h
require_literal "NAPI LAB experiment resolver" "ResolveLabGraphicsExperiment" \
    entry/src/main/cpp/bridge/napi_init.cpp
require_literal "NAPI LAB experiment API" "setHostGraphicsExperimentForLab" \
    entry/src/main/cpp/bridge/napi_init.cpp
require_literal "WineHua legacy Host profile API remains callable" "setHostShadowProfile" \
    entry/src/main/cpp/bridge/napi_init.cpp
require_literal "Legacy Host profile converges through product policy" "gLegacyHostShadowProfile" \
    entry/src/main/cpp/bridge/napi_init.cpp
require_literal "NAPI product route resolver" "ResolveProductGraphicsPolicy" \
    entry/src/main/cpp/bridge/napi_init.cpp
require_literal "Wine env product Guest resolver" \
    "BuildProductGuestGraphicsEnvironment" \
    entry/src/main/cpp/wine/wine_env.cpp
require_literal "Wine session product route resolver" \
    "ResolveProductGraphicsPolicy" \
    entry/src/main/cpp/wine/env_profiles.cpp
require_literal "Wine session applies product DXVK capability" \
    "AppendProductDxvkEnv(env, d3dBackend, graphicsExperiment);" \
    entry/src/main/cpp/wine/env_profiles.cpp
require_literal "Product Host route follows selected Guest backend" \
    "hostGraphicsBackendOverride : requestedD3DBackend" \
    entry/src/main/ets/service/WineEngineService.ets
require_literal "Program present route derives from D3D backend" \
    "UsesVenusPresent(backend)" entry/src/main/cpp/wine/wine_exe.cpp
require_literal "Managed programs expose surface/offscreen intent" \
    "presentToSurface" entry/src/main/cpp/types/libentry/Index.d.ts
require_literal "Smoke runner uses Native program policy" \
    "testNapi.runWineProgram" entry/src/main/ets/service/SmokeRunner.ets
require_literal "Smoke runner uses product engine lifecycle" \
    "WineEngineService.getInstance().isReady()" entry/src/main/ets/service/SmokeRunner.ets
require_literal "Clean smoke switches the broker-owned prefix lifecycle" \
    "ensureSmokeReady" entry/src/main/ets/service/WineEngineService.ets
require_literal "Clean smoke restores the product session" \
    "restoreProductSession" entry/src/main/ets/service/SmokeRunner.ets
require_literal "Smoke runner is data driven" \
    "smoke/suites.json" entry/src/main/ets/service/SmokeRunner.ets
reject_literal "Product Index does not import upstream smoke sidebar" \
    "SmokeRunner" entry/src/main/ets/pages/Index.ets
require_literal "Engine prepares managed payload independently of test orchestration" \
    "from './ManagedSmokePayloadService'" entry/src/main/ets/service/WineEngineService.ets
require_literal "WineHua program interface keeps present-backend compatibility" \
    "presentBackend" entry/src/main/cpp/wine/wine_exe.h
require_literal "Native derives or honors the common present backend" \
    "WINEHUA_PRESENT_BACKEND" entry/src/main/cpp/wine/wine_exe.cpp
require_literal "WineHua launch control plane keeps DXVK compatibility" \
    "std::string dxvkBackend" entry/src/main/cpp/wine/wine_launch.h
require_literal "Session environment consumes the common DXVK field" \
    "p.d3dBackend, p.dxvkBackend, p.binDir" \
    entry/src/main/cpp/wine/env_profiles.cpp
require_literal "WineHua launch control plane keeps locale compatibility" \
    "std::string wineLang" entry/src/main/cpp/wine/wine_launch.h
require_literal "Launch NAPI accepts product and WineHua layouts" \
    "sixthType == napi_boolean" entry/src/main/cpp/bridge/napi_init.cpp
require_literal "Wine locale supplies musl fallback" \
    'env.push_back("LC_ALL=" + locale + ".UTF-8");' \
    entry/src/main/cpp/wine/wine_env.cpp
require_literal "Process list keeps upstream desktop-shell field" \
    '"desktopShell"' entry/src/main/cpp/bridge/napi_init.cpp
require_literal "Wineboot waits for broker worker" \
    "progress.workerRunning" entry/src/main/cpp/wine/wine_launch.cpp
require_literal "Wineboot observes only the current spawn attempt" \
    "const winehua::WinebootAttempt bootAttempt(GetProcessListSnapshot());" entry/src/main/cpp/wine/wine_launch.cpp
require_literal "Wineboot rejects known child failure before desktop launch" \
    "if (progress.failedPid > 0)" entry/src/main/cpp/wine/wine_launch.cpp
require_literal "Prefix health checks a critical Wine COM registration" \
    "bcde0395-e52f-467c-8e3d-c4579291692e" \
    entry/src/main/cpp/wine/wine_launch.cpp
require_literal "Incomplete prefix repair forces wine.inf registration" \
    'repairIncompletePrefix ? "--update" : "--init"' \
    entry/src/main/cpp/wine/wine_launch.cpp
require_literal "Unchanged wine.inf avoids prefix reinstall" \
    "preserved unchanged wine.inf timestamp" \
    entry/src/main/ets/service/WineEngineService.ets
require_literal "Runtime identity uses semantic content" \
    "runtime_content_sha" scripts/assemble.sh
reject_literal "Removed product-to-LAB resolver" \
    "ProductionHostProfileForBackend" entry/src/main/cpp/graphics/graphics_profile.h
require_literal "DXVK runtime resolver" "ResolveDxvkRuntimeProfile" \
    entry/src/main/cpp/graphics/graphics_profile.cpp
require_literal "Product-derived LAB experiment authority" \
    "ResolveLabGraphicsExperiment" entry/src/main/cpp/graphics/graphics_profile.cpp
reject_literal "Removed Host-only LAB resolver wrapper" \
    "ResolveLabHostGraphicsProfile" entry/src/main/cpp/graphics/graphics_profile.h
reject_literal "Removed Guest-only LAB resolver wrapper" \
    "ResolveLabGuestGraphicsPolicy" entry/src/main/cpp/graphics/graphics_profile.h
reject_literal "Removed combinatorial LAB profile table" \
    "std::array<HostGraphicsProfile" entry/src/main/cpp/graphics/graphics_profile.cpp
require_literal "Guest environment serializer authority" \
    "BuildLabGuestGraphicsEnvironment" entry/src/main/cpp/graphics/graphics_profile.cpp
require_literal "Explicit LAB Guest policy consumer" \
    "BuildLabGuestGraphicsEnvironment" entry/src/main/cpp/wine/wine_env.cpp
require_literal "LAB Guest policy API remains available to isolated diagnostics" \
    "resolveGuestGraphicsEnvironmentForLab" \
    entry/src/main/cpp/types/libentry/Index.d.ts
reject_literal "Payload preparation must not launch Wine" "testNapi" \
    entry/src/main/ets/service/ManagedSmokePayloadService.ets
for policy_file in \
    entry/src/main/cpp/graphics/graphics_profile.cpp \
    entry/src/main/cpp/proc/wine_child.cpp \
    entry/src/main/cpp/wine/wine_launch.cpp \
    entry/src/main/cpp/wine/wine_env.cpp; do
    reject_literal "Removed duplicate Guest profile environment key" \
        "WINEHUA_PERF_PROFILE" "$policy_file"
done
reject_literal "Removed redundant single-ring LAB alias" \
    "shadow-precise-single-ring" entry/src/main/cpp/graphics/graphics_profile.cpp
reject_literal "Removed historical combined LAB ids" \
    "shadow-precise-dirty-ring-frame-timeline" \
    entry/src/main/cpp/graphics/graphics_profile.cpp
for automation_script in \
    automation/Start-WineHuaGameTest.ps1 \
    automation/Measure-WineHuaFrameOrder.ps1 \
    automation/Measure-WineHuaDxvkPerformance.ps1 \
    automation/Invoke-WineHuaAutomation.ps1; do
    reject_literal "Automation duplicate experiment registry" \
        "ValidateSet('baseline'" "$automation_script"
done
require_literal "DXVK performance Legacy baseline" \
    "id='legacy-1.10-product'" automation/GraphicsTestPolicy.ps1
require_literal "DXVK performance Modern baseline" \
    "id='modern-2.6-product'" automation/GraphicsTestPolicy.ps1
require_literal "DXVK performance inherits product batching" \
    "batchMappedFlushMode='product'" automation/GraphicsTestPolicy.ps1
require_literal "DXVK performance consumes the shared product matrix" \
    'Get-DxvkProductTestConditions $ConditionSet' automation/Measure-WineHuaDxvkPerformance.ps1
require_literal "Normal launcher rejects disabled batching" \
    'Assert-GraphicsTestMappedFlush -Mode $BatchMappedFlushMode' automation/Start-WineHuaGameTest.ps1
require_literal "Core regression uses normal game launch" \
    "Start-WineHuaGameTest.ps1" automation/NormalSmoke.ps1
require_literal "Product core regression keeps normal game launch" \
    'Assert-NormalSmokeSuite $Suite $Prefix' automation/Invoke-WineHuaAutomation.ps1
require_literal "Data-driven regression uses App smoke mode" \
    '"winehua.mode", "smoke"' automation/run_regression.py
require_literal "Automation omits batch override unless requested" \
    "ContainsKey('BatchMappedFlush')" automation/Invoke-WineHuaAutomation.ps1
require_literal "DXVK performance product-equivalent observation" \
    "observe-frame-timeline" \
    automation/Measure-WineHuaDxvkPerformance.ps1
require_literal "DXVK Modern optional mapped-flush stats" \
    "CollectModernMappedFlushStats" \
    automation/Measure-WineHuaDxvkPerformance.ps1
require_literal "DXVK Modern structured mapped-flush result" \
    "modernMappedFlush" \
    automation/Measure-WineHuaDxvkPerformance.ps1
require_literal "DXVK performance records actual present transport" \
    "presentTransport" automation/Measure-WineHuaDxvkPerformance.ps1
require_literal "DXVK aggregate prevents mixed transport attribution" \
    "transportsObserved" automation/Measure-WineHuaDxvkPerformance.ps1
require_literal "DXVK performance validates transport action" \
    "presentActionContract" automation/Measure-WineHuaDxvkPerformance.ps1
require_literal "DXVK performance waits for rendered workload" \
    "Wait-PresenterReady" automation/Measure-WineHuaDxvkPerformance.ps1
require_literal "DXVK performance waits for prior app process exit" \
    "Wait-ForBundleProcessExit" automation/Measure-WineHuaDxvkPerformance.ps1
require_literal "Device automation keeps the display awake for long runs" \
    "power-shell timeout -o 2147483647" \
    automation/Start-WineHuaGameTest.ps1
require_literal "Zero-copy keeps producer Vulkan classification per surface" \
    "surface.vulkan))" entry/src/main/cpp/graphics/egl_renderer.cpp
require_literal "Zero-copy stale binding tracks surface serial" \
    "uint32_t zeroCopySurfaceSerial_" entry/src/main/cpp/graphics/egl_renderer.h
require_literal "Zero-copy promotion follows latest-present ordering" \
    "candidate != currentSurface" entry/src/main/cpp/graphics/egl_renderer.cpp
require_literal "Zero-copy promotion is gated on no produced frame" \
    "const bool currentHasFrame = zeroCopyFrames_ != 0 || zeroCopyHasFrame_ ||" \
    entry/src/main/cpp/graphics/egl_renderer.cpp
require_literal "Media probe participates in assemble invalidation" \
    '$(ROOT)/smoke/winehua_media_smoke.cpp' Makefile
require_literal "Media probe is self-contained on the Wine runtime" \
    "-static-libgcc -static-libstdc++" scripts/assemble.sh
require_literal "Media probe has a versioned managed payload" \
    "phase2-vulkan-dxvk-v10-vkd3d-product-media" scripts/assemble.sh
require_literal "Media probe packages the x64 PE" \
    '"x64/winehua_media_smoke.exe"' scripts/assemble.sh
require_literal "Media probe packages the x86 PE" \
    '"x86/winehua_media_smoke.exe"' scripts/assemble.sh
require_literal_count "Media probe is required for both PE widths" \
    "winehua_media_smoke.exe'" 2 entry/src/main/ets/service/ManagedSmokePayloadService.ets
require_literal "DXVK performance excludes startup from sample" \
    "Sampling starts from a clean log buffer" \
    automation/Measure-WineHuaDxvkPerformance.ps1
require_literal "DXVK performance uses interval FPS" \
    "samplePresenterFps" automation/Measure-WineHuaDxvkPerformance.ps1
require_literal "DXVK performance keeps console output compact" \
    "consoleSummary" automation/Measure-WineHuaDxvkPerformance.ps1
require_literal "Frame-order validates transport action" \
    "presentActionContract" automation/Measure-WineHuaFrameOrder.ps1
require_literal "Frame-order supports the WineD3D VirGL route" \
    "'product-virgl'" automation/Measure-WineHuaFrameOrder.ps1
require_literal "Prefix migration validates the HarmonyOS UI font mapping" \
    "{ name: 'MS Shell Dlg', replacement: 'HarmonyOS Sans SC' }" \
    entry/src/main/ets/service/WineEngineService.ets
require_literal "Prefix migration validates every managed font mapping" \
    "MASTER_FONT_SUBSTITUTES.every" \
    entry/src/main/ets/service/WineEngineService.ets
require_literal "Frame-order parser accepts Direct transport summary" \
    '\[VENUS-PRESENT\]\[NCP\].*\bframes=' \
    automation/Measure-WineHuaFrameOrder.ps1
require_literal "DXVK performance plan" \
    "# DXVK 1.10 / 2.6 性能差距分析与优化计划" \
    docs/graphics/DXVK_GENERATION_PERFORMANCE_PLAN.md
require_literal "Media playback attribution boundary is documented" \
    "媒体播放与图形路由：已确认的边界" \
    docs/graphics/DXVK_GENERATION_PERFORMANCE_PLAN.md
require_literal "Per-producer media presenter ownership is documented" \
    "NativeImage consumer 的 presenter 类型由 producer surface 归属" \
    docs/graphics/DIRECT_PRESENT_REFERENCE_AUDIT.md

dxvk_shadow_selector="$(require_value defaults dxvk_shadow_selector)"
require_literal "DXVK product Host policy" \
    "\"$dxvk_shadow_selector\"};" \
    entry/src/main/cpp/graphics/graphics_profile.cpp
vkd3d_shadow_mode="$(require_value defaults vkd3d_shadow_mode)"
require_literal "VKD3D product Host policy" \
    "resolved.host = {resolved.route, \"$vkd3d_shadow_mode\", \"0\"};" \
    entry/src/main/cpp/graphics/graphics_profile.cpp
require_literal "Explicit Host summary control-plane bit" \
    "WINEHUA_VIRGL_HOST_PERF_SUMMARY" entry/src/main/cpp/bridge/napi_init.cpp
require_literal "Broker consumes Host summary bit" \
    "WINEHUA_VIRGL_HOST_PERF_SUMMARY" entry/src/main/cpp/graphics/graphics_broker.cpp
require_literal "WHIP field 8 carries Host summary" \
    "std::string perfSummary;" entry/src/main/cpp/graphics/virgl_host_config.h
require_literal "WHIP Host summary is binary" \
    "IsBinaryFlag(config.perfSummary)" entry/src/main/cpp/graphics/virgl_host_config.cpp
require_literal "Host summary reaches presenter" \
    'AppendEnv(params, "WINEHUA_VTEST_PRESENT_PERF_SUMMARY",' \
    entry/src/main/cpp/graphics/virgl_host_config.cpp
for retired_present_file in \
    entry/src/main/cpp/bridge/napi_init.cpp \
    entry/src/main/cpp/graphics/graphics_broker.cpp \
    entry/src/main/cpp/graphics/virgl_child.cpp \
    entry/src/main/cpp/graphics/virgl_host_config.cpp \
    entry/src/main/cpp/graphics/present_policy.h; do
    reject_literal "Retired public present-mode key" \
        "WINEHUA_VENUS_PRESENT_MODE" "$retired_present_file"
    reject_literal "Retired Host present-mode key" \
        "WINEHUA_VIRGL_HOST_PRESENT_MODE" "$retired_present_file"
done
for fixed_present_file in \
    entry/src/main/cpp/graphics/venus_surface_presenter.cpp \
    entry/src/main/cpp/graphics/present_policy.h \
    entry/src/main/cpp/graphics/virgl_host_config.cpp \
    entry/src/main/cpp/graphics/graphics_broker.cpp \
    entry/src/main/cpp/graphics/virgl_child.cpp; do
    reject_literal "Retired mailbox control branch" "mailbox" "$fixed_present_file"
    reject_literal "Retired async present control branch" \
        "fifo-async" "$fixed_present_file"
    reject_literal "Retired poll present control branch" \
        "fifo-poll" "$fixed_present_file"
done
require_literal "Venus product queue fixed to FIFO" \
    "create.presentMode = VK_PRESENT_MODE_FIFO_KHR;" \
    entry/src/main/cpp/graphics/venus_surface_presenter.cpp
require_literal "Vulkan Direct target is part of virgl child" \
    "native_window_vk_target.cpp" entry/src/main/cpp/CMakeLists.txt
require_literal "Vulkan Direct target links NativeBuffer API" \
    "libnative_buffer.so" entry/src/main/cpp/CMakeLists.txt
require_literal "Vulkan Direct imports OHOS NativeBuffer" \
    "vkGetNativeBufferPropertiesOHOS" \
    entry/src/main/cpp/graphics/native_window_vk_target.cpp
require_literal "Vulkan Direct imports acquire fence on GPU" \
    "vkAcquireImageOHOS" entry/src/main/cpp/graphics/native_window_vk_target.cpp
require_literal "Vulkan Direct exports GPU release fence" \
    "vkQueueSignalReleaseImageOHOS" \
    entry/src/main/cpp/graphics/native_window_vk_target.cpp
require_literal "Vulkan Direct flushes SurfaceQueue buffer" \
    "OH_NativeWindow_NativeWindowFlushBuffer" \
    entry/src/main/cpp/graphics/native_window_vk_target.cpp
require_literal "Vulkan Direct owner includes surface generation" \
    "slot.surfaceKey == surfaceKey_" \
    entry/src/main/cpp/graphics/native_window_vk_target.cpp
require_literal "Vulkan Direct owner includes device identity" \
    "slot.physicalDevice == physicalDevice_ && slot.device == device_" \
    entry/src/main/cpp/graphics/native_window_vk_target.cpp
require_literal "Vulkan Direct import cache has a fail-safe bound" \
    "kMaxImportedSlotsPerAttach = 64" \
    entry/src/main/cpp/graphics/native_window_vk_target.h
reject_literal "Vulkan Direct import cache never evicts a live slot" \
    "DestroySlot(slots_[0])" entry/src/main/cpp/graphics/native_window_vk_target.cpp
reject_literal "Vulkan Direct target has no product runtime switch" \
    "WINEHUA_DIRECT" entry/src/main/cpp/graphics/native_window_vk_target.cpp
reject_literal "Venus Direct selection has no product runtime switch" \
    "WINEHUA_DIRECT" entry/src/main/cpp/graphics/venus_surface_presenter.cpp
reject_literal "Vulkan Direct target bypasses WSI acquire" \
    "vkAcquireNextImageKHR" entry/src/main/cpp/graphics/native_window_vk_target.cpp
reject_literal "Vulkan Direct target bypasses WSI present" \
    "vkQueuePresentKHR" entry/src/main/cpp/graphics/native_window_vk_target.cpp
require_literal "Venus Direct has latched WSI fallback" \
    "transport fallback latched" entry/src/main/cpp/graphics/venus_surface_presenter.cpp
require_literal "Venus Direct does not wait after successful present" \
    "post_present_cpu_wait=0" entry/src/main/cpp/graphics/venus_surface_presenter.cpp
require_literal "Venus transports use common present copy recorder" \
    "RecordPresentCopyLocked(" entry/src/main/cpp/graphics/venus_surface_presenter.cpp
require_literal_count "Venus has one Vulkan image-copy implementation" \
    "vkCmdCopyImage(" 1 entry/src/main/cpp/graphics/venus_surface_presenter.cpp
require_literal_count "Venus has one Vulkan image-blit implementation" \
    "vkCmdBlitImage(" 1 entry/src/main/cpp/graphics/venus_surface_presenter.cpp
require_literal_count "Venus has one checked frame-fence reset" \
    "vkResetFences(" 1 entry/src/main/cpp/graphics/venus_surface_presenter.cpp
require_literal "Venus tracks actually submitted frame slots" \
    "bool submitted = false;" entry/src/main/cpp/graphics/venus_surface_presenter.cpp
require_literal "Venus skips redundant waits for completed slots" \
    "if (frame.submitted)" entry/src/main/cpp/graphics/venus_surface_presenter.cpp
require_literal "Venus WSI rebuild waits only outstanding slots" \
    "WaitForOutstandingFramesLocked()" \
    entry/src/main/cpp/graphics/venus_surface_presenter.cpp
require_literal_count "Venus has one recoverable WSI target-loss policy" \
    "bool IsRecoverableWsiTargetLoss(" 1 \
    entry/src/main/cpp/graphics/venus_surface_presenter.cpp
require_literal "Current Mesa private Present ordering is audited" \
    "vn_ring_wait_all(dev->primary_ring)" \
    docs/graphics/DIRECT_PRESENT_REFERENCE_AUDIT.md
require_literal "Current Host queue guard fallback is audited" \
    "vkr_winehua_release_queue(&queue_guard)" \
    docs/graphics/DIRECT_PRESENT_REFERENCE_AUDIT.md
for presenter_file in \
    entry/src/main/cpp/graphics/virgl_surface_presenter.cpp \
    entry/src/main/cpp/graphics/venus_surface_presenter.cpp; do
    require_literal "Common presenter period normalization" \
        "NormalizePresentFramePeriodNs" "$presenter_file"
    reject_literal "Removed presenter-local period normalization" \
        "NormalizeFramePeriodNs" "$presenter_file"
done
require_literal "GL queue matches its display-paced consumer" \
    "QueuePresentPacingPeriodNs" \
    entry/src/main/cpp/graphics/virgl_surface_presenter.cpp
require_literal "Venus keeps deadline dispatch lead" \
    "PresentPacingPeriodNs" \
    entry/src/main/cpp/graphics/venus_surface_presenter.cpp
require_literal "ArkTS backend intent API" \
    "setHostGraphicsBackend(this.hostGraphicsBackend)" \
    entry/src/main/ets/service/WineEngineService.ets
require_literal "LAB automation experiment resolver" \
    "selectHostGraphicsExperimentForLab(graphicsExperiment)" \
    entry/src/main/ets/entryability/EntryAbility.ets
require_literal "Applied Host experiment propagated per launch" \
    'WINEHUA_GRAPHICS_PROFILE=${this.appliedHostGraphicsLabExperiment}' \
    entry/src/main/ets/service/WineEngineService.ets
require_literal "Direct Wine child forwards graphics experiment to session policy" \
    "policy.extraEnv = options.environment;" \
    entry/src/main/cpp/wine/wine_exe.cpp
require_literal "Session environment extracts graphics experiment" \
    'FindEnvValue(probeBase, "WINEHUA_GRAPHICS_PROFILE")' \
    entry/src/main/cpp/wine/env_profiles.cpp
require_literal "Session environment resolves Guest experiment" \
    "ResolveLabGraphicsExperiment" \
    entry/src/main/cpp/wine/env_profiles.cpp
require_literal "Session environment applies resolved experiment" \
    "AppendProductDxvkEnv(env, d3dBackend, graphicsExperiment)" \
    entry/src/main/cpp/wine/env_profiles.cpp
vkd3d_dxvk_companion="$(require_value defaults vkd3d_dxvk_companion)"
require_literal "VKD3D DXVK companion" \
    "AppendD3dBackendEnv(env, \"dxvk_modern_2_6\", binDir);" \
    entry/src/main/cpp/wine/wine_env.cpp
require_literal "VKD3D companion directory" \
    "\"$vkd3d_dxvk_companion\", \"2.6.2\"," \
    entry/src/main/cpp/graphics/graphics_profile.cpp
require_literal "Modern mapped flush capability owner" \
    "if (dxvkRuntime.batchMappedFlush)" entry/src/main/cpp/wine/wine_env.cpp
require_literal "Legacy mapped flush defaults to the product high-performance path" \
    "true,  // batchMappedFlush" entry/src/main/cpp/graphics/graphics_profile.cpp
if ! sed -n '/^void AppendProductDxvkEnv(/,/^}/p' \
        "$ROOT/entry/src/main/cpp/wine/wine_env.cpp" | \
        grep -Fq -- "ResolveDxvkRuntimeProfile(backend, &dxvkRuntime)"; then
    fail "Modern mapped flush runtime capability is not resolved in AppendProductDxvkEnv"
fi
printf 'graphics-contract: OK: Modern mapped flush runtime capability scope\n'
reject_literal "Modern mapped flush per-range diagnostic atomic" \
    "g_winehuaMappedFlushBatchLogged" \
    thirdparty/dxvk-modern/src/dxvk/dxvk_cmdlist.cpp
require_literal "Modern mapped flush online allocation coalescing" \
    "previous.storage.ptr() == storage.ptr()" \
    thirdparty/dxvk-modern/src/dxvk/dxvk_cmdlist.cpp
require_literal "Modern mapped flush ignores empty ranges before enqueue" \
    "if (!range.size)" \
    thirdparty/dxvk-modern/src/dxvk/dxvk_cmdlist.cpp
require_literal "Modern mapped flush records pre-coalescing request count" \
    "m_winehuaMappedFlushQueuedRanges += 1" \
    thirdparty/dxvk-modern/src/dxvk/dxvk_cmdlist.cpp
require_literal "Modern mapped flush uses shared range merger" \
    "winehuaMergeMappedRange(" \
    thirdparty/dxvk-modern/src/dxvk/dxvk_cmdlist.cpp
require_literal_count "Modern mapped flush has one range-end implementation" \
    "inline uint64_t winehuaMappedRangeEnd(" 1 \
    thirdparty/dxvk-modern/src/dxvk/dxvk_winehua_mapped_range.h
require_literal "Modern API trace compiled out by default" \
    "#define DXVK_WINEHUA_ENABLE_API_TRACE 0" \
    thirdparty/dxvk-modern/src/util/util_winehua_api_trace.h
require_literal "Modern disabled API trace is a no-op" \
    "#define WINEHUA_API_TRACE() do { } while (0)" \
    thirdparty/dxvk-modern/src/util/util_winehua_api_trace.h

vkd3d_map_patch="$ROOT/patches/vkd3d-proton/0001-probe-recover-validated-VKD3D-2.6-500K-profile.patch"
vkd3d_execute_patch="$ROOT/patches/vkd3d-proton/0019-vkd3d-flush-mapped-upload-width-on-execute.patch"
[[ -f "$vkd3d_map_patch" ]] || fail "missing VKD3D Map Width patch"
[[ -f "$vkd3d_execute_patch" ]] || fail "missing VKD3D Execute Width patch"
mapfile -t vkd3d_patches < <(find "$ROOT/patches/vkd3d-proton" -maxdepth 1 \
    -type f -name '*.patch' -print | sort)
[[ "${#vkd3d_patches[@]}" -eq 19 ]] || \
    fail "VKD3D patch count expected=19 actual=${#vkd3d_patches[@]}"
for index in "${!vkd3d_patches[@]}"; do
    expected_prefix="$(printf '%04d-' "$((index + 1))")"
    patch_name="$(basename "${vkd3d_patches[$index]}")"
    [[ "$patch_name" == "$expected_prefix"* ]] || \
        fail "VKD3D patch order expected=$expected_prefix actual=$patch_name"
    head -n 1 "${vkd3d_patches[$index]}" | sed 's/\r$//' | \
        grep -Eq '^From [0-9a-f]{40} Mon Sep 17 00:00:00 2001$' || \
        fail "VKD3D patch mail header invalid: $patch_name"
done
printf 'graphics-contract: OK: VKD3D patch series=0001..0019\n'
assert_equal "VKD3D Map Width patch" \
    "$(require_value patches vkd3d_map_width_sha256)" \
    "$(normalized_sha256 "$vkd3d_map_patch")"
assert_equal "VKD3D Execute Width patch" \
    "$(require_value patches vkd3d_execute_width_sha256)" \
    "$(normalized_sha256 "$vkd3d_execute_patch")"
mapped_upload_fix="$(require_value reference mapped_upload_fix)"
require_literal "VKD3D mapped upload provenance" \
    "From $mapped_upload_fix Mon Sep 17 00:00:00 2001" \
    patches/vkd3d-proton/0019-vkd3d-flush-mapped-upload-width-on-execute.patch
require_literal "VKD3D Execute Width hook" \
    "vkd3d_winehua_flush_mapped_upload_buffers(command_queue->device);" \
    patches/vkd3d-proton/0019-vkd3d-flush-mapped-upload-width-on-execute.patch

require_literal "Graphics cache validates input key" \
    'grep -qx "input_sha256=$input_key"' scripts/build_cache.sh
require_literal "Graphics cache validates artifact hashes" \
    'WINEHUA_CACHE_MISS_REASON="artifact-hash:$index"' scripts/build_cache.sh
require_literal "Graphics cache rejects parent repository discovery" \
    'printf '\''invalid-repository\n'\''' scripts/build_cache.sh
require_literal "Graphics cache fingerprints shader compiler" \
    'meson ninja python3 glslangValidator patch' scripts/build_cache.sh
require_literal "Modern cache repairs stale shader compiler path" \
    'DXVK Modern cached glslangValidator disappeared' scripts/build_dxvk_modern.sh
require_literal "VKD3D isolated source has deterministic build id" \
    'deterministic_build_id="${base_commit:0:15}"' scripts/build_vkd3d_proton.sh
require_literal "VKD3D manifest records deterministic build id" \
    '"buildId": "$deterministic_build_id"' scripts/build_vkd3d_proton.sh
for cached_build_script in \
    scripts/build_dxvk.sh \
    scripts/build_dxvk_modern.sh \
    scripts/build_vkd3d_proton.sh \
    scripts/build_ohos_guest_gfx.sh \
    scripts/build_ohos_guest_vulkan.sh; do
    require_literal "Graphics build sources content cache" \
        'source "$SCRIPT_DIR/build_cache.sh"' "$cached_build_script"
    require_literal "Graphics build verifies content cache" \
        'winehua_cache_verify "$CACHE_MANIFEST"' "$cached_build_script"
    require_literal "Graphics build records content cache" \
        'winehua_cache_write "$CACHE_MANIFEST"' "$cached_build_script"
done
require_literal "Guest Vulkan cache includes Loader commit" \
    '"loader=$LOADER_COMMIT"' scripts/build_ohos_guest_vulkan.sh
require_literal "Guest Vulkan cache includes replay inputs" \
    '"$LOADER_PATCH" "$ROOT/smoke" "$ROOT/replay_spv"' scripts/build_ohos_guest_vulkan.sh
require_literal "Guest Vulkan cache verifies full output tree" \
    'find "$OUTPUT_ROOT" -type f -print0' scripts/build_ohos_guest_vulkan.sh
require_literal "Guest graphics cache includes OHOS clang" \
    '"ohos-clang=$CACHE_OHOS_CLANG"' scripts/build_ohos_guest_gfx.sh
require_literal "Guest graphics cache verifies selected output tree" \
    'find "$CACHE_OUTPUT_ROOT" -type f -print0' scripts/build_ohos_guest_gfx.sh
require_literal "Make always verifies legacy DXVK cache" \
    '$(DXVK_ARTIFACTS): dxvk-cache-check' Makefile
require_literal "Make always verifies modern DXVK cache" \
    '$(DXVK_MODERN_ARTIFACTS): dxvk-modern-cache-check' Makefile
require_literal "Make always verifies VKD3D cache" \
    '$(VKD3D_PROTON_ARTIFACTS): vkd3d-proton-cache-check' Makefile

printf 'graphics-contract: all checks passed\n'
