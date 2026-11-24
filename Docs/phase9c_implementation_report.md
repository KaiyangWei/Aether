# Aether Engine — Phase 9C 实现与验证报告
## Ray Query Compute Pass、Q 键渲染模式切换与复杂 Cornell Box 默认验证场景

> **范围与诚实声明**：本阶段实现基于 Compute Shader 的硬件 **Ray Query 诊断渲染通路**、**`Q` 键渲染模式切换**（`Rasterization <-> Ray Query`）以及**复杂风格化 Cornell Box 默认验证室**，**未实现**完整 Path Tracing、Ray Tracing Pipeline / SBT、RT 阴影、RT 反射或降噪器。所有测试、VMA 统计、GPU Timestamp 与截图均来自最终构建产物在 **NVIDIA GeForce RTX 5090** 上的实机执行结果。

---

## 1. Executive Summary（执行摘要）

Phase 9C 完成了 Aether Engine 硬件光线追踪基础从“加速结构基础设施”到“可实时交互渲染与视觉验收”的关键闭环：

1. **硬件 Ray Query 计算渲染通道（`VulkanRayQueryPass` + `rayquery_debug.comp.hlsl`）**：通过 `cs_6_5` Compute Shader 发射与光栅化像素级一致的相机主射线，直接查询 Phase 9B 每帧槽 TLAS，并在未命中时复用活跃 `VulkanEnvironment` 天空盒，支持 4 种实时可切换诊断视图 + 1 种确定性数值 Readback 模式。
2. **实时 `Q` 键渲染模式切换（`Rasterization <-> Ray Query`）**：启动默认保持 `Rasterization`；按 `Q` 在完全相同的 `Scene`、`Camera`、变换、`VulkanMesh`、材质与 HDR 环境上切换模式（按 `G` 循环切换诊断视图），零场景复制、零相机复制、零 `vkDeviceWaitIdle()`，切换时强制使 TAA 时间历史失效，不支持硬件光追时优雅保持光栅化并打印提示。
3. **复杂风格化 Cornell Box 默认验证室（`CornellBoxScene`）**：取代原有简单方块场景成为无参数启动的内置默认场景（21 共享 PBR 网格、49 实例、零外部文件依赖），集成了红/绿/中性建筑墙体与采光天窗横梁、冷青/暖琥珀 HDR 霓虹灯带与基座灯带、大尺寸斜置镜面与镜面铬球、拉丝金球、阳极氧化铜环、哑光陶瓷球、旋转高柱与高频百叶栅栏，并正式确立为后续所有渲染特性的长期默认视觉验证场景。
4. **验证结果**：Debug 构建 **0 错误、项目源码 0 警告**；**11/11** 纯 CPU CTest 通过；**22/22** 隔离 RTX 5090 GPU/WSI 回归场景全部通过（`VUID-` = 0、`SYNC-HAZARD` = 0、`WARNING: Validation` = 0）；`helmet-preview` 与 2 项桌面压测全部通过；10 张 RTX 5090 实机对比截图肉眼验收通过；VMA 零泄漏。

---

## 2. 实际新增与修改的文件清单

### 新增文件
| 文件路径 | 职责说明 |
| --- | --- |
| `Engine/Renderer/RayTracingConfig.h` | 纯 CPU 渲染模式枚举（`RenderMode`、`RayQueryDebugMode`）、`ToggleRenderMode()` 策略、`SanitizeRayQueryConfig()` 与 `GenerateCameraPrimaryRay()` 相机主射线推导辅助。 |
| `Shaders/rayquery_debug.comp.hlsl` | `cs_6_5` 硬件 Ray Query 计算着色器（Set 0：TLAS / `rgba16f` Storage Output / Params UBO；Set 1：共享 `VulkanEnvironment` 采样器与环境 Cubemap）。 |
| `Engine/RHI/Vulkan/Passes/VulkanRayQueryPass.h` / `.cpp` | 独立 Ray Query Compute Pass，仅拥有自身计算管线、描述符集、每帧槽 `R16G16B16A16_SFLOAT` 输出图像/参数 UBO 与 GPU Timestamp QueryPool。 |
| `Engine/Assets/CornellBoxScene.h` / `.cpp` | 纯程序化自包含复杂 Cornell Box 验证室生成器（`MakeCornellBoxScene()`、`GetDefaultCornellBoxCamera()`、`InspectCornellBoxAsset()`）。 |
| `Tests/RayTracingVisualTests.cpp` | 实现 `rt.rayquery`（解析距离/命中/图元/实例数值 Readback、Q 切换、TAA 失效、Resize、空场景、重复初始化/销毁零泄漏、禁用 RT 降级）与 `rt.visual`（10 张 RTX 5090 实机截图 + GPU Timestamp 采集）。 |
| `Docs/phase9c_implementation_report.md` | 本阶段完整实现与验证报告。 |

