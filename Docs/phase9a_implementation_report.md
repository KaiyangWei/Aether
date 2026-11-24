# Aether Engine — Phase 9A 实现与验证报告
## Ray Tracing Capability & Buffer Foundation

> **范围声明**：本阶段严格限定为 Phase 9A（可选光追能力探测/启用与底层 Buffer/Mesh/VMA 基础），**未实现** BLAS、TLAS、`VulkanAccelerationStructure`、`VulkanRayTracingScene`、Ray Query Compute Pass、Ray Query Shader 或 `Q` 渲染模式切换逻辑。所有测试与验证数据均来自实际构建与 RTX 5090 实机执行输出。

---

## 1. 实际新增与修改的文件清单

### 新增文件
| 文件路径 | 职责说明 |
| --- | --- |
| `Engine/RHI/Vulkan/VulkanRayTracingCapabilities.h` | 定义只读能力结构 `VulkanRayTracingCapabilities`、5 个 KHR 加速结构函数指针表 `VulkanRayTracingFunctions`，以及纯 CPU 原子状态评估函数 `EvaluateRayTracingHardwareSupport` / `FinalizeRayTracingDeviceCapabilities`。 |
| `Tests/RayTracingCapabilitiesTests.cpp` | 纯 CPU 单元测试（CTest：`rt.capabilities`），验证默认未就绪状态、任意单扩展/单特性/无 Compute 队列缺口淘汰、禁用降级路径、部分函数指针加载时的原子清零回退，以及全量就绪判定（6/6 断言）。 |
| `Docs/phase9a_implementation_report.md` | 本阶段实现与验证报告。 |

### 修改文件
| 文件路径 | 修改内容摘要 |
| --- | --- |
| `Engine/RHI/Vulkan/VulkanContext.h` / `VulkanContext.cpp` | 保持 `PhysicalDeviceSelector` 不强制 RT；选出物理设备后执行 `ProbeAndConfigureOptionalRayTracing()`，原子探测并启用 3 个非核心 KHR 扩展与 3 项特性；逻辑设备创建成功后加载 5 个 KHR 函数指针，暴露只读 `GetRayTracingCapabilities()`，并在 `Shutdown()` 中完整清零；提供私有测试钩子 `m_disableRayTracingForTest`（仅 `friend class AetherApplication` 可见）。 |
| `Engine/RHI/Vulkan/VulkanMemoryManager.h` / `VulkanMemoryManager.cpp` | 仅修改 VMA 初始化逻辑：当且仅当 `context.GetRayTracingCapabilities().IsBufferDeviceAddressEnabled()` 为 `true` 时附加 `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT`；记录只读状态位 `IsBufferDeviceAddressEnabled()` 并在 `Shutdown()` 复位；严格保持仅管理 `VmaAllocator` 的所有权边界。 |
| `Engine/RHI/Vulkan/Resources/VulkanBuffer.h` / `VulkanBuffer.cpp` | 记录实际创建标志 `m_usage`（`GetUsage()` / `HasDeviceAddressUsage()`）；新增 `GetDeviceAddress()`（经 `vkGetBufferDeviceAddress`），对未初始化、BDA 未启用或缺少 `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` 提供明确错误日志并返回 `0`；在 `Initialize()` 中拦截 BDA 未启用却请求 `SHADER_DEVICE_ADDRESS_BIT` 的非法调用并返回 `VK_ERROR_FEATURE_NOT_PRESENT`。 |
| `Engine/RHI/Vulkan/Resources/VulkanMesh.h` / `VulkanMesh.cpp` | 新增 `m_vertexCount` 与 `GetVertexCount()`；新增 `VulkanMeshCreateOptions{ enableRayTracingUsage }` 重载，仅显式启用时向顶点/索引缓冲追加 `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT \| VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR`；**修复原 `VulkanMesh.cpp:24` 故障注入严格相等 bug**（`usage == VK_BUFFER_USAGE_INDEX_BUFFER_BIT` → `(usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) != 0`）；未改动 `Vertex.h`。 |
| `Engine/RHI/Vulkan/Assets/VulkanAssetUploader.h` / `VulkanAssetUploader.cpp` | 为 `Upload(...)` 新增默认值为 `false` 的 `enableRayTracingUsage` 参数并透传至 `VulkanMesh::Initialize(...)`，完全兼容现有调用方且不无条件开启 RT 用途。 |
| `Engine/RHI/Vulkan/VulkanRenderer.h` / `VulkanRenderer.cpp` | 暴露只读 `GetRayTracingCapabilities()` 与 `IsRayTracingReady()`，初始化时打印 `Ray Tracing Ready: Yes/No`，不修改现有光栅化 / Shadow / TAA / Bloom / IBL / ToneMap 行为。 |
| `Engine/Core/AetherApplication.h` / `AetherApplication.cpp` | 在 `CreateScene()` 与 `AddImportedAsset()` 中根据 `m_vulkanContext.GetRayTracingCapabilities().IsReady()` 受控传递 `enableRayTracingUsage`；声明 `RunRayTracingFoundationTest()`；保持光栅化为默认路径，不复制 Scene/Camera，不提前实现空壳 `Q` 快捷键。 |
| `Tests/SwapchainRecovery.cpp` | 注册 `--swapchain-test rt-foundation` 场景实现 `RunRayTracingFoundationTest()`，在真实 Vulkan 上下文中验证 RTX 5090 默认启用路径与软件禁用 RT 降级路径。 |
| `CMakeLists.txt` | 注册 `AetherRayTracingCapabilitiesTests`（`rt.capabilities`）、`rt.foundation` CTest，并将新测试目标加入 MSVC+Ninja `OBJECT_DEPENDS` 头文件追踪列表。 |
| `ARCHITECTURE.md` | 更新第 1、2、7 节并新增第 8 节，记录 Phase 9A 能力与资源基础，以及 **Phase 9C 单快捷键 `Q`（`Rasterization <-> Ray Tracing`）强制架构契约**。 |

