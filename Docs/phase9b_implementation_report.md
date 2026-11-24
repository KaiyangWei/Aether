# Aether Engine — Phase 9B Implementation & Validation Report
## BLAS / TLAS Acceleration Structure Construction

> **Scope & Honesty Notice**: This phase implements ONLY the GPU Acceleration Structure infrastructure (BLAS/TLAS resources, multi-frame isolation, scratch alignment, Synchronization2 barriers, deferred BLAS destruction, and CPU/GPU validation). It does **NOT** implement Ray Query Compute Shaders, Ray Query visual rendering, RT shadows/reflections, Path Tracing, SBT, or the Phase 9C `Q` rendering-mode shortcut. All test outcomes, VMA allocation statistics, and RTX 5090 GPU timings reported below were directly measured from the final compiled Debug build.

---

## 1. Executive Summary

Phase 9B establishes Aether Engine's production-oriented Vulkan Hardware Ray Tracing acceleration structure layer on top of Phase 9A's capability/BDA foundation:

- Added **`VulkanAccelerationStructure`** (`BOTTOM_LEVEL` / `TOP_LEVEL` handle + backing `VulkanBuffer` + `VkDeviceAddress` + rollback-safe initialization and handle-before-buffer destruction).
- Added **`VulkanRayTracingScene`** managing BLAS caching/reuse by monotonic `VulkanMesh::GetUniqueId()`, per-frame-slot (`MAX_FRAMES_IN_FLIGHT = 2`) independent TLAS/Instance/Scratch resources, device-aligned scratch addresses (`minAccelerationStructureScratchOffsetAlignment`), Synchronization2 dependency chains, fence-driven deferred BLAS destruction without routine `vkDeviceWaitIdle()`, and GPU Timestamp Queries.
- Added pure CPU math/validation helpers (`Engine/Renderer/RayTracingMath.h`) and 19 automated assertions across `rt.math` (CPU) and `rt.as` (RTX 5090 GPU covering all 21 required scenarios + fault injection + 1000-instance benchmarks).
- **Result**: Clean Debug build (0 errors, 0 source warnings under `/W4`), **11/11** pure CPU CTests passed, **20/20** isolated RTX 5090 GPU/WSI scenarios passed (`valHits = 0`), `helmet-preview` + 2 desktop stress tests passed (`valHits = 0`), VMA allocations return cleanly to baseline (`55 -> 64 -> 55`, zero leaks).

---

## 2. Actual Architecture & Module Ownership

Strictly adhering to `AGENTS.md` Sections 2–3 ("shared access does not imply shared ownership"):

| Module | Owns | Borrows / Coordinates | Explicitly Does NOT Own |
| --- | --- | --- | --- |
| `VulkanMemoryManager` | `VmaAllocator` | Context (during `Initialize`) | `VkBuffer`, `VkImage`, `VkAccelerationStructureKHR`, BLAS/TLAS registries, Scene/Mesh semantics |
| `VulkanBuffer` | `VkBuffer`, `VmaAllocation`, `m_usage` | `VulkanMemoryManager` | BLAS/TLAS business semantics |
| `VulkanMesh` | Vertex/Index `VulkanBuffer`, `vertexCount`, `indexCount`, monotonic `uniqueId` | `VulkanMemoryManager` | BLAS handles |
| `VulkanAccelerationStructure` | `VkAccelerationStructureKHR`, backing storage `VulkanBuffer`, `m_deviceAddress` | `VulkanMemoryManager`, KHR destroy pointer | Direct VMA allocations |
| `VulkanRayTracingScene` | BLAS cache (`shared_ptr<VulkanAccelerationStructure>`), deferred BLAS retirement queue, per-frame-slot TLAS / InstanceBuffer / ScratchBuffers / metadata snapshots, Timestamp `VkQueryPool` | `const VulkanMesh*` during `RecordFrame()` | `Scene`, `SceneObject`, `VulkanMesh` |
| `Scene` | `SceneObject` list & `Transform`s | None | Any Vulkan handle or AS |
| `VulkanRenderer` | `m_rtScene` alongside `m_shadowPass` / `m_taaPass` / `m_bloomPass` | `VulkanContext`, `VulkanCommandManager`, `VulkanSyncManager`, `VulkanMemoryManager`, `DrawItem`s | `Scene`, `VulkanMesh` |
| `AetherApplication` | `Scene`, `Camera`, `m_meshes`, `m_previewMaterials`, `m_environment` | Drives `DrawScene()` and asset replacement | AS build internals |

