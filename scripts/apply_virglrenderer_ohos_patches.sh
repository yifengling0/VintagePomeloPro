#!/bin/bash
set -euo pipefail
[ "$#" -eq 1 ] || { echo "usage: $0 <virglrenderer-source-root>" >&2; exit 64; }
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SOURCE="$(cd "$1" && pwd)"
# Later overlays can change the context of earlier ones, so reverse-testing
# each patch separately is insufficient for an already complete stack. Accept
# that case only when EVERY touched file and patch matches the reviewed lock.
if python3 - "$SOURCE" "$SCRIPT_DIR/.." <<'PY'
import hashlib, json, re, sys
from pathlib import Path
source, root = map(Path, sys.argv[1:])
lock = json.loads((root/'docs/graphics/graphics-stack.lock.yaml').read_text())
rows = lock['managed_patches']['virglrenderer']
patches = sorted((root/'patches/virglrenderer').glob('*.patch'))
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
if [(r['path'], r['sha256']) for r in rows] != [(str(p.relative_to(root)), sha(p)) for p in patches]:
    sys.exit(1)
files = {n for p in patches for n in re.findall(r'^\+\+\+ b/(.+)$', p.read_text(), re.M)}
effective = lock['effective_files']['virglrenderer']
sys.exit(0 if files and all(n in effective and (source/n).is_file() and not (source/n).is_symlink() and
    sha(source/n) == effective[n] for n in files) else 1)
PY
then
    echo "virglrenderer: complete reviewed overlay already applied"
    exit 0
fi
for name in 0001-fix-blit-srgb-capability-routing.patch 0002-gles-minimum-ubo-plain-sysvals.patch 0003-gles-query-conditional-render.patch 0004-resource-copy-preserve-srgb-bits.patch 0005-gles-attachment-srgb-write-policy.patch 0006-gl-present-producer-context.patch 0007-ohos-native-memory-context.patch 0008-ohos-native-resource-contract.patch 0009-device-object-cleanup.patch 0010-render-resource-fd-close.patch 0011-negotiate-precise-shadow-per-context.patch 0012-validate-precise-context-at-renderer-entry.patch; do
    PATCH="$SCRIPT_DIR/../patches/virglrenderer/$name"
    # Source exports may sit inside a different Git repository (or have no
    # Git metadata in Docker). Check the actual files, with a fixed direction.
    if patch -d "$SOURCE" -p1 --batch --force --fuzz=0 --dry-run -R < "$PATCH" >/dev/null 2>&1; then
        continue
    fi
    patch -d "$SOURCE" -p1 --batch --force --fuzz=0 --dry-run < "$PATCH"
    patch -d "$SOURCE" -p1 --batch --force --fuzz=0 --no-backup-if-mismatch < "$PATCH"
done
