#!/bin/bash
# Apply the pinned Mesa OHOS deltas required by the Wine runtime.
set -euo pipefail

[ "$#" -eq 1 ] || {
    echo "usage: $0 <mesa-source-root>" >&2
    exit 64
}

MESA_SOURCE="$1"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PATCHES=(
    "$SCRIPT_DIR/../patches/mesa/0001-ohos-arm64-keep-vtest-map-fd.patch"
    "$SCRIPT_DIR/../patches/mesa/0002-virgl-vtest-wine-owned-low-map.patch"
    "$SCRIPT_DIR/../patches/mesa/0003-venus-present-retry-single-ring-drain.patch"
    "$SCRIPT_DIR/../patches/mesa/0004-venus-wine-owned-low-map.patch"
    "$SCRIPT_DIR/../patches/mesa/0005-zink-ohos-explicit-vulkan-loader.patch"
    "$SCRIPT_DIR/../patches/mesa/0006-virgl-vtest-serialize-socket-transactions.patch"
    "$SCRIPT_DIR/../patches/mesa/0007-virgl-gpu-srgb-surface-storage.patch"
    "$SCRIPT_DIR/../patches/mesa/0008-virgl-explicit-pbuffer-front-present.patch"
)

[ -f "$MESA_SOURCE/meson.build" ] || {
    echo "Mesa source root is invalid: $MESA_SOURCE" >&2
    exit 1
}
MESA_SOURCE="$(cd "$MESA_SOURCE" && pwd)"
for PATCH in "${PATCHES[@]}"; do
    [ -f "$PATCH" ] || {
        echo "Mesa OHOS patch is missing: $PATCH" >&2
        exit 1
    }
    if git -c safe.directory="$MESA_SOURCE" -C "$MESA_SOURCE" \
        apply --reverse --check "$PATCH" >/dev/null 2>&1; then
        continue
    fi
    git -c safe.directory="$MESA_SOURCE" -C "$MESA_SOURCE" apply --check "$PATCH" || {
        echo "Mesa source does not match the pinned OHOS patch: $MESA_SOURCE ($PATCH)" >&2
        exit 1
    }
    git -c safe.directory="$MESA_SOURCE" -C "$MESA_SOURCE" apply "$PATCH"
done
# Mounted linked worktrees can lack their external Git metadata. git apply
# still validates every hunk there; only the repository diff needs metadata.
if git -c safe.directory="$MESA_SOURCE" -C "$MESA_SOURCE" rev-parse \
    --show-toplevel >/dev/null 2>&1; then
    git -c safe.directory="$MESA_SOURCE" -C "$MESA_SOURCE" diff --check
fi