---

## 3. `VulkanAccelerationStructure` Implementation

Files: `Engine/RHI/Vulkan/Resources/VulkanAccelerationStructure.h` / `.cpp`

- Supports `VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR` and `VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR` (rejects invalid types or zero size with `VK_ERROR_INITIALIZATION_FAILED`; rejects non-ready RT capabilities with `VK_ERROR_FEATURE_NOT_PRESENT`).
- Backing storage `VulkanBuffer` is created via existing `VulkanBuffer::Initialize(...)` with:
  - Usage: `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`
  - Memory: `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT`
- Creates `VkAccelerationStructureKHR` via `capabilities.functions.vkCreateAccelerationStructureKHR`, then queries `m_deviceAddress` via `vkGetAccelerationStructureDeviceAddressKHR`.
- **Rollback safety**: Any failure (storage allocation, AS handle creation, or zero device address) immediately invokes `Shutdown()` before returning the error, leaving zero dangling handles or VMA allocations.
- **Destruction order**: `Shutdown()` destroys `VkAccelerationStructureKHR` first via `m_destroyFunc(m_device, m_handle, nullptr)` and **then** calls `m_storageBuffer.Shutdown()`. Safe to call repeatedly.

---

## 4. BLAS Geometry Mapping

Verified directly against `Engine/Renderer/Vertex.h` via compile-time `static_assert`:
- `offsetof(Vertex, position) == 0`
- `sizeof(Vertex) == 104`
- Indices: `uint32_t` (`VK_INDEX_TYPE_UINT32`), `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST`.

Per unique valid `VulkanMesh`:
- Validates `meshId != 0`, `mesh.IsRayTracingUsageEnabled()`, `vertexCount > 0`, `indexCount >= 3`, `indexCount % 3 == 0`, `primitiveCount = indexCount / 3 <= caps.maxPrimitiveCount`, non-zero vertex & index `VkDeviceAddress`.
- Configures `VkAccelerationStructureGeometryTrianglesDataKHR`:
  - `vertexFormat = VK_FORMAT_R32G32B32_SFLOAT`
  - `vertexData.deviceAddress = mesh.GetVertexBuffer().GetDeviceAddress()`
  - `vertexStride = sizeof(Vertex)` (`104`)
  - `maxVertex = vertexCount - 1`
  - `indexType = VK_INDEX_TYPE_UINT32`
  - `indexData.deviceAddress = mesh.GetIndexBuffer().GetDeviceAddress()`
  - `transformData.deviceAddress = 0`
- Queries exact build sizes via `vkGetAccelerationStructureBuildSizesKHR` (zero hardcoded sizes).
- Build flags: `VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR` (no compaction in Phase 9B).
- **Alpha geometry preservation**: BLAS geometry sets `flags = 0` (not `VK_GEOMETRY_OPAQUE_BIT_KHR`), delegating opacity to TLAS instance flags (`FORCE_OPAQUE` vs `FORCE_NO_OPAQUE`). Thus a single BLAS can be shared simultaneously by Opaque and Alpha-MASK instances without duplicating BLAS storage, while preserving Alpha-MASK candidate hit semantics for Phase 9C Ray Query. Alpha-BLEND objects are excluded from the opaque/mask TLAS (consistent with `ShadowPass` and `VulkanTaaPass`).

---

## 5. BLAS Reuse & Reference Tracking

