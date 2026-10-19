#!/usr/bin/env bash
set -euo pipefail
ROOT="$(git rev-parse --show-toplevel)"
TOOL_HOME="${TOOL_HOME:-/apps/harmony}"
NODE="$TOOL_HOME/tool/node/bin/node"
TSC="$TOOL_HOME/sdk/default/openharmony/ets/build-tools/ets-loader/node_modules/typescript/lib/tsc.js"
OUTPUT="${MODEL_TEST_DIR:-$ROOT/build/host_tests/models}"
mkdir -p "$OUTPUT/ts/model" "$OUTPUT/ts/common" "$OUTPUT/js"
sources=(model/AppModels model/AppCatalogRules model/PerformanceHudSettings model/ValueAddedModels model/MembershipCommerce model/CloudSaveModels model/SaveBackupModels model/AccountSyncModels common/EvdevKeyNames)
inputs=()
for source in "${sources[@]}"; do
    cp "$ROOT/entry/src/main/ets/$source.ets" "$OUTPUT/ts/$source.ts"
    inputs+=("$OUTPUT/ts/$source.ts")
done
"$NODE" "$TSC" --target ES2020 --module commonjs --skipLibCheck \
    --rootDir "$OUTPUT/ts" --outDir "$OUTPUT/js" "${inputs[@]}"
"$NODE" "$ROOT/scripts/run_value_added_unit_tests.cjs" "$OUTPUT/js"
"$NODE" "$ROOT/scripts/run_catalog_unit_tests.cjs" "$OUTPUT/js/model"
"$NODE" "$ROOT/scripts/run_input_controls_unit_tests.cjs" "$OUTPUT/js"
"$NODE" "$ROOT/scripts/run_app_session_unit_tests.cjs" "$OUTPUT/js" "${TSC%/tsc.js}/typescript.js"
"$NODE" "$ROOT/scripts/run_host_window_task_tests.cjs" "${TSC%/tsc.js}/typescript.js"
"$NODE" "$ROOT/scripts/run_managed_window_input_tests.cjs" "$OUTPUT/js" "${TSC%/tsc.js}/typescript.js"
"$NODE" "$ROOT/scripts/run_linux_steam_window_host_tests.cjs" "${TSC%/tsc.js}/typescript.js"
"$NODE" "$ROOT/scripts/run_linux_steam_cloud_tool_tests.cjs" "${TSC%/tsc.js}/typescript.js"
"$NODE" "$ROOT/scripts/run_linux_steam_library_tests.cjs" "${TSC%/tsc.js}/typescript.js"

"$NODE" "$ROOT/scripts/run_local_save_unit_tests.cjs" "$OUTPUT/js" "${TSC%/tsc.js}/typescript.js"
"$NODE" "$ROOT/scripts/run_value_added_service_tests.cjs" "$OUTPUT/js" "${TSC%/tsc.js}/typescript.js"
"$NODE" "$ROOT/scripts/run_coffee_unit_tests.cjs" "${TSC%/tsc.js}/typescript.js"
"$NODE" "$ROOT/scripts/run_automatic_sync_unit_tests.cjs" "$OUTPUT/js" "${TSC%/tsc.js}/typescript.js"
"$NODE" "$ROOT/scripts/run_native_contract_unit_tests.cjs" "${TSC%/tsc.js}/typescript.js"
"$NODE" "$ROOT/scripts/run_cloud_save_unit_tests.cjs" "$OUTPUT/js" "${TSC%/tsc.js}/typescript.js"
"$NODE" "$ROOT/scripts/run_mouse_input_unit_tests.cjs" "${TSC%/tsc.js}/typescript.js"
"$NODE" "$ROOT/scripts/run_configuration_unit_tests.cjs" "${TSC%/tsc.js}/typescript.js"
"$NODE" "$ROOT/scripts/run_wine_upgrade_guard_tests.cjs" "${TSC%/tsc.js}/typescript.js"
