#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:?usage: build.sh /absolute/output/directory}
mkdir -p "$out"
src="$repo/entry/src/main/cpp/steam_decompress/zstd"
sources=(zstd_decompress.c zstd_decompress_block.c zstd_ddict.c zstd_common.c
  huf_decompress.c fse_decompress.c entropy_common.c error_private.c debug.c hist.c xxhash.c)
inputs=()
for f in "${sources[@]}"; do inputs+=("$src/$f"); done
decoder_source="$repo/scripts/steam_legacy_zstd/vpsteamzstd.c"
if [[ "${VPP_STEAM_DECODE_DIAGNOSTIC:-0}" == 1 ]]; then
  decoder_source="$repo/scripts/steam_legacy_zstd/decoder_diagnostic.c"
  [[ "${VPP_STEAM_TRUST_CLIENT_SHA256:-}" =~ ^[0-9a-f]{64}$ ]] || {
    echo 'Diagnostic build requires the exact diagnostic-patched PE32 client SHA256' >&2
    exit 1
  }
fi
for arch in i686 x86_64; do
  bits=32; [[ "$arch" == x86_64 ]] && bits=64
  # GNU ld otherwise derives the preferred base from the output path. That
  # changes the PE32 decoder hash and breaks the exact startup trust guard.
  # Keep ASLR/relocations enabled; only make the preferred base reproducible.
  image_base=0x6cac0000
  [[ "$bits" == 64 ]] && image_base=0x2fc080000
  if [[ "$bits" == 64 && "${VPP_STEAM_NOCRT64:-0}" == 1 ]]; then
    "$arch-w64-mingw32-gcc" -O2 -Wall -Wextra -Werror -fno-builtin \
      -fno-tree-loop-distribute-patterns -c \
      "$repo/scripts/steam_legacy_zstd/vpsteamcrt_minimal.c" \
      -o "$out/vpsteamcrt_minimal64.o"
    "$arch-w64-mingw32-gcc" -O2 -Wall -Wextra -Werror -shared -nostdlib -DNDEBUG \
      -DZSTD_DISABLE_ASM=1 -DZSTD_LEGACY_SUPPORT=0 -I"$src" \
      "$decoder_source" "${inputs[@]}" \
      "$out/vpsteamcrt_minimal64.o" -Wl,--entry,VPHelperEntry \
      -Wl,--image-base,0x30bc30000 -Wl,--exclude-all-symbols \
      -Wl,--no-insert-timestamp -lkernel32 -lgcc \
      -o "$out/vpsteamzstd64.dll"
    continue
  fi
  "$arch-w64-mingw32-gcc" -O2 -Wall -Wextra -Werror -shared -static-libgcc \
    -DZSTD_DISABLE_ASM=1 -DZSTD_LEGACY_SUPPORT=0 -I"$src" \
    "$decoder_source" "${inputs[@]}" \
    -Wl,--image-base,"$image_base" -Wl,--exclude-all-symbols -Wl,--no-insert-timestamp \
    -o "$out/vpsteamzstd$bits.dll"
done
trust_args=()
if [[ "${VPP_STEAM_DECODE_DIAGNOSTIC:-0}" == 1 ]]; then
  python3 - "$out/vpsteamzstd32.dll" "$out/trust_expected.h" "$VPP_STEAM_TRUST_CLIENT_SHA256" <<'PY'
import hashlib, sys
from pathlib import Path
decoder = hashlib.sha256(Path(sys.argv[1]).read_bytes()).hexdigest()
records = [('client_sha', sys.argv[3]), ('decoder_sha', decoder)]
Path(sys.argv[2]).write_text('\n'.join(
    'static const unsigned char '+name+'[32] = {'+
    ','.join('0x'+digest[i:i+2] for i in range(0,64,2))+'};'
    for name, digest in records)+'\n')
PY
  trust_args+=("-DVP_STEAM_TRUST_HASH_HEADER=\"$out/trust_expected.h\"")
fi
i686-w64-mingw32-gcc -O2 -Wall -Wextra -Werror -shared -static-libgcc \
  "${trust_args[@]}" "$repo/scripts/steam_legacy_zstd/vpsteamtrust.c" -lbcrypt \
  -Wl,--image-base,0x6fa40000 -Wl,--exclude-all-symbols -Wl,--no-insert-timestamp \
  -o "$out/vpsteamtrust32.dll"