- **Reliable Mesh Identity**: `VulkanMesh` assigns a monotonically increasing non-zero `uint64_t m_uniqueId` (via `std::atomic<uint64_t>`) on each successful `Initialize()` and resets it to `0` on `Shutdown()`. Raw `const VulkanMesh*` pointers are never used as cache keys, preventing false hits from heap address reuse or in-place re-initialization.
- `VulkanRayTracingScene::m_blasCache` maps `meshUniqueId -> BlasEntry { shared_ptr<VulkanAccelerationStructure> blas, vertexCount, indexCount, builtInSubmitSerial, lastUsedSubmitSerial }`.
- Multiple `SceneObject`s referencing the same `VulkanMesh` deduplicate via `activeMeshIds` and reuse the single cached BLAS (verified with 3 instances and 1000 instances both yielding `GetActiveBlasCount() == 1`).

---

## 6. TLAS Instance Generation & Future Ray Query Mapping Contract

- Each valid non-`AlphaMode::Blend` scene item becomes a `VkAccelerationStructureInstanceKHR` via `BuildVkAccelerationStructureInstance()` (`Engine/Renderer/RayTracingMath.h`):
  - `transform`: 3×4 row-major affine matrix from `ToVkTransformMatrix(item.model)`.
  - `instanceCustomIndex`: dense 24-bit index (`0 .. N-1`, bounds-checked against `0x00FFFFFFu`).
  - `mask`: `0xFF`.
  - `instanceShaderBindingTableRecordOffset`: `0` (bounds-checked against `0x00FFFFFFu`).
  - `flags`: `ComputeRtInstanceFlags(doubleSided, mirrored, alphaMask)`:
    - `doubleSided` -> `VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR`
    - `mirrored` (`det(mat3(model)) < 0`) -> `VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR`
    - `alphaMask` -> `VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR`; Opaque -> `VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR`.
  - `accelerationStructureReference`: BLAS `GetDeviceAddress()`.
- **Phase 9C Ray Query Mapping Contract**: `FrameResources[f].instanceMetadata[customIndex]` stores `RtInstanceMetadata { sceneObjectIndex, meshUniqueId, doubleSided, mirrored, alphaMask }`, where `sceneObjectIndex` is captured from `Scene::GetObjects()` in `AetherApplication::DrawScene()` **before** PBR alpha-blend sorting. In Phase 9C, `RayQuery::CommittedInstanceID()` maps directly to `instanceMetadata[id]`.
- **Empty scene handling**: When `instances.empty()`, no zero-primitive build is submitted; `frame.tlas.Shutdown()` releases any stale handle, `HasValidTlas(f)` becomes `false`, and unused BLASes enter deferred retirement.

---

## 7. GLM Transform Conversion Derivation

- GLM stores `glm::mat4` in **column-major** order (`model[col][row]`), where column 3 (`model[3][0..2]`) is translation and a transformed point is:
  $$p'_r = \sum_{c=0}^{2} \text{model}[c][r] \cdot p_c + \text{model}[3][r], \quad r \in \{0, 1, 2\}$$
- Vulkan `VkTransformMatrixKHR::matrix[3][4]` is **row-major**, storing the top 3 rows of the affine matrix with translation in `matrix[0..2][3]`:
  $$\text{matrix}[r][c] = \text{model}[c][r], \quad r \in \{0, 1, 2\},\; c \in \{0, 1, 2, 3\}$$
- Verified in `Tests/RayTracingMathTests.cpp` (`rt.math`) by comparing `TransformPoint(ToVkTransformMatrix(M), p)` against `glm::vec3(M * glm::vec4(p, 1.0f))` across identity, pure translation, arbitrary quaternion rotation, non-uniform & negative mirrored scaling, and combined `Transform::GetMatrix()` TRS matrices.

---

## 8. Per-Frame Resource Layout (`MAX_FRAMES_IN_FLIGHT = 2`)

