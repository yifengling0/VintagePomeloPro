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
make test-windows-steam
"$runtime_node" scripts/run_input_surface_bounds_tests.cjs "$runtime_typescript"
"$runtime_node" scripts/run_mouse_input_unit_tests.cjs "$runtime_typescript"
"$runtime_node" scripts/run_desktop_restore_tests.cjs "$runtime_typescript"
bash scripts/apply_virglrenderer_ohos_patches.sh thirdparty/virglrenderer
python3 host_tests/venus_precise_context_test.py
python3 host_tests/venus_shadow_submission_test.py
python3 scripts/apply_dxvk_compat_patches.py --source thirdparty/dxvk
python3 host_tests/dxgi_shared_instance_test.py
make test-font-import test-wine-font-compat
python3 host_tests/v2_product_contract_test.py
python3 host_tests/managed_render_route_test.py
python3 host_tests/wayland_close_lifecycle_test.py
python3 host_tests/gpu_owner_contract_test.py
python3 host_tests/steam_gpu_contract_test.py
python3 host_tests/gpu_scene_input_test.py
python3 host_tests/egl_multi_consumer_test.py
python3 host_tests/compositor_migration_test.py --asan
for suite in run_host_window_tasks_tests run_edge_window_tests run_wine_ime_tests run_steam_startup_diagnostics_tests; do
  "$runtime_node" "scripts/$suite.cjs" "$runtime_typescript"
done
