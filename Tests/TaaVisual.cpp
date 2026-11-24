#include <Core/AetherApplication.h>
#include <Assets/GltfImporter.h>
#include <Renderer/MeshData.h>
#include <RHI/Vulkan/Passes/VulkanTaaPass.h>
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

    // High-contrast scene with thin tilted bars, sharp diagonal silhouettes, metallic speculars,
    // and emissive highlights to stress geometric edge AA and temporal stability.
    Aether::ImportedAsset BuildTaaTestScene()
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

        // Dark matte floor.
        ImportedMaterial floorMat;
        floorMat.baseColor = { 0.16f, 0.17f, 0.20f, 1.0f };
        floorMat.metallic = 0.05f;
        floorMat.roughness = 0.80f;
        addMeshNode(MakeGroundPlane(8.0f), floorMat, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.0f, 0.0f)));

        // Index 1: Foreground moving/rotating tilted dark-metallic cube.
        ImportedMaterial darkMetal;
        darkMetal.baseColor = { 0.12f, 0.14f, 0.18f, 1.0f };
        darkMetal.metallic = 0.85f;
        darkMetal.roughness = 0.18f;
        const glm::mat4 fgCube =
            glm::translate(glm::mat4(1.0f), glm::vec3(-0.35f, -0.2f, 0.65f)) *
            glm::rotate(glm::mat4(1.0f), glm::radians(22.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
            glm::rotate(glm::mat4(1.0f), glm::radians(14.0f), glm::vec3(0.0f, 0.0f, 1.0f)) *
            glm::scale(glm::mat4(1.0f), glm::vec3(1.05f, 1.05f, 1.05f));
        addMeshNode(CreateCubeMesh(), darkMetal, fgCube);

        // Index 2: Glossy chrome sphere with bright specular highlights.
        ImportedMaterial chrome;
        chrome.baseColor = { 0.96f, 0.96f, 0.98f, 1.0f };
        chrome.metallic = 1.0f;
        chrome.roughness = 0.08f;
        addMeshNode(MakeSphereMesh(0.72f), chrome, glm::translate(glm::mat4(1.0f), glm::vec3(1.25f, -0.28f, 0.2f)));

        // Index 3..9: Thin tilted high-contrast fence bars (classic subpixel geometric aliasing stress test!).
        ImportedMaterial brightBar;
        brightBar.baseColor = { 0.92f, 0.92f, 0.94f, 1.0f };
        brightBar.metallic = 0.1f;
        brightBar.roughness = 0.35f;
        for (int i = 0; i < 7; ++i)
        {
            const float x = -2.4f + static_cast<float>(i) * 0.72f;
            const glm::mat4 bar =
                glm::translate(glm::mat4(1.0f), glm::vec3(x, 0.35f, -1.65f)) *
                glm::rotate(glm::mat4(1.0f), glm::radians(12.0f), glm::vec3(0.0f, 0.0f, 1.0f)) *
                glm::scale(glm::mat4(1.0f), glm::vec3(0.07f, 2.7f, 0.07f));
            addMeshNode(CreateCubeMesh(), brightBar, bar);
        }

        // Emissive cyan accent rod for Bloom + TAA synergy.
        ImportedMaterial neon;
        neon.baseColor = { 0.1f, 0.2f, 0.25f, 1.0f };
        neon.emissive = { 0.4f, 4.5f, 7.5f };
        neon.metallic = 0.0f;
        neon.roughness = 0.3f;
        const glm::mat4 rod =
            glm::translate(glm::mat4(1.0f), glm::vec3(-1.85f, 0.15f, -0.35f)) *
            glm::rotate(glm::mat4(1.0f), glm::radians(-16.0f), glm::vec3(0.0f, 0.0f, 1.0f)) *
            glm::scale(glm::mat4(1.0f), glm::vec3(0.14f, 2.2f, 0.14f));
        addMeshNode(CreateCubeMesh(), neon, rod);

        return asset;
    }
}