### 修改文件
| 文件路径 | 修改内容 |
| --- | --- |
| `Engine/RHI/Vulkan/Assets/VulkanEnvironment.cpp` | 将 `CreateLayout` 描述符阶段标志从 `VK_SHADER_STAGE_FRAGMENT_BIT` 扩展为 `VK_SHADER_STAGE_FRAGMENT_BIT \| VK_SHADER_STAGE_COMPUTE_BIT`，使 Ray Query Compute Shader 与光栅化 Skybox/PBR 直接共享同一环境描述符集。 |
| `Engine/RHI/Vulkan/VulkanRenderer.h` / `.cpp` | 持有并生命周期管理 `VulkanRayQueryPass`（与 `VulkanRayTracingScene`、`ShadowPass`、`VulkanTaaPass`、`VulkanBloomPass` 并列），接入 `Initialize` / `OnSwapchainRecreated` / `DrawFrame`（支持 `RenderMode::RayQuery` 路径与共享 `ToneMap` 输出） / `Shutdown`。 |
| `Engine/Core/AetherApplication.h` / `.cpp` | 默认无参数启动调用 `LoadDefaultCornellBoxScene()` 加载 Cornell Box；新增 `ToggleRenderModeInteractive()`、`Q` 键模式切换与 `G` 键诊断视图切换；透传未抖动 `unjitteredClipToWorldDirection` 与相机位置。 |
| `Tests/RayTracingMathTests.cpp` | 扩展纯 CPU 单元测试至 11 组（新增相机主射线重构精度、`ToggleRenderMode` 策略、`SanitizeRayQueryConfig`、Cornell Box 结构/材质完整性校验）。 |
| `Tests/SwapchainRecovery.cpp` | 注册 `--swapchain-test rt-rayquery` 与 `rt-visual` 分发入口。 |
| `CMakeLists.txt` | 注册 `rayquery_debug.comp.hlsl`（`cs_6_5` → `vulkan1.3`）、`Tests/RayTracingVisualTests.cpp`、`CornellBoxScene.cpp` / `Camera.cpp` 与 `rt.rayquery` / `rt.visual` CTest。 |
| `ARCHITECTURE.md` / `README.md` | 正式记录双渲染模式、`Q`/`G` 快捷键、“同场景/同相机/同数据”契约，以及 Cornell Box 作为长期默认视觉验证场景的工程约定。 |

---

## 3. Ray Query 着色器编译路径与实测验证

- **实测编译命令**（Vulkan SDK `1.4.357.0` 自带 DXC `1.9.0.5399`）：
  ```text
  dxc.exe -spirv -fspv-target-env=vulkan1.3 -T cs_6_5 -E main Shaders/rayquery_debug.comp.hlsl -Fo out/build/x64-Debug/Shaders/rayquery_debug.comp.spv
  ```
