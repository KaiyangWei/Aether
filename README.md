# Aether Engine

Aether 使用 Vulkan 1.3 dynamic rendering 和 synchronization2 绘制多个共享网格的场景实例。

## Windows Debug 构建

在 Visual Studio 2026 的 x64 开发者命令提示符中运行。需要 CMake 3.24 或更新版本、Ninja 和 Vulkan SDK 1.4.357.0（含 DXC）。

```bat
chcp 65001
cmake -S . -B out/phase1-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build out/phase1-debug
ctest --test-dir out/phase1-debug --output-on-failure
```

首次配置和后续构建使用同一代码页。在中文 MSVC 环境中，如果旧构建目录未记录头文件依赖，请用新的构建目录重新配置，并用 `ninja -C out/phase1-debug -t deps CMakeFiles/Aether.dir/App/main.cpp.obj` 检查依赖。

运行 `out\phase1-debug\bin\Aether.exe`。程序使用 Vulkan 验证层，并从构建目录的 `Shaders` 读取生成的 SPIR-V。运行 CTest 需要可创建 Vulkan 窗口的桌面会话。

## 默认验证场景（Cornell Box）与视觉基准

无参数启动 `Aether.exe` 默认进入内置的**风格化复杂 Cornell Box 验证室**（纯程序化自包含 PBR 资产，22 个共享网格 / 50 个场景实例，无需外部模型），包含红/绿/中性建筑墙体与采光天窗横梁、冷青/暖琥珀 HDR 霓虹灯带与基座轮廓光、大尺寸斜置镜面与镜面铬球（`0.02`）、阳极氧化铜环（`0.18`）、拉丝金球（`0.28`）、**粗糙青铜球（`0.62`）**、哑光陶瓷球（`0.76`）及高频百叶结构，并附带 4 盏显式物理区域光/点光源（天窗矩形光、冷青霓虹光、暖琥珀霓虹光、基座点光源），作为引擎后续所有渲染特性（PBR、HDR/Bloom、阴影、TAA、硬件光追、反射与降噪）的**长期默认视觉验证场景**。使用 `Aether.exe --single-cube` 运行原有单旋转立方体回归场景，使用 `Aether.exe --material-test "DamagedHelmet.glb"` 运行 5×5 PBR 材质球阵列与头盔预览。鼠标左键拖动环绕，中键拖动平移，滚轮缩放。相机使用 GLM 右手坐标和 Vulkan 的 [0, 1] NDC 深度。投影矩阵翻转一次 Y，对应正高度 viewport。交换链重建后使用其实际 extent 更新相机宽高比。

`MeshData` 保存交错排列的位置、颜色、法线、UV 和切线（XYZ 为方向，W 为副切线手性），索引使用 uint32 三角形列表。默认演示使用顶点颜色 Shader。导入预览使用 PBR 管线。`VulkanMesh` 复用 VulkanBuffer 上传顶点和索引，应用持有 Mesh 资源并在 GPU 空闲后释放。Scene 按值持有对象并支持按索引查询与移除，对象只记录 Mesh ID 和 TRS，应用将其解析为 Renderer 借用的逐对象绘制数据。

`camera.matrices`、`mesh.data` 和 `scene.transforms` 覆盖 CPU 数据及矩阵。`swapchain.multi-mesh` 检查两个立方体共享同一对缓冲区，四面体使用独立缓冲区，调整窗口尺寸后继续提交多实例帧。

## 导入 glTF / GLB

启动 Aether 后，从 Windows 资源管理器把 .gltf 或 .glb 文件拖到渲染窗口。可一次拖入多个文件。导入成功后，新模型将替换当前场景并成为预览主体，相机会聚焦到该模型的包围范围。同批多个文件时，最终显示最后一个成功导入的模型。原始场景和 --single-cube 模式均可使用拖放。失败原因写入标准错误，当前场景保留，后续文件仍会继续处理。

