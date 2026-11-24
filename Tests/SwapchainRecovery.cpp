#include <Core/AetherApplication.h>
#include <Assets/ImageDecoder.h>
#include <array>
#include <Renderer/MeshData.h>
#include <GLFW/glfw3.h>

#ifdef _WIN32
#define NOMINMAX
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <shlobj.h>
#include <cstring>
#endif

#include <algorithm>
#include <cmath>
#include <iostream>

int Aether::AetherApplication::RunSwapchainTest(std::string_view scenario)
{
    if (scenario == "pbr-visual" || scenario == "helmet-preview")
        return RunPbrVisualTest(scenario == "helmet-preview");
    if (scenario == "shadow-gpu")
        return RunShadowGpuTest();
    if (scenario == "shadow-visual")
        return RunShadowVisualTest();
    if (scenario == "bloom-gpu")
        return RunBloomGpuTest();
    if (scenario == "bloom-visual")
        return RunBloomVisualTest();
    if (scenario == "taa-gpu")
        return RunTaaGpuTest();
    if (scenario == "taa-visual")
        return RunTaaVisualTest();
    if (scenario == "rt-foundation")
        return RunRayTracingFoundationTest();
    if (scenario == "rt-as")
        return RunRayTracingAsTest();
    if (scenario == "rt-rayquery")
        return RunRayQueryTest();
    if (scenario == "rt-visual")
        return RunRayQueryVisualTest();
    if (scenario == "rt-shadow")
        return RunRtShadowTest();
    if (scenario == "rt-reflection")
        return RunRtReflectionTest();
    if (scenario == "rt-denoise")
        return RunRtDenoiserTest();
    if (scenario == "rt-hybrid")
        return RunHybridRtTest();

    const bool multiMesh = scenario == "multi-mesh";
    if (!Initialize(!multiMesh))
        return 1;

    if (scenario == "mesh-upload-failure")
    {
        const auto allocationCount = [this] { return m_memoryManager.GetAllocationCountForTest(); };
        const auto baseline = allocationCount();
        VulkanMesh mesh;
        const auto data = CreateCubeMesh();
        const auto device = m_vulkanContext.GetDevice();
        const auto empty = [&] {
            return !mesh.GetVertexBuffer().GetBuffer() && !mesh.GetIndexBuffer().GetBuffer() &&
                mesh.GetIndexCount() == 0 && allocationCount() == baseline;
        };
        // The injected result is reached only after vertex upload and index buffer allocation.
        if (mesh.Initialize(device, m_memoryManager, data, true) != VK_ERROR_UNKNOWN || !empty())
            return 1;
        mesh.Shutdown();
        if (!empty() || mesh.Initialize(device, m_memoryManager, data) != VK_SUCCESS ||
            !mesh.GetVertexBuffer().GetBuffer() || !mesh.GetIndexBuffer().GetBuffer() ||
            mesh.GetIndexCount() != data.indices.size() || allocationCount() != baseline + 2)
            return 1;
        mesh.Shutdown();
        if (!empty())
            return 1;
        std::cout << "Mesh upload rollback and retry verified\n";
        return 0;
    }

    const auto draw = [this] {
        m_window.PollEvents();
        return DrawScene() == VulkanRenderer::FrameResult::Rendered;
    };
#ifdef _WIN32
    if (scenario == "asset-drop")
    {
        const auto parent = std::filesystem::absolute("asset-fixtures");
        std::wstring paths;
        for (const auto* name : {L"broken.glb", L"embedded.glb", L"\u6a21\u578b.gltf"})
        {
            paths += (parent / name).wstring();
            paths.push_back(L'\0');
        }
        paths.push_back(L'\0');
        const auto size = paths.size() * sizeof(wchar_t);
        HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + size);
        if (!block)
            return 1;
        auto* drop = static_cast<DROPFILES*>(GlobalLock(block));
        if (!drop)
        {
            GlobalFree(block);
            return 1;
        }
        drop->pFiles = sizeof(DROPFILES);
        drop->fWide = TRUE;
        std::memcpy(drop + 1, paths.data(), size);
        GlobalUnlock(block);
        // GLFW handles WM_DROPFILES and releases the payload with DragFinish.
        if (!PostMessageW(glfwGetWin32Window(m_window.GetNativeHandle()), WM_DROPFILES,
                          reinterpret_cast<WPARAM>(block), 0))
        {
            GlobalFree(block);
            return 1;
        }
        const auto meshesBefore = m_meshes.size();
        const auto targetBefore = m_camera.GetTarget();
        m_window.PollEvents();
        if (m_meshes.size() != meshesBefore || m_previewAsset.has_value())
            return 1;
        ProcessDroppedAssets();
        if (!m_previewAsset || m_meshes.size() != 3 ||
            m_scene.GetObjects().size() != 5 || m_camera.GetTarget() == targetBefore || !draw())
            return 1;
        std::cout << "Win32 file drop, failure recovery and UTF-8 paths verified\n";
        return 0;
    }