- **实验结论**：在 `-fspv-target-env=vulkan1.3` + `-T cs_6_5` 下，DXC 自动生成合法的 `SPV_KHR_ray_query` SPIR-V 模块并通过 `spirv-val --target-env vulkan1.3`（`exit=0`），**无需额外 `-fspv-extension` 标志**，直接复用现有 `add_aether_shader()` 宏。
- **关键格式属性实测修正**：首次运行 Vulkan Validation 时捕获 `Undefined-Value-StorageImage-FormatMismatch-ImageView` 警告（HLSL `RWTexture2D<float4>` 默认映射 SPIR-V `Rgba32f`，而输出图像为 `VK_FORMAT_R16G16B16A16_SFLOAT`）。在着色器声明添加 **`[[vk::image_format("rgba16f")]]`** 后，DXC 精确生成 `OpTypeImage ... Rgba16f`，Validation 警告彻底清零（**0 errors, 0 warnings**）。

---

## 4. `VulkanRayQueryPass` 设计

- **每帧槽资源隔离（`MAX_FRAMES_IN_FLIGHT = 2`）**：每个帧槽独立拥有 `outputImage`（`VK_FORMAT_R16G16B16A16_SFLOAT`，`STORAGE | SAMPLED | TRANSFER_SRC | TRANSFER_DST`）、`paramsBuffer`（112B Host-Visible Coherent UBO）、`computeSet`（Set 0）与 `toneMapSet`（兼容 `VulkanRenderer::m_hdrLayout`）。
- **Synchronization2 依赖链**：
  1. `ACCELERATION_STRUCTURE_BUILD (AS_WRITE)` → `COMPUTE_SHADER (AS_READ)`（当帧内更新/构建了 TLAS 时发出）+ `HOST (HOST_WRITE)` → `COMPUTE_SHADER (UNIFORM_READ)`；
  2. `outputImage`: `oldLayout` → `GENERAL` (`COMPUTE_SHADER / STORAGE_WRITE`)；
  3. `vkCmdDispatch((width + 7) / 8, (height + 7) / 8, 1)`（包裹 `vkCmdWriteTimestamp2`）；
  4. 可选 FP16 Readback 拷贝（`GENERAL` → `TRANSFER_SRC_OPTIMAL` → `CopyImageToBuffer` → `HOST_READ`）；
  5. `outputImage` → `SHADER_READ_ONLY_OPTIMAL`（`FRAGMENT_SHADER / SAMPLED_READ`），直接供 `VulkanRenderer::ToneMap()` 采样。
- **空场景安全保护**：当 `tlasHandle == VK_NULL_HANDLE` 时绝不向 `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR` 写入空句柄，改用 `vkCmdClearColorImage` 清除输出并安全转换布局。

---

## 5. 描述符与资源所有权边界

- **Set 0（`VulkanRayQueryPass` 独占）**：`binding 0` = TLAS（借用 `VulkanRayTracingScene` 句柄）、`binding 1` = `outputImage`（`STORAGE_IMAGE`）、`binding 2` = `RayQueryUniforms`（`UNIFORM_BUFFER`）。
- **Set 1（借用 `VulkanEnvironment`）**：`binding 3` = `SamplerState`、`binding 4` = `TextureCube`（与光栅化 Skybox/PBR 完全复用同一套描述符集与内存）。
- **严格边界**：`VulkanRayQueryPass` 不持有 `Scene`、`Camera`、`VulkanMesh` 或 BLAS/TLAS；`VulkanMemoryManager` 仍严格仅管理 `VmaAllocator`。

---

## 6. 相机主射线推导与像素级一致性

- `Camera` 采用右手系 `lookAtRH` 与 Vulkan `[0, 1]` 深度 `perspectiveRH_ZO`（且 `P[1][1] *= -1.0f` 以匹配正高度 Vulkan Viewport）。
- `AetherApplication::DrawScene()` 计算去除平移后的未抖动逆矩阵：
  $$\text{clipToWorldDirection} = (P_{\text{unjittered}} \cdot R_{\text{view}})^{-1}$$
- 对像素 `(px, py)`，归一化屏幕中心 `uv = (float2(px, py) + 0.5) / float2(W, H)`、`ndc = uv * 2.0 - 1.0`，反投影 `worldH = mul(clipToWorldDirection, float4(ndc, 1.0, 1.0))` 得 `rayDir = normalize(worldH.xyz / worldH.w)`，`rayOrigin = camera.GetPosition()`。
- **验证**：CPU 测试 `rt.math` 将 5 个三维空间点经 `P * V` 投影到像素再重构射线，点积精度均 `> 0.9999`；RTX 5090 实拍 `rq-01` vs `rq-02` 及 `rq-09` vs `rq-10` 呈现亚像素级几何构图重合（仅光栅化开启 TAA 时存在常规亚像素 Halton 抖动，Ray Query 始终采用未抖动投影以保持射线稳定）。

