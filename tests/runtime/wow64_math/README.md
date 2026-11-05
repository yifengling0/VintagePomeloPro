# WoW64 embedded Mono math regression

Build MathSmoke.cs as an x86 .NET Framework executable against the target Mono mscorlib, and mono_embed.c with i686-w64-mingw32-clang. Run:

```
mono_embed.exe <mono.dll> <managed-assemblies-directory> <MathSmoke.exe>
```

This loads the supplied Mono without modifying it. No game files or saves are written. Results go to C:/windows/temp/mono-math-result.txt and mono-cpu-features.txt. The 65536 cases exercise Exp/Log/Pow/Floor as used in Vorbis lookup dimension calculations, with a separate integer root oracle. Delete old result files before a run. Compare the same Mono under Box, FEX reduced precision, and FEX strict precision. A PASS does not validate game video/audio or the entire runtime.

Do not distribute proprietary Mono/mscorlib or game assemblies with this test. Reuse a locally installed runtime for diagnostics. See docs/architecture/box64-game-engine-compatibility-20261105.md for measured boundaries.