#endif

    if (scenario == "environment-compute")
    {
        const auto baseline=m_memoryManager.GetAllocationCountForTest();
        {
            HdrImage white{8,4,std::vector<glm::vec4>(32,glm::vec4(4,2,1,1))};
            VulkanEnvironment environment;
            std::vector<glm::vec4> result;
            if (m_renderer.UploadEnvironment(environment,white)!=VK_SUCCESS ||
                m_renderer.PrecomputeEnvironment(environment,&result)!=VK_SUCCESS) return 1;
            const auto lut=result.size()-128*128;
            for(size_t i=0;i<lut;++i)
                if(glm::length(glm::vec3(result[i])-glm::vec3(4,2,1))>.002f) return 1;
            for(size_t i=lut;i<result.size();++i)
                if(!std::isfinite(result[i].x+result[i].y) || result[i].x<0 || result[i].y<0 ||
                    result[i].x+result[i].y>1.1f) return 1;
            if(result[lut+127].x<.95f || result[lut].y<.5f) return 1;
        }
        const auto previous=m_environment->Descriptor();
        if(SwitchEnvironment(LoadStudioEnvironment(true),true) || previous!=m_environment->Descriptor() ||
            m_memoryManager.GetAllocationCountForTest()!=baseline) return 1;
        if(!SwitchEnvironment(LoadStudioEnvironment(true)) || previous==m_environment->Descriptor()) return 1;
        return m_memoryManager.GetAllocationCountForTest()==baseline ? 0 : 1;
    }
    if (scenario == "texture-upload")
    {
        const auto imported = ImportGltf(std::filesystem::path("asset-fixtures") / "embedded.glb");
        if (!imported.asset)
            return 1;
        try
        {
            const auto decoded = DecodeImage(imported.asset->images.at(0).bytes);
            if (decoded.width != 1 || decoded.height != 1 || decoded.rgba.size() != 4)
                return 1;
        }
        catch (const std::exception& error)
        {
            std::cerr << error.what() << '\n';
            return 1;
        }
        constexpr std::array<std::byte, 4 * 8 * 4> pixels{};
        const auto baseline = m_memoryManager.GetAllocationCountForTest();
        VulkanTexture texture;
        const auto empty = [&] {
            return !texture.GetImage().GetImage() && !texture.GetImage().GetView() &&
                !texture.GetSampler() && m_memoryManager.GetAllocationCountForTest() == baseline;
        };
        if (m_renderer.UploadTexture(texture, {8, 4}, true, pixels, {}, true) != VK_ERROR_UNKNOWN || !empty())
            return 1;
        if (m_renderer.UploadTexture(texture, {8, 4}, true, pixels, {}) != VK_SUCCESS ||
            texture.GetImage().GetFormat() != VK_FORMAT_R8G8B8A8_SRGB || texture.GetImage().GetMipLevels() != 4 ||
            !texture.GetImage().GetView() || !texture.GetSampler() || m_memoryManager.GetAllocationCountForTest() != baseline + 1)
            return 1;
        texture.Shutdown();
        TextureSampling noMip;
        noMip.mipmapped = false;
        if (!empty() || m_renderer.UploadTexture(texture, {8, 4}, false, pixels, noMip) != VK_SUCCESS ||
            texture.GetImage().GetFormat() != VK_FORMAT_R8G8B8A8_UNORM || texture.GetImage().GetMipLevels() != 1)
            return 1;
        texture.Shutdown();
        if (!empty())
            return 1;
        std::cout << "Image decoding, mipmaps, texture formats and rollback verified\n";
        return 0;
    }

    if (scenario == "asset-upload")
    {
        auto imported = ImportGltf(std::filesystem::path("asset-fixtures") / "embedded.glb");
        if (!imported.asset)
        {
            std::cerr << imported.error << '\n';
            return 1;
        }
        const auto baseline = m_memoryManager.GetAllocationCountForTest();
        const auto previousMeshes = m_meshes.size();
        const auto previousObjects = m_scene.GetObjects().size();
        const auto failure = AddImportedAsset(std::move(*imported.asset), 1);
        if (failure.find("mesh 0 primitive 1: GPU upload failed") == std::string::npos ||
            m_memoryManager.GetAllocationCountForTest() != baseline ||
            m_meshes.size() != previousMeshes || m_scene.GetObjects().size() != previousObjects ||
            m_previewAsset.has_value() || !draw())
            return 1;

        auto secondMaterial = imported.asset->materials.front();
        secondMaterial.alphaMode = ImportedMaterial::AlphaMode::Blend;
        imported.asset->materials.push_back(secondMaterial);
        imported.asset->meshes[0][1].material = 1;
        imported.asset->meshes[0][1].extraTexCoords = imported.asset->meshes[0][0].extraTexCoords;
        if (!AddImportedAsset(std::move(*imported.asset)).empty() ||
            m_meshes.size() != 3 || m_memoryManager.GetAllocationCountForTest() != baseline - 2 * previousMeshes + 12 ||
            !m_previewAsset || m_scene.GetObjects().size() != 5 || !draw())
            return 1;

        if (!m_previewMaterials || m_previewMaterials->GetTextureCount() != 3 ||
            m_previewMaterials->GetSet(0) == m_previewMaterials->GetSet(1) ||
            m_previewMaterials->GetSet(1) == m_previewMaterials->GetSet(std::nullopt))
            return 1;
        const auto previewBuffer = m_meshes.front()->GetVertexBuffer().GetBuffer();
        const auto previewSet = m_previewMaterials->GetSet(0);
        const auto previewAllocations = m_memoryManager.GetAllocationCountForTest();
        auto invalidTexture = ImportGltf(std::filesystem::path("asset-fixtures") / "embedded.glb");
        invalidTexture.asset->images.front().bytes.clear();
        if (AddImportedAsset(std::move(*invalidTexture.asset)).find("Texture 0") == std::string::npos ||
            m_memoryManager.GetAllocationCountForTest() != previewAllocations ||
            m_meshes.front()->GetVertexBuffer().GetBuffer() != previewBuffer ||
            m_previewMaterials->GetSet(0) != previewSet || !draw())
            return 1;

        const auto draws = std::span(m_drawItems);
        // Opaque and masked primitives draw before the two blended instances.
        if (draws[0].mesh != draws[2].mesh || draws[3].mesh != draws[4].mesh ||
            draws[0].mesh == draws[1].mesh || draws[3].mesh == draws[0].mesh ||
            draws[0].mvp == draws[2].mvp || !draws[0].doubleSided || !draws[1].mirrored ||
            !draws[3].alphaBlend || !draws[4].alphaBlend || draws[0].material == draws[3].material)
            return 1;
        const auto* sharedMesh = draws[0].mesh;
        const auto buffer = sharedMesh->GetVertexBuffer().GetBuffer();
        glfwSetWindowSize(m_window.GetNativeHandle(), 900, 600);
        m_window.PollEvents();
        if (!RecreateSwapchain() || !draw() || m_drawItems.front().mesh != sharedMesh ||
            sharedMesh->GetVertexBuffer().GetBuffer() != buffer)
            return 1;
        std::cout << "Asset rollback, shared primitives and resize verified\n";
        return 0;
    }

    if (!draw())
        return 1;

    const auto& mesh = *m_meshes.front();
    const auto vertexBuffer = mesh.GetVertexBuffer().GetBuffer();
    const auto indexBuffer = mesh.GetIndexBuffer().GetBuffer();
    const auto* otherMesh = multiMesh && m_meshes.size() == 2 ? m_meshes[1].get() : nullptr;
    const auto otherVertex = otherMesh ? otherMesh->GetVertexBuffer().GetBuffer() : VK_NULL_HANDLE;
    const auto otherIndex = otherMesh ? otherMesh->GetIndexBuffer().GetBuffer() : VK_NULL_HANDLE;
    if (multiMesh &&
        (!otherMesh || !otherVertex || !otherIndex || otherVertex == vertexBuffer ||
         otherIndex == indexBuffer || mesh.GetIndexCount() != 36 || otherMesh->GetIndexCount() != 12 ||
         m_drawItems.size() != 3 ||
         m_drawItems[0].mesh != &mesh || m_drawItems[1].mesh != &mesh || m_drawItems[2].mesh != otherMesh ||
         m_drawItems[0].mvp == m_drawItems[1].mvp || m_drawItems[0].mvp == m_drawItems[2].mvp ||
         m_drawItems[1].mvp == m_drawItems[2].mvp))
        return 1;

    const auto oldCount = m_vulkanContext.GetSwapchainImages().size();
    const auto oldExtent = m_vulkanContext.GetSwapchainExtent();
    const auto oldFormat = m_vulkanContext.GetSwapchainImageFormat();
    const auto oldPipeline = m_renderer.m_graphicsPipeline.GetPipeline();
    auto& options = m_vulkanContext.m_swapchainTestOptions;
    const auto physical = m_vulkanContext.GetPhysicalDevice();
    const auto surface = m_vulkanContext.m_surface;

    if (scenario == "image-count")
    {
        VkSurfaceCapabilitiesKHR caps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps) != VK_SUCCESS)
            return 1;
        if (caps.maxImageCount && oldCount >= caps.maxImageCount)
        {
            std::cout << "SKIP: surface cannot request more swapchain images\n";
            return 77;
        }
        options.minImageCount = static_cast<uint32_t>(oldCount + 1);
    }
    else if (scenario == "color-format")
    {
        uint32_t count = 0;
        if (vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr) != VK_SUCCESS)
            return 1;
        std::vector<VkSurfaceFormatKHR> formats(count);
        if (vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data()) != VK_SUCCESS)
            return 1;
        auto alternate = std::find_if(formats.begin(), formats.end(), [oldFormat](auto format) {
            return format.format != oldFormat && format.format != VK_FORMAT_UNDEFINED;
        });
        if (alternate == formats.end())
        {
            std::cout << "SKIP: surface exposes no alternate color format\n";
            return 77;
        }
        options.format = *alternate;
    }
    else if (scenario == "resize" || multiMesh)
    {
        glfwSetWindowSize(m_window.GetNativeHandle(), 900, 600);
        m_window.PollEvents();
    }
    else if (scenario == "create-failure")
        options.failCreate = true;
    else if (scenario == "view-failure")
        options.failView = 2;
    else
        return 1;

    const bool recreated = RecreateSwapchain();
    const auto& images = m_vulkanContext.GetSwapchainImages();
    const auto& views = m_vulkanContext.GetSwapchainImageViews();
    if (options.failCreate || options.failView)
    {
        const auto extent = m_vulkanContext.GetSwapchainExtent();
        if (recreated || m_vulkanContext.GetSwapchain() || !images.empty() ||
            !views.empty() || !m_vulkanContext.m_swapchainResources.empty() || extent.width || extent.height ||
            m_vulkanContext.GetSwapchainImageFormat() != VK_FORMAT_UNDEFINED)
            return 1;

        Shutdown();
        if (m_renderer.m_depthAttachment.GetImage() || m_renderer.m_depthAttachment.GetView())
            return 1;
        std::cout << "Rollback verified\n";
        return 0;
    }

    if (!recreated || !m_vulkanContext.GetSwapchain() ||
        views.size() != images.size() || m_renderer.m_currentFrame != 0 ||
        m_syncManager.m_renderFinishedSemaphores.size() != images.size() ||
        m_renderer.m_imageLayouts.size() != images.size())
        return 1;
    for (size_t i = 0; i < images.size(); ++i)
    {
        if (!images[i] || !views[i] || !m_syncManager.GetRenderFinishedSemaphore(static_cast<uint32_t>(i)) ||
            m_renderer.m_imageLayouts[i] != VK_IMAGE_LAYOUT_UNDEFINED)
            return 1;
    }

    const auto extent = m_vulkanContext.GetSwapchainExtent();
    const auto& projection = m_camera.GetProjectionMatrix();
    const float expectedAspect = static_cast<float>(extent.width) / extent.height;
    if (std::abs(-projection[1][1] / projection[0][0] - expectedAspect) > 1e-5f)
        return 1;
    const auto depthExtent = m_renderer.m_depthAttachment.GetExtent();
    if (!m_renderer.m_depthAttachment.GetImage() || !m_renderer.m_depthAttachment.GetView() ||
        depthExtent.width != extent.width || depthExtent.height != extent.height ||
        m_renderer.m_depthLayout != VK_IMAGE_LAYOUT_UNDEFINED)
        return 1;

    const auto newFormat = m_vulkanContext.GetSwapchainImageFormat();
    if (m_renderer.m_pipelineColorFormat != newFormat || !m_renderer.m_graphicsPipeline.GetPipeline())
        return 1;
    if (scenario == "image-count" &&
        (images.size() <= oldCount || oldFormat != newFormat ||
            oldPipeline != m_renderer.m_graphicsPipeline.GetPipeline()))
        return 1;
    if ((scenario == "resize" || multiMesh) &&
        ((extent.width == oldExtent.width && extent.height == oldExtent.height) ||
         oldFormat != newFormat || oldPipeline != m_renderer.m_graphicsPipeline.GetPipeline()))
        return 1;
    if (scenario == "color-format" && newFormat != options.format.format)
        return 1;

    for (size_t i = 0; i < 2 * images.size(); ++i)
    {
            if (!draw())
            return 1;
    }
    if (mesh.GetVertexBuffer().GetBuffer() != vertexBuffer || mesh.GetIndexBuffer().GetBuffer() != indexBuffer)
        return 1;
    if (otherMesh && (otherMesh->GetVertexBuffer().GetBuffer() != otherVertex ||
                      otherMesh->GetIndexBuffer().GetBuffer() != otherIndex))
        return 1;
    if (m_renderer.m_depthLayout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
        return 1;
    Shutdown();
    std::cout << "Recovery verified\n";
    return 0;
}