---

## 2. 审计假设核对结果

实施前逐文件对照了 `Docs/phase9_architecture_audit.md` 与当前仓库源码：
- 确认 `VulkanContext.cpp`、`VulkanMemoryManager.cpp`、`VulkanBuffer.cpp`、`VulkanMesh.cpp`、`VulkanAssetUploader.cpp`、`AetherApplication.cpp`、`CMakeLists.txt` 与审计报告一致。
- 确认 `VulkanMesh.cpp:24` 确实存在 `usage == VK_BUFFER_USAGE_INDEX_BUFFER_BIT` 精确相等判断缺陷，已按位与修复。
- 核实项目内 vendored `third_party/vk-bootstrap/src/VkBootstrap.cpp`（`is_extension_present`、`enable_extensions_if_present`、`enable_extension_features_if_present`）及 `VkBootstrapFeatureChain.inl` 已原生支持 `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES`、`VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR` 与 `VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR`。
- 核实 `vkb::Device::get_queue_index(QueueType::graphics)` 固定选取首个包含 `VK_QUEUE_GRAPHICS_BIT` 的队列族，因此在 `SelectPhysicalDevice()` 阶段按同一规则校验该队列族具备 `VK_QUEUE_COMPUTE_BIT`，并在 `CreateLogicalDevice()` 获取实际 `m_graphicsQueueFamily` 后再次复核。

---

## 3. RT 能力探测与原子启用设计

### 3.1 三层只读状态区分（`VulkanRayTracingCapabilities`）
1. **硬件/物理设备支持（`IsHardwareSupported()`）**：
   - 必需非核心设备扩展（遵循 Vulkan 1.2+ Core 提升，**不重复请求** `VK_KHR_buffer_device_address`）：
     - `VK_KHR_deferred_host_operations`
     - `VK_KHR_acceleration_structure`
     - `VK_KHR_ray_query`
   - 必需特性（通过单次 `vkGetPhysicalDeviceFeatures2` 链表查询）：
     - `VkPhysicalDeviceVulkan12Features::bufferDeviceAddress`
     - `VkPhysicalDeviceAccelerationStructureFeaturesKHR::accelerationStructure`
     - `VkPhysicalDeviceRayQueryFeaturesKHR::rayQuery`
   - 必需队列能力：图形队列族支持 `VK_QUEUE_COMPUTE_BIT`。
   - 同步通过 `vkGetPhysicalDeviceProperties2` 查询 `VkPhysicalDeviceAccelerationStructurePropertiesKHR`（`minScratchOffsetAlignment`、`maxGeometryCount`、`maxInstanceCount`、`maxPrimitiveCount`、`maxPerStageDescriptorAccelerationStructures`）。
