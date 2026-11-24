#include <Core/AetherApplication.h>
#include <Assets/MaterialStudy.h>

#define NOMINMAX
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <utility>

namespace
{
    bool DropFile(GLFWwindow* window, const std::filesystem::path& file)
    {
        const auto path = std::filesystem::absolute(file).wstring();
        const auto size = (path.size() + 2) * sizeof(wchar_t);
        auto block = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + size);
        if (!block)
            return false;
        auto* data = static_cast<DROPFILES*>(GlobalLock(block));
        if (!data)
        {
            GlobalFree(block);
            return false;
        }
        data->pFiles = sizeof(DROPFILES);
        data->fWide = TRUE;
        std::memcpy(data + 1, path.data(), path.size() * sizeof(wchar_t));
        GlobalUnlock(block);
        if (PostMessageW(glfwGetWin32Window(window), WM_DROPFILES, reinterpret_cast<WPARAM>(block), 0))
            return true;
        GlobalFree(block);
        return false;
    }

    bool WriteBitmap(const char* name, VkExtent2D extent, std::vector<uint8_t> pixels, bool bgra)
    {
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
        output.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
        return static_cast<bool>(output);
    }

    size_t ChangedPixels(const std::vector<uint8_t>& first, const std::vector<uint8_t>& second)
    {
        size_t changed = 0;
        for (size_t i = 0; i < first.size(); i += 4)
            if (std::abs(int(first[i]) - second[i]) > 3 ||
                std::abs(int(first[i + 1]) - second[i + 1]) > 3 ||
                std::abs(int(first[i + 2]) - second[i + 2]) > 3)
                ++changed;
        return changed;
    }

    bool WriteDirectionalHdr(const std::filesystem::path& path)
    {
        constexpr int width = 64, height = 32;
        constexpr float pi = 3.14159265f;
        std::ofstream output(path, std::ios::binary);
        output << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " << height << " +X " << width << '\n';
        for (int y = 0; y < height; ++y)
        {
            const float polar = pi * (y + 0.5f) / height;
            for (int x = 0; x < width; ++x)
            {
                const float azimuth = 2 * pi * ((x + 0.5f) / width - 0.5f);
                const unsigned char rgbe[] = {
                    static_cast<unsigned char>(96 + 72 * std::cos(polar)),
                    static_cast<unsigned char>(96 + 72 * std::sin(polar) * std::cos(azimuth)),
                    static_cast<unsigned char>(96 + 72 * std::sin(polar) * std::sin(azimuth)),
                    129
                };
                output.write(reinterpret_cast<const char*>(rgbe), sizeof(rgbe));
            }
        }
        return static_cast<bool>(output);
    }

    // Use a sky-only frame to distinguish pixels covered by the mesh from background pixels.
    bool ChangedMeshAndSky(const std::vector<uint8_t>& before, const std::vector<uint8_t>& after,
                          const std::vector<uint8_t>& sky, bool brighter)
    {
        size_t meshChanges = 0, skyChanges = 0;
        for (size_t i = 0; i < before.size(); i += 4)
        {
            int meshDifference = 0, frameDifference = 0;
            for (size_t channel = 0; channel < 3; ++channel)
            {
                meshDifference += std::abs(int(before[i + channel]) - sky[i + channel]);
                const int delta = int(after[i + channel]) - before[i + channel];
                frameDifference += brighter ? delta : std::abs(delta);
            }
            if (frameDifference > 12)
                ++(meshDifference > 12 ? meshChanges : skyChanges);
        }
        return meshChanges > 100 && skyChanges > 1000;
    }
}

