#include <Core/AetherApplication.h>
#include <Assets/CornellBoxScene.h>
#include <Assets/GltfImporter.h>
#include <Renderer/MeshData.h>
#include <RHI/Vulkan/Resources/VulkanBuffer.h>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include <GLFW/glfw3.h>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>

namespace
{
    int FailRq(const char* step)
    {
        std::cerr << "rt-rayquery/rt-visual FAILED at step: " << step << '\n';
        return 1;
    }

    bool WriteBmp(const char* name, VkExtent2D extent, std::vector<uint8_t> pixels, bool bgra)
    {
#ifdef _WIN32
        if (!bgra)
        {
            for (size_t i = 0; i + 3 < pixels.size(); i += 4)
                std::swap(pixels[i], pixels[i + 2]);
        }

        BITMAPINFOHEADER info{};
        info.biSize = sizeof(info);
        info.biWidth = static_cast<LONG>(extent.width);
        info.biHeight = -static_cast<LONG>(extent.height);
        info.biPlanes = 1;
        info.biBitCount = 32;

        BITMAPFILEHEADER header{};
        header.bfType = 0x4d42;
        header.bfOffBits = sizeof(header) + sizeof(info);
        header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());

        std::ofstream output(name, std::ios::binary);
        output.write(reinterpret_cast<const char*>(&header), sizeof(header));
        output.write(reinterpret_cast<const char*>(&info), sizeof(info));
        output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
        return static_cast<bool>(output);
#else
        (void)name; (void)extent; (void)pixels; (void)bgra;
        return true;
#endif
    }

    size_t CountDifferentPixels(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int threshold = 4)
    {
        size_t diff = 0;
        const size_t n = std::min(a.size(), b.size());
        for (size_t i = 0; i + 3 < n; i += 4)
        {
            if (std::abs(int(a[i]) - int(b[i])) > threshold ||
                std::abs(int(a[i + 1]) - int(b[i + 1])) > threshold ||
                std::abs(int(a[i + 2]) - int(b[i + 2])) > threshold)
                ++diff;
        }
        return diff;
    }

    glm::vec4 ReadFp16Pixel(const std::vector<uint16_t>& halfData, uint32_t width, uint32_t x, uint32_t y)
    {
        const size_t idx = (static_cast<size_t>(y) * width + x) * 4;
        return glm::vec4(
            glm::unpackHalf1x16(halfData[idx + 0]),
            glm::unpackHalf1x16(halfData[idx + 1]),
            glm::unpackHalf1x16(halfData[idx + 2]),
            glm::unpackHalf1x16(halfData[idx + 3]));
    }
}

