# RTX 5090 桌面 Vulkan 验证

使用 Windows 11、NVIDIA GeForce RTX 5090 和 Vulkan SDK 1.4.357.0。原始报告来自 `ibl-prephase-desktop.log` 和 `skybox-helmet.log`。CTest 的 `Testing/Temporary/LastTest.log` 记录了同一 GPU 的自动化运行。下文的日志都位于 `out/build/x64-Debug`。

| 报告 | 根因与代码位置 |
| --- | --- |
| `VUID-VkSwapchainCreateInfoKHR-imageFormat-01778` | `VulkanContext::BuildSwapchain` 为 FIFO 交换链申请颜色附件和 Surface 支持的传输读回。LunarG API Dump 记录到隔离 RTSS 后的 `imageUsage=17`，以及 RTSS 注入时的 `imageUsage=25`。Surface 的 `supportedUsageFlags=159` 包含 Storage，但 RTX 5090 的 `VK_FORMAT_B8G8R8A8_SRGB` 缺少 Storage Image 格式特性。验证回调的调用栈显示 `RTSSVkLayer64.dll` 和 `RTSSHooks64.dll` 转交了增加 `STORAGE_BIT` 的创建请求。HDR 目标是独立的 FP16 图像，使用颜色附件和采样。 |
| `VUID-VkImageViewCreateInfo-usage-02275` | `VulkanImage::InitializeSwapchainView` 用交换链实际格式创建视图，视图继承图像用途。RTSS 改写后，Aether 和 RTSS 创建的 sRGB 视图都继承了不兼容的 Storage 用途。调用栈记录了两组创建者。普通 `VulkanImage` 按其调用方指定的用途创建，销毁时先释放视图，再销毁自有图像并释放内存。交换链图像由 WSI 持有。 |
| `SYNC-HAZARD-PRESENT-AFTER-WRITE` | 这是同步验证的 hazard ID。RTSS 注入的 API Dump 记录了 `vkCmdEndRenderPass` 和随后一次没有信号量 signal 的 `vkQueueSubmit`，Present 仍等待 Aether 原先的完成信号量。Present 验证调用栈经过 `RTSSHooks64.dll`。Aether 的 `VulkanRenderer::DrawFrame` 使用动态渲染和显式 `PRESENT_SRC_KHR` 屏障。隔离 RTSS 后，相同渲染与提交路径通过同步验证。 |

交换链的 Surface 用途限制、实际格式支持和视图用途是独立条件。[Swapchain 规范](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainCreateInfoKHR.html) · [ImageView 规范](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageViewCreateInfo.html) · [ImageView 用途覆盖规则](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageViewUsageCreateInfo.html)

Acquire 信号量、命令缓冲区和 fence 按帧槽复用。应用先等待该槽的 fence，取得图像后才在提交前重置 fence。Present 等待的信号量按交换链图像索引选取，在重新 Acquire 该图像并等待其 Acquire 信号后才能再次 signal。`VK_ERROR_OUT_OF_DATE_KHR` 的 Acquire 不提交，`VK_SUBOPTIMAL_KHR` 消费已完成的 Acquire 再请求重建。常规帧间同步使用 fence、信号量与屏障。交换链重建和关停走现有的设备空闲路径。基础 WSI 的设备空闲等待对 Present 资源回收缺少显式 Present fence 所提供的规范保证，这仍是跨驱动的边界。[Khronos 信号量复用指南](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)

## 复现与隔离

用 VS x64 Developer PowerShell，在 Debug 构建目录编译启动器。头盔读回场景使用当前目录的 `DamagedHelmet.glb` 和 `skybox-external.hdr`。

```powershell
cd E:\Aether\out\build\x64-Debug
cl /nologo /std:c++20 /EHsc /W4 /Fe:validation-launch.exe ..\..\..\Tests\WindowsDesktopValidation.cpp user32.lib
$app = (Resolve-Path bin/Aether.exe).Path
.\validation-launch.exe validation-desktop-baseline.log normal wait $app '--swapchain-test helmet-preview'
.\validation-launch.exe validation-loader-only.log loader-only wait $app '--swapchain-test helmet-preview'
.\validation-launch.exe validation-desktop-helmet.log isolated wait $app '--swapchain-test helmet-preview'
.\validation-launch.exe validation-desktop-cube.log isolated stress $app '--single-cube'
.\validation-launch.exe validation-desktop-material.log isolated stress $app '--material-test DamagedHelmet.glb'
```

三种启动均启用同步验证。`normal` 保留桌面注入。`loader-only` 只为子进程设置 RTSS 自带的 `DISABLE_RTSS_LAYER=1`，可复现三类报告。`isolated` 同时使用 Windows 的按进程扩展点策略隔离全局钩子。桌面 A/B 同一可执行文件在此模式下运行，Khronos 验证层仍已加载，三类报告均消失。操作系统策略的作用范围是启动器创建的子进程。使用 `interactive` 可在这个模式下手动操作窗口，直至正常关闭。[Windows 进程创建策略](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute)

普通桌面启动仍受本机 RTSS 配置影响。应用级 RTSS 配置写入未能验证，测试使用了进程级隔离。对常用启动入口，可在 RTSS 中为 `Aether.exe` 设置单独的 *Application detection level: None*，然后按上面的普通启动命令确认结果。

## 本次证据

- `validation-desktop-trace.log` 保存临时验证回调输出。可按 `Validation stack for` 查找 RTSS 模块和调用地址。正式构建已移除调用栈插桩。
- `validation-api-run.log` 和 `validation-api-rtss.log` 保存隔离与注入 RTSS 的 Vulkan API Dump，可对照 Surface 能力、交换链用途和 Present 前的提交。
- `validation-full-build.log` 和 `validation-ctest.log` 记录 CMake/Ninja Debug 全量重建和 17/17 通过的 CTest。
- `validation-desktop-{baseline,helmet,cube,material}.log` 保存完整桌面输出。两个普通启动模式各完成 8 次 Resize、3 次 Minimize/Restore、4 次 HDR 拖放和正常关闭。对应的 `*-actions.log` 保存窗口操作记录。
- `validation-frame-hashes.log` 记录 Studio 与外部 HDR 的头盔帧。这两张图与清理前的 PNG 逐字节一致。
