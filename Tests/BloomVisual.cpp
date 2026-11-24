#include <Core/AetherApplication.h>
#include <Assets/GltfImporter.h>
#include <Renderer/MeshData.h>
#include <RHI/Vulkan/Passes/VulkanBloomPass.h>
#include <RHI/Vulkan/Resources/VulkanBuffer.h>
#include <RHI/Vulkan/Resources/VulkanImage.h>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace
{
    bool WriteBitmap(const char* name, VkExtent2D extent, std::vector<uint8_t> pixels, bool bgra)
    {
#ifdef _WIN32
        if (!bgra)
            for (size_t i = 0; i < pixels.size(); i += 4)
                std::swap(pixels[i], pixels[i + 2]);

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

    size_t ChangedPixels(const std::vector<uint8_t>& first, const std::vector<uint8_t>& second, int threshold = 3)
    {
        size_t changed = 0;
        const size_t count = std::min(first.size(), second.size());
        for (size_t i = 0; i + 3 < count; i += 4)
        {
            if (std::abs(int(first[i]) - int(second[i])) > threshold ||
                std::abs(int(first[i + 1]) - int(second[i + 1])) > threshold ||
                std::abs(int(first[i + 2]) - int(second[i + 2])) > threshold)
                ++changed;
        }
        return changed;
    }

    Aether::MeshData MakeGroundPlane(float halfExtent)
    {
        Aether::MeshData mesh;
        const glm::vec3 n(0.0f, 1.0f, 0.0f);
        const glm::vec4 t(1.0f, 0.0f, 0.0f, 1.0f);
        const glm::vec3 c(1.0f);
        mesh.vertices = {
            { { -halfExtent, 0.0f,  halfExtent }, c, n, { 0.0f, 0.0f }, t },
            { {  halfExtent, 0.0f,  halfExtent }, c, n, { 1.0f, 0.0f }, t },
            { {  halfExtent, 0.0f, -halfExtent }, c, n, { 1.0f, 1.0f }, t },
            { { -halfExtent, 0.0f, -halfExtent }, c, n, { 0.0f, 1.0f }, t }
        };
        mesh.indices = { 0, 1, 2, 2, 3, 0 };
        return mesh;
    }

    Aether::MeshData MakeSphereMesh(float radius)
    {
        Aether::MeshData sphere;
        constexpr unsigned rings = 24, segments = 48;
        for (unsigned j = 0; j <= rings; ++j)
        {
            for (unsigned i = 0; i <= segments; ++i)
            {
                const float polar = j * 3.14159265f / rings;
                const float azimuth = i * 6.2831853f / segments;
                const glm::vec3 normal(
                    std::sin(polar) * std::cos(azimuth),
                    std::cos(polar),
                    std::sin(polar) * std::sin(azimuth));
                Aether::Vertex v{};
                v.position = normal * radius;
                v.color = glm::vec3(1.0f);
                v.normal = normal;
                v.uv = { static_cast<float>(i) / segments, static_cast<float>(j) / rings };
                v.tangent = glm::vec4(glm::normalize(glm::vec3(-std::sin(azimuth), 0.0f, std::cos(azimuth))), 1.0f);
                sphere.vertices.push_back(v);
                if (j < rings && i < segments)
                {
                    const unsigned a = j * (segments + 1) + i;
                    const unsigned b = a + segments + 1;
                    sphere.indices.insert(sphere.indices.end(), { a, a + 1, b, a + 1, b + 1, b });
                }
            }
        }
        return sphere;
    }

    // Dedicated HDR Bloom + PBR + Shadow showcase scene with bright emissive light sources
    // and metallic highlights over a dark-matte floor so bloom glow and shadows are unmistakable.
    Aether::ImportedAsset BuildBloomTestScene()
    {
        using namespace Aether;
        ImportedAsset asset;

        const auto addMeshNode = [&](MeshData geometry, const ImportedMaterial& mat, const glm::mat4& world) {
            const size_t matIdx = asset.materials.size();
            asset.materials.push_back(mat);
            const size_t meshIdx = asset.meshes.size();
            asset.meshes.push_back({ ImportedPrimitive{ std::move(geometry), matIdx, {} } });
            ImportedNode node;
            node.mesh = meshIdx;
            node.local = node.world = world;
            asset.roots.push_back(asset.nodes.size());
            asset.nodes.push_back(std::move(node));
        };

        // Dark charcoal matte floor (keeps non-emissive areas clean & shows cast shadows).
        ImportedMaterial floorMat;
        floorMat.baseColor = { 0.18f, 0.19f, 0.22f, 1.0f };
        floorMat.metallic = 0.05f;
        floorMat.roughness = 0.78f;
        addMeshNode(MakeGroundPlane(8.0f), floorMat, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.0f, 0.0f)));

        // Polished chrome sphere (bright specular highlight + environment reflections).
        ImportedMaterial chromeMat;
        chromeMat.baseColor = { 0.95f, 0.96f, 0.98f, 1.0f };
        chromeMat.metallic = 1.0f;
        chromeMat.roughness = 0.12f;
        addMeshNode(MakeSphereMesh(0.85f), chromeMat, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.15f, 0.2f)));

        // Matte shadow-casting monolith behind/right of sphere.
        ImportedMaterial stoneMat;
        stoneMat.baseColor = { 0.30f, 0.32f, 0.36f, 1.0f };
        stoneMat.metallic = 0.0f;
        stoneMat.roughness = 0.85f;
        const glm::mat4 monolith =
            glm::translate(glm::mat4(1.0f), glm::vec3(1.9f, 0.2f, -0.4f)) *
            glm::scale(glm::mat4(1.0f), glm::vec3(0.75f, 2.4f, 0.75f));
        addMeshNode(CreateCubeMesh(), stoneMat, monolith);

        // Emissive warm amber neon bar (left).
        ImportedMaterial amberNeon;
        amberNeon.baseColor = { 0.2f, 0.1f, 0.05f, 1.0f };
        amberNeon.emissive = { 7.5f, 2.2f, 0.35f };
        amberNeon.metallic = 0.0f;
        amberNeon.roughness = 0.4f;
        const glm::mat4 leftNeon =
            glm::translate(glm::mat4(1.0f), glm::vec3(-1.95f, 0.15f, 0.15f)) *
            glm::scale(glm::mat4(1.0f), glm::vec3(0.32f, 2.3f, 0.32f));
        addMeshNode(CreateCubeMesh(), amberNeon, leftNeon);

        // Emissive cyan neon core (back-left).
        ImportedMaterial cyanNeon;
        cyanNeon.baseColor = { 0.05f, 0.15f, 0.2f, 1.0f };
        cyanNeon.emissive = { 0.4f, 4.8f, 8.5f };
        cyanNeon.metallic = 0.0f;
        cyanNeon.roughness = 0.3f;
        addMeshNode(MakeSphereMesh(0.42f), cyanNeon, glm::translate(glm::mat4(1.0f), glm::vec3(-0.95f, 0.65f, -1.45f)));

        // Emissive magenta beacon strip mounted on top of the monolith.
        ImportedMaterial magentaNeon;
        magentaNeon.baseColor = { 0.2f, 0.05f, 0.15f, 1.0f };
        magentaNeon.emissive = { 6.8f, 0.6f, 4.2f };
        magentaNeon.metallic = 0.0f;
        magentaNeon.roughness = 0.3f;
        const glm::mat4 topStrip =
            glm::translate(glm::mat4(1.0f), glm::vec3(1.9f, 1.52f, -0.4f)) *
            glm::scale(glm::mat4(1.0f), glm::vec3(0.8f, 0.22f, 0.8f));
        addMeshNode(CreateCubeMesh(), magentaNeon, topStrip);

        return asset;
    }

    float Luminance(const glm::vec4& c)
    {
        return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
    }
}