2. **逻辑设备原子启用（`IsDeviceEnabled()` / `IsBufferDeviceAddressEnabled()`）**：
   - `PhysicalDeviceSelector` 维持仅要求 Vulkan 1.3 + Swapchain + `synchronization2` + `dynamicRendering`。
   - 仅当 `hardwareSupported` 为 `true`（且未触发测试禁用标志）时，在 `m_physicalDevice` 的**局部副本 `candidate`** 上依次启用 3 项扩展与 3 项特性结构体；只有全部 4 步均成功才将 `candidate` 提交回 `m_physicalDevice`，确保绝不出现部分扩展/特性残留。
3. **运行时函数表可用性（`AreFunctionsAvailable()`）**：
   - 仅在逻辑设备成功创建且扩展集已启用的情况下，通过 `vkGetDeviceProcAddr(m_device.device, ...)` 加载全部 5 个必需 KHR 函数指针：
     - `vkCreateAccelerationStructureKHR`
     - `vkDestroyAccelerationStructureKHR`
     - `vkGetAccelerationStructureBuildSizesKHR`
     - `vkCmdBuildAccelerationStructuresKHR`
     - `vkGetAccelerationStructureDeviceAddressKHR`
   - 若任一指针为 `nullptr`，`FinalizeRayTracingDeviceCapabilities()` 原子清空全部 5 个指针并将 `functionsLoaded` 置为 `false`，使总门控 `IsReady()` 返回 `false`。

---

## 4. Buffer Device Address（BDA）实现（`VulkanBuffer`）

- 新增 `m_usage`（`VkBufferUsageFlags`），在 `Initialize()` 成功时记录、在 `Shutdown()` 时清零，提供 `GetUsage()` 与 `HasDeviceAddressUsage()`。
- **不自动**为普通缓冲区附加 `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`。
- 当调用方请求 `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` 但 `VulkanMemoryManager::IsBufferDeviceAddressEnabled()` 为 `false` 时，`VulkanBuffer::Initialize()` 提前返回 `VK_ERROR_FEATURE_NOT_PRESENT` 并打印清晰错误日志，避免无意义分配或触发 Vulkan Validation 错误。
- `VulkanBuffer::GetDeviceAddress()` 依次校验：
  1. 缓冲区是否已初始化（`m_device` / `m_buffer` / `m_memory` 有效）；
  2. `m_memory->IsBufferDeviceAddressEnabled()` 是否为 `true`；
  3. `(m_usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0`。
  任一条件不满足时输出清晰诊断日志并返回 `0`；满足时调用 `vkGetBufferDeviceAddress` 返回有效设备地址。

---

## 5. Mesh 用途标志改动与 L24 Bug 修复（`VulkanMesh` & `VulkanAssetUploader`）

- `VulkanMesh` 新增 `m_vertexCount` 与 `GetVertexCount()`，同时在 `Initialize()` 守卫与 `Shutdown()` 中同步维护；`Vertex.h` 布局与 `uint32_t` 索引表示保持完全不变。
- 新增 `VulkanMeshCreateOptions{ bool enableRayTracingUsage = false; }` 公共重载（避免与友元故障注入重载 `Initialize(..., bool failIndexUpload, bool enableRayTracingUsage = false)` 产生二义性）。
- 仅当 `enableRayTracingUsage == true` 时，为顶点缓冲与索引缓冲追加：
  - `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`
  - `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR`
- 当 `enableRayTracingUsage == false`（默认路径或 RT 不可用时），顶点/索引缓冲仍严格保持 `VK_BUFFER_USAGE_VERTEX_BUFFER_BIT` / `VK_BUFFER_USAGE_INDEX_BUFFER_BIT`。
- **关键历史 Bug 修复**：`VulkanMesh.cpp:24` 由 `if (failIndexUpload && usage == VK_BUFFER_USAGE_INDEX_BUFFER_BIT)` 改为 `if (failIndexUpload && (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) != 0)`，确保追加 RT 用途标志后 `swapchain.mesh-upload-failure` 及带 RT 标志的索引上传回滚均能正确触发并零泄漏释放。
- `VulkanAssetUploader::Upload()` 新增默认参数 `bool enableRayTracingUsage = false` 并透传给 `VulkanMesh::Initialize()`，现有调用方行为保持不变。

