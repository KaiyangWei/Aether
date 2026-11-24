# Aether Engine — Phase 11: Hardware Ray-Traced Reflections Report

## 1. Architecture & Resource Bindings

[`VulkanRtReflectionPass`](../Engine/RHI/Vulkan/Passes/VulkanRtReflectionPass.h) + [`Shaders/rt_reflection.comp.hlsl`](../Shaders/rt_reflection.comp.hlsl) (`cs_6_5`, `8×8` workgroups) compute physically based specular reflections directly from the G-Buffer and TLAS:

- **Descriptor Set 0** (13 bindings):
  - `b0`: `ReflectionParamsUbo` (736 bytes: camera pos + TLAS flag, sun params, roughness cutoffs, bias, light flags, up to 8 `RtGpuLight` records)
  - `t1`: `RaytracingAccelerationStructure g_tlas`
  - `t2..t5`: G-Buffer (`rt0AlbedoMetallic`, `rt1NormalRoughness`, `rt2EmissiveAo`, `rt3WorldPosInst`)
  - `t6..t8`: Consolidated `RtGpuInstanceData[]`, `RtGpuVertex[]`, `uint[]` indices
  - `t9..t10`: `g_alphaMasks[8]`, `g_alphaSamplers[8]`
  - `u11..u12`: `reflectionImage` (`R16G16B16A16_SFLOAT`), `reflectionAuxImage` (`R16G16_SFLOAT`: `x = hit/miss (1/0)`, `y = committed hit distance`)
- **Descriptor Set 1**: Shared `VulkanEnvironment` layout (`irradianceMap`, `prefilteredMap`, `brdfLut`, `envSampler`, `envCube`) for seamless sky/HDR reflection fallback.

---

## 2. Specular Ray Generation & Hit Shading

### 2.1 Mirror Fast-Path & GGX VNDF Importance Sampling
- **Matte Cutoff**: Surfaces above `reflectionMaxRoughness = 0.72` early-out and smoothly crossfade to prefiltered IBL in the composite pass (`smoothstep(0.78 * maxRough, maxRough, roughness)`).
- **Mirror Fast-Path (`roughness <= minMirrorRoughness = 0.05`)**: Traces exact specular reflection `L = reflect(-V, N)` weighted by `SchlickFresnel(dot(N, V), f0)`.
- **Rough Specular (`0.05 < roughness <= 0.72`)**: Samples the **Heitz GGX Visible Normal Distribution Function (VNDF)** (`SampleGgxVndfWorld` in tangent frame using IGN + R2 sequence), reflects `-V` across sampled microfacet half-vector `H`, and weights by `SchlickFresnel(dot(V, H), f0) * SmithG1(dot(N, L), alpha^2)`.

### 2.2 Hit-Point Lighting & Secondary Shadows
- Reads committed triangle attributes (`hitInst`, barycentric interpolated object normal transformed via `invModelRows` inverse-transpose).
- Evaluates full reflection radiance:
  - Hit surface `emissive` (preserving crisp reflections of the Cornell cyan/amber neon tubes and pedestal strips)
  - Directional sun BRDF modulated by hardware secondary shadow ray (`TraceSecondaryShadow`) when `secondaryShadow` is enabled
  - Fill light + explicit scene local lights (`RectArea`, `Point`, `Spot`)
  - Indirect diffuse/specular IBL (`g_irradianceMap`, `g_prefilteredMap`, `g_brdfLut`) modulated by `hitAo`
- **Miss Fallback**: Uses sharp `g_envCube` (`mip = 0`) for mirrors and mip-mapped `g_prefilteredMap` (`roughness * 7.0`, firefly-clamped at luminance `24.0`) for glossy/rough surfaces.
- Empty scenes (`tlasHandle == VK_NULL_HANDLE`) clear `reflectionImage`/`reflectionAuxImage` with `VK_IMAGE_USAGE_TRANSFER_DST_BIT` cleanly.

---

## 3. Visual & Numerical Validation (RTX 5090)

Isolated `rt-reflection` (`VK_LAYER_VALIDATE_SYNC=1`) passed with **`exit=0`, `valHits=0`**:
1. **Analytical Mirror Hit Distance & Reflected Hue**: Mirror floor at `y=0` reflecting a green emissive card at `(0, 2, -2)` (`z=-1.9`) measured exact geometric hit distance (`expectedDist = 1.9 * sqrt(2) ≈ 2.687 m` within `0.25 m`) and strong green-dominant reflected radiance (`g > 1.0`, `g > 3r`, `g > 3b`).
2. **Open-Sky HDR Fallback on Miss**: Moving the emitter behind the camera (`z=+8.0`) produced `hit = 0` and valid non-zero HDR environment radiance (`maxEnv > 0.005`).
3. **Cornell Box & `DamagedHelmet.glb` Visual Checks** (`ng-04`, `ng-07`, `ng-08`, `ng-10`, `ng-13`): Verified sharp mirror reflections on `HeroMirrorPanel` & `HeroChromeSphere`, progressive GGX lobe softening across `CopperTorus` (`0.18`), `GoldSphere` (`0.28`), and `RoughBronzeSphere` (`0.62`), and crisp metallic visor reflections on `DamagedHelmet.glb`.

Measured GPU timestamps (`rt-hybrid` benchmark, Warmup=8, Measured=24):
- `1280×720`: `0.0352 ms` median
- `1920×1080`: `0.0654 ms` median
- `3840×2160`: `0.2476 ms` median