Each slot `m_frames[f]` (`std::unique_ptr<FrameResources>`, `f ∈ {0, 1}`) independently owns:
- `VulkanAccelerationStructure tlas`
- `VulkanBuffer instanceBuffer` (`HOST_VISIBLE | HOST_COHERENT`, grows geometrically on demand)
- `VulkanBuffer tlasScratchBuffer` (`DEVICE_LOCAL`, padded for alignment, grows geometrically)
- `VulkanBuffer blasScratchBuffer` (`DEVICE_LOCAL`, padded for alignment, grows geometrically)
- `instanceCapacity`, `instanceCount`, `hasValidTlas`, `builtWithAllowUpdate`, `tlasBuildSize`, `tlasUpdateScratchSize`
- `referencedBlas` (`std::vector<std::shared_ptr<VulkanAccelerationStructure>>` snapshot)
- `previousTopology`, `previousTransforms`, `instanceMetadata`
- `submitSerial`, timestamp pending/recorded flags, and last aligned scratch addresses

No writable Instance Buffer, TLAS destination, or Scratch Buffer is shared across frame slots. `VulkanRenderer::DrawFrame()` waits on `inFlightFence[m_currentFrame]` before calling `m_rtScene.OnFrameFenceCompleted(m_currentFrame)` and `m_rtScene.RecordFrame(...)`, then calls `m_rtScene.OnFrameSubmitted(m_currentFrame)` after `vkQueueSubmit2`.

---

## 9. Scratch Alignment Implementation

- Queries actual `VkPhysicalDeviceAccelerationStructurePropertiesKHR::minAccelerationStructureScratchOffsetAlignment` from `VulkanRayTracingCapabilities` (`128` bytes on RTX 5090; never hardcoded).
- Allocates `ComputePaddedScratchSize(requiredSize, align) = requiredSize + (align - 1)`.
- Retrieves actual GPU `rawAddr = scratchBuffer.GetDeviceAddress()` and aligns it via `alignedAddr = AlignUpDeviceAddress(rawAddr, align)`.
- Explicitly verifies `IsAddressAligned(alignedAddr, align)` and `(alignedAddr - rawAddr) + requiredSize <= scratchBuffer.GetSize()` before passing `alignedAddr` to `vkCmdBuildAccelerationStructuresKHR`.
- Sequential multi-BLAS builds in the same frame share `frame.blasScratchBuffer` sized to `max(buildScratchSize)` with explicit Synchronization2 scratch-reuse barriers between consecutive builds.

---

## 10. BLAS / TLAS Synchronization2 Dependencies

All barriers use `vkCmdPipelineBarrier2` (`VkMemoryBarrier2`):

| Transition | `srcStageMask` / `srcAccessMask` | `dstStageMask` / `dstAccessMask` | Purpose |
| --- | --- | --- | --- |
| Host vertex/index upload -> BLAS build | `HOST_BIT` / `HOST_WRITE_BIT` | `ACCELERATION_STRUCTURE_BUILD_BIT_KHR` / `SHADER_READ_BIT` | Ensures host-written mesh geometry is visible to BLAS build |
| BLAS `i` -> BLAS `i+1` scratch reuse | `ACCELERATION_STRUCTURE_BUILD_BIT_KHR` / `AS_WRITE \| AS_READ` | `ACCELERATION_STRUCTURE_BUILD_BIT_KHR` / `AS_WRITE \| AS_READ` | Serializes sequential reuse of `frame.blasScratchBuffer` |
| Host instance upload -> TLAS build/update | `HOST_BIT` / `HOST_WRITE_BIT` | `ACCELERATION_STRUCTURE_BUILD_BIT_KHR` / `SHADER_READ_BIT` | Ensures CPU-written `instanceBuffer[f]` is visible to TLAS build |
| BLAS build -> TLAS build (same-frame or in-flight prior submission) | `ACCELERATION_STRUCTURE_BUILD_BIT_KHR` / `ACCELERATION_STRUCTURE_WRITE_BIT_KHR` | `ACCELERATION_STRUCTURE_BUILD_BIT_KHR` / `ACCELERATION_STRUCTURE_READ_BIT_KHR` | Ensures BLAS writes are visible to TLAS reads across submissions on the graphics queue |
| **Documented for Phase 9C**: TLAS build -> Ray Query Compute | `ACCELERATION_STRUCTURE_BUILD_BIT_KHR` / `ACCELERATION_STRUCTURE_WRITE_BIT_KHR` | `COMPUTE_SHADER_BIT` / `ACCELERATION_STRUCTURE_READ_BIT_KHR` | Will be emitted in Phase 9C before `vkCmdDispatch` |

