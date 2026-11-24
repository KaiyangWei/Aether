#include <Core/AetherApplication.h>
#include <Assets/GltfImporter.h>
#include <Renderer/MeshData.h>
#include <RHI/Vulkan/Resources/VulkanShadowMap.h>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

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

    double MeanLuminance(const std::vector<uint8_t>& pixels, bool bgra)
    {
        double sum = 0.0;
        const size_t pixelCount = pixels.size() / 4;
        if (pixelCount == 0)
            return 0.0;
        for (size_t i = 0; i + 3 < pixels.size(); i += 4)
        {
            const double r = bgra ? pixels[i + 2] : pixels[i];
            const double g = pixels[i + 1];
            const double b = bgra ? pixels[i] : pixels[i + 2];
            sum += 0.2126 * r + 0.7152 * g + 0.0722 * b;
        }
        return sum / static_cast<double>(pixelCount);
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

    Aether::ImportedAsset BuildShadowTestScene(Aether::ImportedAsset optionalHelmet)
    {
        using namespace Aether;
        ImportedAsset asset = std::move(optionalHelmet);

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

        // Ground receiver surface (matte light stone).
        ImportedMaterial groundMat;
        groundMat.baseColor = { 0.72f, 0.72f, 0.70f, 1.0f };
        groundMat.metallic = 0.0f;
        groundMat.roughness = 0.85f;

        if (!asset.nodes.empty())
        {
            // Helmet + ground receiver + side caster pillar so helmet both casts & receives shadows.
            addMeshNode(MakeGroundPlane(7.0f), groundMat, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.15f, 0.0f)));

            ImportedMaterial pillarMat;
            pillarMat.baseColor = { 0.78f, 0.45f, 0.22f, 1.0f };
            pillarMat.metallic = 0.15f;
            pillarMat.roughness = 0.45f;
            const glm::mat4 pillarWorld =
                glm::translate(glm::mat4(1.0f), glm::vec3(1.65f, 0.05f, 0.85f)) *
                glm::scale(glm::mat4(1.0f), glm::vec3(0.45f, 2.4f, 0.45f));
            addMeshNode(CreateCubeMesh(), pillarMat, pillarWorld);
            return asset;
        }

        addMeshNode(MakeGroundPlane(7.5f), groundMat, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.0f, 0.0f)));

        // Copper sphere caster.
        ImportedMaterial sphereMat;
        sphereMat.baseColor = { 0.82f, 0.44f, 0.22f, 1.0f };
        sphereMat.metallic = 0.65f;
        sphereMat.roughness = 0.28f;
        addMeshNode(MakeSphereMesh(0.75f), sphereMat, glm::translate(glm::mat4(1.0f), glm::vec3(-1.35f, -0.25f, 0.35f)));

        // Rotated matte ceramic cube receiving shadow from raised upper cube & casting onto ground.
        ImportedMaterial cubeMat;
        cubeMat.baseColor = { 0.35f, 0.62f, 0.80f, 1.0f };
        cubeMat.metallic = 0.05f;
        cubeMat.roughness = 0.55f;
        const glm::mat4 lowerCube =
            glm::translate(glm::mat4(1.0f), glm::vec3(0.25f, -0.45f, -0.15f)) *
            glm::rotate(glm::mat4(1.0f), glm::radians(25.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
            glm::scale(glm::mat4(1.0f), glm::vec3(1.1f, 1.1f, 1.1f));
        addMeshNode(CreateCubeMesh(), cubeMat, lowerCube);

        // Raised caster block positioned toward the directional light so it casts a shadow onto the lower cube.
        ImportedMaterial upperMat;
        upperMat.baseColor = { 0.86f, 0.78f, 0.30f, 1.0f };
        upperMat.metallic = 0.2f;
        upperMat.roughness = 0.4f;
        const glm::mat4 upperCube =
            glm::translate(glm::mat4(1.0f), glm::vec3(1.05f, 0.95f, 0.55f)) *
            glm::rotate(glm::mat4(1.0f), glm::radians(-18.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
            glm::scale(glm::mat4(1.0f), glm::vec3(0.65f, 0.65f, 0.65f));
        addMeshNode(CreateCubeMesh(), upperMat, upperCube);

        // Tall pillar casting a long shadow across the ground.
        ImportedMaterial pillarMat;
        pillarMat.baseColor = { 0.75f, 0.30f, 0.32f, 1.0f };
        pillarMat.metallic = 0.1f;
        pillarMat.roughness = 0.6f;
        const glm::mat4 pillar =
            glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.25f, -0.85f)) *
            glm::scale(glm::mat4(1.0f), glm::vec3(0.55f, 2.5f, 0.55f));
        addMeshNode(CreateCubeMesh(), pillarMat, pillar);

        // Small emissive marker in shadow region to verify emissive is unaffected by directional shadows.
        ImportedMaterial emissiveMat;
        emissiveMat.baseColor = { 0.1f, 0.1f, 0.1f, 1.0f };
        emissiveMat.emissive = { 2.4f, 0.45f, 0.15f };
        emissiveMat.metallic = 0.0f;
        emissiveMat.roughness = 0.8f;
        const glm::mat4 emissiveBox =
            glm::translate(glm::mat4(1.0f), glm::vec3(-2.35f, -0.75f, -0.65f)) *
            glm::scale(glm::mat4(1.0f), glm::vec3(0.5f, 0.5f, 0.5f));
        addMeshNode(CreateCubeMesh(), emissiveMat, emissiveBox);

        return asset;
    }
}

