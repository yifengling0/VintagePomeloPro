"""Exercise the real make stamp recipe across diagnostic/clean switches."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='fex-make-identity-') as tmp:
    root = Path(tmp)
    shutil.copyfile(repo / 'Makefile', root / 'Makefile')
    shutil.copytree(repo / 'scripts', root / 'scripts')
    source = root / 'source'
    source.mkdir()
    build = root / 'build'
    # Only replace the compiler invocation; keep the actual make recipe and
    # identity gate. This fixture never invokes an SDK or compiles FEX.
    (root / 'scripts/build_fex.sh').write_text('''#!/bin/bash
set -euo pipefail
mkdir -p "$BUILD_DIR/fex-pe/Bin" "$BUILD_DIR/fex-ec/Bin"
printf x > "$BUILD_DIR/fex-pe/Bin/libwow64fex.dll"
printf x > "$BUILD_DIR/fex-ec/Bin/libarm64ecfex.dll"
flags='-DFEX_WOW64_EXACT_STORE=1'
if [ "${FEX_EXACTSTORE_DIAGNOSTICS:-0}" = 1 ]; then
    flags="$flags -DFEX_WOW64_EXACT_STORE_DIAGNOSTICS=1"
fi
if [ "${FEX_STRICT_MUL24:-0}" = 1 ]; then
    flags="$flags -DFEX_WOW64_STRICT_MUL24=1"
fi
printf 'CMAKE_CXX_FLAGS:STRING=%s\n' "$flags" > "$BUILD_DIR/fex-pe/CMakeCache.txt"
printf 'CMAKE_CXX_FLAGS:STRING=\n' > "$BUILD_DIR/fex-ec/CMakeCache.txt"
printf 'build\n' >> "$BUILD_DIR/builds.txt"
''')

    def run(mode, count, src=source, mul24=0):
        out = subprocess.run(['make', '--no-print-directory', '-C', str(root), 'fex',
            f'BUILD_DIR={build}', 'NATIVE_ARCH=arm64-v8a', 'WINE_ARCH=aarch64',
            f'FEX_SRC={src}', f'FEX_EXACTSTORE_DIAGNOSTICS={mode}', f'FEX_STRICT_MUL24={mul24}'],
            capture_output=True, text=True, timeout=20, check=True)
        assert (build / 'builds.txt').read_text().count('build\n') == count, out.stdout + out.stderr

    run(0, 1)
    run(0, 1)
    run(1, 2)
    run(1, 2)
    run(0, 3)
    # A hash change with restored mtime must still rebuild.
    patch = root / 'scripts/patches/fex-wow64-exact-store-diagnostics.patch'
    old = patch.stat()
    patch.write_text(patch.read_text() + '\n')
    os.utime(patch, ns=(old.st_atime_ns, old.st_mtime_ns))
    run(0, 4)
    other = root / 'other-source'
    other.mkdir()
    run(0, 5, other)
    stamp = build / '.stamps/fex-arm64-v8a-aarch64.identity'
    stamp.unlink()
    run(0, 6, other)
    (build / 'fex-ec/CMakeCache.txt').write_text('CMAKE_CXX_FLAGS:STRING=-DFEX_WOW64_EXACT_STORE=1\n')
    run(0, 7, other)
    (build / 'fex-pe/CMakeCache.txt').write_text(
        'CMAKE_CXX_FLAGS:STRING=-DFEX_WOW64_EXACT_STORE=1 -DFEX_WOW64_EXACT_STORE_DIAGNOSTICS=1\n')
    run(0, 8, other)
    run(0, 8, other)
    run(0, 9, other, mul24=1)
    run(0, 9, other, mul24=1)
    run(0, 10, other, mul24=0)
    (build / 'fex-ec/CMakeCache.txt').write_text('CMAKE_CXX_FLAGS:STRING=-DFEX_WOW64_STRICT_MUL24=1\n')
    run(0, 11, other, mul24=0)
print('PASS: make diagnostic/clean identity, overlay contents, source selector, missing record, and cache contamination')
