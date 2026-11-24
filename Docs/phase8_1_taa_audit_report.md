# Aether Phase 8.1 — TAA 独立审计与稳定性验证

## 1. 摘要

Phase 8 的投影抖动符号正确，但原 Resolve 在抖动后的像素坐标上只叠加了未抖动的几何速度。静止场景因此也会产生最多接近一像素的历史采样偏差。天空盒还把当前抖动像素当作未抖动坐标送进重投影矩阵。修复后，几何、Mask 和天空统一输出带前后帧抖动的 UV 差。Resolve 用这个差寻址，并扣除抖动差来衡量真实运动。

历史只在 GPU 提交成功后可用。应用在完整帧成功返回后更新相机、对象矩阵及 Halton 相位。深度拒绝改用同一表面在上一帧的投影深度，拒绝天空与几何互相借用历史。Vulkan 资源所有权和两帧并行模式沿用现有架构。

## 2. 坐标、jitter 与运动矢量

Camera 使用 `lookAtRH` 与 `perspectiveRH_ZO`。视图前方满足 `view.z < 0`，`clip.w = -view.z > 0`，近面和远面分别映射到 NDC 深度 0 与 1。GLM 的索引是列、行，乘法是 `P * V * M * p`，DXC 的顶点 Shader 以 `mul(matrix, position)` 使用同一数学约定。Camera 将 `P[1][1]` 取负。正高度 viewport 令 NDC `(-1,-1)` 落在窗口左上，UV 是 `ndc.xy * 0.5 + 0.5`，Y 随像素行向下增加。[Vulkan viewport 约定](https://docs.vulkan.org/spec/latest/chapters/vertexpostproc.html)

设偏移像素为 `h = Halton(frame % length + 1, {2,3}) - 0.5`，NDC 偏移为 `d = 2h / (W,H)`。计算 `P'[2][0/1] = P[2][0/1] - d` 后有 `clip'.xy = clip.xy - d * view.z = clip.xy + d * clip.w`。除以 w 后增加 `d`，因此 UV 位移为正的 `j = d/2 = h/(W,H)`，z 和 w 保持原值。

```text
Ucurr = UcurrGeom + jcurr
Uprev = UprevGeom + jprev
Vgeom = UprevGeom - UcurrGeom
Vstored = Uprev - Ucurr = Vgeom + (jprev - jcurr)
historyUV = Ucurr + Vstored
motionRejectionSpeed = length(Vstored - (jprev - jcurr))
```

几何以当前抖动 MVP 光栅化，同时插值当前与上一帧抖动 clip。透视正确插值以当前 clip.w 作权重，同一点的两个 clip 向量各自除以自己的 w 得到对应 UV。前一帧 w 非正或过小时拒绝采样。对于天空，`R` 是去除平移的视图旋转，当前像素使用 `Pprev' * Rprev * inverse(Pcurr' * Rcurr)` 重投影。纯相机平移只剩 jitter 差。纯旋转有天空的方向变化，几何使用完整矩阵。

## 3. 历史、同步与深度生命周期

| 资源 | 已核对的依赖 |
| --- | --- |
| 每帧 UBO、命令缓冲和 timestamp queries | 帧槽 fence 在 CPU 改写前等待。Query range 按槽分开。记录成功后由 `vkQueueSubmit2` 提交，成功提交才发布 history 槽和 query 状态。 |
| Velocity | 在图形队列上，前次颜色写与 fragment sampled read 都进入本次写前依赖。当前颜色写通过 `COLOR_ATTACHMENT_OUTPUT / COLOR_ATTACHMENT_WRITE` 到 `FRAGMENT_SHADER / SHADER_SAMPLED_READ` 后供 Resolve 采样。 |
| 两份历史颜色 | 固定写槽 `f` 并绑定前一槽 `1-f`。只有该槽是上一份成功提交的连续历史才采样。写后屏障使颜色对后续 Resolve、Bloom 或 ToneMap 可见。覆盖旧槽前，屏障覆盖以前的颜色写与其他帧的 fragment 采样。帧槽 fence 本身无法保护其他槽对历史的读取。 |
| 当前场景深度 | HDR 场景保存不透明和 Mask 深度。Early/Late attachment write 到 Early/Late depth read 与 fragment sampled read，使速度 pass 与 Resolve 看到保存值。速度 pass 用只读深度及 `LESS_OR_EQUAL`，禁止深度写入。Resolve 后将深度 aspect 转到 `TRANSFER_SRC_OPTIMAL`，拷贝后恢复 attachment 布局。 |
| Shared previous depth | Resolve 的 fragment 读和前次 copy 写都进下一次 copy 写前屏障。Resolve 完成后复制当前深度，copy 写到 fragment sampled read 的屏障使下一帧读到这份深度。当前及历史深度同格式，只复制一个 depth aspect、mip 和 layer。 |

这些屏障录在动态渲染区间之外，对同一图形队列上前后命令缓冲生效。两帧在 CPU 侧可以同时提交，GPU 的 RAW、WAR 与 WAW 由上述屏障约束。Resize 和退出沿用原有 WSI 生命周期及其等待机制，本阶段未增加设备空闲等待。 [Synchronization2 规范](https://docs.vulkan.org/spec/latest/chapters/synchronization.html) · [Vulkan 图像复制规范](https://docs.vulkan.org/spec/latest/chapters/copies.html)

首次使用只把未写过的历史描述符目标转到声明的可采样布局。初帧及 Reset 后的首帧直接输出经有限值保护的当前 HDR，无历史纹理读，并完整覆盖输出槽。Reset 使两槽旧内容逻辑失效，同时保留布局跟踪。若 GPU 录制或提交失败，渲染器进入停机路径。Acquire 返回 Out-of-date 时没有录制或提交。Present 返回 Out-of-date 或 Suboptimal 时，已提交的 GPU 工作仍按原 WSI 生命周期收尾，由重建丢弃这段历史。

## 4. 历史失效与时间状态

| 条件 | 处理 |
| --- | --- |
| TAA 关闭、Shadow Debug 旁路 | 丢弃上一时间区间。重新开启从 Halton 0 开始。 |
| Resize、交换链重建、最小化 | 重建尺寸资源并重新绑定描述符。最小化停止提交并使时间历史无效，同尺寸恢复也会重新开始。 |
| FOV、宽高比、裁剪面与 jitter 序列变更 | 与上一成功帧的投影和序列配置比较，检测到变化即 reset。 |
| 相机跳变 | 导入场景时显式 reset。其他相机更新按两帧前向夹角超过 60°，或平移超过上一帧观察距离判定。普通 Orbit 和 Pan 保留历史。 |
| 场景、HDR、阴影切换与手动 reset | 成功完成场景或环境替换后 reset。资源上传失败不改变时间区间。阴影开关和手动 R 显式 reset。 |
| 物体生成、移除 | 新物体没有上一帧 clip，用无效表面标记拒绝其历史。移除或移动露出的表面通过上一投影深度和天空/几何分类剔除历史。 |

对象变换在成功帧之后写回，和相机及 Halton 一起推进。GPU 读回回归也检查了一个因无效 mesh 在提交前被拒绝的帧，其对象历史和 Halton 序号保持原值。

## 5. Resolve、残影与稳健性

Resolve 对当前 HDR 的 3×3 邻域计算 Reinhard 压缩后的 YCoCg 一、二阶矩，使用 `mean ± gamma * stddev` 限幅历史，再在压缩空间按运动自适应混合。历史颜色双线性采样，深度和速度点采样。深度拒绝首先核对当前像素、速度表面标记、上一深度的天空或几何分类，再比较上一帧透视除法所得 z/w 与实际 previous depth。移除了把邻近前景速度扩张到当前天空或其他表面的路径。

当前与历史颜色清洗 NaN、Inf 和负值，上限约 65000。解压分母下限与 HDR 上限对齐，避免原有的约 10000 亮度截断。速度或深度非有限、上一帧 UV 越界、上一帧深度不合法或历史权重为零时回退到当前像素。FP16 历史读回检查数值有限性。

Mask 的 alpha 使用与主材质一致的 factor、顶点 alpha、Base Color 采样与 cutoff，GPU 用 0.25 和 0.75 验证 cutoff 0.5 两侧的速度覆盖。Blend 当前只写 HDR 颜色，它会借用后方几何或天空的运动与深度。玻璃边缘和交叠表面仍有历史混合误差。双线性历史色也可能跨越深度边界，单个 nearest depth 无法描述采样足迹。以设备 Z 进行的固定阈值在远处较宽松。相机跳变阈值是按场景尺度的启发式判断。后续如要改善透明物体，可先用本报告的连帧基线量化残影，再评估 reactive mask。

## 6. 修复项与变更文件

| 严重度 | 发现及修复 | 文件 |
| --- | --- | --- |
| High | 省略前后 jitter 差造成静态历史偏移。天空旋转重投影把 jitter 后坐标当作无 jitter。统一为两帧实际抖动 UV。 | `Engine/Core/AetherApplication.{h,cpp}`，`Engine/RHI/Vulkan/VulkanRenderer.{h,cpp}`，`Shaders/taa_velocity*.hlsl`，`Shaders/taa_resolve.frag.hlsl` |
| High | 用跨帧当前深度差来判遮挡会混淆正常进深运动，邻域扩张会把前景运动分配到后景。速度输出增加上一投影深度和表面标记。 | `Engine/RHI/Vulkan/Passes/VulkanTaaPass.{h,cpp}`，`Shaders/taa_velocity*.hlsl`，`Shaders/taa_resolve.frag.hlsl` |
| High | 录制时发布历史和过早更新对象状态，使播放状态依赖 CPU 顺序。现在提交才发布 GPU 历史，成功返回才提交 CPU 时序，显式验证前槽。 | `Engine/Core/AetherApplication.cpp`，`Engine/RHI/Vulkan/VulkanRenderer.cpp`，`Engine/RHI/Vulkan/Passes/VulkanTaaPass.{h,cpp}` |
| Medium | 环境切换、相机跳变、投影变化和旁路未统一失效。补充失效触发条件，并显式覆盖上一帧写入与读取的同步范围。 | `Engine/Core/AetherApplication.{h,cpp}`，`Engine/RHI/Vulkan/Passes/VulkanTaaPass.cpp` |
| Medium | 非有限 history 权重绕开上下界，HDR 解压下限压短高亮范围。默认值现在也经过限制。 | `Engine/Renderer/Passes/TaaPass.cpp`，`Shaders/taa_resolve.frag.hlsl` |
| Low | 以前的空场景时间测试只转换布局，未初始化采样内容。现在先清 HDR 和 depth。 | `Tests/TaaVisual.cpp` |

`Tests/TaaTests.cpp` 与 `Tests/TaaVisual.cpp` 扩展 CPU 和设备回归。`ARCHITECTURE.md` 更新速度 RGBA 布局与描述符约定。记录/提交接口增加了确认步骤，使用方为 VulkanRenderer 和原有独立 GPU 测试。原 Resolve 的绑定编号 0 至 7，Bloom 与 ToneMap 消费历史输出的接口保持兼容。速度图在 RGBA16F 主路径增加 4 字节每像素，在 4K 约增加 31.6 MiB。

## 7. 构建、CPU 和 GPU 回归

使用现有 `E:\Aether\out\build\x64-Debug` Ninja 配置。以下命令从仓库根目录执行，使用已配置的 VS x64 开发者环境（本机工具链 v18.12）。

```powershell
chcp 65001
cmake --build out/build/x64-Debug --parallel 8
ctest --test-dir out/build/x64-Debug -C Debug -R '^taa\.' --output-on-failure --output-log out/build/x64-Debug/taa-audit-targeted.log
ctest --test-dir out/build/x64-Debug -C Debug --output-on-failure --output-log out/build/x64-Debug/taa-audit-ctest.log
```

- **CPU 9 项通过。** 包括 `taa.math` 的 Halton 回绕、投影与 Y 约定、静态/移动几何、相机纯平移/纯旋转、组合运动及天空矩阵结果。CPU 参考算式独立投影当前位置与历史位置，并验证几何速度加抖动差等于实际 UV 差。
- **GPU / WSI 18 项通过。** `taa.gpu` 直接从速度附件读出几何与天空样本，用当前相机射线与立方体前平面求交，并在旧相机中投影结果。UV 误差门限为 `7.5e-5`，预测深度误差门限为 `5e-4`。额外覆盖 Mask、FOV、裁剪平面、相机跳变、手动 reset、提交前中止、环境切换、旁路、对象生成/移除和 Resize。连续提交测试不等待每一帧 GPU 空闲。三轮独立 Pass 创建、失败 Resize 回滚和销毁均使 VMA 分配计数回到基线。
- **RTX 5090 桌面复测通过。** 使用既有 `validation-launch.exe` 隔离 RTSS 钩子，保留 Vulkan SDK 1.4.357.0 Khronos 验证层并开启同步验证。两种普通启动模式各执行 8 次 Resize、3 次最小化恢复和 4 次 HDR 拖放，正常退出。自动化和隔离桌面日志中均无 Vulkan Validation 错误及同步 hazard。RTSS 注入的非隔离桌面仍受之前记录的外部问题影响。[既有验证说明](VulkanValidation.md)

```powershell
cd out/build/x64-Debug
$app = (Resolve-Path bin/Aether.exe).Path
$env:AETHER_TAA_FULL_RES_BENCH = '1'
.\validation-launch.exe taa-audit-desktop-visual.log isolated wait $app '--swapchain-test taa-visual'
.\validation-launch.exe taa-audit-desktop-gpu.log isolated wait $app '--swapchain-test taa-gpu'
.\validation-launch.exe taa-audit-desktop-material.log isolated stress $app '--material-test DamagedHelmet.glb'
.\validation-launch.exe taa-audit-desktop-cube.log isolated stress $app '--single-cube'
```

[构建日志](../out/build/x64-Debug/taa-audit-build.log) · [CTest 摘要](../out/build/x64-Debug/taa-audit-ctest.log) · [CTest 明细](../out/build/x64-Debug/taa-audit-ctest-detail.log) · [桌面 TAA GPU](../out/build/x64-Debug/taa-audit-desktop-gpu.log) · [材质场景](../out/build/x64-Debug/taa-audit-desktop-material.log) · [立方体](../out/build/x64-Debug/taa-audit-desktop-cube.log)

## 8. 连帧视觉结果

静态场景同相位的八帧比较中，开启历史时的相邻帧 RGB RMSE 为 **2.959**（8-bit 通道码值），仅 jitter 时为 **4.766**，约下降 **38%**。以任一通道变化超过 3 计，平均变化像素从 32,581 增至 42,266。4× 放大可看到细斜边的高幅度跳变变弱，同时影响一圈更宽的低幅边缘像素。RMSE 反映该序列的变化强度，无法证明所有场景都稳定。

| 实际 GPU 帧或放大区域 | 图像 |
| --- | --- |
| 静态、两个连续 Halton 相位 | [帧 0](../out/build/x64-Debug/taa-audit-evidence/taa-audit-static-0.png) · [帧 1](../out/build/x64-Debug/taa-audit-evidence/taa-audit-static-1.png) · [4× 细边与仅 jitter 对照](../out/build/x64-Debug/taa-audit-evidence/static-edge-sequence-4x.png) |
| 相机慢移转快移、相机原地旋转 | [平移帧 2](../out/build/x64-Debug/taa-audit-evidence/taa-audit-camera-pan-2.png) · [平移帧 3](../out/build/x64-Debug/taa-audit-evidence/taa-audit-camera-pan-3.png) · [旋转帧 0](../out/build/x64-Debug/taa-audit-evidence/taa-audit-camera-rotation-0.png) · [旋转帧 1](../out/build/x64-Debug/taa-audit-evidence/taa-audit-camera-rotation-1.png) |
| 前景移动与旋转、遮挡变化 | [帧 1](../out/build/x64-Debug/taa-audit-evidence/taa-audit-moving-opaque-1.png) · [帧 2](../out/build/x64-Debug/taa-audit-evidence/taa-audit-moving-opaque-2.png) |
| Blend 前景移动，使用相同相位的仅 jitter 帧作为对照 | [2× 透明交叠区，两帧](../out/build/x64-Debug/taa-audit-evidence/transparent-edge-sequence-2x.png) |
| Sunset 切换及 PBR、Shadows、Bloom、TAA 合成 | [切换首帧](../out/build/x64-Debug/taa-audit-evidence/taa-audit-sunset-after-switch-0.png) · [切换第二帧](../out/build/x64-Debug/taa-audit-evidence/taa-audit-sunset-after-switch-1.png) · [DamagedHelmet](../out/build/x64-Debug/taa-audit-evidence/taa-08-helmet-on.png) |

透明放大对照可见方块与球体交叠处仍有一短段与历史混合形成的轮廓。该结果与透明表面借用后方运动场的实现相符，由现有 clipping 限制强度。

## 9. RTX 5090 GPU 计时

表内单位均为毫秒。计时使用图形队列的 Vulkan timestamp queries，范围为速度 pass、Resolve 与深度拷贝。PBR、Shadows、Bloom 参与完整场景绘制，它们各自的 pass 时间在测量范围外。

| 实际分辨率 | Phase 8 保留基线 | 本次同类测试 | 完整动态几何场景，中位数 | 完整 DamagedHelmet 场景，中位数 |
| --- | ---: | ---: | ---: | ---: |
| 1280×720 | 0.0371 | 0.0311 | 0.0359 | 0.0857 |
| 1920×1080 | 0.0570 | 0.0521 | 0.0706 | 0.1070 |
| 3840×2160 | 0.1918 | 0.2024 | 0.2317 | 0.2762 |

基线和本次同类测试的 1080p 与 4K 都使用空几何列表。本次额外初始化了离屏 HDR 与 depth，确保 Resolve 的输入确定。完整场景数据来自实际 WSI framebuffer，先跑 6 帧并舍弃，再读取 12 个帧槽时间的中位数。4K 使用无边框测试窗口，交换链实际确认 3840×2160。以上为一次桌面运行的时间，GPU 时钟、相邻帧工作量以及测试输入差异均会影响结果。 [桌面 GPU 计时原始日志](../out/build/x64-Debug/taa-audit-desktop-visual.log)

## 10. 剩余限制

- 现有深度匹配仍使用设备 Z 和最近的上一帧深度。远处贴近的表面、薄边、快速高光、亚像素遮挡和双线性历史足迹仍会有残余不稳定。需要进一步收紧时，以本报告的帧序列和深度读回作为对照。
- 透明表面缺少独立速度或响应权重。当前证据只覆盖测试中的 Blend 方块及球体交叠。建议先测试实际内容中的高速透明物体，再决定是否引入额外的透明反馈。
- 相机切换启发式对场景尺度敏感。未来的编辑器或脚本如果执行明确的相机切换，可直接调用现有历史 reset。
- 设备验证覆盖 RTX 5090。本次未实测 D16 深度、RGBA32F 速度回退、不同图形/Present 队列硬件组合以及实际驱动提交失败。现有基础 WSI 的跨驱动回收边界仍以验证文档为准。