int Aether::AetherApplication::RunShadowGpuTest()
{
    if (!Initialize(false))
        return 1;

    const auto device = m_vulkanContext.GetDevice();
    const auto phys = m_vulkanContext.GetPhysicalDevice();

    // 1. Verify shadow depth format is valid and repeated VulkanShadowMap create/destroy has zero VMA leaks.
    const VkFormat shadowFormat = m_renderer.GetShadowDepthFormat();
    if (shadowFormat != VK_FORMAT_D32_SFLOAT &&
        shadowFormat != VK_FORMAT_D24_UNORM_S8_UINT &&
        shadowFormat != VK_FORMAT_D16_UNORM)
    {
        std::cerr << "Unexpected shadow depth format: " << shadowFormat << '\n';
        return 1;
    }

    const uint32_t allocBaseline = m_memoryManager.GetAllocationCountForTest();
    for (int cycle = 0; cycle < 5; ++cycle)
    {
        VulkanShadowMap map;
        if (map.Initialize(phys, device, m_memoryManager, 512u << (cycle % 2)) != VK_SUCCESS ||
            !map.GetDepthView() || !map.GetCompareSampler() || !map.GetDebugSampler())
            return 1;
        map.Shutdown();
        if (m_memoryManager.GetAllocationCountForTest() != allocBaseline)
        {
            std::cerr << "Shadow map VMA allocation leak detected on cycle " << cycle << '\n';
            return 1;
        }
    }

    // 2. Load multi-object PBR shadow scene.
    const auto error = AddImportedAsset(BuildShadowTestScene({}));
    if (!error.empty())
    {
        std::cerr << "Failed to load shadow test scene: " << error << '\n';
        return 1;
    }
    m_camera.LookAt({ 0.2f, 3.2f, 6.2f }, { 0.0f, -0.35f, 0.0f });
    m_lightAngle = 0.65f;

    // 3. Test runtime resolution change without rewriting ShadowPass.
    if (!m_renderer.SetShadowResolution(1024) || m_renderer.GetShadowResolution() != 1024)
        return 1;
    for (int f = 0; f < 4; ++f)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return 1;
    }
    if (!m_renderer.SetShadowResolution(2048) || m_renderer.GetShadowResolution() != 2048)
        return 1;

    // 4. Test frames-in-flight safety with moving caster & rotating directional light.
    for (int f = 0; f < 12; ++f)
    {
        m_window.PollEvents();
        m_lightAngle += 0.12f;
        if (auto objects = m_scene.GetObjects(); objects.size() > 2 && objects[2].modelMatrix)
        {
            objects[2].modelMatrix = glm::translate(glm::mat4(1.0f), glm::vec3(0.25f + 0.04f * f, -0.45f, -0.15f));
        }
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return 1;
    }

    // 5. Test swapchain resize with shadows active.
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

    // 6. Verify shadows ON vs OFF actually changes rendered pixels on GPU.
    if (m_vulkanContext.m_swapchain.image_usage_flags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
    {
        const auto extent = m_vulkanContext.GetSwapchainExtent();
        const auto format = m_vulkanContext.GetSwapchainImageFormat();
        const bool bgra = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
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

        std::vector<uint8_t> onPixels, offPixels;
        m_shadowConfig.enabled = true;
        if (!capture(onPixels))
            return 1;
        m_shadowConfig.enabled = false;
        if (!capture(offPixels))
            return 1;
        m_shadowConfig.enabled = true;

        const size_t diff = ChangedPixels(onPixels, offPixels);
        const double lumOn = MeanLuminance(onPixels, bgra);
        const double lumOff = MeanLuminance(offPixels, bgra);
        std::cout << "Shadow GPU check: diff=" << diff
                  << " pixels, meanLum(ON)=" << lumOn
                  << ", meanLum(OFF)=" << lumOff
                  << ", lastShadowGpuMs=" << m_renderer.GetLastShadowGpuTimeMs() << '\n';
        if (diff < 1000 || !(lumOn < lumOff))
            return 1;
    }

    std::cout << "Shadow GPU lifecycle, resize, frames-in-flight and sampling verified\n";
    return 0;
}