---

## 7. 渲染模式切换设计（`Q` / `G`）

- **`Q` 键**：调用 `ToggleRenderModeInteractive()`（底层纯函数 `ToggleRenderMode(current, m_renderer.CanUseRayQueryMode())`）。
  - 启动默认模式：**`Rasterization`**。
  - 支持硬件光追：`Rasterization <-> Ray Query`，即时打印激活模式与诊断模式日志；不重建 `Scene` / `Camera` / `VulkanMesh` / `VulkanMaterialSet` / `VulkanEnvironment`；切回光栅化时保留已构建 BLAS，不调用 `vkDeviceWaitIdle()`。
  - 不支持硬件光追（或 RT 禁用）：拒绝切换并输出清晰日志，安全停留在 `Rasterization`，绝不崩溃。
- **`G` 键**：在 4 种诊断视图间循环切换：`Diagnostic Shading`（默认，实例伪彩 + 重心边缘线 + 距离衰减 + 活跃 HDR 天空盒未命中背景）→ `Hit / Miss` → `Hit Distance` → `Primitive & Instance ID`。

---

## 8. TAA / 时间历史失效策略

- `ToggleRenderModeInteractive()` 只要发生模式切换（无论 `Rasterization → Ray Query` 还是 `Ray Query → Rasterization`），立即调用 `ResetTemporalHistory()`（清零 `m_hasPrevCamera`、`m_taaFrameIndex` 并调用 `m_renderer.ResetTaaHistory()`）。
- 在 `Ray Query` 模式期间，`DrawScene()` 主动停用 TAA 抖动（`taaActive = false`），且 `VulkanRenderer::DrawFrame()` 在提交后保持 `m_taaPass.ResetHistory()`，确保切回光栅化首帧绝不采样陈旧历史。

---

## 9. Ray Query 模式的后处理策略

- **TAA**：有意旁路并失效（Phase 9C Ray Query 诊断通路不生成光栅速度场/深度历史）。
- **Bloom**：有意旁路（防止伪彩诊断图、距离图、三角形/实例 ID 边界被泛光模糊污染）。
- **Tone Mapping（Exposure + ACES Filmic + sRGB）**：统一保留并直接复用 `VulkanRenderer::ToneMap()`，确保 HDR 环境未命中像素、曝光 EV 控制与 `m_captureTarget` 截屏读回行为与光栅化一致。

---

## 10. 复杂 Cornell Box 默认场景设计 rationale

位于 `Engine/Assets/CornellBoxScene.h/.cpp`（**21 个共享 PBR 网格、49 个场景对象、15 种 PBR/自发光材质**，零外部资产依赖）：
1. **非对称建筑级房间结构**：6.0×4.6×6.0 m 封闭展室，左侧深红墙配 4 根垂直声学肋条、右侧翠绿墙配错层展示壁龛与 5 根高频暗金属斜置百叶、后墙暖灰灰泥与顶梁、抛光暗石板地板与中央镶嵌地台。
2. **采光天窗（Shadow-Friendly）**：顶部采用 4 块围合吊顶 + 中央采光天窗 + 2 根横梁，使方向光以斜角射入室内，在后墙、高柱、基座与地板上投射出清晰易辨的建筑与几何阴影。
3. **冷/暖 HDR 自发光霓虹体系**：冷青色角落竖向霓虹灯管 + 后墙横条 + 中央基座灯带（`emissive = (0.6, 4.8, 8.5)`）、暖琥珀右侧 L 形霓虹框 + 竖向灯管（`(8.0, 2.2, 0.65)`）以及天窗暖白灯条，直接驱动真实线性 HDR Bloom。
4. **镜面与高反射目标**：左侧斜置大尺寸暗金属画架镜面面板（`metallic = 1.0, roughness = 0.02`）+ 中央双层基座顶部的抛光镜面铬球（为未来 RT 反射与当前 IBL 镜面反射提供直接对照）。
5. **多元 PBR 材质与几何对比**：哑光陶瓷球（粗糙绝缘体 `roughness = 0.76`）、拉丝金球（中等粗糙金属 `roughness = 0.28`）、阳极氧化铜斜置圆环（`roughness = 0.18`，考验曲面高光与 Ray Query 三角形遍历）、旋转哑光高柱与壁龛微型材质球组。