Frame-slot reuse is protected by the existing `inFlightFence[m_currentFrame]`; no routine `vkDeviceWaitIdle()` is used during rendering.

---

## 11. Deferred Destruction Strategy

State transitions for a cached BLAS:
1. **Active**: Stored in `m_blasCache[meshUniqueId]` (`shared_ptr`) and also referenced by every frame slot `f` whose `tlas[f]` includes it (`frame[f].referencedBlas`). `lastUsedSubmitSerial` updates on each frame submission using it.
2. **Evicted -> Deferred Retirement**: When `meshUniqueId` is no longer present in the current scene, `m_blasCache` moves its `shared_ptr` into `m_deferredRetirements` tagged with `retireAfterSerial = lastUsedSubmitSerial`. Any other in-flight frame slot still holds its own `shared_ptr` in `referencedBlas`, keeping the BLAS alive on the GPU.
3. **Collected & Destroyed**: On subsequent `OnFrameFenceCompleted(f)` (which advances `m_completedSerial`) and after `frame[f].referencedBlas` is replaced during `RecordFrame`, `CollectDeferredRetirements()` destroys a retired entry **only when** both:
   - `entry.retireAfterSerial <= m_completedSerial` (all GPU submissions that could access it have completed via Fence), AND
   - `entry.blas.use_count() == 1` (no current scene cache entry and no frame-slot TLAS snapshot references it).

Verified in `rt.as` Step 7: removing an object moves its BLAS to `DeferredBlasCount >= 1` during the first frame, and advancing 2 frames (across both slot fences, zero `vkDeviceWaitIdle()`) drops `DeferredBlasCount` cleanly to `0`.

---

## 12. TLAS Update / Rebuild / Skip Policy

Per frame slot `f`:
- **Skip (`TlasBuildAction::Skipped`)**: `hasValidTlas && topologyMatches && transformsMatch && !anyReferencedBlasInFlightBuild` -> zero GPU work.
- **In-Place Update (`TlasBuildAction::Update`)**: `VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR` (`src == dst == frame.tlas`) is used **only when** `hasValidTlas`, `builtWithAllowUpdate`, `instanceCount` and per-instance topology `(meshUniqueId, blasDeviceAddress, customIndex, mask, flags)` are 100% identical, `tlasUpdateScratchSize > 0`, and only `transform` changed.
- **InitialBuild / Rebuild (`VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR`)**: Used for first build, instance count change, object add/remove, mesh replacement, or instance flag/mask changes; always built with `PREFER_FAST_TRACE_BIT_KHR | ALLOW_UPDATE_BIT_KHR`.
- **Empty Scene (`TlasBuildAction::None`)**: Shuts down slot TLAS and marks `HasValidTlas(f) == false`.

---

## 13. Fault Injection Results

All fault injection paths verified in `rt.as` on RTX 5090 with **0 VMA leaks** and **0 Validation errors**:
1. Direct `VulkanAccelerationStructure` storage allocation failure (`VK_ERROR_OUT_OF_DEVICE_MEMORY`) -> clean failure, 0 leaks.
2. Direct `VulkanAccelerationStructure` handle creation failure (`VK_ERROR_UNKNOWN`) -> backing `VulkanBuffer` rolled back cleanly, 0 leaks.
3. Invalid arguments (size `0`, unsupported `GENERIC` AS type) -> `VK_ERROR_INITIALIZATION_FAILED`, 0 leaks.
4. `VulkanRayTracingScene::RecordFrame` injected failures: `failBlasStorageAllocation`, `failBlasAsCreate`, `failScratchAllocation`, `failTlasStorageAllocation`, `failTlasAsCreate` -> all returned `false` cleanly with VMA allocation count unchanged (`== faultBaseline`).

---

## 14. CPU & GPU Test Results

Build directory: `E:\Aether\out\build\x64-Debug` (CMake + Ninja + MSVC C++20 `/W4` + Vulkan SDK `1.4.357.0`).
Build result: **0 errors, 0 project source warnings**.