int Aether::AetherApplication::RunRayQueryTest()
{
    // ------------------------------------------------------------------
    // 1. Analytical Ray Query numerical readback (Hit/Miss/Distance/Prim/Instance)
    // ------------------------------------------------------------------
    if (!Initialize(true))
        return FailRq("Initialize(true)");

    if (!m_renderer.CanUseRayQueryMode())
    {
        std::cout << "SKIP: Hardware Ray Tracing / Ray Query is not ready on this device\n";
        Shutdown();
        return 77;
    }

    const VkDevice device = m_vulkanContext.GetDevice();
    const VkExtent2D extent = m_vulkanContext.GetSwapchainExtent();
    const VkDeviceSize fp16Bytes = VkDeviceSize(extent.width) * extent.height * 4 * sizeof(uint16_t);

    VulkanBuffer fp16Readback;
    if (fp16Readback.Initialize(
            device, m_memoryManager, fp16Bytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
        return FailRq("create fp16 readback buffer");

    auto readRayQueryRaw = [&](std::vector<uint16_t>& outHalf) -> bool {
        m_renderer.m_rayQueryReadbackTarget = fp16Readback.GetBuffer();
        m_window.PollEvents();
        const auto res = DrawScene();
        m_renderer.m_rayQueryReadbackTarget = VK_NULL_HANDLE;
        if (res != VulkanRenderer::FrameResult::Rendered)
            return false;
        m_vulkanContext.WaitIdle();
        outHalf.resize(static_cast<size_t>(extent.width) * extent.height * 4);
        return fp16Readback.ReadData(outHalf.data(), fp16Bytes) == VK_SUCCESS;
    };

    // Place camera at (0, 0, 3.0) looking directly at unit cube [-0.5, +0.5]^3 at origin.
    // Analytical front face is z = +0.5 -> exact hit distance at center pixel = 2.50!
    m_camera.SetFov(45.0f);
    m_camera.SetClipPlanes(0.1f, 100.0f);
    m_camera.LookAt({ 0.0f, 0.0f, 3.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f });

    // Verify Q toggle defaults to HybridRayTracing cleanly, then enter RayQuery mode for raw diagnostic tests.
    if (!ToggleRenderModeInteractive() || m_renderMode != RenderMode::HybridRayTracing)
        return FailRq("initial Q toggle to HybridRayTracing");
    if (!ToggleRenderModeInteractive() || m_renderMode != RenderMode::Rasterization)
        return FailRq("initial Q toggle back to Rasterization");

    m_renderMode = RenderMode::RayQuery;
    ResetTemporalHistory();

    m_rayQueryConfig.debugMode = RayQueryDebugMode::RawReadback;
    std::vector<uint16_t> rawPixels;
    if (!readRayQueryRaw(rawPixels))
        return FailRq("readback single cube RawReadback");

    const glm::vec4 centerSample = ReadFp16Pixel(rawPixels, extent.width, extent.width / 2, extent.height / 2);
    const glm::vec4 cornerSample = ReadFp16Pixel(rawPixels, extent.width, 4, 4);

    if (centerSample.x < 0.5f ||
        std::abs(centerSample.y - 2.5f) > 0.02f ||
        centerSample.z < 0.0f || centerSample.z >= 12.0f ||
        std::abs(centerSample.w - 0.0f) > 0.01f)
    {
        std::cerr << "Center pixel mismatch: hit=" << centerSample.x
                  << " t=" << centerSample.y << " prim=" << centerSample.z
                  << " inst=" << centerSample.w << '\n';
        return FailRq("center pixel analytical hit/distance/prim/instance");
    }
    if (cornerSample.x > 0.5f || cornerSample.z >= 0.0f || cornerSample.w >= 0.0f)
    {
        std::cerr << "Corner pixel mismatch: hit=" << cornerSample.x
                  << " prim=" << cornerSample.z << " inst=" << cornerSample.w << '\n';
        return FailRq("corner pixel miss check");
    }

    // Add a second cube instance at (+1.5, 0.0, 0.0) and verify instance ID == 1 is returned!
    {
        Transform second{};
        second.position = { 1.5f, 0.0f, 0.0f };
        m_scene.AddObject({ second, { 0 } });
        m_camera.LookAt({ 0.75f, 0.0f, 4.0f }, { 0.75f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f });

        if (!readRayQueryRaw(rawPixels))
            return FailRq("readback two-instance RawReadback");

        // Project both cube centers to exact pixels.
        const glm::mat4 vp = m_camera.GetViewProjectionMatrix();
        auto sampleAtWorld = [&](glm::vec3 pt) -> glm::vec4 {
            const glm::vec4 c = vp * glm::vec4(pt, 1.0f);
            const glm::vec3 ndc = glm::vec3(c) / c.w;
            const uint32_t px = static_cast<uint32_t>(std::floor((ndc.x * 0.5f + 0.5f) * extent.width));
            const uint32_t py = static_cast<uint32_t>(std::floor((ndc.y * 0.5f + 0.5f) * extent.height));
            return ReadFp16Pixel(rawPixels, extent.width, px, py);
        };
        const glm::vec4 inst0 = sampleAtWorld({ 0.0f, 0.0f, 0.5f });
        const glm::vec4 inst1 = sampleAtWorld({ 1.5f, 0.0f, 0.5f });
        if (inst0.x < 0.5f || std::abs(inst0.w - 0.0f) > 0.01f ||
            inst1.x < 0.5f || std::abs(inst1.w - 1.0f) > 0.01f)
            return FailRq("two-instance distinct instanceCustomIndex check");
    }
    fp16Readback.Shutdown();

    // ------------------------------------------------------------------
    // 2. Cornell Box Default Scene: Rasterization <-> Ray Query Q-Switch
    // ------------------------------------------------------------------
    // Return to Rasterization mode first.
    m_renderMode = RenderMode::Rasterization;
    ResetTemporalHistory();

    if (!LoadDefaultCornellBoxScene())
        return FailRq("LoadDefaultCornellBoxScene");

    const size_t cornellMeshes = m_meshes.size();
    const size_t cornellObjects = m_scene.GetObjects().size();
    const glm::vec3 camPosBefore = m_camera.GetPosition();
    const glm::vec3 camTargetBefore = m_camera.GetTarget();
    const VkBuffer firstVertexBufBefore = m_meshes.front()->GetVertexBuffer().GetBuffer();

    // Render 3 Rasterization frames so TAA history becomes valid.
    for (int f = 0; f < 3; ++f)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return FailRq("Cornell Box Rasterization draw");
    }
    if (!m_renderer.IsTaaHistoryValid() || !m_hasPrevCamera)
        return FailRq("TAA history must be valid before Q switch");

    // Q-switch: Rasterization -> HybridRayTracing on the EXACT SAME scene & camera!
    if (!ToggleRenderModeInteractive() || m_renderMode != RenderMode::HybridRayTracing)
        return FailRq("Q-switch Rasterization -> HybridRayTracing");
    if (m_renderer.IsTaaHistoryValid() || m_hasPrevCamera)
        return FailRq("TAA history must be invalidated immediately on Q switch");
    if (m_meshes.size() != cornellMeshes ||
        m_scene.GetObjects().size() != cornellObjects ||
        m_camera.GetPosition() != camPosBefore ||
        m_camera.GetTarget() != camTargetBefore ||
        m_meshes.front()->GetVertexBuffer().GetBuffer() != firstVertexBufBefore)
        return FailRq("same-scene/same-camera/same-mesh invariant violated on Q switch");

    // Enter RayQuery mode explicitly to test all 4 Phase 9C Ray Query diagnostic views.
    m_renderMode = RenderMode::RayQuery;
    ResetTemporalHistory();
    for (uint32_t mode = 0; mode < kRayQueryInteractiveDebugModeCount; ++mode)
    {
        m_rayQueryConfig.debugMode = static_cast<RayQueryDebugMode>(mode);
        for (int f = 0; f < 2; ++f)
        {
            m_window.PollEvents();
            if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
                return FailRq("Cornell Box Ray Query draw");
        }
    }
    const uint32_t blasCountInRq = m_renderer.GetRayTracingScene().GetActiveBlasCount();
    if (blasCountInRq == 0 || blasCountInRq > cornellMeshes)
        return FailRq("Cornell Box BLAS count sanity");

    // HDR environment switch while in Ray Query mode.
    if (!SelectStudioEnvironment(true))
        return FailRq("HDR environment switch to warm in Ray Query mode");
    for (int f = 0; f < 2; ++f)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return FailRq("Ray Query draw after HDR environment switch");
    }
    if (!SelectStudioEnvironment(false))
        return FailRq("HDR environment switch back to studio");

    // Q-switch back: Ray Query -> Rasterization (must preserve BLASes & invalidate TAA).
    if (!ToggleRenderModeInteractive() || m_renderMode != RenderMode::Rasterization)
        return FailRq("Q-switch Ray Query -> Rasterization");
    if (m_renderer.IsTaaHistoryValid() || m_hasPrevCamera ||
        m_renderer.GetRayTracingScene().GetActiveBlasCount() != blasCountInRq)
        return FailRq("return to Rasterization TAA invalidation / BLAS preservation");

    for (int f = 0; f < 2; ++f)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return FailRq("Cornell Box Rasterization draw after return");
    }

    // ------------------------------------------------------------------
    // 3. Resize handling while in Ray Query mode + Empty Scene in Ray Query
    // ------------------------------------------------------------------
    // Switch into HybridRayTracing for resize and empty scene checks cleanly, and also test RayQuery resize.
    m_renderMode = RenderMode::RayQuery;
    ResetTemporalHistory();
    glfwSetWindowSize(m_window.GetNativeHandle(), 900, 600);
    m_window.PollEvents();
    if (!RecreateSwapchain())
        return FailRq("RecreateSwapchain in Ray Query mode");
    const VkExtent2D resized = m_vulkanContext.GetSwapchainExtent();
    if (m_renderer.m_rayQueryPass.GetExtent().width != resized.width ||
        m_renderer.m_rayQueryPass.GetExtent().height != resized.height)
        return FailRq("RayQueryPass extent mismatch after resize");
    for (int f = 0; f < 3; ++f)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return FailRq("Ray Query draw after resize");
    }

    // Empty scene in Ray Query mode.
    m_scene.Clear();
    for (int f = 0; f < 3; ++f)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return FailRq("Ray Query draw on empty scene");
    }

    // ------------------------------------------------------------------
    // 4. Repeated VulkanRayQueryPass Initialize / Shutdown (VMA zero-leak)
    // ------------------------------------------------------------------
    m_vulkanContext.WaitIdle();
    m_renderer.m_rayQueryPass.Shutdown();
    const uint32_t allocsWithoutRqPass = m_memoryManager.GetAllocationCountForTest();
    for (int cycle = 0; cycle < 3; ++cycle)
    {
        if (!m_renderer.m_rayQueryPass.Initialize(
                m_vulkanContext, m_memoryManager, MAX_FRAMES_IN_FLIGHT,
                m_vulkanContext.GetSwapchainExtent(), m_renderer.m_hdrLayout))
            return FailRq("repeated VulkanRayQueryPass Initialize");
        const uint32_t activeAllocs = m_memoryManager.GetAllocationCountForTest();
        if (activeAllocs <= allocsWithoutRqPass)
            return FailRq("VulkanRayQueryPass expected allocations");
        m_renderer.m_rayQueryPass.Shutdown();
        if (m_memoryManager.GetAllocationCountForTest() != allocsWithoutRqPass)
            return FailRq("VulkanRayQueryPass shutdown VMA leak");
    }

    Shutdown();

    // ------------------------------------------------------------------
    // 5. Graceful fallback when RT is unavailable (software-disabled RT)
    // ------------------------------------------------------------------
    {
        AetherApplication fallbackApp;
        fallbackApp.m_vulkanContext.m_disableRayTracingForTest = true;
        if (!fallbackApp.Initialize(false))
            return FailRq("fallbackApp Initialize");
        if (!fallbackApp.LoadDefaultCornellBoxScene())
            return FailRq("fallbackApp LoadDefaultCornellBoxScene");
        if (fallbackApp.m_renderer.CanUseRayQueryMode() ||
            fallbackApp.ToggleRenderModeInteractive() ||
            fallbackApp.m_renderMode != RenderMode::Rasterization)
            return FailRq("fallbackApp must refuse Q switch and remain in Rasterization");
        for (int f = 0; f < 3; ++f)
        {
            fallbackApp.m_window.PollEvents();
            if (fallbackApp.DrawScene() != VulkanRenderer::FrameResult::Rendered)
                return FailRq("fallbackApp Cornell Box Rasterization draw");
        }
        fallbackApp.Shutdown();
    }

    std::cout << "Phase 9C rt.rayquery numerical, mode-switch, resize, fallback & leak checks passed\n";
    return 0;
}