---

## 11. 为何确立为长期默认视觉验证场景

该 Cornell Box 验证室同时覆盖了引擎现有与未来规划的全部核心维度，且不依赖任何外部拖放文件，在 `ARCHITECTURE.md` 与 `README.md` 中正式确立为**长期默认视觉验证基准**：
- 光栅化与 Ray Query 同视角构图对照；
- PBR 粗糙度/金属度梯度与 IBL 响应；
- HDR 曝光与线性 Bloom 光晕；
- 方向光阴影与天窗建筑投影；
- TAA 细斜边（百叶、霓虹、镜框）抗锯齿；
- 未来 Phase 9D+ 的 RT 阴影、RT 反射（镜面与铬球直接受益）、全局光照与时间降噪。

---

## 12. CPU / GPU 自动化测试结果（`out/build/x64-Debug`）

- **Debug 构建**：`0` 错误，项目源码（`Engine/` / `App/` / `Tests/`）`0` 警告（`/W4`）。
- **纯 CPU CTest（11 / 11 Passed，`1.67 s`）**：`camera.matrices`、`mesh.data`、`asset.obj`、`scene.transforms`、`asset.gltf`、`shadow.math`、`bloom.config`、`taa.math`、`rt.capabilities`、`rt.math`（11/11 断言含相机射线与 Cornell Box 校验）、`environment.radiance`。
- **隔离 GPU/WSI 测试（22 / 22 Passed，`valHits = 0`）**：
  - 既有 20 项回归（`swapchain.*`、`asset.*`、`shadow.*`、`bloom.*`、`taa.*`、`rt.foundation`、`rt.as`）全部通过；
  - 新增 **`rt.rayquery`** 通过：解析单位立方体中心像素 Hit 距离实测精确匹配 `2.50`（误差 `< 0.02`）、角落像素 Miss、双实例 `instanceCustomIndex = 0 / 1` 区分、Cornell Box Q 切换与 TAA 失效、4 种诊断模式、HDR 环境切换、900×600 Resize、空场景安全清除、3 轮 `VulkanRayQueryPass` 重复初始化/销毁零泄漏、禁用 RT 优雅保持光栅化；
  - 新增 **`rt.visual`** 通过：完整生成并像素差异校验全部 10 张 RTX 5090 实机截图。
- **附加场景与桌面压测**：`helmet-preview`、`--single-cube`、`--material-test DamagedHelmet.glb` 全部通过（`exit=0, valHits=0`）。

---

## 13. Validation / Synchronization Validation 结果

通过 `validation-launch.exe isolated`（开启 Khronos Validation Layer + `VK_LAYER_VALIDATE_SYNC=1` + 禁用第三方 RTSS 注入）：
- 全部 **22** 项 GPU 场景 + `helmet-preview` + **2** 项桌面压测：
  - `VUID-` 错误：**0**
  - `SYNC-HAZARD` 报错：**0**
  - `Validation Error`：**0**
  - `[WARNING: Validation]`：**0**（`[[vk::image_format("rgba16f")]]` 修复后全量清零）

---

## 14. VMA 分配统计

- `VulkanRayQueryPass` 每帧槽固定占用 2 项 VMA 分配（1 × `R16G16B16A16_SFLOAT` 输出图像 + 1 × 参数 UBO），2 帧槽共 **4** 项分配。
- `rt.rayquery` 连续 3 轮执行 `VulkanRayQueryPass::Initialize()` → `Shutdown()`，VMA 分配计数每次均精确回落至关闭前基线（**0 泄漏**）；`rt.as` 保持 `55 -> 64 -> 55`（**0 泄漏**）。