| Test Suite | Phase 9A Baseline | Phase 9B Final | Status |
| --- | --- | --- | --- |
| **Pure CPU CTests** (`ctest --test-dir out/build/x64-Debug`) | 10 / 10 | **11 / 11** (`1.66 s`) | Added `rt.math` (8/8 assertions) |
| **Isolated GPU/WSI Suite** (`validation-launch.exe isolated`, `VK_LAYER_VALIDATE_SYNC=1`) | 19 / 19 (`valHits=0`) | **20 / 20** (`valHits=0`) | Added `rt.as` (all 21 scenarios passed) |
| **`helmet-preview`** (`validation-launch.exe isolated wait`) | Passed (`valHits=0`) | **Passed** (`exit=0, valHits=0`) | Unchanged / clean |
| **Desktop Stress** (`--single-cube`, `--material-test DamagedHelmet.glb`) | 2 / 2 (`valHits=0`) | **2 / 2** (`exit=0, valHits=0`) | Rasterization intact |

---

## 15. Validation Layer Results

- Executed with **Khronos Validation Layer** + **Synchronization Validation** (`VK_LAYER_VALIDATE_SYNC=1`) via the project's RTSS-isolated launcher (`out/build/x64-Debug/validation-launch.exe`).
- Across all **20** isolated GPU/WSI tests + `helmet-preview` + **2** desktop stress tests:
  - `VUID-` errors: **0**
  - `SYNC-HAZARD` reports: **0**
  - `Validation Error` reports: **0**

---

## 16. VMA Allocation Statistics (`rt.as` on RTX 5090)

- Pre-RT baseline (`Initialize(true)`): **`55`** VMA allocations.
- Active RT scene (1000 instances sharing 1 BLAS, 2 in-flight frame slots, post-resize): **`64`** VMA allocations (`+9`: 1 BLAS storage + 2 TLAS storage + 2 instance buffers + 2 TLAS scratch buffers + 2 BLAS scratch buffers).
- Post-`rtScene.Shutdown()`: **`55`** VMA allocations (**exact return to baseline, 0 leaked allocations**).
- All 5 fault-injection tests and 3 repeated init/shutdown cycles returned to exact pre-test VMA allocation count.

---

## 17. RTX 5090 GPU Timing (GPU-Only Execution via `VkQueryPool` Timestamps)

Measured on **NVIDIA GeForce RTX 5090** (`minAccelerationStructureScratchOffsetAlignment = 128 bytes`). Timings measure **GPU-only execution** (`vkCmdWriteTimestamp2` delta at `VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR`, excluding CPU recording and queue submission overhead):

| Operation | Scene / Geometry | Measured GPU Time (ms) |
| --- | --- | --- |
| Single-Mesh BLAS Build | Cube (24 verts, 12 tris) | **0.0620 ms** |
| Multi-Mesh Sequential BLAS Build | 2 unique meshes (Tetrahedron + 2nd Cube, shared scratch) | **0.1070 ms** |
| High-Poly glTF BLAS Build | `DamagedHelmet.glb` | **0.7790 ms** |
| TLAS Initial Build | 1 instance | **0.0252 ms** |
| TLAS Rebuild | 3 instances (1 shared BLAS) | **0.0345 ms** |
| TLAS In-Place Update | 3 instances (transform-only) | **0.0125 ms** |
| TLAS Rebuild | 1,000 instances (1 shared BLAS) | **0.1002 ms** |
| TLAS In-Place Update | 1,000 instances (transform-only) | **0.0194 ms** |

*(TLAS in-place update is ~2.8× faster at 3 instances and ~5.2× faster at 1,000 instances than a full rebuild on RTX 5090.)*

---

## 18. Files Added & Modified

