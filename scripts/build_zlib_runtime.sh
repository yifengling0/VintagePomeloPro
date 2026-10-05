#!/bin/bash
# Optional real OHOS guest libz for builds whose dependency cache has SDK stubs.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/env.sh"
guest_arch="${GUEST_ARCH:-$WINE_ARCH}"
case "$guest_arch" in aarch64|x86_64) ;; *) err "unsupported zlib guest arch: $guest_arch" ;; esac
archive="$ROOT/.temp/zlib-1.3.1.tar.gz"
# Same upstream archive pin as thirdparty/mesa/subprojects/zlib.wrap.
expected=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
mkdir -p "$ROOT/.temp" "$BUILD_DIR/zlib-runtime/$guest_arch"
if [ ! -f "$archive" ]; then
    curl -fL --connect-timeout 20 --max-time 60 \
        https://zlib.net/fossils/zlib-1.3.1.tar.gz -o "$archive.part"
    printf '%s  %s\n' "$expected" "$archive.part" | sha256sum -c -
    mv "$archive.part" "$archive"
fi
printf '%s  %s\n' "$expected" "$archive" | sha256sum -c -
work="$(mktemp -d "$BUILD_DIR/zlib-runtime-$guest_arch.XXXXXX")"
tar -xzf "$archive" -C "$work" --strip-components=1
sources=(adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees trees uncompr zutil)
inputs=()
for unit in "${sources[@]}"; do inputs+=("$work/$unit.c"); done
output="$BUILD_DIR/zlib-runtime/$guest_arch/lib"
mkdir -p "$output"
"$CLANG" --target="$guest_arch-linux-ohos" --sysroot="$SYSROOT" \
    -O2 -fPIC -DHAVE_UNISTD_H -DHAVE_HIDDEN -D_LARGEFILE64_SOURCE=1 \
    -shared -fuse-ld=lld -Wl,--no-undefined -Wl,-soname,libz.so \
    -I "$work" "${inputs[@]}" -o "$work/libz.so"
python3 "$SCRIPT_DIR/runtime_library_guard.py" "$work/libz.so"
mv "$work/libz.so" "$output/libz.so"
cp "$work/README" "$BUILD_DIR/zlib-runtime/$guest_arch/README.upstream"
printf 'zlib=1.3.1\nsource_sha256=%s\ntarget=%s-linux-ohos\n' "$expected" "$guest_arch" \
    > "$BUILD_DIR/zlib-runtime/$guest_arch/build-info.txt"
sha256sum "$output/libz.so" >> "$BUILD_DIR/zlib-runtime/$guest_arch/build-info.txt"
log "Real zlib runtime: $output/libz.so (full deflate/inflate/gzip APIs)"