---

## 6. VMA 分配器改动（`VulkanMemoryManager`）

- 仅修改 `VulkanMemoryManager::Initialize(const VulkanContext& context)`：
  - 当 `context.GetRayTracingCapabilities().IsBufferDeviceAddressEnabled()` 为 `true` 时，为 `allocatorCreateInfo.flags` 附加 `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT`；
  - 否则不设置该标志。
- 严格遵守 `AGENTS.md` 第 3 节所有权约束：未添加任何加速结构管理、未创建/销毁/持有 `VkImage` 或 `VkBuffer`、未暴露 `VmaAllocator` 绕过受控接口。

---

## 7. 兼容性与降级行为

- **物理非 RT GPU 兼容性设计**：
  - `PhysicalDeviceSelector` 不要求任何 RT 扩展或特性；
  - 扩展探测、特性探测、函数加载、VMA BDA 标志及 Mesh RT 用途标志全部受控按能力门控；
  - 渲染默认始终走既有光栅化管线（Shadow + HDR PBR/Skybox + TAA + Bloom + ToneMap）。
- **验证边界诚实说明**：
  - 本机仅配备 **NVIDIA GeForce RTX 5090**（具备完整硬件光追能力），**未在无硬件光追的物理 GPU 上执行实机测试**，不伪造非 RT 物理 GPU 测试结果。
  - 已通过 `rt-foundation` 测试中的 Part B（设置 `m_disableRayTracingForTest = true`）在真实 Vulkan 运行时完整验证了**软件禁用 RT 降级代码路径**：`IsDeviceEnabled() == false`、`AreFunctionsAvailable() == false`、5 个 KHR 函数指针全部为 `nullptr`、VMA BDA `Disabled`、Mesh 无 RT 标志、非法 BDA Buffer 创建干净返回 `VK_ERROR_FEATURE_NOT_PRESENT` 且零 VMA 泄漏、光栅化连续 4 帧正常渲染且 0 Validation / Sync 错误。

---

## 8. 构建结果

- **构建目录**：`E:\Aether\out\build\x64-Debug`（沿用现有 CMake + Ninja + MSVC C++20 `/W4` + Vulkan SDK `1.4.357.0` 配置）
- **构建命令**：`cmd /c "...\scratch\build.cmd"`（`vcvars64.bat` + `chcp 65001` + `cmake --build out/build/x64-Debug`）
- **结果**：全部目标（含 `Aether.exe` 与 `AetherRayTracingCapabilitiesTests.exe`）**编译与链接成功，0 错误**；`Engine/`、`App/`、`Tests/` 下**0 新增/现存编译警告**（仅第三方 `third_party/vma/include/vk_mem_alloc.h` 既有上游 `/W4` 未使用参数警告）。

---

## 9. CTest 改动前后对比

> 注：与 `Docs/VulkanValidation.md` 记录一致，本机全局注册的第三方桌面层 `RTSSVkLayer64.dll` 会在裸跑 GPU 测试时向交换链注入 `VK_IMAGE_USAGE_STORAGE_BIT`，因此所有 GPU/WSI 场景均通过项目既有隔离验证启动器 `out/build/x64-Debug/validation-launch.exe <log> isolated wait ...`（开启 `VK_LAYER_VALIDATE_SYNC=1` + 禁用 RTSS 注入）在改动前后以完全相同条件实测对比。