int Aether::AetherApplication::RunPbrVisualTest(bool helmet)
{
    if (!Initialize(false))
        return 1;
    // PBR/Skybox pixel-exact regression assertions expect unjittered single-frame determinism.
    m_taaConfig.enabled = false;
    if (!(m_vulkanContext.m_swapchain.image_usage_flags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
    {
        std::cerr << "Surface does not support frame readback\n";
        return 1;
    }
    auto extent = m_vulkanContext.GetSwapchainExtent();
    const auto format = m_vulkanContext.GetSwapchainImageFormat();
    const bool bgra = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
    if (!bgra && format != VK_FORMAT_R8G8B8A8_SRGB && format != VK_FORMAT_R8G8B8A8_UNORM)
        return 1;
    const auto device = m_vulkanContext.GetDevice();
    VulkanBuffer readback;
    size_t bytes = 0;
    const auto initializeReadback = [&] {
        bytes = static_cast<size_t>(extent.width) * extent.height * 4;
        readback.Shutdown();
        return readback.Initialize(device, m_memoryManager, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT) == VK_SUCCESS;
    };
    if (!initializeReadback())
        return 1;

    const auto capture = [&](std::vector<uint8_t>& pixels) {
        m_window.PollEvents();
        m_renderer.m_captureTarget = readback.GetBuffer();
        const auto result = DrawScene();
        m_renderer.m_captureTarget = VK_NULL_HANDLE;
        pixels.resize(bytes);
        return vkDeviceWaitIdle(device) == VK_SUCCESS && result == VulkanRenderer::FrameResult::Rendered &&
            readback.ReadData(pixels.data(), bytes) == VK_SUCCESS;
    };
    const auto display = [&](ImportedAsset asset, std::vector<uint8_t>& pixels) {
        const auto error = AddImportedAsset(std::move(asset));
        if (!error.empty())
            std::cerr << error << '\n';
        return error.empty() && capture(pixels);
    };
    const auto captureSky = [&](std::vector<uint8_t>& pixels) {
        auto scene = std::exchange(m_scene, Scene{});
        const bool success = capture(pixels);
        m_scene = std::move(scene);
        return success;
    };
    const auto drop = [&](const std::filesystem::path& path) {
        if (!DropFile(m_window.GetNativeHandle(), path))
            return false;
        m_window.PollEvents();
        ProcessDroppedAssets();
        return true;
    };

    std::vector<uint8_t> demo, baseline, variant;
    if (m_scene.GetObjects().size() != 3 || !capture(demo))
        return 1;

    ImportedAsset asset;
    if (helmet)
    {
        if (!DropFile(m_window.GetNativeHandle(), "DamagedHelmet.glb"))
            return 1;
        m_window.PollEvents();
        ProcessDroppedAssets();
        if (!m_previewAsset || m_scene.GetObjects().size() != 1 || !capture(baseline) ||
            !WriteBitmap("phase4-helmet.bmp", extent, baseline, bgra))
            return 1;
        asset = *m_previewAsset;
    }
    else
    {
        auto fixture = ImportGltf(std::filesystem::path("asset-fixtures") / "embedded.glb");
        if (!fixture.asset)
            return 1;
        asset = std::move(*fixture.asset);
        asset.roots = {2};
        asset.meshes[0].resize(1);
        auto& material = asset.materials.front();
        material.baseColor = glm::vec4(0.8f, 0.6f, 0.35f, 1.0f);
        material.metallicRoughnessTexture.reset();
        material.metallic = 0.45f;
        material.roughness = 0.35f;
        material.alphaMode = ImportedMaterial::AlphaMode::Opaque;
        if (!display(asset, baseline) || m_scene.GetObjects().size() != 1)
            return 1;
    }

    if (ChangedPixels(demo, baseline) < 1000)
        return 1;

    const auto compare = [&](const char* label, const auto& edit) {
        ImportedAsset changed = asset;
        for (auto& material : changed.materials)
            edit(material);
        if (!display(std::move(changed), variant))
            return false;
        const auto count = ChangedPixels(baseline, variant);
        std::cout << label << ": " << count << " changed pixels\n";
        return count > 32;
    };
    if (!compare("Normal map", [](auto& m) { m.normalTexture.reset(); }) ||
        !compare("Metallic", [](auto& m) { m.metallic = 0; }) ||
        !compare("Roughness", [](auto& m) { m.roughness = 0.95f; m.metallicRoughnessTexture.reset(); }))
        return 1;

    if (!display(asset, baseline))
        return 1;
    if (helmet && !WriteBitmap("skybox-studio-helmet.bmp", extent, baseline, bgra))
        return 1;

    const auto view = m_camera.GetViewMatrix();
    const auto projection = m_camera.GetProjectionMatrix();
    std::vector<uint8_t> studioSky, sunsetSky;
    if (!captureSky(studioSky) || ChangedPixels(baseline, studioSky) < 100)
        return 1;
    if (helmet)
    {
        const auto camera = m_camera;
        const auto target = m_camera.GetTarget();
        const float distance = glm::distance(m_camera.GetPosition(), target);
        // Look toward a Studio light above the horizon, outside the default preview's framing.
        const auto direction = glm::normalize(glm::vec3(0.3f, 0.55f, -1.0f));
        m_camera.LookAt(target - direction * distance, target);
        if (!capture(variant) || !WriteBitmap("skybox-studio-orbit-helmet.bmp", extent, variant, bgra))
            return 1;
        m_camera = camera;
    }
    else
    {
        auto translucent = asset;
        translucent.materials.front().baseColor.a = 0.45f;
        translucent.materials.front().alphaMode = ImportedMaterial::AlphaMode::Blend;
        if (!display(std::move(translucent), variant))
            return 1;
        size_t blendedPixels = 0;
        for (size_t i = 0; i < variant.size(); i += 4)
        {
            for (size_t channel = 0; channel < 3; ++channel)
            {
                const auto [low, high] = std::minmax(studioSky[i + channel], baseline[i + channel]);
                if (variant[i + channel] > low + 4 && variant[i + channel] < high - 4)
                {
                    ++blendedPixels;
                    break;
                }
            }
        }
        if (blendedPixels < 100 || !display(asset, baseline))
            return 1;
        std::cout << "Transparent material blended over the skybox\n";
    }
    const auto* model = m_meshes.front().get();
    if (!SelectStudioEnvironment(true) || !captureSky(sunsetSky) ||
        ChangedPixels(studioSky, sunsetSky) < 1500 || !capture(variant) ||
        !ChangedMeshAndSky(baseline, variant, studioSky, false))
        return 1;

    const std::filesystem::path directionalHdr = "skybox-directional-fixture.hdr";
    if (!WriteDirectionalHdr(directionalHdr))
        return 1;
    const auto externalHdr = helmet ? std::filesystem::path("skybox-external.hdr") : directionalHdr;
    const auto sunsetDescriptor = m_environment->Descriptor();
    baseline = variant;
    if (!drop(externalHdr) || m_environment->Descriptor() == sunsetDescriptor ||
        m_meshes.front().get() != model || m_camera.GetViewMatrix() != view ||
        m_camera.GetProjectionMatrix() != projection ||
        !capture(variant) || !ChangedMeshAndSky(baseline, variant, sunsetSky, false))
        return 1;
    if (helmet && !WriteBitmap("skybox-external-helmet.bmp", extent, variant, bgra))
        return 1;

    const std::filesystem::path invalidHdr = "skybox-invalid-fixture.hdr";
    {
        std::ofstream invalid(invalidHdr);
        invalid << "This HDR file is invalid.\n";
        if (!invalid)
            return 1;
    }
    baseline = variant;
    const auto externalDescriptor = m_environment->Descriptor();
    if (!drop(invalidHdr) || m_environment->Descriptor() != externalDescriptor ||
        !capture(variant) || ChangedPixels(baseline, variant) != 0)
        return 1;
    std::vector<uint8_t> externalSky;
    if (!captureSky(externalSky))
        return 1;
    m_exposure = 1;
    if (!capture(variant) || !ChangedMeshAndSky(baseline, variant, externalSky, true))
        return 1;
    m_exposure = 0;

    const auto originalCamera = m_camera;
    if (helmet && !drop(directionalHdr))
        return 1;
    m_camera.LookAt({0, 0, 8}, {0, 0, 0});
    if (!captureSky(baseline))
        return 1;
    const auto sample = [&](const std::vector<uint8_t>& pixels, uint32_t x, uint32_t y, unsigned channel) {
        return pixels[(static_cast<size_t>(y) * extent.width + x) * 4 + (bgra ? 2 - channel : channel)];
    };
    if (sample(baseline, extent.width / 2, extent.height / 4, 0) <=
            sample(baseline, extent.width / 2, extent.height * 3 / 4, 0) + 10 ||
        sample(baseline, extent.width * 3 / 4, extent.height / 2, 1) <=
            sample(baseline, extent.width / 4, extent.height / 2, 1) + 10)
        return 1;

    m_cameraController.Pan(m_camera, 3, 2);
    if (!captureSky(variant) || ChangedPixels(baseline, variant) != 0)
        return 1;
    m_cameraController.Orbit(m_camera, 1.0f, 0.2f);
    if (!captureSky(variant) || ChangedPixels(baseline, variant) < 1500)
        return 1;

    if (!helmet)
    {
        const auto oldExtent = extent;
        const auto pipeline = m_renderer.m_skyboxPipeline.GetPipeline();
        glfwSetWindowSize(m_window.GetNativeHandle(), 900, 600);
        m_window.PollEvents();
        if (!RecreateSwapchain())
            return 1;
        extent = m_vulkanContext.GetSwapchainExtent();
        if ((extent.width == oldExtent.width && extent.height == oldExtent.height) ||
            m_renderer.m_skyboxPipeline.GetPipeline() != pipeline ||
            !initializeReadback() || !captureSky(variant) || !capture(demo) ||
            ChangedPixels(variant, demo) < 100)
            return 1;
        m_camera = originalCamera;
        UpdateCameraAspect();
    }
    else
        m_camera = originalCamera;
    if (!SelectStudioEnvironment(false) || !capture(baseline))
        return 1;
    std::cout << "Skybox orientation, pan, orbit, mesh compositing, exposure and HDR drop rollback verified\n";

    if (helmet)
    {
        if (!compare("Occlusion", [](auto& m) { m.occlusionTexture.reset(); }) ||
            !compare("Emissive", [](auto& m) { m.emissive = glm::vec3(0); }) ||
            !compare("Base color", [](auto& m) { m.baseColorTexture.reset(); }))
            return 1;
        if (!display(std::move(asset), variant))
            return 1;
        m_lightAngle += 1.5f;
        if (!capture(variant) || ChangedPixels(baseline, variant) < 100 ||
            !WriteBitmap("phase4-helmet-relit.bmp", extent, variant, bgra))
            return 1;
        m_cameraController.Orbit(m_camera, 0.7f, -0.15f);
        if (!capture(variant) || ChangedPixels(baseline, variant) < 100 ||
            !WriteBitmap("phase4-helmet-orbit.bmp", extent, variant, bgra))
            return 1;
    }

    m_camera.LookAt({0,0,10},{0,0,0});
    auto study=MakeMaterialStudy(helmet?*m_previewAsset:ImportedAsset{});
    if(!display(std::move(study),baseline)) return 1;
    if(helmet && !WriteBitmap("ibl-studio.bmp",extent,baseline,bgra)) return 1;
    if(!DropFile(m_window.GetNativeHandle(),std::filesystem::path(AETHER_ENVIRONMENT_DIR)/"sunset.hdr")) return 1;
    m_window.PollEvents();
    ProcessDroppedAssets();
    if(!capture(variant) || ChangedPixels(baseline,variant)<1500) return 1;
    if(helmet && !WriteBitmap("ibl-sunset.bmp",extent,variant,bgra)) return 1;
    baseline=variant;
    m_exposure=1;
    if(!capture(variant) || ChangedPixels(baseline,variant)<1500) return 1;
    if(helmet && !WriteBitmap("ibl-sunset-ev1.bmp",extent,variant,bgra)) return 1;
    std::cout << "Environment switch, material chart and exposure verified\n";
    std::cout << "PBR frame readback verified\n";
    return 0;
}