int Aether::AetherApplication::RunShadowVisualTest()
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

    // Load dedicated multi-object + ground receiver shadow scene.
    if (const auto err = AddImportedAsset(BuildShadowTestScene({})); !err.empty())
    {
        std::cerr << err << '\n';
        return 1;
    }
    m_camera.LookAt({ 0.2f, 3.4f, 6.4f }, { 0.0f, -0.35f, 0.0f });
    m_lightAngle = 0.65f;
    m_exposure = 0.0f;

    // Warm-up frames for GPU timestamp stabilization.
    for (int i = 0; i < 4; ++i)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return 1;
    }

    std::vector<uint8_t> onImg, offImg, rotImg, pcfOffImg, biasZeroImg, biasHighImg, studioImg, sunsetImg, debugImg;

    // 1. Default shadow scene ON (Studio HDR, 3x3 PCF).
    m_shadowConfig = ShadowConfig{};
    if (!capture(onImg) || !WriteBitmap("shadow-01-default-on.bmp", extent, onImg, bgra))
        return 1;
    std::cout << "Shadow pass GPU time (2048x2048): " << m_renderer.GetLastShadowGpuTimeMs() << " ms\n";

    // 2. Default shadow scene OFF.
    m_shadowConfig.enabled = false;
    if (!capture(offImg) || !WriteBitmap("shadow-02-default-off.bmp", extent, offImg, bgra))
        return 1;
    const size_t onOffDiff = ChangedPixels(onImg, offImg);
    std::cout << "Shadow ON vs OFF changed pixels: " << onOffDiff << '\n';
    if (onOffDiff < 2000 || !(MeanLuminance(onImg, bgra) < MeanLuminance(offImg, bgra)))
        return 1;

    // 3. Rotated directional light.
    m_shadowConfig.enabled = true;
    m_lightAngle = 2.15f;
    if (!capture(rotImg) || !WriteBitmap("shadow-04-rotated-light.bmp", extent, rotImg, bgra))
        return 1;
    if (ChangedPixels(onImg, rotImg) < 2000)
        return 1;
    m_lightAngle = 0.65f;

    // 4. PCF OFF (hard shadow) vs PCF ON comparison.
    m_shadowConfig.pcfEnabled = false;
    if (!capture(pcfOffImg) || !WriteBitmap("shadow-05-pcf-off.bmp", extent, pcfOffImg, bgra))
        return 1;
    const size_t pcfDiff = ChangedPixels(onImg, pcfOffImg);
    std::cout << "PCF ON vs OFF changed pixels: " << pcfDiff << '\n';
    if (pcfDiff < 50)
        return 1;
    m_shadowConfig.pcfEnabled = true;

    // 5. Bias comparison (zero bias vs controlled vs exaggerated high bias).
    m_shadowConfig.constantDepthBias = 0.0f;
    m_shadowConfig.slopeDepthBias = 0.0f;
    m_shadowConfig.receiverDepthBias = 0.0f;
    m_shadowConfig.normalBias = 0.0f;
    if (!capture(biasZeroImg) || !WriteBitmap("shadow-06-bias-zero.bmp", extent, biasZeroImg, bgra))
        return 1;

    m_shadowConfig = ShadowConfig{};
    m_shadowConfig.constantDepthBias = 12.0f;
    m_shadowConfig.slopeDepthBias = 16.0f;
    m_shadowConfig.receiverDepthBias = 0.02f;
    m_shadowConfig.normalBias = 0.12f;
    if (!capture(biasHighImg) || !WriteBitmap("shadow-06b-bias-high.bmp", extent, biasHighImg, bgra))
        return 1;
    if (ChangedPixels(onImg, biasHighImg) < 200)
        return 1;

    // 6. HDR Studio & Sunset environments with shadows.
    m_shadowConfig = ShadowConfig{};
    if (!SelectStudioEnvironment(false) || !capture(studioImg) ||
        !WriteBitmap("shadow-07-studio.bmp", extent, studioImg, bgra))
        return 1;
    if (!SelectStudioEnvironment(true) || !capture(sunsetImg) ||
        !WriteBitmap("shadow-08-sunset.bmp", extent, sunsetImg, bgra))
        return 1;
    if (ChangedPixels(studioImg, sunsetImg) < 2000)
        return 1;

    // 7. Shadow map depth debug view.
    m_shadowConfig.debugView = true;
    if (!capture(debugImg) || !WriteBitmap("shadow-09-debug-depth.bmp", extent, debugImg, bgra))
        return 1;
    if (ChangedPixels(sunsetImg, debugImg) < 2000)
        return 1;
    m_shadowConfig.debugView = false;

    // 8. DamagedHelmet.glb casting and receiving shadows (if present in working directory).
    if (!SelectStudioEnvironment(false))
        return 1;
    if (std::filesystem::exists("DamagedHelmet.glb"))
    {
        auto imported = ImportGltf("DamagedHelmet.glb");
        if (!imported.asset)
            return 1;
        const auto err = AddImportedAsset(BuildShadowTestScene(std::move(*imported.asset)));
        if (!err.empty())
            return 1;
        m_camera.LookAt({ -1.8f, 1.4f, 3.6f }, { 0.1f, -0.2f, 0.0f });
        m_lightAngle = 0.55f;
        std::vector<uint8_t> helmetOn, helmetOff;
        m_shadowConfig = ShadowConfig{};
        if (!capture(helmetOn) || !WriteBitmap("shadow-03-helmet.bmp", extent, helmetOn, bgra))
            return 1;
        m_shadowConfig.enabled = false;
        if (!capture(helmetOff))
            return 1;
        const size_t helmetDiff = ChangedPixels(helmetOn, helmetOff);
        std::cout << "DamagedHelmet shadow ON vs OFF changed pixels: " << helmetDiff << '\n';
        if (helmetDiff < 1500)
            return 1;
    }

    std::cout << "Shadow visual verification and captures completed\n";
    return 0;
}
