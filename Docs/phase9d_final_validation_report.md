# Aether Engine — Phase 9D Final Validation Report

## 1. Mission & Scope

Phase 9D audited and finalized the existing **Vulkan Ray Query + Acceleration Structure** foundation (Phases 9A–9C) on Windows 11 + NVIDIA GeForce RTX 5090 before introducing Hybrid Ray Tracing (Phases 10–12).

Key goals:
1. Archive Phase 9D visual baseline screenshots before modifying shaders or passes.
2. Separate `RenderMode::HybridRayTracing` from diagnostic `RenderMode::RayQuery` while preserving non-RT device fallback and `Q`/`G` interactive switching without `vkDeviceWaitIdle()`.
3. Expose consolidated GPU vertex, index, previous-frame model transform, and alpha-mask texture tables from `VulkanRayTracingScene` with zero VMA allocations in `Initialize()`.
4. Expand `CornellBoxScene` with a **rough metallic** tier (`RoughBronzeSphere`, `metallic = 0.92`, `roughness = 0.62`) and explicit physical scene lights (`GetCornellBoxLights()`).

---

## 2. Implementation Summary

### 2.1 Render Modes & Keyboard Controls ([`Engine/Renderer/RayTracingConfig.h`](../Engine/Renderer/RayTracingConfig.h))
- Extended `RenderMode`:
  - `Rasterization = 0` (startup default and non-RT fallback)
  - `HybridRayTracing = 1` (primary `Q` toggle target when `CanUseHybridRtMode() == true`)
  - `RayQuery = 2` (Phase 9C/9D diagnostic Ray Query path, accessible via `HybridDebugView::RayQueryDiagnostic` or direct `SetRenderMode`)
- Updated `ToggleRenderModeInteractive()` to switch `Rasterization <-> HybridRayTracing` on `Q` press, resetting `VulkanTaaPass` and `VulkanRtDenoiserPass` history cleanly while reusing cached BLAS/TLAS resources.

### 2.2 Consolidated RT Scene Metadata ([`Engine/RHI/Vulkan/RayTracing/VulkanRayTracingScene.h`](../Engine/RHI/Vulkan/RayTracing/VulkanRayTracingScene.h), [`VulkanRayTracingScene.cpp`](../Engine/RHI/Vulkan/RayTracing/VulkanRayTracingScene.cpp))
- Preserved zero VMA allocations in `Initialize()` (validated by `rt.foundation` and `rt.as` allocation tests).
- Built consolidated per-BLAS vertex/index storage buffers (`RtGpuVertex` = 32 bytes: `posAndU`, `normalAndV`; `uint32_t` index stream) and dense per-instance storage buffers (`RtGpuInstanceData` = 208 bytes: `baseColorAndAlphaCutoff`, `emissiveAndMetallic`, `surfaceParams`, `modelRows[3]`, `prevModelRows[3]`, `invModelRows[3]`, `firstVertex`, `firstIndex`, `alphaMaskTexIndex`, `sceneObjectIndex`).
- Collected up to `kMaxRtAlphaMaskTextures = 8` active alpha-mask textures plus a lazy 1×1 white fallback image/sampler, retiring replaced GPU buffers through fence-tracked deferred reclamation (`use_count()` + completed submission serial).

### 2.3 Cornell Box Material Coverage & Physical Lights ([`Engine/Assets/CornellBoxScene.cpp`](../Engine/Assets/CornellBoxScene.cpp))
- Added `RoughBronzeSphere` (`metallic = 0.92`, `roughness = 0.62`) alongside mirror (`0.02`), brushed/satin metallics (`0.18`, `0.28`), and matte dielectrics (`0.76`).
- Verified `CornellBoxValidationSummary` counts (`roughMetallicMaterialCount >= 1`, `midRoughMetallicMaterialCount >= 2`, `mirrorLikeMaterialCount >= 1`, `emissiveMaterialCount >= 3`).
- Added `GetCornellBoxLights()` providing 4 explicit `RtGpuLight` descriptors (80 bytes each):
  - Overhead warm skylight `RectArea` (`(0, 4.42, 0)`, half-extents `1.35 × 1.35`, intensity `3.4`)
  - Back-left cool cyan neon `RectArea` (`(-1.35, 3.30, -2.72)`, intensity `5.2`)
  - Back-right warm amber neon `RectArea` (`(1.85, 3.15, -2.72)`, intensity `5.0`)
  - Pedestal cyan accent `Point` light (`(-0.22, 0.52, 0.42)`, intensity `1.4`)

---

## 3. Baseline Evidence & Validation

| Artifact | Purpose |
| --- | --- |
| `out/build/x64-Debug/nextgen-rendering-evidence/00a-phase9d-baseline-cornell-raster.bmp` | Pre-Phase-10 Cornell Box Rasterization baseline |
| `out/build/x64-Debug/nextgen-rendering-evidence/00b-phase9d-baseline-cornell-rayquery.bmp` | Pre-Phase-10 Cornell Box Ray Query diagnostic baseline |
| `out/build/x64-Debug/nextgen-rendering-evidence/00c-phase9d-baseline-helmet-raster.bmp` | Pre-Phase-10 `DamagedHelmet.glb` Rasterization baseline |
| `out/build/x64-Debug/nextgen-rendering-evidence/00d-phase9d-baseline-helmet-rayquery.bmp` | Pre-Phase-10 `DamagedHelmet.glb` Ray Query diagnostic baseline |

Validation results (isolated environment with `VK_LAYER_VALIDATE_SYNC=1`, NVIDIA GeForce RTX 5090):
- `rt.capabilities` (CPU): **PASS**
- `rt.math` (CPU, 15 assertions): **PASS**
- `rt.foundation` (GPU/WSI): **PASS** (`exit=0`, `valHits=0`)
- `rt.as` (GPU/WSI): **PASS** (`exit=0`, `valHits=0`)
- `rt.rayquery` (GPU/WSI): **PASS** (`exit=0`, `valHits=0`)
