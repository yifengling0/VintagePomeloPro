# win-fg — sources & licenses

Where every part of win-fg comes from, how it's built, and under what license.

> Verified current as of 2026-08-12 (through device bring-up).

## What Win-FG Native is

> **Note (2026-09-09, v0.3.0):** the project is named **Win-FG Native** since the chain
> moved host-side. Provenance is unaffected by where it runs — the same shaders,
> from the same sources, are compiled into the host compositor and into the
> layer `.so`. Build outputs keep their original names (`libwin_fg.so`,
> `VkLayer_win_framegen.json`).


A color-only frame-generation layer for Android / Vulkan (Turnip / Adreno). From
two real frames it synthesises an interpolated in-between frame to raise perceived
frame rate. Two stages — optical-flow motion estimation, then frame synthesis —
wired into a standard Vulkan implicit layer.

## Shaders

Motion estimation is our own subgroup-free reimplementation of the **AMD
FidelityFX FSR3 optical flow** algorithm (MIT); synthesis is written from first
principles. Full FidelityFX attribution in
[`NOTICE_FIDELITYFX_OPTICALFLOW.md`](../NOTICE_FIDELITYFX_OPTICALFLOW.md).

| Component | File | Built from | License |
|---|---|---|---|
| Perceptual luma | `shaders/of3_luma.comp` | our adaptation of FSR3 `prepare_luma` | MIT (AMD FidelityFX) |
| Luma pyramid | `shaders/of3_downsample.comp` | our adaptation of FSR3 luminance pyramid | MIT (AMD FidelityFX) |
| Coarse→fine flow | `shaders/of3_flow.comp` | our subgroup-free reimpl of the FSR3 SAD block search | MIT (AMD FidelityFX) |
| Flow expand | `shaders/of3_expand.comp` | our adaptation of the FSR3 scale pass | MIT (AMD FidelityFX) |
| Bidirectional flow | `shaders/of3_flow_m4.comp` | our extension (independent fwd + bwd search) | MIT (ours) |
| Occlusion-gated expand | `shaders/of3_expand_m4.comp` | our extension of the above (+ C1 global-motion add-back) | MIT (ours) |
| **Global-motion LK reduce (C1)** | `shaders/of3_gm_reduce.comp` | **written from first principles** (inverse-compositional LK normal equations) | MIT (ours) |
| **Global-motion pre-warp (C1)** | `shaders/of3_gm_prewarp.comp` | **written from first principles** (affine image warp) | MIT (ours) |
| **Flow regularization (C2)** | `shaders/of3_flowreg.comp` | **written from first principles** (TV-L1 smoothness prior, semi-implicit) | MIT (ours) |
| **Frame synthesis** | `shaders/wfg_synth.comp` | **written from first principles** | MIT (ours) |

## Synthesis math

`wfg_synth.comp` is motion-compensated interpolation: a bidirectional backward
warp, an importance-weighted (softmax) blend of the two warped candidates, a
flow-magnitude clamp, and a photometric + forward/backward-consistency gate that
cross-fades where the flow is unreliable. The underlying math is **reimplemented
from published papers** (equations only — no third-party code):

| Idea | Source |
|---|---|
| Importance-weighted blend ("softmax splatting") | Niklaus & Liu, CVPR 2020 (arXiv:2003.05534) |
| Brightness-constancy photometric residual | Horn & Schunck, 1981 |
| Forward/backward consistency (occlusion) | Sundaram et al. 2010; OCAI (arXiv:2403.18092) |
| Dual disocclusion / reprojection fallback | AMD FidelityFX FSR3 frame interpolation (MIT) |

## Global-motion pre-warp math (C1)

`of3_gm_reduce.comp` + the CPU 6x6 solve in `src/record_impl.inc` +
`of3_gm_prewarp.comp` estimate the per-frame camera affine and remove it before
the dense flow search, so the search only sees object-only residual motion (kills
fast-pan "melt"). The math is **reimplemented from published sources** (equations
only — no code from LSFG, any `wnfg_*` blob, GameScope, or any NC-licensed
source):