| 测试类别 | Phase 9A 基线（改动前） | Phase 9A 完成（改动后） | 变化说明 |
| --- | --- | --- | --- |
| **纯 CPU CTest** (`ctest --test-dir out/build/x64-Debug`) | **9 / 9 Passed** | **10 / 10 Passed** (`0.19s`) | 新增 `rt.capabilities`（6 组原子状态断言通过） |
| **隔离 GPU/WSI 回归** (`validation-launch.exe isolated`, `VK_LAYER_VALIDATE_SYNC=1`) | **18 / 18 Passed** (`valHits=0`) | **19 / 19 Passed** (`valHits=0`) | 新增 `rt.foundation`（`rt-foundation` 场景通过，0 Validation / Sync 错误） |
| **`helmet-preview` 场景** (`validation-launch.exe isolated wait`) | 基线已通过 | **Passed** (`exit=0, valHits=0`) | 正常 |
| **桌面应用压测** (`--single-cube`, `--material-test DamagedHelmet.glb`, `isolated stress`) | 基线已通过 | **2 / 2 Passed** (`exit=0, valHits=0`) | 普通光栅化启动与 PBR 渲染完全正常 |

---

## 10. RTX 5090 运行时实测结果

来自最终构建产物 `out/build/x64-Debug/bin/Aether.exe --swapchain-test rt-foundation`（`validation-launch.exe isolated` + `VK_LAYER_VALIDATE_SYNC=1`）的真实输出：

- **默认启用路径（Part A）**：
  - `Selected GPU: NVIDIA GeForce RTX 5090`
  - `Graphics Queue Family: 0` / `Present Queue Family: 0`
  - `Ray Tracing Capabilities - Hardware: Supported, Device Enabled: Yes, Functions Loaded: Yes, Ready: Yes`
  - `AS Min Scratch Alignment: 128 bytes`
  - `Vulkan Memory Allocator created successfully! (BDA: Enabled)`
  - `Vulkan Renderer Initialized Successfully! ... Ray Tracing Ready: Yes`
  - 场景 Mesh 均具备有效 `GetVertexCount()` 与非零顶点/索引 `VkDeviceAddress`；普通非 BDA 缓冲返回 `0` 并输出明确诊断；显式 BDA 缓冲返回非零地址；带 RT 标志的 `failIndexUpload` 故障注入准确返回 `VK_ERROR_UNKNOWN` 且 VMA 分配计数精确回落至基线；光栅化渲染 4 帧正常；`Shutdown()` 后 RT 能力与 BDA 状态完整复位。
- **禁用降级路径（Part B）**：
  - `Ray Tracing Capabilities - Hardware: Supported, Device Enabled: No, Functions Loaded: No, Ready: No`
  - `Vulkan Memory Allocator created successfully! (BDA: Disabled)`
  - `Vulkan Renderer Initialized Successfully! ... Ray Tracing Ready: No`
  - 5 个 KHR 函数指针均为 `nullptr`；场景 Mesh 未附加 RT 标志；请求 BDA 缓冲返回 `VK_ERROR_FEATURE_NOT_PRESENT` 且 0 VMA 泄漏；光栅化渲染 4 帧正常。
- **验证层统计**：全部 19 项隔离 GPU 场景 + `helmet-preview` + 2 项桌面压测合计 **`VUID-` = 0、`SYNC-HAZARD` = 0、`Validation Error` = 0**。

---

## 11. 遗留限制与 Phase 9B 准入确认

### 当前遗留限制
1. **未在物理无光追 GPU 上实机验证**：仅通过受控禁用开关验证了软件降级路径，未来若具备集成显卡/无 RT 硬件测试环境建议补充一次实机冒烟。
2. **Mesh 缓冲内存类型仍为 `HOST_VISIBLE | HOST_COHERENT`**：遵循 Phase 9A 最小变更原则，暂未改造 `VulkanMesh` 为 `DEVICE_LOCAL` Staging 上传（符合 Vulkan BLAS 构建规范，后续按需优化）。
3. **尚未实现加速结构与 Ray Query 渲染**：BLAS、TLAS、`VulkanAccelerationStructure`、`VulkanRayTracingScene`、`VulkanRayQueryPass`、Ray Query Shader 以及 `Q` 键 `Rasterization <-> Ray Tracing` 切换均按计划留待 Phase 9B / 9C 实现。

### Phase 9B 准入确认
**明确确认：Phase 9A 所有验收标准（Definition of Done）已全部满足，现可正式启动 Phase 9B（`VulkanAccelerationStructure` 与 `VulkanRayTracingScene` 的 BLAS/TLAS 资源构建与多帧同步实现）。**
