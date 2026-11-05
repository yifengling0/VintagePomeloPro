# OHOS Wine audio regressions

Build either `.cpp` with the existing llvm-mingw x86_64 compiler:

```sh
x86_64-w64-mingw32-clang++ -O2 -std=c++17 -static spatial-audio-probe.cpp -lole32 -luuid -o spatial-audio-probe.exe
x86_64-w64-mingw32-clang++ -O2 -std=c++17 -static multichannel-probe.cpp -lole32 -luuid -o multichannel-probe.exe
```

Run through the normal OHOS Wine launcher. Results are written to
`C:\spatial-audio-result.txt` and `C:\multichannel-result.txt`.
No game assets or saves are required. Both emit a short 440 Hz test tone.

The spatial probe uses the real `ISpatialAudioObjectRenderStream` event contract:
12 static speaker channels, front-left object data, 480 frames per 10 ms period,
and 300 updates. It deliberately does not sleep or check padding to hide an
incorrect writable-event notification. Expect 144000 frames in about three
seconds, not a burst of events followed by `AUDCLNT_E_BUFFER_TOO_LARGE`.
The ordinary WASAPI probe also writes 144000 frames and exercises all 12 input
channels while respecting buffer padding.

On Edge, before patch 0081 the spatial probe failed with HRESULT 0x88890006 after
27 updates / 12960 frames in 38 ms. With 0080 (multichannel downmix) and 0081
(render event pacing), it passed 300 updates / 144000 frames in 3068 ms. This is
an API regression result; speaker audibility and actual games require separate
verification.