| Idea | Source |
|---|---|
| Inverse-compositional Lucas-Kanade image alignment (6-param affine H = Σ SD·SDᵀ, b = Σ SD·e, template-gradient steepest-descent images, W ← W ∘ W(Δp)⁻¹ update) | Baker & Matthews, "Lucas-Kanade 20 Years On", IJCV 2004 (CMU-RI TR 2001) |
| Parametric image alignment / affine warp background | Szeliski, "Computer Vision: Algorithms and Applications", §6.2 |

## Flow regularization math (C2)

`of3_flowreg.comp` applies a variational **TV-L1 smoothness prior** to the dense
flow field left by C1 (the object-only residual at `flowLvl_[kFlowFinest]`, 1/4-res).
It suppresses incoherent / spurious per-block vectors while **preserving true
motion discontinuities** (object silhouettes) — an L1 data term + edge-aware
Total-Variation regularizer, minimising

    min_u   Σ  g(x)·|∇u(x)|   +   λ·|u(x) − f0(x)|

where `u` is the regularized flow, `f0` the raw SAD flow (fixed data term), `|∇u|`
the Total-Variation (L1-of-gradient) term, and `g(x)∈(0,1]` a luma-gradient
edge-stop weight. The math is **reimplemented from published sources** (equations
only — no code from LSFG, any `wnfg_*` blob, GameScope, or any NC-licensed source):

| Idea | Source |
|---|---|
| TV-L1 optical-flow energy + L1 data soft-threshold (thresholding step) | Zach, Pock & Bischof, "A Duality Based Approach for Realtime TV-L1 Optical Flow", DAGM 2007 |
| Primal-dual TV framework | Chambolle & Pock, "A First-Order Primal-Dual Algorithm for Convex Problems…", JMIV 2011 |
| Total-Variation (ROF) regularization | Rudin, Osher & Fatemi, "Nonlinear total variation based noise removal", Physica D 1992 |
| Lagged-diffusivity / semi-implicit TV solver (the scheme actually used) | Vogel & Oman 1996; Chan, Golub & Mulet, SIAM J. Sci. Comput. 1999 |
| Edge-aware (image-gradient) weighting of the smoothness term | Perona & Malik 1990; Nagel & Enkelmann 1986; Weickert 1998 |
| L2 smoothness baseline this L1 variant deliberately replaces | Horn & Schunck, 1981 |

**Scheme note.** The textbook Chambolle-Pock primal-dual keeps a persistent dual
field and uses step sizes `(σ,τ)` that must satisfy `στL²≤1` or it diverges — a
tuning-sensitive instability we avoid on Turnip, and one that breaks the
one-shader / ping-pong-two-flow-images contract (it needs extra dual images).
`of3_flowreg.comp` solves the **same energy** with a lagged-diffusivity
semi-implicit (Jacobi) step: the optimal TV dual `p = g·∇u/|∇u|_ε` (which has
`|p|≤1` by construction) is re-derived in closed form each iteration, the primal
update is a convex combination of the centre and its neighbours — **unconditionally
stable for any dt>0, never NaN** — followed by the exact L1 data-term
soft-threshold `u = uDiff − clamp(uDiff−f0, −θ, θ)`, `θ = dt·λ`. Only `u` is
carried between iterations (ping-pong two rgba16f flow images), N iterations.
Runs at `kFlowFinest` (1/4-res); the cleaned finest flow is copied back in place,
so `of3_expand`'s 5-tap median + C1 global-motion add-back consume it unchanged.
**0 iterations ⇒ flow field untouched ⇒ byte-identical to pre-C2.**
| Huber M-estimator (robustify the fit against foreground/occlusion) | Huber, 1964 (standard robust least-squares) |
| Bilinear affine image resample (the pre-warp itself) | Szeliski, §3.6 |

