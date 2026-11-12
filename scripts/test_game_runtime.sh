#!/usr/bin/env bash
# One runtime acceptance list for the local build container and HAP CI.
set -euo pipefail
runtime_repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$runtime_repo_root"
runtime_tool_home="${TOOL_HOME:-/apps/harmony}"
runtime_node="${NODE_BIN:-$runtime_tool_home/tool/node/bin/node}"
runtime_typescript="${ARKTS_TYPESCRIPT:-$runtime_tool_home/sdk/default/openharmony/ets/build-tools/ets-loader/node_modules/typescript}"
if [[ ! -x "$runtime_node" || ! -f "$runtime_typescript/lib/typescript.js" ]]; then
  printf '%s\n' 'Set NODE_BIN and ARKTS_TYPESCRIPT (or TOOL_HOME) to the installed Harmony toolchain.' >&2
  exit 2
fi
mkdir -p ci-artifacts
python3 scripts/build_linux_game_components.py --verify-only
"$runtime_node" scripts/run_input_surface_bounds_tests.cjs "$runtime_typescript"
"$runtime_node" scripts/run_mouse_input_unit_tests.cjs "$runtime_typescript"
python3 host_tests/proton_bootstrap_closure_test.py
g++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined host_tests/proton_setup_cache_test.cpp -o ci-artifacts/proton-setup-cache-test
ci-artifacts/proton-setup-cache-test
for suite in proton_user_paths cloud_tool_selection; do
  g++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined "host_tests/${suite}_test.cpp" -o "ci-artifacts/$suite-test"
  "ci-artifacts/$suite-test"
done
"$runtime_node" scripts/run_desktop_restore_tests.cjs "$runtime_typescript"
python3 host_tests/linux_steam_cloud_paths_test.py
python3 host_tests/linux_steam_exec_access_test.py
python3 host_tests/linux_steam_depot_links_test.py
python3 host_tests/linux_steam_native_fex_test.py
python3 host_tests/linux_steam_arm_plan_test.py
python3 host_tests/linux_proton_lease_test.py
python3 host_tests/linux_proton_group_test.py
python3 host_tests/linux_steam_socket_metadata_test.py
python3 host_tests/linux_steam_native_sdk_test.py
python3 host_tests/linux_steam_native_repairs_test.py
python3 host_tests/linux_steam_native_udev_test.py
python3 host_tests/linux_steam_native_guest_audio_test.py
python3 host_tests/linux_steam_native_guest_ipc_test.py
python3 host_tests/linux_steam_native_signal_context_test.py
python3 host_tests/linux_steam_native_kernel_maps_test.py
python3 host_tests/linux_steam_native_x11_endpoint_test.py
python3 host_tests/linux_steam_native_guest_mapping_test.py
python3 host_tests/linux_steam_native_guest_abi_test.py
python3 scripts/prepare_game_runtime_test_source.py
python3 scripts/build_linux_proton_venus.py --source workspace_temp/hsl-sync/mesa-test-base --output build/gnu-venus-overlays --prepare-only
bash scripts/apply_virglrenderer_ohos_patches.sh thirdparty/virglrenderer
python3 host_tests/gnu_venus_mapped_fd_test.py
python3 host_tests/gnu_venus_bc_contract_test.py
python3 host_tests/venus_precise_context_test.py
python3 host_tests/venus_shadow_submission_test.py
python3 scripts/apply_dxvk_compat_patches.py --source thirdparty/dxvk
python3 host_tests/dxgi_shared_instance_test.py
python3 host_tests/linux_steam_native_window_test.py
python3 host_tests/linux_steam_desktop_test.py
python3 host_tests/linux_native_bc_recording_test.py --build-dir workspace_temp/native-bc-recording
python3 host_tests/linux_runtime_components_test.py
make test-font-import test-wine-font-compat
python3 host_tests/linux_steam_offline_package_test.py
python3 host_tests/release_offline_packages_test.py
python3 host_tests/managed_render_route_test.py
# Exercise production GPU ownership and independent window consumers locally and in CI.
python3 host_tests/wayland_close_lifecycle_test.py
python3 host_tests/gpu_owner_contract_test.py
python3 host_tests/steam_gpu_contract_test.py
python3 host_tests/gpu_scene_input_test.py
python3 host_tests/egl_multi_consumer_test.py
python3 host_tests/compositor_migration_test.py --asan
python3 host_tests/steam_client_restart_test.py --output ci-artifacts/steam-client-restart
for suite in run_linux_proton_modules_tests run_linux_proton_dxvk_tests run_linux_proton_media_tests run_linux_proton_bootstrap_tests run_linux_proton_component_tests run_linux_steam_supervisor_tests run_linux_steam_window_host_tests run_linux_steam_native_host_tests run_edge_window_tests run_wine_ime_tests run_linux_steam_game_launch_tests run_linux_steam_download_tests run_linux_steam_deck_tests run_steam_startup_diagnostics_tests run_steam_local_unit_tests run_steam_local_service_tests; do
  "$runtime_node" "scripts/$suite.cjs" \
    "$runtime_typescript"
done
