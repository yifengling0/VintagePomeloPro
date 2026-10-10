#!/bin/bash
set -euo pipefail
[ "$#" -eq 1 ] || { echo "usage: $0 <virglrenderer-source-root>" >&2; exit 64; }
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SOURCE="$(cd "$1" && pwd)"
for name in 0001-fix-blit-srgb-capability-routing.patch 0002-gles-minimum-ubo-plain-sysvals.patch 0003-gles-query-conditional-render.patch 0004-resource-copy-preserve-srgb-bits.patch; do
    PATCH="$SCRIPT_DIR/../patches/virglrenderer/$name"
    if git -c safe.directory="$SOURCE" -C "$SOURCE" apply --reverse --check "$PATCH" >/dev/null 2>&1; then
        continue
    fi
    git -c safe.directory="$SOURCE" -C "$SOURCE" apply --check "$PATCH"
    git -c safe.directory="$SOURCE" -C "$SOURCE" apply "$PATCH"
done
