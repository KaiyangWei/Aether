# Aether Engine — Phase 12: Spatiotemporal Denoising & Hybrid Rendering Report

## 1. Unified Hybrid Rendering Pipeline

Phase 12 integrates [`VulkanHybridGBufferPass`](../Engine/RHI/Vulkan/Passes/VulkanHybridGBufferPass.h), [`VulkanRtShadowPass`](../Engine/RHI/Vulkan/Passes/VulkanRtShadowPass.h), [`VulkanRtReflectionPass`](../Engine/RHI/Vulkan/Passes/VulkanRtReflectionPass.h), [`VulkanRtDenoiserPass`](../Engine/RHI/Vulkan/Passes/VulkanRtDenoiserPass.h), and [`VulkanBloomPass`](../Engine/RHI/Vulkan/Passes/VulkanBloomPass.h) (`BloomInputSource::HybridComposite`) into a cohesive production Hybrid RT pipeline:

```text
TLAS Update/Build
  └─► Hybrid G-Buffer (RT0..RT3 + Depth)
        ├─► RT Shadows (cs_6_5 Inline Ray Query)
        └─► RT Reflections (cs_6_5 Mirror + GGX VNDF)
              └─► Temporal Reprojection & Variance Moments (rt_denoise_temporal.comp)
                    └─► Edge-Avoiding À-Trous 3× Filter (rt_denoise_atrous.comp)
                          └─► Unified Composite & 12 Debug Views (hybrid_composite.comp)
                                └─► HDR Bloom (HybridComposite) → Exposure/ACES ToneMap → Present
```

- **TAA Isolation**: When `RenderMode::HybridRayTracing` runs, `VulkanTaaPass` is cleanly bypassed and `VulkanRenderer::ResetRtDenoiserHistory()` is triggered whenever switching modes, resizing, dropping scenes, or changing camera framing abruptly.
- **Interactive Controls**:
  - `Q`: Switches `Rasterization <-> HybridRayTracing` without duplicating scene/camera/mesh buffers and without calling `vkDeviceWaitIdle()`.
  - `G`: Cycles all 12 `HybridDebugView` visualizations (`FinalHybrid`, `RtShadowsOnly`, `RtReflectionsOnly`, `ShadowVisibility`, `ShadowSampleCount`, `ReflectionContribution`, `ReflectionHitMiss`, `ReflectionHitDistance`, `SurfaceRoughness`, `TemporalConfidence`, `DenoiserVariance`, `RayQueryDiagnostic`).

---

## 2. Spatiotemporal Denoiser Design

### 2.1 Temporal Reprojection & Disocclusion Rejection (`rt_denoise_temporal.comp.hlsl`)
- Back-projects current pixel world position to previous-frame object/world space via `inst.invModelRows` and `inst.prevModelRows`, then projects through `prevUnjitteredViewProj`.
- Evaluates 2×2 bilinear history taps from the previous frame slot's `RT1`/`RT3` and history targets (`shadowHistory`, `reflHistory`, `momentsHistory`), rejecting taps when:
  - Instance ID mismatches (`pInstId != currInstId`)
  - Normal deviates (`dot(currN, pNormal) < 0.90f`)
  - World-space discontinuity exceeds `max(0.08, 0.04 * viewDist)`
- Tracks first and second luminance moments `(sM1, sM2, rM1, rM2)` and blends temporal variance with a 3×3 spatial variance fallback when `histLen < 4.0`.
- Adaptive blend rates: `[temporalAlphaMin = 0.06, temporalAlphaMax = 0.25]`; mirrors (`roughness <= 0.05`) retain fast response (`>= 0.35`) to prevent reflection ghosting during motion.

### 2.2 Edge-Avoiding À-Trous Spatial Filter (`rt_denoise_atrous.comp.hlsl`)
- Executes `atrousIterations = 3` iterations (`stepSize = 1, 2, 4`) with 5×5 B-spline kernel `[0.0625, 0.25, 0.375, 0.25, 0.0625]`.
- Preserves crisp geometry, material, and mirror boundaries via joint edge-stopping weights on:
  - Instance ID equality
  - Normal similarity (`dot(cN, nN)^64`)
  - Tangent-plane distance (`sigmaDepth = 0.03 * stepSize`)
  - Variance-guided luminance (`sigmaLuma = 2.0 * stdDev`) and roughness (`exp(-|Δr| * 16.0)`)
- **Mirror Bypass**: Pixels with `roughness <= minMirrorRoughness (0.05)` skip reflection blurring while still denoising direct soft shadows cleanly.

---

## 3. Quantitative Validation & Multi-Resolution Benchmark (RTX 5090)

### 3.1 `rt-denoise` Quantitative Verification (`exit=0`, `valHits=0`)
- Static convergence across 16 frames: `conf0 = 0.06250 → conf15 = 0.99799`
- 180° camera snap disocclusion rejection: `confJump = 0.06250`, recovering cleanly within 14 frames (`>= 0.70`)
- Quantitative local spatial gradient noise reduction (Denoiser OFF vs ON, 1 spp): `noiseOff = 0.10790 → noiseOn = 0.08818`
- `Q`-switch back to `Rasterization`: verified `IsRtDenoiserHistoryValid() == false`.

### 3.2 Full RTX 5090 Multi-Resolution GPU Timestamp Benchmark (`rt-hybrid`, Warmup=8, Measured=24, 4 spp, `exit=0`, `valHits=0`)

| Pass / Metric (Median ms) | 1280×720 | 1920×1080 | 3840×2160 (4K) |
| --- | ---: | ---: | ---: |
| Raster Total Frame (Shadow+TAA+Bloom) | 0.4071 ms | 0.5324 ms | 1.1121 ms |
| **Hybrid Total Frame** | **0.8542 ms** | **1.7971 ms** | **6.1018 ms** |
| TLAS Update (dynamic object) | 0.0180 ms | 0.0175 ms | 0.0178 ms |
| Hybrid G-Buffer (4-MRT) | 0.1655 ms | 0.1839 ms | 0.6287 ms |
| RT Shadows (4 spp + multi-light) | 0.2140 ms | 0.4864 ms | 2.1587 ms |
| RT Reflections (Mirror + GGX VNDF) | 0.0352 ms | 0.0654 ms | 0.2476 ms |
| Denoiser Temporal | 0.0315 ms | 0.0796 ms | 0.3344 ms |
| Denoiser À-Trous (3 iterations) | 0.2496 ms | 0.5336 ms | 1.9964 ms |
| Hybrid Composite | 0.0320 ms | 0.0545 ms | 0.1834 ms |
| Hybrid Bloom (`HybridComposite`) | 0.0641 ms | 0.0883 ms | 0.2097 ms |
| RayQuery Diagnostic Dispatch | 0.0285 ms | 0.0671 ms | 0.2721 ms |

### 3.3 Visual Evidence & Full Regression Status
- All **25** captured BMPs (`00a..00d` Phase 9D baseline + `01..21` Cornell Box, `DamagedHelmet.glb`, and all debug views) were converted to PNG and visually verified.
- Pure CPU CTests: **11/11 passed**.
- Isolated GPU tests (`VK_LAYER_VALIDATE_SYNC=1`): **6/6** Next-Gen RT tests passed (`valHits=0`) and **21/21** existing regression tests passed (`valHits=0`), including non-RT software fallback (`rt-hybrid` fallback instance), empty-scene stability, multi-resolution resize, and zero VMA leaks.