int Aether::AetherApplication::RunRayTracingFoundationTest()
{
    // Part A: Default startup (enables optional RT on RT-capable GPUs such as RTX 5090).
    if (!Initialize(false))
        return 1;

    const auto& caps = m_vulkanContext.GetRayTracingCapabilities();
    if (m_renderer.IsRayTracingReady() != caps.IsReady() ||
        m_memoryManager.IsBufferDeviceAddressEnabled() != caps.IsBufferDeviceAddressEnabled())
        return 1;

    const auto cubeData = CreateCubeMesh();
    const auto tetraData = CreateTetrahedronMesh();
    if (m_meshes.size() != 2 ||
        m_meshes[0]->GetVertexCount() != cubeData.vertices.size() ||
        m_meshes[0]->GetIndexCount() != cubeData.indices.size() ||
        m_meshes[1]->GetVertexCount() != tetraData.vertices.size() ||
        m_meshes[1]->GetIndexCount() != tetraData.indices.size())
        return 1;

    if (caps.IsHardwareSupported())
    {
        if (!caps.IsDeviceEnabled() || !caps.IsBufferDeviceAddressEnabled() ||
            !caps.AreFunctionsAvailable() || !caps.IsReady() ||
            caps.minScratchOffsetAlignment == 0 ||
            caps.maxGeometryCount == 0 || caps.maxInstanceCount == 0 || caps.maxPrimitiveCount == 0 ||
            !caps.functions.vkCreateAccelerationStructureKHR ||
            !caps.functions.vkDestroyAccelerationStructureKHR ||
            !caps.functions.vkGetAccelerationStructureBuildSizesKHR ||
            !caps.functions.vkCmdBuildAccelerationStructuresKHR ||
            !caps.functions.vkGetAccelerationStructureDeviceAddressKHR)
            return 1;

        constexpr VkBufferUsageFlags expectedRtFlags =
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        for (const auto& mesh : m_meshes)
        {
            if (!mesh->IsRayTracingUsageEnabled() ||
                (mesh->GetVertexBuffer().GetUsage() & (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | expectedRtFlags)) !=
                    (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | expectedRtFlags) ||
                (mesh->GetIndexBuffer().GetUsage() & (VK_BUFFER_USAGE_INDEX_BUFFER_BIT | expectedRtFlags)) !=
                    (VK_BUFFER_USAGE_INDEX_BUFFER_BIT | expectedRtFlags) ||
                mesh->GetVertexBuffer().GetDeviceAddress() == 0 ||
                mesh->GetIndexBuffer().GetDeviceAddress() == 0)
                return 1;
        }
    }

    const auto device = m_vulkanContext.GetDevice();
    const auto baselineAllocs = m_memoryManager.GetAllocationCountForTest();

    // Uninitialized buffer BDA check.
    {
        VulkanBuffer uninitialized;
        if (uninitialized.GetUsage() != 0 || uninitialized.HasDeviceAddressUsage() ||
            uninitialized.GetDeviceAddress() != 0)
            return 1;
    }

    // Ordinary non-BDA buffer must not expose a device address.
    {
        VulkanBuffer plainBuffer;
        if (plainBuffer.Initialize(device, m_memoryManager, 256,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS ||
            plainBuffer.GetUsage() != VK_BUFFER_USAGE_VERTEX_BUFFER_BIT ||
            plainBuffer.HasDeviceAddressUsage() ||
            plainBuffer.GetDeviceAddress() != 0)
            return 1;
    }

    if (caps.IsReady())
    {
        // Explicit BDA buffer returns valid non-zero device address.
        {
            VulkanBuffer bdaBuffer;
            const VkBufferUsageFlags usage =
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            if (bdaBuffer.Initialize(device, m_memoryManager, 256, usage,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != VK_SUCCESS ||
                bdaBuffer.GetUsage() != usage ||
                !bdaBuffer.HasDeviceAddressUsage() ||
                bdaBuffer.GetDeviceAddress() == 0)
                return 1;
        }

        // Ordinary mesh without RT option keeps non-BDA flags.
        {
            VulkanMesh nonRtMesh;
            if (nonRtMesh.Initialize(device, m_memoryManager, cubeData, VulkanMeshCreateOptions{ false }) != VK_SUCCESS ||
                nonRtMesh.GetVertexCount() != cubeData.vertices.size() ||
                nonRtMesh.IsRayTracingUsageEnabled() ||
                nonRtMesh.GetVertexBuffer().GetDeviceAddress() != 0 ||
                nonRtMesh.GetIndexBuffer().GetDeviceAddress() != 0)
                return 1;
        }

        // Bitwise index fault-injection check with RT flags appended (VulkanMesh.cpp L24 fix).
        {
            VulkanMesh failingRtMesh;
            if (failingRtMesh.Initialize(device, m_memoryManager, cubeData, true, true) != VK_ERROR_UNKNOWN ||
                failingRtMesh.GetVertexBuffer().GetBuffer() != VK_NULL_HANDLE ||
                failingRtMesh.GetIndexBuffer().GetBuffer() != VK_NULL_HANDLE ||
                failingRtMesh.GetVertexCount() != 0 ||
                failingRtMesh.GetIndexCount() != 0)
                return 1;
        }
    }

    if (m_memoryManager.GetAllocationCountForTest() != baselineAllocs)
        return 1;

    for (int frame = 0; frame < 4; ++frame)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return 1;
    }

    Shutdown();
    if (m_vulkanContext.GetRayTracingCapabilities().IsReady() ||
        m_vulkanContext.GetRayTracingCapabilities().IsDeviceEnabled() ||
        m_vulkanContext.GetRayTracingCapabilities().AreFunctionsAvailable() ||
        m_memoryManager.IsBufferDeviceAddressEnabled())
        return 1;

    // Part B: Software-disabled RT fallback path verification (non-RT device behavior).
    {
        AetherApplication fallbackApp;
        fallbackApp.m_vulkanContext.m_disableRayTracingForTest = true;
        if (!fallbackApp.Initialize(false))
            return 1;

        const auto& fbCaps = fallbackApp.m_vulkanContext.GetRayTracingCapabilities();
        if (fbCaps.IsDeviceEnabled() || fbCaps.IsBufferDeviceAddressEnabled() ||
            fbCaps.AreFunctionsAvailable() || fbCaps.IsReady() ||
            fbCaps.functions.vkCreateAccelerationStructureKHR != nullptr ||
            fbCaps.functions.vkDestroyAccelerationStructureKHR != nullptr ||
            fbCaps.functions.vkGetAccelerationStructureBuildSizesKHR != nullptr ||
            fbCaps.functions.vkCmdBuildAccelerationStructuresKHR != nullptr ||
            fbCaps.functions.vkGetAccelerationStructureDeviceAddressKHR != nullptr ||
            fallbackApp.m_memoryManager.IsBufferDeviceAddressEnabled() ||
            fallbackApp.m_renderer.IsRayTracingReady())
            return 1;

        for (const auto& mesh : fallbackApp.m_meshes)
        {
            if (mesh->GetVertexCount() == 0 || mesh->GetIndexCount() == 0 ||
                mesh->IsRayTracingUsageEnabled() ||
                mesh->GetVertexBuffer().GetDeviceAddress() != 0 ||
                mesh->GetIndexBuffer().GetDeviceAddress() != 0)
                return 1;
        }

        // Requesting a BDA buffer when BDA is disabled must fail cleanly without leaking or VUID errors.
        const auto fbBaseline = fallbackApp.m_memoryManager.GetAllocationCountForTest();
        {
            VulkanBuffer rejectedBda;
            if (rejectedBda.Initialize(fallbackApp.m_vulkanContext.GetDevice(),
                    fallbackApp.m_memoryManager, 256,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != VK_ERROR_FEATURE_NOT_PRESENT ||
                rejectedBda.GetBuffer() != VK_NULL_HANDLE ||
                rejectedBda.GetDeviceAddress() != 0)
                return 1;
        }
        if (fallbackApp.m_memoryManager.GetAllocationCountForTest() != fbBaseline)
            return 1;

        for (int frame = 0; frame < 4; ++frame)
        {
            fallbackApp.m_window.PollEvents();
            if (fallbackApp.DrawScene() != VulkanRenderer::FrameResult::Rendered)
                return 1;
        }

        fallbackApp.Shutdown();
    }

    std::cout << "Phase 9A Ray Tracing foundation and fallback path verified\n";
    return 0;
}