### Added
- `Engine/Renderer/RayTracingMath.h` — GLM→`VkTransformMatrixKHR` conversion, point transform helper, scratch alignment & padding, 24-bit instance validation, instance flag mapping.
- `Engine/RHI/Vulkan/Resources/VulkanAccelerationStructure.h` / `.cpp` — BLAS/TLAS handle + storage `VulkanBuffer` ownership.
- `Engine/RHI/Vulkan/RayTracing/VulkanRayTracingScene.h` / `.cpp` — BLAS cache/reuse, per-frame TLAS/Instance/Scratch isolation, Synchronization2 barriers, deferred BLAS destruction, GPU timestamp queries.
- `Tests/RayTracingMathTests.cpp` — Pure CPU unit test suite (`rt.math`).
- `Tests/RayTracingAsTests.cpp` — RTX 5090 GPU acceleration structure test & benchmark suite (`rt.as`).
- `Docs/phase9b_implementation_report.md` — This report.

### Modified
- `Engine/RHI/Vulkan/Resources/VulkanMesh.h` / `.cpp` — Added monotonic `m_uniqueId` / `GetUniqueId()` for stable BLAS caching identity.
- `Engine/RHI/Vulkan/VulkanRenderer.h` / `.cpp` — Integrated `VulkanRayTracingScene` lifecycle, `DrawItem::sceneObjectIndex`, optional `updateRayTracingScene` parameter in `DrawFrame()`, `OnFrameFenceCompleted`/`OnFrameSubmitted`/`OnDeviceIdle`/`Shutdown`.
- `Engine/Core/AetherApplication.h` / `.cpp` — Populated `sceneObjectIndex` prior to alpha-blend sort, added `m_updateRayTracingScene` (default `false` in Rasterization mode), notified `m_rtScene.OnDeviceIdle()` on asset replacement, declared `RunRayTracingAsTest()`.
- `Tests/SwapchainRecovery.cpp` — Dispatched `--swapchain-test rt-as`.
- `CMakeLists.txt` — Registered `RayTracingAsTests.cpp`, `AetherRayTracingMathTests` (`rt.math`), and `rt.as` CTest (with `gltf-assets` fixture).
- `ARCHITECTURE.md`, `README.md` — Documented Phase 9B architecture, ownership boundaries, test coverage, and preserved Phase 9C `Q` rendering-mode contract.

---

## 19. Remaining Limitations

1. **Implemented & Tested (Phase 9B)**: BLAS/TLAS creation/destruction, BLAS reuse by monotonic mesh ID, per-frame TLAS isolation, scratch alignment, Synchronization2 barriers, fence-driven deferred BLAS destruction, TLAS Skip/Update/Rebuild, empty-scene handling, fault injection, GPU timestamps.
2. **Planned for Phase 9C (NOT implemented in Phase 9B)**: Ray Query Compute Shader (`cs_6_5`), `VulkanRayQueryPass`, descriptor binding of `tlas[f]` + output storage image, GPU numerical hit/distance/primitive readback verification, and the single-shortcut **`Q`** (`Rasterization <-> Ray Query`) rendering-mode switch (sharing the exact same `Scene`, `Camera`, transforms, meshes, materials, and environment, with Rasterization default and TAA history invalidation on switch).
3. **Alpha-BLEND & Compaction**: Alpha-BLEND objects are excluded from the opaque/mask TLAS; BLAS compaction is intentionally deferred until needed.
4. **Physical Non-RT GPU**: Only verified on NVIDIA GeForce RTX 5090 (plus software-disabled RT fallback path in `rt.foundation`); not tested on a physical GPU lacking RT extensions.

---

## 20. Phase 9C Readiness Assessment

**Explicit Confirmation: Phase 9B is COMPLETE and Phase 9C is READY TO BEGIN.**

All Phase 9B Definition of Done criteria are verified on RTX 5090 with zero validation errors, zero synchronization hazards, zero VMA leaks, and full rasterization regression pass. `VulkanRayTracingScene` already exposes `HasValidTlas(frameIndex)`, `GetTlasHandle(frameIndex)`, and `GetInstanceMetadata(frameIndex)` per frame slot, so Phase 9C (`VulkanRayQueryPass` + `cs_6_5` Ray Query shader + `Q` mode switch) can bind the per-frame TLAS directly without re-architecting BLAS/TLAS ownership or synchronization.