int Aether::AetherApplication::RunBloomGpuTest()
{
    if (!Initialize(false))
        return 1;

    const auto device = m_vulkanContext.GetDevice();
    const VkExtent2D synthExtent{ 128, 128 };
    const size_t pixelCount = static_cast<size_t>(synthExtent.width) * synthExtent.height;
    const VkDeviceSize byteSize = pixelCount * 4 * sizeof(uint16_t);

    // 1. Deterministic Synthetic FP16 HDR Test:
    //    - Dark background (0.05)
    //    - Small bright HDR patch (2.5) around (32, 32)
    //    - Medium bright patch (1.3) around (96, 32)
    //    - High-intensity HDR patch (12.0, 6.0, 2.0) around (64, 96)
    {
        std::vector<uint64_t> srcHalf(pixelCount);
        for (uint32_t y = 0; y < synthExtent.height; ++y)
        {
            for (uint32_t x = 0; x < synthExtent.width; ++x)
            {
                glm::vec4 color(0.05f, 0.05f, 0.05f, 1.0f);
                if (x >= 28 && x < 36 && y >= 28 && y < 36)
                    color = glm::vec4(2.5f, 2.5f, 2.5f, 1.0f);
                else if (x >= 92 && x < 100 && y >= 28 && y < 36)
                    color = glm::vec4(1.3f, 1.3f, 1.3f, 1.0f);
                else if (x >= 58 && x < 70 && y >= 90 && y < 102)
                    color = glm::vec4(12.0f, 6.0f, 2.0f, 1.0f);
                srcHalf[static_cast<size_t>(y) * synthExtent.width + x] = glm::packHalf4x16(color);
            }
        }

        VulkanImage syntheticHdr;
        if (syntheticHdr.Initialize(
                device, m_memoryManager, synthExtent, VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS)
            return 1;

        VulkanBuffer stagingUp, stagingDown;
        if (stagingUp.Initialize(device, m_memoryManager, byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS ||
            stagingUp.UploadData(srcHalf.data(), byteSize) != VK_SUCCESS ||
            stagingDown.Initialize(device, m_memoryManager, byteSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != VK_SUCCESS)
            return 1;

        VulkanBloomPass bloom;
        if (!bloom.Initialize(
                m_vulkanContext, m_memoryManager, 1, synthExtent,
                syntheticHdr.GetView(), m_renderer.m_hdrLayout))
            return 1;

        // Upload synthetic HDR image once to SHADER_READ_ONLY_OPTIMAL.
        const auto uploadStatus = m_renderer.EnvironmentCommands([&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier2 toDst{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            toDst.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            toDst.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toDst.image = syntheticHdr.GetImage();
            toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &toDst;
            vkCmdPipelineBarrier2(cmd, &dep);

            VkBufferImageCopy region{};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { synthExtent.width, synthExtent.height, 1 };
            vkCmdCopyBufferToImage(cmd, stagingUp.GetBuffer(), syntheticHdr.GetImage(),
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            VkImageMemoryBarrier2 toRead = toDst;
            toRead.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            toRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            toRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            toRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            dep.pImageMemoryBarriers = &toRead;
            vkCmdPipelineBarrier2(cmd, &dep);
        });
        if (uploadStatus != VK_SUCCESS)
            return 1;

        auto runAndReadback = [&](const BloomConfig& cfg, bool readReconstructed, std::vector<glm::vec4>& outPixels) {
            const auto& targetImg = readReconstructed ? bloom.GetReconstructedImage(0) : bloom.GetCompositeImage(0);
            const auto st = m_renderer.EnvironmentCommands([&](VkCommandBuffer cmd) {
                bloom.Execute(cmd, 0, cfg);

                VkImageMemoryBarrier2 toSrc{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
                toSrc.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                toSrc.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                toSrc.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                toSrc.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                toSrc.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                toSrc.srcQueueFamilyIndex = toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toSrc.image = targetImg.GetImage();
                toSrc.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers = &toSrc;
                vkCmdPipelineBarrier2(cmd, &dep);

                VkBufferImageCopy region{};
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.imageExtent = { synthExtent.width, synthExtent.height, 1 };
                vkCmdCopyImageToBuffer(cmd, targetImg.GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    stagingDown.GetBuffer(), 1, &region);

                VkImageMemoryBarrier2 back = toSrc;
                back.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                back.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                back.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                back.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                back.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                dep.pImageMemoryBarriers = &back;
                vkCmdPipelineBarrier2(cmd, &dep);
            });
            if (st != VK_SUCCESS)
                return false;

            std::vector<uint64_t> raw(pixelCount);
            if (stagingDown.ReadData(raw.data(), byteSize) != VK_SUCCESS)
                return false;
            outPixels.resize(pixelCount);
            for (size_t i = 0; i < pixelCount; ++i)
            {
                outPixels[i] = glm::unpackHalf4x16(raw[i]);
                if (!std::isfinite(outPixels[i].r) || !std::isfinite(outPixels[i].g) || !std::isfinite(outPixels[i].b))
                    return false;
            }
            return true;
        };

        auto at = [&](const std::vector<glm::vec4>& img, uint32_t x, uint32_t y) {
            return img[static_cast<size_t>(y) * synthExtent.width + x];
        };

        BloomConfig baseCfg{};
        baseCfg.threshold = 0.8f;
        baseCfg.softKnee = 0.5f;
        baseCfg.intensity = 0.5f;
        baseCfg.pyramidLevels = 4;

        std::vector<glm::vec4> reconBase, compLowInt, compHighInt, reconHighThresh;
        if (!runAndReadback(baseCfg, true, reconBase))
            return 1;

        // Assert 1: Dark corner (4, 4) produces near-zero bloom.
        if (Luminance(at(reconBase, 4, 4)) > 0.02f)
            return 1;
        // Assert 2: High HDR patch (64, 96) produces strong HDR bloom (> 1.0 preserved).
        if (Luminance(at(reconBase, 64, 96)) <= 1.0f)
            return 1;
        // Assert 3: Bloom spreads energy into neighboring dark pixels outside the bright patch (e.g. 64, 84).
        if (Luminance(at(reconBase, 64, 84)) <= 0.05f)
            return 1;

        // Assert 4: Higher intensity increases composite contribution around the bright patch.
        BloomConfig lowInt = baseCfg;
        lowInt.intensity = 0.2f;
        BloomConfig highInt = baseCfg;
        highInt.intensity = 0.9f;
        if (!runAndReadback(lowInt, false, compLowInt) || !runAndReadback(highInt, false, compHighInt))
            return 1;
        if (!(Luminance(at(compHighInt, 64, 84)) > Luminance(at(compLowInt, 64, 84)) + 0.05f))
            return 1;

        // Assert 5: Higher threshold reduces extracted contribution on the medium-bright (1.3) patch at (96, 32).
        BloomConfig highThresh = baseCfg;
        highThresh.threshold = 3.0f;
        if (!runAndReadback(highThresh, true, reconHighThresh))
            return 1;
        if (!(Luminance(at(reconHighThresh, 96, 32)) < Luminance(at(reconBase, 96, 32)) * 0.5f))
            return 1;

        std::cout << "Synthetic FP16 HDR Bloom checks passed: dark=" << Luminance(at(reconBase, 4, 4))
                  << ", highCenter=" << Luminance(at(reconBase, 64, 96))
                  << ", halo=" << Luminance(at(reconBase, 64, 84))
                  << ", medLowT=" << Luminance(at(reconBase, 96, 32))
                  << ", medHighT=" << Luminance(at(reconHighThresh, 96, 32)) << '\n';
    }

    // 2. Repeated initialization/shutdown and failure rollback VMA allocation leak test.
    {
        const uint32_t baseline = m_memoryManager.GetAllocationCountForTest();
        for (int cycle = 0; cycle < 3; ++cycle)
        {
            VulkanBloomPass pass;
            if (!pass.Initialize(
                    m_vulkanContext, m_memoryManager, 2, { 640, 360 },
                    m_renderer.m_hdrColor.GetView(), m_renderer.m_hdrLayout))
                return 1;
            const uint32_t activeAllocs = m_memoryManager.GetAllocationCountForTest();
            // Inject failure during resize: must fail cleanly and preserve previous valid resources!
            if (pass.OnResize({ 800, 450 }, m_renderer.m_hdrColor.GetView(), true))
                return 1;
            if (m_memoryManager.GetAllocationCountForTest() != activeAllocs ||
                pass.GetSceneExtent().width != 640u)
                return 1;
            pass.Shutdown();
            if (m_memoryManager.GetAllocationCountForTest() != baseline)
            {
                std::cerr << "Bloom VMA allocation leak on cycle " << cycle << '\n';
                return 1;
            }
        }
    }

    // 3. Frames-in-flight safety, resize, and Bloom-disabled baseline equivalence.
    if (const auto err = AddImportedAsset(BuildBloomTestScene()); !err.empty())
        return 1;
    m_camera.LookAt({ 0.2f, 1.8f, 5.4f }, { 0.0f, 0.1f, -0.2f });

    for (int f = 0; f < 10; ++f)
    {
        m_window.PollEvents();
        m_bloomConfig.enabled = true;
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return 1;
    }

    glfwSetWindowSize(m_window.GetNativeHandle(), 900, 600);
    m_window.PollEvents();
    if (!RecreateSwapchain())
        return 1;
    for (int f = 0; f < 4; ++f)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return 1;
    }

    std::cout << "Bloom GPU lifecycle, rollback, resize and frames-in-flight verified (lastBloomGpuMs="
              << m_renderer.GetLastBloomGpuTimeMs() << ")\n";
    return 0;
}

int Aether::AetherApplication::RunBloomVisualTest()
{
    if (!Initialize(false))
        return 1;

    if (!(m_vulkanContext.m_swapchain.image_usage_flags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
    {
        std::cerr << "Surface does not support frame readback\n";
        return 1;
    }

    const auto extent = m_vulkanContext.GetSwapchainExtent();
    const auto format = m_vulkanContext.GetSwapchainImageFormat();
    const bool bgra = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
    if (!bgra && format != VK_FORMAT_R8G8B8A8_SRGB && format != VK_FORMAT_R8G8B8A8_UNORM)
        return 1;

    const auto device = m_vulkanContext.GetDevice();
    const size_t bytes = static_cast<size_t>(extent.width) * extent.height * 4;
    VulkanBuffer readback;
    if (readback.Initialize(device, m_memoryManager, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != VK_SUCCESS)
        return 1;

    auto capture = [&](std::vector<uint8_t>& pixels) {
        m_window.PollEvents();
        m_renderer.m_captureTarget = readback.GetBuffer();
        const auto res = DrawScene();
        m_renderer.m_captureTarget = VK_NULL_HANDLE;
        pixels.resize(bytes);
        return vkDeviceWaitIdle(device) == VK_SUCCESS && res == VulkanRenderer::FrameResult::Rendered &&
            readback.ReadData(pixels.data(), bytes) == VK_SUCCESS;
    };

    if (const auto err = AddImportedAsset(BuildBloomTestScene()); !err.empty())
    {
        std::cerr << err << '\n';
        return 1;
    }
    m_camera.LookAt({ 0.2f, 1.7f, 5.5f }, { 0.0f, 0.15f, -0.2f });
    m_lightAngle = 0.65f;
    m_exposure = 0.0f;
    m_shadowConfig = ShadowConfig{};

    std::vector<uint8_t> offImg, onImg, highTImg, lowTImg, lowIntImg, highIntImg;
    std::vector<uint8_t> studioImg, sunsetImg, dbgBright, dbgLvl2, dbgRecon;

    // 1. Default Bloom scene: OFF
    m_bloomConfig = BloomConfig{};
    m_bloomConfig.enabled = false;
    if (!capture(offImg) || !WriteBitmap("bloom-01-default-off.bmp", extent, offImg, bgra))
        return 1;

    // Measure GPU timing across pyramid depths (3, 5, 7 levels).
    for (uint32_t levels : { 3u, 5u, 7u })
    {
        m_bloomConfig = BloomConfig{};
        m_bloomConfig.enabled = true;
        m_bloomConfig.pyramidLevels = levels;
        for (int i = 0; i < 4; ++i)
        {
            m_window.PollEvents();
            if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
                return 1;
        }
        std::cout << "Bloom GPU time (" << levels << " levels, 1280x720): "
                  << m_renderer.GetLastBloomGpuTimeMs() << " ms\n";
    }

    // 2. Default Bloom scene: ON (5 levels, default cinematic intensity)
    m_bloomConfig = BloomConfig{};
    m_bloomConfig.enabled = true;
    if (!capture(onImg) || !WriteBitmap("bloom-02-default-on.bmp", extent, onImg, bgra))
        return 1;
    const size_t onOffDiff = ChangedPixels(offImg, onImg);
    std::cout << "Bloom ON vs OFF changed pixels: " << onOffDiff << '\n';
    if (onOffDiff < 3000)
        return 1;

    // 3. HDR Studio & Sunset with Bloom
    if (!SelectStudioEnvironment(false) || !capture(studioImg) ||
        !WriteBitmap("bloom-05-studio.bmp", extent, studioImg, bgra))
        return 1;
    if (!SelectStudioEnvironment(true) || !capture(sunsetImg) ||
        !WriteBitmap("bloom-06-sunset.bmp", extent, sunsetImg, bgra))
        return 1;
    if (ChangedPixels(studioImg, sunsetImg) < 3000)
        return 1;
    if (!SelectStudioEnvironment(false))
        return 1;

    // 4. High vs Low Threshold comparison
    m_bloomConfig = BloomConfig{};
    m_bloomConfig.threshold = 3.0f;
    if (!capture(highTImg) || !WriteBitmap("bloom-07-threshold-high.bmp", extent, highTImg, bgra))
        return 1;

    m_bloomConfig.threshold = 0.4f;
    if (!capture(lowTImg) || !WriteBitmap("bloom-08-threshold-low.bmp", extent, lowTImg, bgra))
        return 1;
    if (ChangedPixels(highTImg, lowTImg) < 2000)
        return 1;

    // 5. Low vs High Intensity comparison
    m_bloomConfig = BloomConfig{};
    m_bloomConfig.intensity = 0.10f;
    if (!capture(lowIntImg) || !WriteBitmap("bloom-09-intensity-low.bmp", extent, lowIntImg, bgra))
        return 1;

    m_bloomConfig.intensity = 0.75f;
    if (!capture(highIntImg) || !WriteBitmap("bloom-09b-intensity-high.bmp", extent, highIntImg, bgra))
        return 1;
    if (ChangedPixels(lowIntImg, highIntImg) < 2000)
        return 1;

    // 6. Debug visualizations (actual intermediate GPU pyramid resources)
    m_bloomConfig = BloomConfig{};
    m_bloomConfig.debugMode = BloomDebugMode::BrightPass;
    if (!capture(dbgBright) || !WriteBitmap("bloom-10-debug-bright.bmp", extent, dbgBright, bgra))
        return 1;

    m_bloomConfig.debugMode = BloomDebugMode::DownsampleLevel;
    m_bloomConfig.debugLevel = 2;
    if (!capture(dbgLvl2) || !WriteBitmap("bloom-10b-debug-level2.bmp", extent, dbgLvl2, bgra))
        return 1;

    m_bloomConfig.debugMode = BloomDebugMode::ReconstructedBloom;
    if (!capture(dbgRecon) || !WriteBitmap("bloom-10c-debug-reconstructed.bmp", extent, dbgRecon, bgra))
        return 1;

    if (ChangedPixels(dbgBright, dbgRecon) < 500)
        return 1;

    // 7. DamagedHelmet.glb with Bloom OFF vs ON (if present in working directory)
    if (std::filesystem::exists("DamagedHelmet.glb"))
    {
        auto imported = ImportGltf("DamagedHelmet.glb");
        if (!imported.asset)
            return 1;
        if (const auto err = AddImportedAsset(std::move(*imported.asset)); !err.empty())
            return 1;

        std::vector<uint8_t> helmetOff, helmetOn;
        m_bloomConfig = BloomConfig{};
        m_bloomConfig.enabled = false;
        if (!capture(helmetOff) || !WriteBitmap("bloom-03-helmet-off.bmp", extent, helmetOff, bgra))
            return 1;

        m_bloomConfig.enabled = true;
        if (!capture(helmetOn) || !WriteBitmap("bloom-04-helmet-on.bmp", extent, helmetOn, bgra))
            return 1;

        const size_t helmetDiff = ChangedPixels(helmetOff, helmetOn);
        std::cout << "DamagedHelmet Bloom ON vs OFF changed pixels: " << helmetDiff << '\n';
        if (helmetDiff < 1500)
            return 1;
    }

    std::cout << "Bloom visual verification and captures completed\n";
    return 0;
}
