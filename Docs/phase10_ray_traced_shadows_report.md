# Aether Engine — Phase 10: Hardware Ray-Traced Shadows Report

## 1. Architecture & Resource Ownership

Phase 10 introduces a hardware-accelerated direct lighting & shadow pass while keeping the existing PCF Directional Shadow Map path and non-RT Rasterization mode completely intact.

- **G-Buffer Pass ([`VulkanHybridGBufferPass`](../Engine/RHI/Vulkan/Passes/VulkanHybridGBufferPass.h))**:
  - Owns 4 per-frame-slot MRT `VulkanImage` color targets (`VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT`) and two Dynamic Rendering pipelines (`VulkanGraphicsPipeline::InitializeMrt`) for PBR (`hybrid_gbuffer.frag.hlsl`) and vertex-color fallback (`hybrid_gbuffer_color.frag.hlsl`).
  - Reuses the renderer's shared depth buffer (`VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL`, cleared on entry):
    - `RT0` (`R16G16B16A16_SFLOAT`): `rgb = albedo`, `a = metallic`
    - `RT1` (`R16G16B16A16_SFLOAT`): `rgb = shadingNormal`, `a = roughness`
    - `RT2` (`R16G16B16A16_SFLOAT`): `rgb = emissive`, `a = ao`
    - `RT3` (`R32G32B32A32_SFLOAT`): `rgb = worldPos`, `a = float(rtInstanceIndex + 1)` (`0.0` indicates sky/background)
  - Inactive frame slots' `RT1`/`RT3` are cleared cleanly on first execution via Synchronization2 barriers so temporal reprojection never reads `VK_IMAGE_LAYOUT_UNDEFINED` images.
- **RT Shadow Pass ([`VulkanRtShadowPass`](../Engine/RHI/Vulkan/Passes/VulkanRtShadowPass.h), [`Shaders/rt_shadow.comp.hlsl`](../Shaders/rt_shadow.comp.hlsl))**:
  - Profile: `cs_6_5` (`8×8` workgroups) using inline Vulkan Ray Query (`RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES>`).
  - Outputs `directLightImage` (`R16G16B16A16_SFLOAT`: `rgb = shadowed direct radiance`, `a = combined visibility [0, 1]`) and `shadowAuxImage` (`R16G16_SFLOAT`: `r = directional sun visibility`, `g = normalized raysTraced / 16.0`).
  - When `tlasHandle == VK_NULL_HANDLE` (e.g., empty scene), clears output targets via `vkCmdClearColorImage` (using `VK_IMAGE_USAGE_TRANSFER_DST_BIT`) and skips `TraceRayInline`.

---

## 2. Shader Algorithm Details

### 2.1 Origin Bias & Self-Intersection Elimination
- Matches CPU helper `ComputeShadowRayOrigin()` in [`Engine/Renderer/RtLighting.h`](../Engine/Renderer/RtLighting.h):
  - Normal offset: `normalBias + viewDist * distanceBias` (defaults `0.004f` and `0.0015f`)
  - View offset: `normalBias * 0.75f + viewDist * distanceBias * 0.5f`
- Prevents grazing shadow acne on curved geometry (Cornell spheres, `DamagedHelmet.glb`) and flat walls.

### 2.2 Soft Sun Disk & Multi-Light Stochastic Evaluation
- **Directional Sun**: Samples within cone angle `tan(clamp(sunAngularRadiusRad, 0.0, 0.25))` using per-pixel **Interleaved Gradient Noise (IGN)** + **R2 low-discrepancy quasirandom sequence**, normalized cleanly by `1 / shadowSamples` (`shadowSamples` clamped to `[1, 16]`).
- **RectArea / Point / Spot Scene Lights**: Evaluates `kMaxRtSceneLights = 8` lights (`GetCornellBoxLights()` in Cornell Box). `RectArea` lights draw stratified 2×2 samples (`clamp(shadowSamples, 1, 4)`) across their rectangular emitter quad with window attenuation and cosine geometry factor.
- **Real Alpha-MASK Cutout Evaluation**: TLAS sets `FORCE_OPAQUE` on opaque instances and `FORCE_NO_OPAQUE` on `alphaMask` instances (`alphaBlend` instances remain excluded from TLAS). Inside `while (q.Proceed())`, `CANDIDATE_NON_OPAQUE_TRIANGLE` fetches per-triangle UVs via `g_indices`/`g_vertices`, interpolates barycentric UV, samples `g_alphaMasks[inst.alphaMaskTexIndex]` via `SampleLevel(..., 0.0)`, and commits only when `alpha >= alphaCutoff`.

---

## 3. Verification & Performance (RTX 5090)

Isolated `rt-shadow` (`VK_LAYER_VALIDATE_SYNC=1`) passed all GPU checks with **`exit=0`, `valHits=0`**:
1. **Unoccluded Bias Check**: Analytical ground visibility `>= 0.98` (zero self-intersection acne).
2. **Hard Shadow Discrimination**: Blocker aligned along `sunDir` drops center pixel visibility to `< 0.05` while distant lit ground stays `>= 0.95`.
3. **Procedural Alpha-MASK Cutout**: 4×2 RGBA PNG (left half `alpha=255`, right half `alpha=0`) accurately casts opaque shadow (`<= 0.1`) on the opaque projection side and transmits full sunlight (`>= 0.9`) on the transparent side.
4. **Soft Penumbra & Unbiased Normalization**: Overhead `RectArea` + angled sun produce full umbra (`<= 0.15`), smooth intermediate penumbra (`(0.08, 0.92)`), full lit region (`>= 0.90`), and open-lit radiance invariance (`<= 15%` variance) across `1 spp` vs `8 spp`.
5. **Lifecycle & Leaks**: Multi-light Cornell Box rendering, swapchain resize (`1280×720 → 960×540`), and clean VMA teardown with 0 leaks.

Measured GPU timestamps at **4 spp** (`rt-hybrid` benchmark, Warmup=8, Measured=24):
- `1280×720`: Hybrid G-Buffer `0.1655 ms`, RT Shadows (4 spp) `0.2140 ms`
- `1920×1080`: Hybrid G-Buffer `0.1839 ms`, RT Shadows (4 spp) `0.4864 ms`
- `3840×2160`: Hybrid G-Buffer `0.6287 ms`, RT Shadows (4 spp) `2.1587 ms`