int Aether::AetherApplication::RunTaaGpuTest()
{
    if (!Initialize(false))
        return 1;

    const auto device = m_vulkanContext.GetDevice();
    const VkFormat velFormat = m_renderer.GetVelocityFormat();
    if (velFormat != VK_FORMAT_R16G16B16A16_SFLOAT && velFormat != VK_FORMAT_R32G32B32A32_SFLOAT)
    {
        std::cerr << "Unexpected velocity format: " << velFormat << '\n';
        return 1;
    }

    // 1. Standalone VulkanTaaPass repeated create/destroy + resize failure rollback (zero VMA leaks).
    {
        const uint32_t baseline = m_memoryManager.GetAllocationCountForTest();
        for (int cycle = 0; cycle < 3; ++cycle)
        {
            VulkanTaaPass pass;
            if (!pass.Initialize(
                    m_vulkanContext, m_memoryManager, 2, { 640, 360 },
                    m_renderer.m_depthAttachment.GetFormat(),
                    m_renderer.m_hdrColor.GetView(),
                    m_renderer.m_depthAttachment.GetView(),
                    m_renderer.m_hdrLayout))
                return 1;
            const uint32_t activeAllocs = m_memoryManager.GetAllocationCountForTest();
            if (pass.OnResize(
                    { 800, 450 }, m_renderer.m_depthAttachment.GetFormat(),
                    m_renderer.m_hdrColor.GetView(), m_renderer.m_depthAttachment.GetView(), true))
                return 1;
            if (m_memoryManager.GetAllocationCountForTest() != activeAllocs ||
                pass.GetExtent().width != 640u)
                return 1;
            pass.Shutdown();
            if (m_memoryManager.GetAllocationCountForTest() != baseline)
            {
                std::cerr << "TAA VMA allocation leak on cycle " << cycle << '\n';
                return 1;
            }
        }
    }

    // 2. Load high-contrast TAA test scene.
    if (const auto err = AddImportedAsset(BuildTaaTestScene()); !err.empty())
        return 1;
    m_camera.LookAt({ 0.0f, 1.15f, 4.6f }, { 0.0f, 0.0f, -0.4f });
    m_taaConfig = TaaConfig{};
    m_bloomConfig.enabled = false;
    ResetTemporalHistory();

    // Sample the actual shader output at an interior cube-face pixel and an unobstructed sky pixel.
    // Compute the reference by ray/plane intersection in local space and world-direction projection.
    {
        const auto savedCamera = m_camera;
        const Scene savedScene = m_scene;
        SceneObject cube = savedScene.GetObjects()[1];
        cube.modelMatrix = glm::mat4(1.f);
        cube.previousModelMatrix.reset();
        m_scene.Clear();
        m_scene.AddObject(cube);
        m_camera.LookAt({0, 0, 5}, {0, 0, 0});
        m_camera.SetClipPlanes(.1f, 100.f);
        ResetTemporalHistory();
        const auto extent = m_vulkanContext.GetSwapchainExtent();
        const bool half = velFormat == VK_FORMAT_R16G16B16A16_SFLOAT;
        const VkDeviceSize stride = half ? sizeof(uint64_t) : sizeof(glm::vec4);
        const VkOffset3D pixels[] = {
            {int32_t(extent.width / 2), int32_t(extent.height / 2), 0},
            {int32_t(extent.width / 8), int32_t(extent.height / 8), 0}
        };
        VulkanBuffer samples;
        if (samples.Initialize(device, m_memoryManager, stride * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != VK_SUCCESS)
            return 1;
        const auto readMotion = [&](glm::vec4 (&out)[2]) {
            const auto status = m_renderer.EnvironmentCommands([&](VkCommandBuffer cmd) {
                const auto image = m_renderer.m_taaPass.GetVelocityImage().GetImage();
                VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = image;
                barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.imageMemoryBarrierCount = 1;
                dependency.pImageMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(cmd, &dependency);
                VkBufferImageCopy copies[2]{};
                for (int i = 0; i < 2; ++i)
                {
                    copies[i].bufferOffset = i * stride;
                    copies[i].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    copies[i].imageOffset = pixels[i];
                    copies[i].imageExtent = {1, 1, 1};
                }
                vkCmdCopyImageToBuffer(cmd, image, barrier.newLayout, samples.GetBuffer(), 2, copies);
                std::swap(barrier.srcStageMask, barrier.dstStageMask);
                std::swap(barrier.srcAccessMask, barrier.dstAccessMask);
                std::swap(barrier.oldLayout, barrier.newLayout);
                vkCmdPipelineBarrier2(cmd, &dependency);
            });
            if (status != VK_SUCCESS)
                return false;
            if (!half)
                return samples.ReadData(out, sizeof(out)) == VK_SUCCESS;
            uint64_t packed[2]{};
            if (samples.ReadData(packed, sizeof(packed)) != VK_SUCCESS)
                return false;
            out[0] = glm::unpackHalf4x16(packed[0]);
            out[1] = glm::unpackHalf4x16(packed[1]);
            return true;
        };
        const auto unproject = [](const glm::mat4& inverse, glm::vec2 ndc, float z) {
            const auto clip = inverse * glm::vec4(ndc, z, 1.f);
            return glm::vec3(clip) / clip.w;
        };
        glm::mat4 previousProjection(1.f), previousView(1.f), previousModel(1.f);
        for (uint32_t step = 0; step < 9; ++step)
        {
            auto& object = m_scene.GetObjects()[0];
            if (step == 3 || step == 5)
                object.modelMatrix = glm::translate(glm::mat4(1), {0.10f * (step - 2), .02f, -.15f});
            if (step == 4 || step == 5)
                m_cameraController.Pan(m_camera, step == 4 ? 0.04f : 0.31f, -0.01f);
            if (step == 6)
                m_camera.LookAt(m_camera.GetPosition(), m_camera.GetTarget() + glm::vec3(0.11f, 0.08f, 0.f));

            const auto jitter = ComputeTaaJitter(step, extent.width, extent.height).ndcOffset;
            const auto projection = ApplyProjectionJitter(m_camera.GetProjectionMatrix(), jitter);
            const auto view = m_camera.GetViewMatrix();
            const auto model = object.GetModelMatrix();
            if (DrawScene() != VulkanRenderer::FrameResult::Rendered || m_taaFrameIndex != step + 1)
                return 1;
            glm::vec4 motion[2]{};
            if (!readMotion(motion))
                return 1;
            if (step == 0 && motion[0].w != 0.f)
                return 1;
            for (int i = 0; step > 0 && i < 2; ++i)
            {
                const glm::vec2 uv = (glm::vec2(pixels[i].x, pixels[i].y) + .5f) /
                    glm::vec2(extent.width, extent.height);
                const auto ndc = 2.f * uv - 1.f;
                glm::vec4 oldClip;
                if (i == 0)
                {
                    const auto inverseMvp = glm::inverse(projection * view * model);
                    const auto nearPoint = unproject(inverseMvp, ndc, 0.f);
                    const auto ray = unproject(inverseMvp, ndc, 1.f) - nearPoint;
                    const auto local = nearPoint + ray * ((.5f - nearPoint.z) / ray.z);
                    oldClip = previousProjection * previousView * previousModel * glm::vec4(local, 1.f);
                }
                else
                {
                    const auto viewRay = unproject(glm::inverse(projection), ndc, 0.f);
                    const auto worldRay = glm::transpose(glm::mat3(view)) * viewRay;
                    oldClip = previousProjection * glm::vec4(glm::mat3(previousView) * worldRay, 0.f);
                }
                const auto expected = glm::vec2(oldClip) / oldClip.w * .5f + .5f - uv;
                const float oldDepth = i == 0 ? oldClip.z / oldClip.w : 1.f;
                if (motion[i].w != float(i + 1) || glm::length(glm::vec2(motion[i]) - expected) > .000075f ||
                    std::abs(motion[i].z - oldDepth) > .0005f)
                {
                    std::cerr << "Velocity/depth reprojection mismatch at step " << step << ", sample " << i << '\n';
                    return 1;
                }
            }
            previousProjection = projection;
            previousView = view;
            previousModel = model;
        }
        const auto restarted = [&] {
            return DrawScene() == VulkanRenderer::FrameResult::Rendered && m_taaFrameIndex == 1 &&
                m_renderer.IsTaaHistoryValid();
        };
        m_camera.SetFov(53.f);
        if (!restarted())
            return 1;
        m_camera.SetClipPlanes(.15f, 90.f);
        if (!restarted())
            return 1;
        m_camera.LookAt({5, 0, 0}, {0, 0, 0});
        if (!restarted())
            return 1;
        m_taaConfig.resetHistory = true;
        if (!restarted() || m_taaConfig.resetHistory)
            return 1;

        auto& object = m_scene.GetObjects()[0];
        const auto meshId = object.mesh.meshId;
        const auto lastModel = object.previousModelMatrix;
        object.modelMatrix = glm::mat4(2.f);
        object.mesh.meshId = m_meshes.size();
        if (DrawScene() != VulkanRenderer::FrameResult::FatalError ||
            object.previousModelMatrix != lastModel || m_taaFrameIndex != 1)
            return 1;
        object.mesh.meshId = meshId;

        for (const float opacity : {0.25f, 0.75f})
        {
            auto masked = BuildTaaTestScene();
            masked.materials[1].alphaMode = ImportedMaterial::AlphaMode::Mask;
            masked.materials[1].alphaCutoff = .5f;
            masked.materials[1].baseColor.a = opacity;
            if (const auto error = AddImportedAsset(std::move(masked)); !error.empty())
                return 1;
            m_scene.Clear();
            m_scene.AddObject(cube);
            m_camera.LookAt({0, 0, 5}, {0, 0, 0});
            for (int f = 0; f < 2; ++f)
                if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
                    return 1;
            glm::vec4 motion[2]{};
            if (!readMotion(motion) || motion[0].w != (opacity > .5f ? 1.f : 2.f))
            {
                std::cerr << "Alpha mask depth and velocity coverage disagreed\n";
                return 1;
            }
        }
        if (const auto error = AddImportedAsset(BuildTaaTestScene()); !error.empty())
            return 1;
        m_scene = savedScene;
        m_camera = savedCamera;
        ResetTemporalHistory();
        std::cout << "GPU motion/depth matched the analytical Halton reprojection; mask, camera cuts and aborted frame verified\n";
    }

    // 3. Static scene convergence + FP16 NaN/Inf verification.
    for (int f = 0; f < 16; ++f)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return 1;
    }
    if (!m_renderer.IsTaaHistoryValid())
        return 1;

    // Verify resolved FP16 history buffer contains zero NaN/Inf.
    {
        const auto ext = m_vulkanContext.GetSwapchainExtent();
        const size_t pixelCount = static_cast<size_t>(ext.width) * ext.height;
        const VkDeviceSize bytes = pixelCount * 4 * sizeof(uint16_t);
        VulkanBuffer readback;
        if (readback.Initialize(device, m_memoryManager, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != VK_SUCCESS)
            return 1;
        const auto& histImg = m_renderer.m_taaPass.GetHistoryImage(0);
        const auto st = m_renderer.EnvironmentCommands([&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier2 toSrc{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            toSrc.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            toSrc.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            toSrc.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            toSrc.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            toSrc.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toSrc.srcQueueFamilyIndex = toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toSrc.image = histImg.GetImage();
            toSrc.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &toSrc;
            vkCmdPipelineBarrier2(cmd, &dep);

            VkBufferImageCopy region{};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { ext.width, ext.height, 1 };
            vkCmdCopyImageToBuffer(cmd, histImg.GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                readback.GetBuffer(), 1, &region);

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
            return 1;
        std::vector<uint64_t> halfPixels(pixelCount);
        if (readback.ReadData(halfPixels.data(), bytes) != VK_SUCCESS)
            return 1;
        for (size_t i = 0; i < pixelCount; ++i)
        {
            const glm::vec4 c = glm::unpackHalf4x16(halfPixels[i]);
            if (!std::isfinite(c.r) || !std::isfinite(c.g) || !std::isfinite(c.b))
            {
                std::cerr << "Non-finite pixel in TAA resolved HDR buffer\n";
                return 1;
            }
        }
    }

    // 4. Camera translation & rotation.
    for (int f = 0; f < 6; ++f)
    {
        m_cameraController.Pan(m_camera, 0.08f, 0.0f);
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return 1;
    }
    for (int f = 0; f < 6; ++f)
    {
        m_cameraController.Orbit(m_camera, 0.06f, 0.01f);
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return 1;
    }

    // 5. Moving & rotating object + sudden depth disocclusion.
    for (int f = 0; f < 8; ++f)
    {
        if (auto objs = m_scene.GetObjects(); objs.size() > 1)
        {
            objs[1].modelMatrix =
                glm::translate(glm::mat4(1.0f), glm::vec3(-0.35f + 0.14f * f, -0.2f, 0.65f)) *
                glm::rotate(glm::mat4(1.0f), 0.25f * f, glm::vec3(0.0f, 1.0f, 0.0f));
        }
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return 1;
    }

    // 6. Object spawning and removal (verifying safe previousModelMatrix init & no dangling refs).
    {
        SceneObject spawned = m_scene.GetObjects()[1];
        spawned.previousModelMatrix.reset();
        spawned.modelMatrix = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.6f, 1.2f));
        const size_t spawnedIndex = m_scene.GetObjects().size();
        m_scene.AddObject(spawned);
        for (int f = 0; f < 3; ++f)
        {
            m_window.PollEvents();
            if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
                return 1;
        }
        if (!m_scene.RemoveObject(spawnedIndex))
            return 1;
        for (int f = 0; f < 3; ++f)
        {
            m_window.PollEvents();
            if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
                return 1;
        }
    }

    // 7. HDR environment switch, Bloom ON/OFF, TAA ON/OFF, Resize & history invalidation.
    if (!SelectStudioEnvironment(true) || m_renderer.IsTaaHistoryValid() || m_taaFrameIndex != 0)
        return 1;
    m_bloomConfig.enabled = true;
    for (int f = 0; f < 4; ++f)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return 1;
    }
    m_taaConfig.enabled = false;
    for (int f = 0; f < 3; ++f)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return 1;
    }
    if (m_renderer.IsTaaHistoryValid())
        return 1;
    m_taaConfig.enabled = true;
    ResetTemporalHistory();

    glfwSetWindowSize(m_window.GetNativeHandle(), 900, 600);
    m_window.PollEvents();
    if (!RecreateSwapchain() || m_renderer.IsTaaHistoryValid())
        return 1;
    for (int f = 0; f < 4; ++f)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return 1;
    }
    if (!m_renderer.IsTaaHistoryValid())
        return 1;

    std::cout << "TAA GPU convergence, motion, spawn/remove, disocclusion, resize & rollback verified "
              << "(velMs=" << m_renderer.GetLastTaaVelocityGpuTimeMs()
              << ", resolveMs=" << m_renderer.GetLastTaaResolveGpuTimeMs() << ")\n";
    return 0;
}