Implementation notes: the LK runs at 1/8-res luma; the GPU only **accumulates**
the normal equations (no float atomics — each of a fixed set of threads writes its
own partial sum, the CPU sums in double and does the Cholesky solve, so it is
portable to bare GLSL on Turnip/Adreno); the solve is **decoupled** (reads back a
reduce SSBO the present ring already fence-waited → no mid-frame GPU stall) and
runs one Gauss-Newton step per frame warm-started from the running estimate. The
prewarp removes the affine and `of3_expand(_m4)` composes it back, so an identity
(or disengaged) affine makes the whole pipeline **byte-identical to pre-C1** — the
bail-to-identity path can never be worse than "no pre-warp".

## Host layer & tooling — win-fg's own code

Everything under `src/` and `tools/`, plus the build and manifest files, is
win-fg's own code, written from the open Vulkan-layer references below.

| Files | What |
|---|---|
| `src/layer.cpp` | Vulkan implicit layer: negotiation, dispatch, swapchain + present interception, frame insertion |
| `src/framegen.{hpp,cpp}`, `src/record_impl.inc` | compute engine: pipelines, per-size resources, the flow→synth dispatch graph |
| `src/vk_dispatch.hpp`, `src/config.hpp`, `src/log.hpp` | dispatch tables, runtime config + hot-reload, logging |
| `src/embedded_shaders.hpp`, `tools/embed_spv.py` | generator + embedded SPIR-V of win-fg's own shaders |
| `CMakeLists.txt`, `manifest/*.json`, `tools/build_shaders.sh`, `VERSION` | build, layer manifest, shader compile, version |

Written from these open, permissively-licensed references:

| Part | Reference | License |
|---|---|---|
| Layer skeleton (negotiation, dispatch, present interception) | Khronos Vulkan-Loader layer interface; renderdoc "Vulkan layer guide" | Apache-2.0 / CC-BY |
| Vulkan headers (`vk_layer.h`, `vulkan.h`) | KhronosGroup/Vulkan-Headers | Apache-2.0 |
| Compute pipeline / descriptor patterns | Khronos Vulkan-Samples; Vulkan specification | Apache-2.0 |

## How it fits together

```
[ real prev frame ]   [ real curr frame ]
        │                     │
        ▼                     ▼
  of3_luma → of3_downsample ─┬─────────────────────────────────────────────┐
        │                    │                                              │
        │            of3_gm_reduce (1/8 res) → SSBO → CPU 6x6 LK solve      │  C1 global-motion
        │                    │            (affine, decoupled/stall-free)    │  pre-warp (MIT, ours)
        │                    ▼                                              │
        │            of3_gm_prewarp: stabilize curr luma (remove camera) ◄──┘
        ▼                    │
  of3_flow (on prev + STABILIZED curr → object-only residual)                 (FSR3 optical flow, MIT)
        │
  of3_flowreg  ── C2: TV-L1 smoothness prior on the residual (N iters, 1/4-res) ── (MIT, ours)
        │        kills spurious vectors, keeps object edges sharp; identity ⇒ pre-C2
        ▼
  of3_expand   (5-tap median → confidence gate → C1 global-motion add-back)    (FSR3 scale, MIT)
        │                                        ▲ adds the global affine back
        ▼   flowFwd (curr→prev) + flowBwd (prev→curr) + confidence  (full = global + residual)
        │
      wfg_synth      warp + importance blend + consistency gate      (written here, MIT)
        │
        ▼
[ generated in-between frame ]
```

## License

win-fg is **MIT**. The FidelityFX-derived optical-flow passes are MIT — see
[`NOTICE_FIDELITYFX_OPTICALFLOW.md`](../NOTICE_FIDELITYFX_OPTICALFLOW.md).
Every third-party reference used is listed above under its own permissive license.
