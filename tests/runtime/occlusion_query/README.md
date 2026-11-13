# D3D11 occlusion query generation/pass probe

Build with the existing MinGW compiler, for example:

```
i686-w64-mingw32-g++ -O2 -static -o occlusion-probe.exe occlusion-probe.cpp -ld3d11
```

Run with the installed legacy DXVK and an external 60-second process timeout.
The test writes `C:\\occlusion-result.txt` (only 20 detailed failures at most).
It draws a visible triangle, a depth-occluded triangle and a nearer triangle,
reuses the same query slots for 128 frames, and checks real 4096/0/4096 counts.
Four variants cover ordinary polling, end-of-frame EVENT waits, per-draw waits,
and immutable constants. Success is 1536 queries, zero incorrect results and
exit code 0. Each GPU wait is also bounded to five seconds.

On Edge the old query path produced incorrect counts even with GPU EVENT waits
and immutable constants. The generation guard plus a render-pass split at query
end passed all variants (1536 queries). This does not qualify every Vulkan GPU.
The managed Maleoon quirk is enabled by default; the standard OHOS/GNU launch
environment enables `DXVK_WINEHUA_COMMAND_QUERY_RESET=1`. Explicit
`DXVK_WINEHUA_SPLIT_OCCLUSION_PASS=0/1` remains available for controlled A/B.
