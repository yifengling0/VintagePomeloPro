# Font import and Valve font compatibility repair

Base: `feature/main_proton` at `73dd4c16cbe0d7edf6d4208dec6545c1721f93ba`.
Wine gitlink remains `cd547f7a0ee9d3d59b3ec47317a7852dedf0443b`.

## Evidence and changes

The ZIP importer itself was identical to main. The native ARM64 layout added
by `63d282da` could package the SDK's link-only `libz.so`; the recorded assemble
log contains an 8,936-byte native copy. `e6825e9` had already isolated Steam's
inflater, but `libentry` still depended on dynamic zlib. This is a verified
packaging defect, not proof of the binding in the user's installed HAP.

- Font extraction now uses its own hidden, static copy of the existing vendored
  inflate sources, with explicit `calloc`/`free` callbacks required by `Z_SOLO`.
  Steam's separate implementation and other consumers' zlib contracts are intact.
- Only non-font extensions increment `bad`. Archive/read/unsupported/limit/
  inflater-init/decompression-or-CRC/font-content/write errors are typed and
  fail the import before live fonts are touched. Partial files are removed;
  preexisting staging files/symlinks are not overwritten. Existing size,
  filename and font-magic checks remain.
- Replacement copies every file to an operation-owned sibling first, then
  renames old fonts to backup and installs the complete new directory. A failed
  install restores the old directory. A failed rollback keeps the complete
  backup and reports its path. Cleanup uses lstat and never follows old font
  symlinks outside the backup. Cancellation never calls replacement.
- The three libz packaging pickers accept a validated built runtime, never an SDK
  fallback. Native schemes can use system libz when no built override exists.
  Only an exact SDK-stub copy at the generated destination is removed; unknown
  stale files block packaging and remain untouched. Guest/Box64 requires a real
  guest runtime rather than silently assuming a wrapped fallback. The existing
  `BOX64_EMULATED_LIBS` entries are unchanged.
- ELF checks reject all-function-alias SDK stubs, check zlib's independent runtime
  functions, and inspect generated libraries plus final HAP/nested wine-data
  payloads. Other dependency selection and SDK `libc` handling are unchanged.
- Ordered Valve overlays `0023`–`0026` restore grayscale defaults, direct prefix
  font scans, locale-aware fallback and musl selected-locale handling. Original
  legacy patch files are retained. `0014` remains the sole bitmap/gray override.
  Non-OHOS paths retain upstream behavior. Japanese/Korean faces remain
  registered; explicit charsets and vertical fonts bypass the new locale
  fallback. Unlike the old patch, matching uses the requested codepage signature.

## Deterministic gates

Run from the repository top-level Makefile (no Docker required):

```
make test-font-import test-wine-font-compat
```

- Actual extractor + actual hidden Z_SOLO objects: real Wine font in Store and
  344-entry Deflate ZIPs; invalid font, non-font extension, unsupported method,
  init/read/write/CRC/decompression/size-limit failures; partial-write cleanup,
  preexisting-file retention; no dynamic libz dependency or exported inflater.
- Unchanged ArkTS transaction body executed by Node 18+ after erasing only its
  exact checked type signature (NODE/PATH/SDK runtime discovery):
  staging copy, backup move, final move and rollback failures, path collisions,
  successful replacement and fresh-directory creation.
- Synthetic ELF provenance: real implementation, SDK alias stub, copied/symlinked
  stub, known/unknown generated residue, required guest runtime, native system
  fallback, and nested HAP payload checks.
- Full registered Wine chain: no-fuzz replay and identical second replay from
  the selected Wine HEAD. Extracted production helpers run in OHOS/non-OHOS
  configurations for default/explicit charsets, grayscale with/without
  fontconfig, prefix scans and selected locale. This is host logic validation,
  not real FreeType rendering or device font discovery.

## Remaining validation and recovery limits

No HAP build, signing, deployment, device linker binding, ArkTS SDK compilation,
or game rendering is claimed here. Validate a rebuilt native ARM64 package,
Store/Deflate import, restart/discovery and bitmap/gray CJK text on device.
A guest build without a genuine guest libz now fails with an actionable error
instead of packaging the SDK stub. This gate does not synthesize a replacement
for every compression API used by unrelated consumers.

The sibling renames are same-filesystem and recover ordinary operation failures,
but are not a crash-proof filesystem transaction. Process termination between
the two renames can leave the old directory in the uniquely named backup; it is
not automatically deleted or adopted by a later import. Recover that backup
before retrying. No source trees, SDK libraries or unknown user files are reset.
