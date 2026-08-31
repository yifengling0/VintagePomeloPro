#!/usr/bin/env bash
set -euo pipefail
ROOT="$(git rev-parse --show-toplevel)"
TOOL_HOME="${TOOL_HOME:-/apps/harmony}"
NODE="$TOOL_HOME/tool/node/bin/node"
TSC="$TOOL_HOME/sdk/default/openharmony/ets/build-tools/ets-loader/node_modules/typescript/lib/tsc.js"
OUTPUT="${MODEL_TEST_DIR:-$ROOT/build/host_tests/models}"
mkdir -p "$OUTPUT/ts/model" "$OUTPUT/ts/common" "$OUTPUT/js"
sources=(model/AppModels model/AppCatalogRules model/PerformanceHudSettings common/EvdevKeyNames)
inputs=()
for source in "${sources[@]}"; do
    cp "$ROOT/entry/src/main/ets/$source.ets" "$OUTPUT/ts/$source.ts"
    inputs+=("$OUTPUT/ts/$source.ts")
done
"$NODE" "$TSC" --target ES2020 --module commonjs --skipLibCheck \
    --rootDir "$OUTPUT/ts" --outDir "$OUTPUT/js" "${inputs[@]}"
"$NODE" "$ROOT/scripts/run_catalog_unit_tests.cjs" "$OUTPUT/js/model"
"$NODE" "$ROOT/scripts/run_input_controls_unit_tests.cjs" "$OUTPUT/js"
