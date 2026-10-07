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
      "$repo/scripts/steam_legacy_zstd/vpsteamzstd.c" "${inputs[@]}" \
      "$out/vpsteamcrt_minimal64.o" -Wl,--entry,VPHelperEntry \
      -Wl,--image-base,0x30bc30000 -Wl,--exclude-all-symbols \
      -Wl,--no-insert-timestamp -lkernel32 -lgcc \
      -o "$out/vpsteamzstd64.dll"
    continue
  fi
  "$arch-w64-mingw32-gcc" -O2 -Wall -Wextra -Werror -shared -static-libgcc \
    -DZSTD_DISABLE_ASM=1 -DZSTD_LEGACY_SUPPORT=0 -I"$src" \
    "$repo/scripts/steam_legacy_zstd/vpsteamzstd.c" "${inputs[@]}" \
    -Wl,--image-base,"$image_base" -Wl,--exclude-all-symbols -Wl,--no-insert-timestamp \
    -o "$out/vpsteamzstd$bits.dll"
done
i686-w64-mingw32-gcc -O2 -Wall -Wextra -Werror -shared -static-libgcc \
  "$repo/scripts/steam_legacy_zstd/vpsteamtrust.c" -lbcrypt \
  -Wl,--image-base,0x6fa40000 -Wl,--exclude-all-symbols -Wl,--no-insert-timestamp \
  -o "$out/vpsteamtrust32.dll"
