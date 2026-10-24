#!/bin/bash
set -euo pipefail
[ "$#" -eq 1 ] || { echo "usage: $0 <virglrenderer-source-root>" >&2; exit 64; }
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SOURCE="$(cd "$1" && pwd)"
for name in 0001-fix-blit-srgb-capability-routing.patch 0002-gles-minimum-ubo-plain-sysvals.patch 0003-gles-query-conditional-render.patch 0004-resource-copy-preserve-srgb-bits.patch 0005-gles-attachment-srgb-write-policy.patch 0006-gl-present-producer-context.patch; do
    PATCH="$SCRIPT_DIR/../patches/virglrenderer/$name"
    # Source exports may sit inside a different Git repository (or have no
    # Git metadata in Docker). Check the actual files, with a fixed direction.
    if patch -d "$SOURCE" -p1 --batch --force --fuzz=0 --dry-run -R < "$PATCH" >/dev/null 2>&1; then
        continue
    fi
    patch -d "$SOURCE" -p1 --batch --force --fuzz=0 --dry-run < "$PATCH"
    patch -d "$SOURCE" -p1 --batch --force --fuzz=0 --no-backup-if-mismatch < "$PATCH"
done