---

## 15. RTX 5090 实测 GPU 性能（`1280×720`）

通过 `VulkanRayQueryPass` 内置 `VkQueryPool(VK_QUERY_TYPE_TIMESTAMP)` 测量（**纯 GPU Compute Dispatch 执行耗时**，不含 CPU 命令录制与队列提交开销）：

| 场景（`1280×720`） | 规模 | Ray Query Compute Dispatch（GPU-only） |
| --- | --- | --- |
| **Cornell Box 默认验证室** | 21 共享 BLAS，49 实例 | **`0.0290 ms`** |
| **`DamagedHelmet.glb`** | 高面数 glTF PBR 头盔 | **`0.0155 ms`** |

稳态模式下切换 `Q` 后，若场景几何未变动，BLAs 完全复用、每帧槽 TLAS 首次构建后进入 `Skipped`（0 ms 加速结构重建开销），Ray Query 单帧核心 GPU 渲染耗时仅约 **`0.015–0.029 ms`**（加 `ToneMap`）。

---

## 16. RTX 5090 实机截图验收清单

全部 10 张 `1280×720` 实机截图已生成、转为 PNG 并完成人工视觉核验（位于对话 Artifact 目录）：
1. `rq-01-cornell-raster.png` — Cornell Box 默认视角（光栅化 PBR + IBL + 阴影 + Bloom + TAA）
2. `rq-02-cornell-rayquery-diag.png` — Cornell Box 完全同视角同场景（`Q` 切换至 Ray Query `Diagnostic Shading`，与 `rq-01` 亚像素级构图一致）
3. `rq-03-cornell-neon-bloom.png` — Cornell Box 霓虹与自发光 Bloom 特写（冷青 + 暖琥珀霓虹光晕清晰）
4. `rq-04-cornell-reflective.png` — Cornell Box 反射与镜面材质特写（斜置镜面面板、镜面铬球、拉丝金球、铜环）
5. `rq-05-cornell-shadows.png` — Cornell Box 阴影构图特写（天窗横梁与高柱/球体投影清晰）
6. `rq-06-cornell-rayquery-hitmiss.png` — Cornell Box Ray Query `Hit / Miss`（室内全命中绿色，天窗开口精确显示未命中深蓝）
7. `rq-07-cornell-rayquery-distance.png` — Cornell Box Ray Query `Hit Distance`（前景暖金→中景青蓝→背景深蓝平滑深度渐变）
8. `rq-08-cornell-rayquery-priminst.png` — Cornell Box Ray Query `Primitive & Instance ID`（实例与三角形边界清晰可辨）
9. `rq-09-helmet-raster.png` — `DamagedHelmet.glb` 光栅化 PBR + HDR 天空盒
10. `rq-10-helmet-rayquery.png` — `DamagedHelmet.glb` Ray Query 诊断渲染（精确重合轮廓与共享 HDR 环境背景）

---

## 17. 遗留限制

1. **当前 Ray Query 为诊断渲染路径**：不执行 PBR 材质贴图采样、二次弹射反射、RT 阴影或路径追踪（留待后续 Phase 扩展）。
2. **Alpha MASK 纹理裁剪尚未接入 Ray Query**：目前非不透明候选命中直接提交几何命中（`CommitNonOpaqueTriangleHit()`），待后续引入 Bindless 材质/纹理表后按 `alphaCutoff` 精确裁剪。
3. **无物理非 RT GPU 实机测试**：仅通过 RTX 5090 上的软件禁用 RT 降级测试验证了非 RT 逻辑路径，未在无硬件光追物理显卡上实测。

---

## 18. Phase 9D 准入评估

**明确确认：Phase 9C 全部验收标准（Definition of Done）均已完成并在 RTX 5090 上实机验证通过，现已完全就绪进入 Phase 9D（或后续基于 Cornell Box 默认验证室展开的 Hybrid RT 阴影/反射与视觉验收增强阶段）。**