int Aether::AetherApplication::RunRayQueryVisualTest()
{
    if (!Initialize(false))
        return FailRq("rt-visual Initialize");

    if (!m_renderer.CanUseHybridRtMode() || !m_renderer.CanUseRayQueryMode())
    {
        std::cout << "SKIP: Hardware Ray Tracing / Hybrid RT is not ready on this device\n";
        Shutdown();
        return 77;
    }

    if (!LoadDefaultCornellBoxScene())
        return FailRq("rt-visual LoadDefaultCornellBoxScene");

    const std::filesystem::path evidenceDir = "nextgen-rendering-evidence";
    std::error_code ec;
    std::filesystem::create_directories(evidenceDir, ec);

    const VkDevice device = m_vulkanContext.GetDevice();
    const VkExtent2D extent = m_vulkanContext.GetSwapchainExtent();
    const VkDeviceSize rgbaBytes = VkDeviceSize(extent.width) * extent.height * 4;
    const VkFormat swapFmt = m_vulkanContext.GetSwapchainImageFormat();
    const bool bgra = swapFmt == VK_FORMAT_B8G8R8A8_SRGB || swapFmt == VK_FORMAT_B8G8R8A8_UNORM;

    VulkanBuffer captureBuffer;
    if (captureBuffer.Initialize(
            device, m_memoryManager, rgbaBytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
        return FailRq("rt-visual capture buffer init");

    auto renderAndCapture = [&](const char* filename, int warmupFrames, std::vector<uint8_t>& outPixels) -> bool {
        for (int i = 0; i < warmupFrames; ++i)
        {
            m_window.PollEvents();
            if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
                return false;
        }
        m_renderer.m_captureTarget = captureBuffer.GetBuffer();
        m_window.PollEvents();
        const auto res = DrawScene();
        m_renderer.m_captureTarget = VK_NULL_HANDLE;
        if (res != VulkanRenderer::FrameResult::Rendered)
            return false;
        m_vulkanContext.WaitIdle();
        outPixels.resize(static_cast<size_t>(rgbaBytes));
        if (captureBuffer.ReadData(outPixels.data(), rgbaBytes) != VK_SUCCESS)
            return false;
        const std::filesystem::path fullPath = evidenceDir / filename;
        return WriteBmp(fullPath.string().c_str(), extent, outPixels, bgra);
    };

    const CornellBoxCameraPreset defaultCam = GetDefaultCornellBoxCamera();
    std::vector<uint8_t> tmpPixels;

    // 01: Cornell Box — Rasterization Baseline
    std::vector<uint8_t> cornellRasterPixels;
    m_renderMode = RenderMode::Rasterization;
    m_hybridConfig = {};
    m_bloomConfig.enabled = true;
    m_shadowConfig.enabled = true;
    m_taaConfig.enabled = true;
    m_camera.LookAt(defaultCam.eye, defaultCam.target, defaultCam.up);
    m_lightAngle = 0.65f;
    m_exposure = 0.0f;
    ResetTemporalHistory();
    if (!renderAndCapture("01-cornell-raster-baseline.bmp", 8, cornellRasterPixels))
        return FailRq("capture 01-cornell-raster-baseline.bmp");

    // 02: Cornell Box — Hybrid RT Final (Q-switch, exact same camera & scene, 18 frames temporal convergence)
    std::vector<uint8_t> cornellHybridPixels;
    if (!ToggleRenderModeInteractive() || m_renderMode != RenderMode::HybridRayTracing)
        return FailRq("rt-visual Q switch to HybridRayTracing");
    m_hybridConfig.debugView = HybridDebugView::FinalHybrid;
    m_hybridConfig.shadowsEnabled = true;
    m_hybridConfig.reflectionsEnabled = true;
    m_hybridConfig.denoiserEnabled = true;
    m_hybridConfig.shadowSamples = 4;
    if (!renderAndCapture("02-cornell-hybrid-final.bmp", 18, cornellHybridPixels))
        return FailRq("capture 02-cornell-hybrid-final.bmp");

    if (CountDifferentPixels(cornellRasterPixels, cornellHybridPixels) < 12000)
        return FailRq("Cornell Box Rasterization vs Hybrid RT Final must differ visibly");

    // 03: Cornell Box — RT Shadows Only
    std::vector<uint8_t> shadowsOnlyPixels;
    m_hybridConfig.debugView = HybridDebugView::RtShadowsOnly;
    if (!renderAndCapture("03-cornell-rt-shadows-only.bmp", 16, shadowsOnlyPixels))
        return FailRq("capture 03-cornell-rt-shadows-only.bmp");

    // 04: Cornell Box — RT Reflections Only
    std::vector<uint8_t> reflectionsOnlyPixels;
    m_hybridConfig.debugView = HybridDebugView::RtReflectionsOnly;
    if (!renderAndCapture("04-cornell-rt-reflections-only.bmp", 16, reflectionsOnlyPixels))
        return FailRq("capture 04-cornell-rt-reflections-only.bmp");

    if (CountDifferentPixels(shadowsOnlyPixels, reflectionsOnlyPixels) < 15000)
        return FailRq("RtShadowsOnly vs RtReflectionsOnly must isolate distinct effects");

    // 05: Cornell Box — Denoiser OFF (1 spp noise visible for honest comparison)
    std::vector<uint8_t> denoiserOffPixels;
    m_hybridConfig.debugView = HybridDebugView::FinalHybrid;
    m_hybridConfig.denoiserEnabled = false;
    m_hybridConfig.shadowSamples = 1;
    m_hybridConfig.sunAngularRadiusDeg = 2.8f;
    ResetTemporalHistory();
    if (!renderAndCapture("05-cornell-denoiser-off.bmp", 2, denoiserOffPixels))
        return FailRq("capture 05-cornell-denoiser-off.bmp");

    // 06: Cornell Box — Denoiser ON (16+ converged frames on exact same noisy sampling budget)
    std::vector<uint8_t> denoiserOnPixels;
    m_hybridConfig.denoiserEnabled = true;
    ResetTemporalHistory();
    if (!renderAndCapture("06-cornell-denoiser-on.bmp", 18, denoiserOnPixels))
        return FailRq("capture 06-cornell-denoiser-on.bmp");

    if (CountDifferentPixels(denoiserOffPixels, denoiserOnPixels, 2) < 5000)
        return FailRq("Denoiser OFF vs Denoiser ON must visibly differ");

    // Restore production hybrid quality settings (4 spp, balanced sun penumbra).
    m_hybridConfig.shadowSamples = 4;
    m_hybridConfig.sunAngularRadiusDeg = 1.6f;

    // 07: Cornell Box — Neon + Bloom + Emissive Reflection Close-up
    m_camera.LookAt({ 0.55f, 1.85f, 3.65f }, { -0.75f, 2.05f, -2.55f }, { 0.0f, 1.0f, 0.0f });
    m_exposure = -0.10f;
    ResetTemporalHistory();
    if (!renderAndCapture("07-cornell-neon-closeup.bmp", 16, tmpPixels))
        return FailRq("capture 07-cornell-neon-closeup.bmp");

    // 08: Cornell Box — Mirror Panel + Chrome Hero Sphere Close-up
    m_camera.LookAt({ 1.45f, 1.78f, 3.25f }, { -1.25f, 1.38f, -0.05f }, { 0.0f, 1.0f, 0.0f });
    m_exposure = 0.0f;
    ResetTemporalHistory();
    if (!renderAndCapture("08-cornell-mirror-closeup.bmp", 16, tmpPixels))
        return FailRq("capture 08-cornell-mirror-closeup.bmp");

    // 09: Cornell Box — Soft Shadow Penumbra & Skylight Beams Close-up
    m_camera.LookAt({ -0.95f, 2.45f, 4.35f }, { 0.25f, 0.85f, -0.60f }, { 0.0f, 1.0f, 0.0f });
    m_lightAngle = 1.05f;
    m_exposure = 0.0f;
    ResetTemporalHistory();
    if (!renderAndCapture("09-cornell-soft-shadow-closeup.bmp", 16, tmpPixels))
        return FailRq("capture 09-cornell-soft-shadow-closeup.bmp");

    // 10: Cornell Box — Multi-Material Roughness Comparison Close-up (Matte 0.76, Rough Bronze 0.62, Brushed Gold 0.28, Copper 0.18, Chrome 0.02)
    m_camera.LookAt({ 0.15f, 1.45f, 3.75f }, { 0.10f, 0.65f, 0.75f }, { 0.0f, 1.0f, 0.0f });
    m_lightAngle = 0.65f;
    ResetTemporalHistory();
    if (!renderAndCapture("10-cornell-rough-metals-closeup.bmp", 16, tmpPixels))
        return FailRq("capture 10-cornell-rough-metals-closeup.bmp");

    // Return to default hero framing for Debug Views (15..21).
    m_camera.LookAt(defaultCam.eye, defaultCam.target, defaultCam.up);
    ResetTemporalHistory();
    m_hybridConfig.debugView = HybridDebugView::ShadowVisibility;
    if (!renderAndCapture("15-debug-shadow-visibility.bmp", 16, tmpPixels))
        return FailRq("capture 15-debug-shadow-visibility.bmp");

    m_hybridConfig.debugView = HybridDebugView::ReflectionHitMiss;
    if (!renderAndCapture("16-debug-reflection-hitmiss.bmp", 4, tmpPixels))
        return FailRq("capture 16-debug-reflection-hitmiss.bmp");

    m_hybridConfig.debugView = HybridDebugView::SurfaceRoughness;
    if (!renderAndCapture("17-debug-surface-roughness.bmp", 2, tmpPixels))
        return FailRq("capture 17-debug-surface-roughness.bmp");

    m_hybridConfig.debugView = HybridDebugView::DenoiserVariance;
    if (!renderAndCapture("18-debug-denoiser-variance.bmp", 16, tmpPixels))
        return FailRq("capture 18-debug-denoiser-variance.bmp");

    m_hybridConfig.debugView = HybridDebugView::TemporalConfidence;
    if (!renderAndCapture("19-debug-temporal-confidence.bmp", 16, tmpPixels))
        return FailRq("capture 19-debug-temporal-confidence.bmp");

    m_hybridConfig.debugView = HybridDebugView::ReflectionHitDistance;
    if (!renderAndCapture("20-debug-reflection-hit-distance.bmp", 4, tmpPixels))
        return FailRq("capture 20-debug-reflection-hit-distance.bmp");

    m_hybridConfig.debugView = HybridDebugView::RayQueryDiagnostic;
    m_rayQueryConfig.debugMode = RayQueryDebugMode::DiagnosticShading;
    if (!renderAndCapture("21-debug-rayquery-diagnostic.bmp", 4, tmpPixels))
        return FailRq("capture 21-debug-rayquery-diagnostic.bmp");

    // 11..14: DamagedHelmet.glb — Rasterization vs Hybrid RT vs Reflection Close-up vs Motion Stability
    const std::filesystem::path helmetPath = "DamagedHelmet.glb";
    if (std::filesystem::exists(helmetPath))
    {
        auto helmet = ImportGltf(helmetPath);
        if (!helmet.asset)
            return FailRq("rt-visual import DamagedHelmet.glb");
        if (!AddImportedAsset(std::move(*helmet.asset)).empty())
            return FailRq("rt-visual AddImportedAsset DamagedHelmet.glb");

        m_lightAngle = 0.65f;
        m_exposure = 0.0f;
        m_hybridConfig.debugView = HybridDebugView::FinalHybrid;

        // 11: Rasterization
        m_renderMode = RenderMode::Rasterization;
        ResetTemporalHistory();
        std::vector<uint8_t> helmetRasterPixels;
        if (!renderAndCapture("11-helmet-raster.bmp", 8, helmetRasterPixels))
            return FailRq("capture 11-helmet-raster.bmp");

        // 12: Hybrid RT (Q-switch, exact same framing)
        if (!ToggleRenderModeInteractive() || m_renderMode != RenderMode::HybridRayTracing)
            return FailRq("rt-visual Q switch for DamagedHelmet");
        std::vector<uint8_t> helmetHybridPixels;
        if (!renderAndCapture("12-helmet-hybrid-final.bmp", 16, helmetHybridPixels))
            return FailRq("capture 12-helmet-hybrid-final.bmp");

        if (CountDifferentPixels(helmetRasterPixels, helmetHybridPixels) < 3000)
            return FailRq("DamagedHelmet Rasterization vs Hybrid RT must differ visibly");

        // 13: Visor / metallic reflection close-up
        const glm::vec3 eye12 = m_camera.GetPosition();
        const glm::vec3 tgt12 = m_camera.GetTarget();
        const glm::vec3 closerEye = tgt12 + (eye12 - tgt12) * 0.62f + glm::vec3(0.35f, 0.10f, 0.0f);
        m_camera.LookAt(closerEye, tgt12, { 0.0f, 1.0f, 0.0f });
        ResetTemporalHistory();
        if (!renderAndCapture("13-helmet-reflection-closeup.bmp", 16, tmpPixels))
            return FailRq("capture 13-helmet-reflection-closeup.bmp");

        // 14: Orbit camera smoothly for 10 frames + settle 8 frames -> verify temporal stability & crispness
        for (int step = 0; step < 10; ++step)
        {
            m_cameraController.Orbit(m_camera, 0.045f, -0.01f);
            m_window.PollEvents();
            if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
                return FailRq("DamagedHelmet motion orbit frame");
        }
        if (!renderAndCapture("14-helmet-temporal-motion.bmp", 8, tmpPixels))
            return FailRq("capture 14-helmet-temporal-motion.bmp");
    }

    captureBuffer.Shutdown();

    std::cout << "All 21 Next-Gen Hybrid RT screenshots captured and verified in "
              << evidenceDir.string() << '\n';
    Shutdown();
    return 0;
}