.glTF 的外部缓冲和贴图路径相对于模型文件解析，请保留导出目录结构。支持 GLB 内嵌图片、数据 URI 和外部 PNG/JPEG。导入预览使用 Metallic-Roughness PBR。Base Color 和 Emissive 采用 sRGB，其余纹理按线性数据采样。支持法线贴图、遮蔽、自发光及 Alpha 模式。按住左右方向键可旋转方向光，观察粗糙度和金属反射变化。支持静态三角形网格，蒙皮、动画、morph、压缩与 glTF 扩展会返回明确的诊断。

首次 CMake 配置需要 Git 和 GitHub 访问。fastgltf v1.0.1、simdjson v5.1.0 和 simdutf v9.3.3 固定到提交。asset.gltf 在构建目录生成测试资产，swapchain.asset-upload 检查 GPU 整批回滚与实例共享，asset.windows-drop 检查 Windows 文件投递、Unicode 路径及错误恢复。

## HDR 环境与材质预览

启动后，PBR 预览使用随构建分发的线性 Radiance RGBE 环境。拖入 `.hdr` 可替换照明，拖入模型可替换预览主体。按 **E** 切换冷色摄影棚与暖色环境，**上 / 下方向键**调整曝光 EV，**左 / 右方向键**旋转方向光，**H** 切换方向光阴影、**M** 切换阴影深度调试视图，**B** 切换 HDR Bloom、**V** 循环切换 Bloom 金字塔调试视图，**T** 切换时间抗锯齿（TAA）、**R** 重置 TAA 历史缓冲。导入环境时会同步进行预计算。

运行 `Aether.exe --material-test` 打开 5 × 5 材质球阵列，或运行 `Aether.exe --material-test "DamagedHelmet.glb"` 将头盔放在阵列旁。列从左到右表示粗糙度 0 至 1，行从上到下表示金属度 0 至 1。

环境图按经纬度采样为 128 × 128 的六面浮点 Cubemap。GPU 计算 32 × 32 的余弦加权漫反射卷积（已除以 π）、八层 GGX 镜面预过滤和 128 × 128 的双通道 BRDF 积分值。直接光与环境光在 FP16 目标中合成，透明材质在线性空间混合。全屏 pass 应用曝光与 ACES Filmic 曲线，最终使用 sRGB 附件转换或 UNORM 输出编码。

## 开发顺序

1. 稳定构建、帧同步与交换链重建，保持立方体回归场景。
2. 让资源对象拥有 Vulkan 句柄，MemoryManager 管理内存分配。建立深度附件和多 Mesh 绘制路径。
3. 将窗口拖放事件交给应用层，解析 glTF/GLB 后分别处理 CPU 场景数据与 GPU 上传。
4. 接入 Metallic-Roughness 材质、纹理、直接光照和 HDR 扩展所需的颜色空间约定。
5. 实现环境光预计算、IBL、曝光和色调映射，再执行完整的验证层与资源生命周期回归。

## Vulkan 资源生命周期

`AetherApplication` 持有 VulkanMesh，后者拥有顶点和索引 Buffer。Renderer 在绘制时借用 Mesh，应用保证 Mesh 存活到 GPU 使用结束。Buffer 使用同一 Vulkan device 及其对应的 MemoryManager。关闭时先等待 GPU 使用完成，再销毁 Buffer、回收分配、关闭 allocator 和 device。

自建图像由 `VulkanImage` 销毁视图和图像，再通过 `VulkanMemoryManager` 释放内存。交换链提供颜色图像，`VulkanImage` 持有对应视图。Renderer 在交换链重建后按新 extent 创建深度附件，并在 allocator 关闭前释放深度资源。

## 交换链回归

| CTest | 覆盖范围 |
| --- | --- |
| `swapchain.image-count` | 主动请求更多图像，检查每个索引的图像、视图、布局和 semaphore，再提交立方体帧。 |
| `swapchain.color-format` | 主动选择 surface 支持的另一颜色格式，检查管线格式并提交立方体帧。 |
| `swapchain.create-failure`、`swapchain.view-failure` | 注入交换链创建失败和第 2 个视图创建失败。要求恢复返回失败、交换链状态清空，并通过验证层检查清理。 |
| `swapchain.resize` | 调整窗口尺寸，检查深度附件尺寸及布局重置，并验证颜色格式不变时复用管线。 |

