#!/usr/bin/env sh
# Compile every win-fg shader to Turnip-safe SPIR-V (Vulkan 1.1 / SPIR-V 1.3).
set -e
cd "$(dirname "$0")/.."
mkdir -p build/spv
for s in of3_luma of3_downsample of3_flow of3_expand of3_flow_m4 of3_expand_m4 of3_gm_reduce of3_gm_prewarp of3_flowreg wfg_synth; do
  glslangValidator -e main -S comp --target-env vulkan1.1 -V "shaders/$s.comp" -o "build/spv/$s.spv"
  echo "OK  $s -> build/spv/$s.spv ($(stat -c%s build/spv/$s.spv) B)"
done
