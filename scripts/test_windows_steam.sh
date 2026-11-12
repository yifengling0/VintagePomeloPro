#!/usr/bin/env bash
# Product Windows Steam acceptance shared by local builds and CI.
set -euo pipefail
steam_repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$steam_repo_root"
steam_tool_home="${TOOL_HOME:-/apps/harmony}"
steam_node="${NODE_BIN:-$steam_tool_home/tool/node/bin/node}"
steam_typescript="${ARKTS_TYPESCRIPT:-$steam_tool_home/sdk/default/openharmony/ets/build-tools/ets-loader/node_modules/typescript}"
if [[ ! -x "$steam_node" || ! -f "$steam_typescript/lib/typescript.js" ]]; then
  printf '%s\n' 'Set NODE_BIN and ARKTS_TYPESCRIPT (or TOOL_HOME) to the installed Harmony toolchain.' >&2
  exit 2
fi
python3 host_tests/windows_steam_runtime_test.py
make test-steam-client-args
for suite in run_steam_local_unit_tests run_steam_local_service_tests; do
  "$steam_node" "scripts/$suite.cjs" "$steam_typescript"
done