图像数或颜色格式受 surface 能力限制时，对应测试会报告跳过。实际 acquire 顺序由驱动决定。CTest 开启 Vulkan 同步验证。最小化和恢复仍可用窗口手动验证。

## 硬件光线追踪与混合渲染管线（Phase 9 – Phase 12）

Aether 已完整实现从核心基础设施到生产级实时混合光线追踪（Hybrid Ray Tracing）的全套管线，并在不支持硬件光追的设备上保持纯光栅化优雅降级：

- **渲染模式与快捷键切换（`Q` 键 / `G` 键）**：
  - 启动默认处于 **`Rasterization`**（包含方向光 PCF 阴影 + TAA + HDR Bloom + ACES Tone Mapping）；
  - 按 **`Q`** 在 **`Rasterization <-> HybridRayTracing`** 之间实时切换（共享完全相同的 `Scene`、`Camera`、变换、`VulkanMesh`、材质与 HDR 环境，不触发 `vkDeviceWaitIdle()`，切换时自动清理 TAA 与光追降噪时域历史）；
  - 在 **`HybridRayTracing`** 模式下按 **`G`** 循环切换 **12 种实时调试视图**：`Final Hybrid Lighting` → `RT Shadows Only` → `RT Reflections Only` → `Shadow Visibility Mask` → `Shadow Sample Count` → `Reflection Radiance` → `Reflection Hit / Miss` → `Reflection Hit Distance` → `Surface Roughness` → `Denoiser Temporal Confidence` → `Denoiser Spatial Variance` → `Ray Query Primary Diagnostic`；
  - 在非实时诊断模式下，`Ray Query` 模式亦提供 **4 种主射线诊断视图**（`Diagnostic Shading` / `Hit-Miss` / `Hit Distance` / `Primitive & Instance ID`）。
- **核心渲染特性**：
  - **Phase 9A–9D（光追基础与验证）**：可选 KHR 光追能力原子探测/启用、BDA 与 `VulkanAccelerationStructure`、按帧槽隔离的 BLAS 复用 + 延迟销毁、TLAS 原地 `Update`/`Rebuild`/`Skip`、整合 GPU 顶点/索引/实例 SSBO 与 Alpha MASK 采样表。
  - **Phase 10（硬件光追阴影）**：4-MRT 混合 G-Buffer + `cs_6_5` 内联 Ray Query 阴影 Pass，支持法线+视距偏移抗痤疮、IGN+R2 太阳圆盘软阴影、物理矩形区域光/点光/聚光灯评估及实时 Alpha MASK 纹理镂空求交。
  - **Phase 11（硬件光追反射）**：镜面快速路径（`roughness <= 0.05`）+ Heitz GGX VNDF 粗糙表面重要性采样（`0.05 < roughness <= 0.72`），支持命中点完整 PBR 着色、二次光追阴影与 IBL 环境回退。
  - **Phase 12（时空降噪与统一合成）**：前帧重投影 + 实例/法线/深度离散剔除 + 亮度二阶矩方差估计 + 镜面旁路的 3 轮边缘保持 À-Trous 空间滤波 + 统一线性 HDR 合成并接入 `BloomInputSource::HybridComposite` → Exposure/ACES ToneMap。
- **自动化验证（37 项 CTest）**：
  - **纯 CPU（11 项）**：`camera.matrices`、`mesh.data`、`asset.obj`、`scene.transforms`、`asset.gltf`、`shadow.math`、`bloom.config`、`taa.math`、`rt.capabilities`、`rt.math`、`environment.radiance`；
  - **GPU / WSI（26 项）**：涵盖 10 项交换链恢复、资产拖放/PBR、阴影/Bloom/TAA 以及 `rt.foundation`、`rt.as`、`rt.rayquery`、`rt.visual`、`rt.shadow`、`rt.reflection`、`rt.denoise`、`rt.hybrid`（详见各阶段验收报告 `Docs/phase9d_final_validation_report.md` 至 `Docs/phase12_denoising_hybrid_rendering_report.md` 与 `ARCHITECTURE.md`）。