int Aether::AetherApplication::RunTaaVisualTest()
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

    if (const auto err = AddImportedAsset(BuildTaaTestScene()); !err.empty())
        return 1;

    const glm::vec3 baseEye(0.0f, 1.15f, 4.6f);
    const glm::vec3 baseTarget(0.0f, 0.0f, -0.4f);
    m_camera.LookAt(baseEye, baseTarget);
    m_lightAngle = 0.65f;
    m_exposure = 0.0f;
    m_shadowConfig = ShadowConfig{};
    m_bloomConfig = BloomConfig{};
    m_bloomConfig.enabled = false;

    std::vector<uint8_t> taaOffImg, taaOnImg, camMoveImg, rotObjImg, movObjImg, bloomTaaImg, sunsetTaaImg;

    // 1. Static geometry edges: TAA OFF
    m_taaConfig = TaaConfig{};
    m_taaConfig.enabled = false;
    if (!capture(taaOffImg) || !WriteBitmap("taa-01-off.bmp", extent, taaOffImg, bgra))
        return 1;

    // 2. Static geometry edges: TAA ON (16 frames convergence)
    m_taaConfig.enabled = true;
    ResetTemporalHistory();
    for (int i = 0; i < 16; ++i)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return 1;
    }
    if (!capture(taaOnImg) || !WriteBitmap("taa-02-on.bmp", extent, taaOnImg, bgra))
        return 1;
    const size_t taaDiff = ChangedPixels(taaOffImg, taaOnImg);
    std::cout << "TAA OFF vs ON changed pixels (1280x720): " << taaDiff << '\n';
    std::cout << "TAA GPU time (1280x720): velocity=" << m_renderer.GetLastTaaVelocityGpuTimeMs()
              << " ms, resolve=" << m_renderer.GetLastTaaResolveGpuTimeMs()
              << " ms, total=" << m_renderer.GetLastTaaTotalGpuTimeMs() << " ms\n";
    if (taaDiff < 500)
        return 1;

    const auto captureSequence = [&](const char* name, unsigned frames, auto&& update, double* rms = nullptr) {
        std::vector<uint8_t> previous, current;
        size_t changes = 0;
        uint64_t squaredError = 0;
        for (unsigned frame = 0; frame < frames; ++frame)
        {
            update(frame);
            const auto file = std::string("taa-audit-") + name + "-" + std::to_string(frame) + ".bmp";
            if (!capture(current) || !WriteBitmap(file.c_str(), extent, current, bgra))
                return false;
            if (!previous.empty())
            {
                changes += ChangedPixels(previous, current);
                for (size_t i = 0; rms && i < current.size(); i += 4)
                    for (size_t channel = 0; channel < 3; ++channel)
                    {
                        const int difference = int(current[i + channel]) - int(previous[i + channel]);
                        squaredError += difference * difference;
                    }
            }
            previous.swap(current);
        }
        std::cout << "TAA sequence " << name << ": " << changes / (frames - 1)
                  << " mean consecutive pixels changed (>3 channel codes)\n";
        if (rms)
        {
            *rms = std::sqrt(double(squaredError) / (3.0 * extent.width * extent.height * (frames - 1)));
            std::cout << "TAA sequence " << name << " consecutive RGB RMSE: " << *rms << '\n';
        }
        return true;
    };
    double resolvedRms = 0.0, jitterOnlyRms = 0.0;
    if (!captureSequence("static", 8, [](unsigned) {}, &resolvedRms))
        return 1;
    const auto fullHistoryConfig = m_taaConfig;
    m_taaConfig.historyWeight = m_taaConfig.minHistoryWeight = m_taaConfig.maxHistoryWeight = 0.f;
    ResetTemporalHistory();
    if (DrawScene() != VulkanRenderer::FrameResult::Rendered ||
        !captureSequence("jitter-only", 8, [](unsigned) {}, &jitterOnlyRms) ||
        !(resolvedRms < jitterOnlyRms))
        return 1;
    m_taaConfig = fullHistoryConfig;
    ResetTemporalHistory();

    if (!captureSequence("camera-pan", 6, [&](unsigned frame) {
            m_cameraController.Pan(m_camera, frame < 3 ? .025f : .42f, 0.f);
        }) ||
        !captureSequence("camera-rotation", 4, [&](unsigned) {
            m_camera.LookAt(m_camera.GetPosition(), m_camera.GetTarget() + glm::vec3(.13f, .03f, 0.f));
        }))
        return 1;
    m_camera.LookAt(baseEye, baseTarget);
    ResetTemporalHistory();

    // 3. Camera movement (orbit + pan) with TAA ON
    for (int i = 0; i < 6; ++i)
    {
        m_cameraController.Orbit(m_camera, 0.04f, 0.005f);
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return 1;
    }
    if (!capture(camMoveImg) || !WriteBitmap("taa-03-camera-move.bmp", extent, camMoveImg, bgra))
        return 1;
    m_camera.LookAt(baseEye, baseTarget);
    ResetTemporalHistory();

    // 4. Rotating object with TAA ON
    for (int i = 0; i < 8; ++i)
    {
        if (auto objs = m_scene.GetObjects(); objs.size() > 1)
        {
            objs[1].modelMatrix =
                glm::translate(glm::mat4(1.0f), glm::vec3(-0.35f, -0.2f, 0.65f)) *
                glm::rotate(glm::mat4(1.0f), glm::radians(22.0f + 7.5f * i), glm::vec3(0.0f, 1.0f, 0.0f)) *
                glm::rotate(glm::mat4(1.0f), glm::radians(14.0f), glm::vec3(0.0f, 0.0f, 1.0f)) *
                glm::scale(glm::mat4(1.0f), glm::vec3(1.05f));
        }
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return 1;
    }
    if (!capture(rotObjImg) || !WriteBitmap("taa-04-rotating-obj.bmp", extent, rotObjImg, bgra))
        return 1;

    // 5. Moving object with TAA ON (testing velocity reprojection & anti-ghosting)
    for (int i = 0; i < 8; ++i)
    {
        if (auto objs = m_scene.GetObjects(); objs.size() > 1)
        {
            objs[1].modelMatrix =
                glm::translate(glm::mat4(1.0f), glm::vec3(-0.9f + 0.18f * i, -0.2f, 0.65f)) *
                glm::rotate(glm::mat4(1.0f), glm::radians(22.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
                glm::scale(glm::mat4(1.0f), glm::vec3(1.05f));
        }
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return 1;
    }
    if (!capture(movObjImg) || !WriteBitmap("taa-05-moving-obj.bmp", extent, movObjImg, bgra))
        return 1;

    // 6. Bloom + TAA (Studio & Sunset)
    m_bloomConfig.enabled = true;
    for (int i = 0; i < 8; ++i)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return 1;
    }
    if (!capture(bloomTaaImg) || !WriteBitmap("taa-06-bloom-taa.bmp", extent, bloomTaaImg, bgra))
        return 1;

    const auto moveForeground = [&](unsigned frame) {
        auto& object = m_scene.GetObjects()[1];
        object.modelMatrix =
            glm::translate(glm::mat4(1), {-0.3f + .26f * frame, -.15f, .65f}) *
            glm::rotate(glm::mat4(1), .35f * frame, glm::vec3(0, 1, 0));
    };
    if (!captureSequence("moving-opaque", 5, moveForeground))
        return 1;
    SceneObject spawned = m_scene.GetObjects()[1];
    spawned.previousModelMatrix.reset();
    spawned.modelMatrix =
        glm::translate(glm::mat4(1), {-.35f, -.08f, 1.35f}) * glm::scale(glm::mat4(1), glm::vec3(.42f));
    const auto spawnIndex = m_scene.GetObjects().size();
    if (!captureSequence("spawn-remove", 4, [&](unsigned frame) {
            if (frame == 1)
                m_scene.AddObject(spawned);
            if (frame == 3)
                m_scene.RemoveObject(spawnIndex);
        }))
        return 1;
    auto blended = BuildTaaTestScene();
    blended.materials[1].alphaMode = ImportedMaterial::AlphaMode::Blend;
    blended.materials[1].baseColor.a = 0.35f;
    if (const auto err = AddImportedAsset(std::move(blended)); !err.empty())
        return 1;
    m_camera.LookAt(baseEye, baseTarget);
    if (!captureSequence("moving-blend", 5, moveForeground))
        return 1;
    m_taaConfig.historyWeight = m_taaConfig.minHistoryWeight = m_taaConfig.maxHistoryWeight = 0.f;
    ResetTemporalHistory();
    if (!captureSequence("moving-blend-jitter-only", 5, moveForeground))
        return 1;
    m_taaConfig = fullHistoryConfig;
    if (const auto err = AddImportedAsset(BuildTaaTestScene()); !err.empty())
        return 1;
    m_camera.LookAt(baseEye, baseTarget);

    if (!SelectStudioEnvironment(true))
        return 1;
    if (!captureSequence("sunset-after-switch", 3, [](unsigned) {}))
        return 1;
    for (int i = 0; i < 8; ++i)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return 1;
    }
    if (!capture(sunsetTaaImg) || !WriteBitmap("taa-09-sunset-taa.bmp", extent, sunsetTaaImg, bgra))
        return 1;
    if (!SelectStudioEnvironment(false))
        return 1;

    // 7. DamagedHelmet.glb: TAA OFF vs TAA ON
    if (std::filesystem::exists("DamagedHelmet.glb"))
    {
        auto imported = ImportGltf("DamagedHelmet.glb");
        if (!imported.asset)
            return 1;
        if (const auto err = AddImportedAsset(std::move(*imported.asset)); !err.empty())
            return 1;

        std::vector<uint8_t> helmetOff, helmetOn;
        m_bloomConfig.enabled = true;
        m_taaConfig.enabled = false;
        if (!capture(helmetOff) || !WriteBitmap("taa-07-helmet-off.bmp", extent, helmetOff, bgra))
            return 1;

        m_taaConfig.enabled = true;
        ResetTemporalHistory();
        for (int i = 0; i < 16; ++i)
        {
            m_window.PollEvents();
            if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
                return 1;
        }
        if (!capture(helmetOn) || !WriteBitmap("taa-08-helmet-on.bmp", extent, helmetOn, bgra))
            return 1;
        std::cout << "TAA DamagedHelmet (" << extent.width << "x" << extent.height << "): velocity="
                  << m_renderer.GetLastTaaVelocityGpuTimeMs() << " ms, resolve="
                  << m_renderer.GetLastTaaResolveGpuTimeMs() << " ms, total="
                  << m_renderer.GetLastTaaTotalGpuTimeMs() << " ms\n";

        const size_t helmetDiff = ChangedPixels(helmetOff, helmetOn);
        std::cout << "DamagedHelmet TAA OFF vs ON changed pixels: " << helmetDiff << '\n';
        if (helmetDiff < 300)
            return 1;
    }

    // 8. Multi-resolution RTX 5090 GPU Timestamp Benchmarks (1920x1080 and 3840x2160 offscreen).
    for (const VkExtent2D benchExt : { VkExtent2D{ 1920, 1080 }, VkExtent2D{ 3840, 2160 } })
    {
        VulkanImage benchHdr, benchDepth;
        const VkFormat depthFmt = m_renderer.m_depthAttachment.GetFormat();
        if (benchHdr.Initialize(device, m_memoryManager, benchExt, VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT) != VK_SUCCESS ||
            benchDepth.Initialize(device, m_memoryManager, benchExt, depthFmt,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                VK_IMAGE_ASPECT_DEPTH_BIT) != VK_SUCCESS)
            continue;

        VulkanTaaPass benchTaa;
        if (!benchTaa.Initialize(m_vulkanContext, m_memoryManager, 2, benchExt, depthFmt,
                benchHdr.GetView(), benchDepth.GetView(), m_renderer.m_hdrLayout))
            continue;

        // Initialize immutable HDR and clear depth for a deterministic empty-scene benchmark.
        if (m_renderer.EnvironmentCommands([&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier2 b[2]{};
            b[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b[0].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            b[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            b[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b[0].srcQueueFamilyIndex = b[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b[0].image = benchHdr.GetImage();
            b[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

            b[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b[1].dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            b[1].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            b[1].srcQueueFamilyIndex = b[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b[1].image = benchDepth.GetImage();
            b[1].subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };

            VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dep.imageMemoryBarrierCount = 2;
            dep.pImageMemoryBarriers = b;
            vkCmdPipelineBarrier2(cmd, &dep);
            VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            color.imageView = benchHdr.GetView();
            color.imageLayout = b[0].newLayout;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue.color = {{.35f, .45f, .6f, 1.f}};
            VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            depth.imageView = benchDepth.GetView();
            depth.imageLayout = b[1].newLayout;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth.clearValue.depthStencil = {1.f, 0};
            VkRenderingInfo render{VK_STRUCTURE_TYPE_RENDERING_INFO};
            render.renderArea.extent = benchExt;
            render.layerCount = 1;
            render.colorAttachmentCount = 1;
            render.pColorAttachments = &color;
            render.pDepthAttachment = &depth;
            vkCmdBeginRendering(cmd, &render);
            vkCmdEndRendering(cmd);
            b[0].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            b[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            b[0].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            b[0].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            b[0].oldLayout = color.imageLayout;
            b[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            dep.imageMemoryBarrierCount = 1;
            vkCmdPipelineBarrier2(cmd, &dep);
        }) != VK_SUCCESS)
            return 1;

        for (uint32_t i = 0; i < 6; ++i)
        {
            bool recorded = false;
            if (m_renderer.EnvironmentCommands([&](VkCommandBuffer cmd) {
                recorded = benchTaa.Execute(cmd, i % 2, benchDepth.GetImage(), {},
                    glm::mat4(1.0f), glm::vec2(0.0f), m_taaConfig);
            }) != VK_SUCCESS || !recorded)
                return 1;
            benchTaa.OnFrameSubmitted(i % 2);
        }
        std::cout << "TAA GPU benchmark (" << benchExt.width << "x" << benchExt.height << "): "
                  << "velocity=" << benchTaa.GetLastVelocityGpuTimeMs()
                  << " ms, resolve=" << benchTaa.GetLastResolveGpuTimeMs()
                  << " ms, total=" << benchTaa.GetLastTotalGpuTimeMs() << " ms\n";
    }

#ifdef _WIN32
    // Opt-in desktop measurements with actual HDR, geometry, depth, Shadows and Bloom.
    if (GetEnvironmentVariableW(L"AETHER_TAA_FULL_RES_BENCH", nullptr, 0) != 0)
    {
        const auto measure = [&](const char* sceneName) {
            const auto actual = m_vulkanContext.GetSwapchainExtent();
            std::vector<double> samples;
            for (int i = 0; i < 18; ++i)
            {
                m_window.PollEvents();
                if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
                    return false;
                if (i >= 6)
                    samples.push_back(m_renderer.GetLastTaaTotalGpuTimeMs());
            }
            std::sort(samples.begin(), samples.end());
            if (samples.front() <= 0.0)
                return false;
            const size_t middle = samples.size() / 2;
            const double median = 0.5 * (samples[middle - 1] + samples[middle]);
            std::cout << "TAA full scene " << sceneName << " (" << actual.width << "x" << actual.height
                      << "): median " << median << " ms over " << samples.size() << " GPU timestamp samples\n";
            return true;
        };
        for (const VkExtent2D requested : {VkExtent2D{1280, 720}, VkExtent2D{1920, 1080}, VkExtent2D{3840, 2160}})
        {
            // Remove the non-client frame when the requested framebuffer fills the monitor.
            if (requested.height == 2160)
                glfwSetWindowAttrib(m_window.GetNativeHandle(), GLFW_DECORATED, GLFW_FALSE);
            glfwSetWindowSize(m_window.GetNativeHandle(), static_cast<int>(requested.width), static_cast<int>(requested.height));
            m_window.PollEvents();
            if (!RecreateSwapchain())
                return 1;
            if (const auto error = AddImportedAsset(BuildTaaTestScene()); !error.empty())
                return 1;
            m_camera.LookAt(baseEye, baseTarget);
            if (!measure("dynamic geometry fixture"))
                return 1;
            auto helmet = ImportGltf("DamagedHelmet.glb");
            if (!helmet.asset || !AddImportedAsset(std::move(*helmet.asset)).empty() || !measure("DamagedHelmet"))
                return 1;
        }
    }
#endif

    std::cout << "TAA visual verification and multi-resolution benchmarks completed\n";
    return 0;
}
