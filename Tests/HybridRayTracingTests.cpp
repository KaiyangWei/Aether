#include <Core/AetherApplication.h>
#include <Assets/CornellBoxScene.h>
#include <Assets/GltfImporter.h>
#include <Renderer/MeshData.h>
#include <Renderer/RtLighting.h>
#include <RHI/Vulkan/Resources/VulkanBuffer.h>

#include <GLFW/glfw3.h>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <vector>

namespace
{
    int FailHybrid(const char* testName, const char* step)
    {
        std::cerr << testName << " FAILED at step: " << step << '\n';
        return 1;
    }

    glm::vec4 ReadHalf4Pixel(const std::vector<uint16_t>& data, uint32_t width, uint32_t x, uint32_t y)
    {
        const size_t idx = (static_cast<size_t>(y) * width + x) * 4;
        return glm::vec4(
            glm::unpackHalf1x16(data[idx + 0]),
            glm::unpackHalf1x16(data[idx + 1]),
            glm::unpackHalf1x16(data[idx + 2]),
            glm::unpackHalf1x16(data[idx + 3]));
    }

    glm::vec2 ReadHalf2Pixel(const std::vector<uint16_t>& data, uint32_t width, uint32_t x, uint32_t y)
    {
        const size_t idx = (static_cast<size_t>(y) * width + x) * 2;
        return glm::vec2(
            glm::unpackHalf1x16(data[idx + 0]),
            glm::unpackHalf1x16(data[idx + 1]));
    }

    bool ProjectWorldPointToPixel(
        const glm::mat4& vp,
        VkExtent2D extent,
        const glm::vec3& pt,
        uint32_t& outX,
        uint32_t& outY)
    {
        const glm::vec4 c = vp * glm::vec4(pt, 1.0f);
        if (c.w <= 1e-5f)
            return false;
        const glm::vec3 ndc = glm::vec3(c) / c.w;
        if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f)
            return false;
        const float fx = (ndc.x * 0.5f + 0.5f) * static_cast<float>(extent.width);
        const float fy = (ndc.y * 0.5f + 0.5f) * static_cast<float>(extent.height);
        outX = std::min(extent.width - 1u, static_cast<uint32_t>(std::max(0.0f, fx)));
        outY = std::min(extent.height - 1u, static_cast<uint32_t>(std::max(0.0f, fy)));
        return true;
    }

    // Minimal valid uncompressed 32-bit RGBA PNG generator for deterministic Alpha-MASK WIC decoding.
    std::vector<std::byte> EncodeUncompressedPngRgba(
        uint32_t width,
        uint32_t height,
        const std::vector<uint8_t>& rgba)
    {
        std::vector<uint8_t> out = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };

        auto appendBe32 = [&](std::vector<uint8_t>& dst, uint32_t v) {
            dst.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
            dst.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
            dst.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
            dst.push_back(static_cast<uint8_t>(v & 0xFF));
        };

        auto crc32 = [](const uint8_t* buf, size_t len) -> uint32_t {
            uint32_t c = 0xFFFFFFFFu;
            for (size_t i = 0; i < len; ++i)
            {
                c ^= buf[i];
                for (int b = 0; b < 8; ++b)
                    c = (c >> 1) ^ ((c & 1u) ? 0xEDB88320u : 0u);
            }
            return c ^ 0xFFFFFFFFu;
        };

        auto appendChunk = [&](const char type[4], const std::vector<uint8_t>& payload) {
            appendBe32(out, static_cast<uint32_t>(payload.size()));
            const size_t typeStart = out.size();
            out.insert(out.end(), type, type + 4);
            out.insert(out.end(), payload.begin(), payload.end());
            const uint32_t crc = crc32(out.data() + typeStart, 4 + payload.size());
            appendBe32(out, crc);
        };

        std::vector<uint8_t> ihdr;
        appendBe32(ihdr, width);
        appendBe32(ihdr, height);
        ihdr.push_back(8); // bit depth
        ihdr.push_back(6); // RGBA
        ihdr.push_back(0); // compression
        ihdr.push_back(0); // filter
        ihdr.push_back(0); // interlace
        appendChunk("IHDR", ihdr);

        std::vector<uint8_t> raw;
        raw.reserve(static_cast<size_t>(height) * (1 + width * 4));
        for (uint32_t y = 0; y < height; ++y)
        {
            raw.push_back(0); // filter None
            const size_t rowStart = static_cast<size_t>(y) * width * 4;
            raw.insert(raw.end(), rgba.begin() + rowStart, rgba.begin() + rowStart + width * 4);
        }

        std::vector<uint8_t> zlib;
        zlib.push_back(0x78);
        zlib.push_back(0x01);
        const uint16_t rawLen = static_cast<uint16_t>(raw.size());
        const uint16_t nlen = static_cast<uint16_t>(~rawLen);
        zlib.push_back(0x01); // BFINAL=1, BTYPE=00 (stored)
        zlib.push_back(static_cast<uint8_t>(rawLen & 0xFF));
        zlib.push_back(static_cast<uint8_t>((rawLen >> 8) & 0xFF));
        zlib.push_back(static_cast<uint8_t>(nlen & 0xFF));
        zlib.push_back(static_cast<uint8_t>((nlen >> 8) & 0xFF));
        zlib.insert(zlib.end(), raw.begin(), raw.end());

        uint32_t s1 = 1u, s2 = 0u;
        for (uint8_t byte : raw)
        {
            s1 = (s1 + byte) % 65521u;
            s2 = (s2 + s1) % 65521u;
        }
        const uint32_t adler = (s2 << 16) | s1;
        appendBe32(zlib, adler);

        appendChunk("IDAT", zlib);
        appendChunk("IEND", {});

        std::vector<std::byte> bytes(out.size());
        for (size_t i = 0; i < out.size(); ++i)
            bytes[i] = static_cast<std::byte>(out[i]);
        return bytes;
    }

    Aether::MeshData MakeQuadMesh(float halfW, float halfH, float yPos, bool horizontalXZ)
    {
        Aether::MeshData mesh;
        const glm::vec3 white(1.0f);
        if (horizontalXZ)
        {
            const glm::vec3 n(0.0f, 1.0f, 0.0f);
            const glm::vec4 t(1.0f, 0.0f, 0.0f, 1.0f);
            mesh.vertices.push_back({ { -halfW, yPos,  halfH }, white, n, { 0.0f, 0.0f }, t });
            mesh.vertices.push_back({ {  halfW, yPos,  halfH }, white, n, { 1.0f, 0.0f }, t });
            mesh.vertices.push_back({ {  halfW, yPos, -halfH }, white, n, { 1.0f, 1.0f }, t });
            mesh.vertices.push_back({ { -halfW, yPos, -halfH }, white, n, { 0.0f, 1.0f }, t });
        }
        else
        {
            const glm::vec3 n(0.0f, 0.0f, 1.0f);
            const glm::vec4 t(1.0f, 0.0f, 0.0f, 1.0f);
            mesh.vertices.push_back({ { -halfW, yPos - halfH, 0.0f }, white, n, { 0.0f, 0.0f }, t });
            mesh.vertices.push_back({ {  halfW, yPos - halfH, 0.0f }, white, n, { 1.0f, 0.0f }, t });
            mesh.vertices.push_back({ {  halfW, yPos + halfH, 0.0f }, white, n, { 1.0f, 1.0f }, t });
            mesh.vertices.push_back({ { -halfW, yPos + halfH, 0.0f }, white, n, { 0.0f, 1.0f }, t });
        }
        mesh.indices = { 0, 1, 2, 0, 2, 3 };
        return mesh;
    }

    struct PassStats
    {
        double median = 0.0;
        double p95 = 0.0;
        double minVal = 0.0;
        double maxVal = 0.0;
    };

    PassStats SummarizeSamples(std::vector<double> samples)
    {
        if (samples.empty())
            return {};
        std::sort(samples.begin(), samples.end());
        const size_t n = samples.size();
        const size_t medIdx = n / 2;
        const size_t p95Idx = std::min(n - 1, static_cast<size_t>(std::ceil(0.95 * static_cast<double>(n))) - 1);
        return { samples[medIdx], samples[p95Idx], samples.front(), samples.back() };
    }
}

int Aether::AetherApplication::RunRtShadowTest()
{
    if (!Initialize(true))
        return FailHybrid("rt-shadow", "Initialize");

    if (!m_renderer.CanUseHybridRtMode())
    {
        std::cout << "SKIP: Hardware Ray Tracing / Hybrid RT is not ready on this device\n";
        Shutdown();
        return 77;
    }

    const VkDevice device = m_vulkanContext.GetDevice();
    VkExtent2D extent = m_vulkanContext.GetSwapchainExtent();
    const VkDeviceSize fp16Bytes = VkDeviceSize(extent.width) * extent.height * 4 * sizeof(uint16_t);

    VulkanBuffer directReadback;
    if (directReadback.Initialize(
            device, m_memoryManager, fp16Bytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
        return FailHybrid("rt-shadow", "create fp16 readback buffer");

    auto readDirectFp16 = [&](std::vector<uint16_t>& out) -> bool {
        m_renderer.m_rtShadowReadbackTarget = directReadback.GetBuffer();
        m_window.PollEvents();
        const auto res = DrawScene();
        m_renderer.m_rtShadowReadbackTarget = VK_NULL_HANDLE;
        if (res != VulkanRenderer::FrameResult::Rendered)
            return false;
        m_vulkanContext.WaitIdle();
        out.resize(static_cast<size_t>(extent.width) * extent.height * 4);
        return directReadback.ReadData(out.data(), fp16Bytes) == VK_SUCCESS;
    };

    // ------------------------------------------------------------------
    // 1. Analytical Ground Plane: Unoccluded Bias Check & Hard Blocker Check
    // ------------------------------------------------------------------
    {
        ImportedAsset asset;
        ImportedMaterial floorMat{};
        floorMat.baseColor = { 0.8f, 0.8f, 0.8f, 1.0f };
        floorMat.metallic = 0.0f;
        floorMat.roughness = 0.85f;
        asset.materials.push_back(floorMat);

        ImportedMaterial blockerMat = floorMat;
        blockerMat.baseColor = { 0.3f, 0.3f, 0.35f, 1.0f };
        asset.materials.push_back(blockerMat);

        asset.meshes.push_back({ { MakeQuadMesh(8.0f, 8.0f, 0.0f, true), 0, {} } });
        asset.meshes.push_back({ { CreateCubeMesh(), 1, {} } });

        ImportedNode floorNode{};
        floorNode.name = "Ground";
        floorNode.mesh = 0;
        asset.roots.push_back(0);
        asset.nodes.push_back(floorNode);

        // Include blocker node in initial upload (parked safely below ground) so mesh 1 is uploaded.
        ImportedNode blockerNode{};
        blockerNode.name = "Blocker";
        blockerNode.mesh = 1;
        blockerNode.local = blockerNode.world = glm::translate(glm::mat4(1.0f), { 0.0f, -10.0f, 0.0f });
        asset.roots.push_back(1);
        asset.nodes.push_back(blockerNode);

        if (!AddImportedAsset(std::move(asset)).empty())
            return FailHybrid("rt-shadow", "upload unoccluded ground asset");

        m_renderMode = RenderMode::HybridRayTracing;
        m_hybridConfig = {};
        m_hybridConfig.shadowsEnabled = true;
        m_hybridConfig.reflectionsEnabled = false;
        m_hybridConfig.denoiserEnabled = false;
        m_hybridConfig.areaLightsEnabled = false;
        m_hybridConfig.shadowSamples = 1;
        m_hybridConfig.sunAngularRadiusDeg = 0.0f;
        m_lightAngle = 0.0f; // sunDir = normalize(1.0, 0.8, 0.0)
        m_camera.SetFov(45.0f);
        m_camera.LookAt({ 0.0f, 4.0f, 5.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f });
        ResetTemporalHistory();

        std::vector<uint16_t> fp16;
        if (!readDirectFp16(fp16))
            return FailHybrid("rt-shadow", "readback unoccluded ground");

        uint32_t px = 0, py = 0;
        if (!ProjectWorldPointToPixel(m_camera.GetViewProjectionMatrix(), extent, { 0.0f, 0.0f, 0.0f }, px, py))
            return FailHybrid("rt-shadow", "project origin");
        const glm::vec4 centerUnoccluded = ReadHalf4Pixel(fp16, extent.width, px, py);
        if (centerUnoccluded.a < 0.98f)
        {
            std::cerr << "Unoccluded ground visibility=" << centerUnoccluded.a << " (possible shadow acne)\n";
            return FailHybrid("rt-shadow", "zero self-intersection acne check");
        }

        // Now move uploaded opaque blocker along sunDir (1.0, 0.8, 0.0) * 1.5 -> (1.5, 1.2, 0.0).
        if (m_scene.GetObjects().size() < 2)
            return FailHybrid("rt-shadow", "missing uploaded blocker object");
        auto& blockerObj = m_scene.GetObjects()[1];
        blockerObj.modelMatrix.reset();
        blockerObj.transform.position = { 1.5f, 1.2f, 0.0f };
        blockerObj.transform.scale = { 0.9f, 0.9f, 0.9f };

        if (!readDirectFp16(fp16))
            return FailHybrid("rt-shadow", "readback occluded ground");
        const glm::vec4 centerShadowed = ReadHalf4Pixel(fp16, extent.width, px, py);
        uint32_t litX = 0, litY = 0;
        ProjectWorldPointToPixel(m_camera.GetViewProjectionMatrix(), extent, { 0.0f, 0.0f, -2.5f }, litX, litY);
        const glm::vec4 distantLit = ReadHalf4Pixel(fp16, extent.width, litX, litY);
        if (centerShadowed.a > 0.05f || distantLit.a < 0.95f)
        {
            std::cerr << "Shadowed=" << centerShadowed.a << " Lit=" << distantLit.a << '\n';
            return FailHybrid("rt-shadow", "hard shadow hit/miss discrimination");
        }
    }

    // ------------------------------------------------------------------
    // 2. Procedural Alpha-MASK Cutout Shadow Evaluation
    // ------------------------------------------------------------------
    {
        // Build 4x2 RGBA PNG: left 2 columns opaque (a=255), right 2 columns transparent (a=0).
        std::vector<uint8_t> rgba(4 * 2 * 4, 255);
        for (uint32_t y = 0; y < 2; ++y)
        {
            for (uint32_t x = 0; x < 4; ++x)
            {
                const size_t idx = (y * 4 + x) * 4;
                rgba[idx + 0] = 220;
                rgba[idx + 1] = 220;
                rgba[idx + 2] = 220;
                rgba[idx + 3] = (x < 2) ? 255 : 0;
            }
        }
        ImportedAsset maskAsset;
        maskAsset.images.push_back({ EncodeUncompressedPngRgba(4, 2, rgba), "image/png" });
        maskAsset.samplers.push_back({ 9728, 9728, 33071, 33071 }); // NEAREST + CLAMP_TO_EDGE
        maskAsset.textures.push_back({ 0, 0 });

        ImportedMaterial groundMat{};
        groundMat.baseColor = { 0.8f, 0.8f, 0.8f, 1.0f };
        groundMat.metallic = 0.0f;
        groundMat.roughness = 0.85f;
        maskAsset.materials.push_back(groundMat);

        ImportedMaterial cutoutMat = groundMat;
        cutoutMat.alphaMode = ImportedMaterial::AlphaMode::Mask;
        cutoutMat.alphaCutoff = 0.5f;
        cutoutMat.doubleSided = true;
        cutoutMat.baseColorTexture = ImportedTextureSlot{ 0, 0, 1.0f };
        maskAsset.materials.push_back(cutoutMat);

        maskAsset.meshes.push_back({ { MakeQuadMesh(8.0f, 8.0f, 0.0f, true), 0, {} } });
        maskAsset.meshes.push_back({ { MakeQuadMesh(2.0f, 2.0f, 0.0f, true), 1, {} } });

        ImportedNode nGround{};
        nGround.name = "Ground";
        nGround.mesh = 0;
        maskAsset.roots.push_back(0);
        maskAsset.nodes.push_back(nGround);

        ImportedNode nMask{};
        nMask.name = "AlphaMaskCard";
        nMask.mesh = 1;
        nMask.local = nMask.world = glm::translate(glm::mat4(1.0f), { 1.875f, 1.5f, 0.0f });
        maskAsset.roots.push_back(1);
        maskAsset.nodes.push_back(nMask);

        if (!AddImportedAsset(std::move(maskAsset)).empty())
            return FailHybrid("rt-shadow", "upload procedural alpha mask asset");

        m_renderMode = RenderMode::HybridRayTracing;
        m_hybridConfig.shadowsEnabled = true;
        m_hybridConfig.denoiserEnabled = false;
        m_hybridConfig.areaLightsEnabled = false;
        m_hybridConfig.shadowSamples = 1;
        m_hybridConfig.sunAngularRadiusDeg = 0.0f;
        m_lightAngle = 0.0f; // sunDir = normalize(1, 0.8, 0)
        // Ray from (-1.0, 0, 0) along sunDir reaches y=1.5 at x = -1.0 + 1.5*(1/0.8) = +0.875 -> u in [0, 0.5) (OPAQUE!)
        // Ray from (+1.0, 0, 0) along sunDir reaches y=1.5 at x = +1.0 + 1.875 = +2.875 -> u in [0.5, 1.0] (TRANSPARENT!)
        m_camera.LookAt({ 0.0f, 4.5f, 5.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f });
        ResetTemporalHistory();

        std::vector<uint16_t> fp16;
        if (!readDirectFp16(fp16))
            return FailHybrid("rt-shadow", "readback alpha mask shadows");

        uint32_t ox = 0, oy = 0, tx = 0, ty = 0;
        ProjectWorldPointToPixel(m_camera.GetViewProjectionMatrix(), extent, { -0.6f, 0.0f, 0.0f }, ox, oy);
        ProjectWorldPointToPixel(m_camera.GetViewProjectionMatrix(), extent, {  0.6f, 0.0f, 0.0f }, tx, ty);
        const glm::vec4 opaqueSide = ReadHalf4Pixel(fp16, extent.width, ox, oy);
        const glm::vec4 transSide = ReadHalf4Pixel(fp16, extent.width, tx, ty);
        if (opaqueSide.a > 0.1f || transSide.a < 0.9f)
        {
            std::cerr << "AlphaMask check failed: opaqueSide=" << opaqueSide.a
                      << " transparentSide=" << transSide.a << '\n';
            return FailHybrid("rt-shadow", "alpha mask cutout evaluation");
        }
    }

    // ------------------------------------------------------------------
    // 3. Soft Penumbra Gradient & Stochastic Sample-Count Normalization
    // ------------------------------------------------------------------
    {
        ImportedAsset penAsset;
        ImportedMaterial floorMat{};
        floorMat.baseColor = { 0.82f, 0.82f, 0.82f, 1.0f };
        floorMat.metallic = 0.0f;
        floorMat.roughness = 0.85f;
        penAsset.materials.push_back(floorMat);
        penAsset.meshes.push_back({ { MakeQuadMesh(10.0f, 10.0f, 0.0f, true), 0, {} } });
        penAsset.meshes.push_back({ { CreateCubeMesh(), 0, {} } });

        ImportedNode nGround{};
        nGround.name = "Ground";
        nGround.mesh = 0;
        penAsset.roots.push_back(0);
        penAsset.nodes.push_back(nGround);

        ImportedNode nOccluder{};
        nOccluder.name = "PenumbraOccluder";
        nOccluder.mesh = 1;
        nOccluder.local = nOccluder.world =
            glm::translate(glm::mat4(1.0f), { 0.9f, 1.5f, 0.0f }) *
            glm::scale(glm::mat4(1.0f), { 3.4f, 0.4f, 3.0f });
        penAsset.roots.push_back(1);
        penAsset.nodes.push_back(nOccluder);

        if (!AddImportedAsset(std::move(penAsset)).empty())
            return FailHybrid("rt-shadow", "upload penumbra test asset");

        // Overhead rectangular area light casting soft shadows across the ground plane.
        RtGpuLight overheadArea{};
        overheadArea.posAndType = { 0.0f, 4.0f, 0.0f, static_cast<float>(RtLightType::RectArea) };
        overheadArea.dirAndRange = { 0.0f, -1.0f, 0.0f, 20.0f };
        overheadArea.colorAndIntensity = { 1.0f, 0.98f, 0.94f, 3.0f };
        overheadArea.areaRightAndHalfW = { 1.0f, 0.0f, 0.0f, 1.2f };
        overheadArea.areaUpAndHalfH = { 0.0f, 0.0f, 1.0f, 1.2f };
        m_customSceneLights = { overheadArea };

        m_renderMode = RenderMode::HybridRayTracing;
        m_hybridConfig.shadowsEnabled = true;
        m_hybridConfig.denoiserEnabled = false;
        m_hybridConfig.areaLightsEnabled = true;
        m_hybridConfig.shadowSamples = 16;
        m_hybridConfig.sunAngularRadiusDeg = 6.0f;
        m_camera.LookAt({ 0.0f, 3.6f, 5.2f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f });
        ResetTemporalHistory();

        std::vector<uint16_t> fp16Soft16;
        if (!readDirectFp16(fp16Soft16))
            return FailHybrid("rt-shadow", "readback 16-sample soft penumbra");

        uint32_t ux = 0, uy = 0, lx = 0, ly = 0;
        ProjectWorldPointToPixel(m_camera.GetViewProjectionMatrix(), extent, { 0.0f, 0.0f, 0.0f }, ux, uy);
        ProjectWorldPointToPixel(m_camera.GetViewProjectionMatrix(), extent, { 0.0f, 0.0f, -3.5f }, lx, ly);
        const float umbraVis = ReadHalf4Pixel(fp16Soft16, extent.width, ux, uy).a;
        const float litVis = ReadHalf4Pixel(fp16Soft16, extent.width, lx, ly).a;

        bool sawIntermediatePenumbra = false;
        for (float pzW = -3.0f; pzW <= 0.0f; pzW += 0.12f)
        {
            uint32_t tx = 0, ty = 0;
            if (ProjectWorldPointToPixel(m_camera.GetViewProjectionMatrix(), extent, { 0.0f, 0.0f, pzW }, tx, ty))
            {
                const float v = ReadHalf4Pixel(fp16Soft16, extent.width, tx, ty).a;
                if (v > 0.08f && v < 0.92f)
                {
                    sawIntermediatePenumbra = true;
                    break;
                }
            }
        }
        if (!sawIntermediatePenumbra)
        {
            for (float pxW = -2.2f; pxW <= 2.2f; pxW += 0.12f)
            {
                uint32_t tx = 0, ty = 0;
                if (ProjectWorldPointToPixel(m_camera.GetViewProjectionMatrix(), extent, { pxW, 0.0f, 0.0f }, tx, ty))
                {
                    const float v = ReadHalf4Pixel(fp16Soft16, extent.width, tx, ty).a;
                    if (v > 0.08f && v < 0.92f)
                    {
                        sawIntermediatePenumbra = true;
                        break;
                    }
                }
            }
        }
        if (umbraVis > 0.15f || litVis < 0.90f || !sawIntermediatePenumbra)
        {
            std::cerr << "Penumbra failed: umbra=" << umbraVis << " lit=" << litVis
                      << " intermediate=" << sawIntermediatePenumbra << '\n';
            return FailHybrid("rt-shadow", "soft penumbra gradient check");
        }

        // Sample count normalization check: average open-lit radiance across 1 spp vs 8 spp.
        m_hybridConfig.shadowSamples = 1;
        std::vector<uint16_t> fp16_1spp;
        if (!readDirectFp16(fp16_1spp))
            return FailHybrid("rt-shadow", "readback 1spp normalization");

        m_hybridConfig.shadowSamples = 8;
        std::vector<uint16_t> fp16_8spp;
        if (!readDirectFp16(fp16_8spp))
            return FailHybrid("rt-shadow", "readback 8spp normalization");

        double sum1 = 0.0, sum8 = 0.0;
        int samplesChecked = 0;
        for (int dy = -6; dy <= 6; ++dy)
        {
            for (int dx = -6; dx <= 6; ++dx)
            {
                const uint32_t sx = std::clamp<int>(int(lx) + dx, 0, int(extent.width) - 1);
                const uint32_t sy = std::clamp<int>(int(ly) + dy, 0, int(extent.height) - 1);
                sum1 += ReadHalf4Pixel(fp16_1spp, extent.width, sx, sy).r;
                sum8 += ReadHalf4Pixel(fp16_8spp, extent.width, sx, sy).r;
                ++samplesChecked;
            }
        }
        const double avg1 = sum1 / samplesChecked;
        const double avg8 = sum8 / samplesChecked;
        if (avg8 <= 0.05 || std::abs(avg1 - avg8) / avg8 > 0.15)
        {
            std::cerr << "Sample normalization diverged: avg1=" << avg1 << " avg8=" << avg8 << '\n';
            return FailHybrid("rt-shadow", "sample count normalization check");
        }
        m_customSceneLights.clear();
    }

    // ------------------------------------------------------------------
    // 4. Cornell Box Multi-Light, Resize & Zero VMA Leak
    // ------------------------------------------------------------------
    directReadback.Shutdown();
    if (!LoadDefaultCornellBoxScene())
        return FailHybrid("rt-shadow", "LoadDefaultCornellBoxScene");
    m_renderMode = RenderMode::HybridRayTracing;
    m_hybridConfig = {};
    for (int f = 0; f < 4; ++f)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return FailHybrid("rt-shadow", "Cornell Box Hybrid draw");
    }
    glfwSetWindowSize(m_window.GetNativeHandle(), 960, 540);
    m_window.PollEvents();
    if (!RecreateSwapchain())
        return FailHybrid("rt-shadow", "RecreateSwapchain");
    for (int f = 0; f < 3; ++f)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return FailHybrid("rt-shadow", "draw after resize");
    }

    Shutdown();
    std::cout << "Phase 10 rt.shadow hardware RT shadow test suite passed\n";
    return 0;
}

int Aether::AetherApplication::RunRtReflectionTest()
{
    if (!Initialize(true))
        return FailHybrid("rt-reflection", "Initialize");

    if (!m_renderer.CanUseHybridRtMode())
    {
        std::cout << "SKIP: Hardware Ray Tracing / Hybrid RT is not ready on this device\n";
        Shutdown();
        return 77;
    }

    const VkDevice device = m_vulkanContext.GetDevice();
    VkExtent2D extent = m_vulkanContext.GetSwapchainExtent();
    const VkDeviceSize fp16RgbaBytes = VkDeviceSize(extent.width) * extent.height * 4 * sizeof(uint16_t);
    const VkDeviceSize fp16RgBytes = VkDeviceSize(extent.width) * extent.height * 2 * sizeof(uint16_t);

    VulkanBuffer reflReadback, auxReadback;
    if (reflReadback.Initialize(
            device, m_memoryManager, fp16RgbaBytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS ||
        auxReadback.Initialize(
            device, m_memoryManager, fp16RgBytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
        return FailHybrid("rt-reflection", "create readback buffers");

    auto readReflection = [&](std::vector<uint16_t>& outRefl, std::vector<uint16_t>& outAux) -> bool {
        m_renderer.m_rtReflectionReadbackTarget = reflReadback.GetBuffer();
        m_renderer.m_rtReflectionAuxReadbackTarget = auxReadback.GetBuffer();
        m_window.PollEvents();
        const auto res = DrawScene();
        m_renderer.m_rtReflectionReadbackTarget = VK_NULL_HANDLE;
        m_renderer.m_rtReflectionAuxReadbackTarget = VK_NULL_HANDLE;
        if (res != VulkanRenderer::FrameResult::Rendered)
            return false;
        m_vulkanContext.WaitIdle();
        outRefl.resize(static_cast<size_t>(extent.width) * extent.height * 4);
        outAux.resize(static_cast<size_t>(extent.width) * extent.height * 2);
        return reflReadback.ReadData(outRefl.data(), fp16RgbaBytes) == VK_SUCCESS &&
               auxReadback.ReadData(outAux.data(), fp16RgBytes) == VK_SUCCESS;
    };

    // ------------------------------------------------------------------
    // 1. Analytical Mirror Reflection: Hit Distance, Reflected Emissive Hue & Open-Sky Miss
    // ------------------------------------------------------------------
    {
        ImportedAsset asset;
        ImportedMaterial mirrorMat{};
        mirrorMat.baseColor = { 0.98f, 0.98f, 0.98f, 1.0f };
        mirrorMat.metallic = 1.0f;
        mirrorMat.roughness = 0.02f;
        asset.materials.push_back(mirrorMat);

        ImportedMaterial greenEmitter{};
        greenEmitter.baseColor = { 0.1f, 0.9f, 0.1f, 1.0f };
        greenEmitter.emissive = { 0.0f, 12.0f, 0.0f };
        greenEmitter.metallic = 0.0f;
        greenEmitter.roughness = 0.2f;
        asset.materials.push_back(greenEmitter);

        ImportedMaterial roughMetal{};
        roughMetal.baseColor = { 0.9f, 0.7f, 0.3f, 1.0f };
        roughMetal.metallic = 0.95f;
        roughMetal.roughness = 0.55f;
        asset.materials.push_back(roughMetal);

        asset.meshes.push_back({ { MakeQuadMesh(6.0f, 6.0f, 0.0f, true), 0, {} } });
        asset.meshes.push_back({ { CreateCubeMesh(), 1, {} } });
        asset.meshes.push_back({ { MakeQuadMesh(2.0f, 2.0f, 0.0f, true), 2, {} } });

        ImportedNode mirrorFloor{};
        mirrorFloor.name = "MirrorFloor";
        mirrorFloor.mesh = 0;
        asset.roots.push_back(0);
        asset.nodes.push_back(mirrorFloor);

        // Card centered at (0, 2.0, -2.0), size (6, 6, 0.2) -> front plane z = -1.9.
        // Ray from (0,0,0) in dir (0, 1/sqrt2, -1/sqrt2) hits z = -1.9 at t = 1.9 * sqrt(2) = 2.687m!
        ImportedNode greenCard{};
        greenCard.name = "GreenEmitter";
        greenCard.mesh = 1;
        greenCard.local = greenCard.world =
            glm::translate(glm::mat4(1.0f), { 0.0f, 2.0f, -2.0f }) *
            glm::scale(glm::mat4(1.0f), { 6.0f, 6.0f, 0.2f });
        asset.roots.push_back(1);
        asset.nodes.push_back(greenCard);

        if (!AddImportedAsset(std::move(asset)).empty())
            return FailHybrid("rt-reflection", "upload mirror scene");

        m_renderMode = RenderMode::HybridRayTracing;
        m_hybridConfig = {};
        m_hybridConfig.reflectionsEnabled = true;
        m_hybridConfig.denoiserEnabled = false;
        m_camera.SetFov(45.0f);
        m_camera.LookAt({ 0.0f, 2.0f, 2.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f });
        ResetTemporalHistory();

        std::vector<uint16_t> reflData, auxData;
        if (!readReflection(reflData, auxData))
            return FailHybrid("rt-reflection", "readback mirror reflection");

        uint32_t cx = 0, cy = 0;
        ProjectWorldPointToPixel(m_camera.GetViewProjectionMatrix(), extent, { 0.0f, 0.0f, 0.0f }, cx, cy);
        const glm::vec4 refl = ReadHalf4Pixel(reflData, extent.width, cx, cy);
        const glm::vec2 aux = ReadHalf2Pixel(auxData, extent.width, cx, cy);
        const float expectedDist = 1.9f * std::sqrt(2.0f);
        if (aux.x < 0.5f || std::abs(aux.y - expectedDist) > 0.25f ||
            refl.g <= 1.0f || refl.g <= refl.r * 3.0f || refl.g <= refl.b * 3.0f)
        {
            std::cerr << "Mirror hit mismatch: hit=" << aux.x << " dist=" << aux.y
                      << " expectedDist=" << expectedDist << " rgb=(" << refl.r << "," << refl.g << "," << refl.b << ")\n";
            return FailHybrid("rt-reflection", "mirror distance & reflected hue check");
        }

        // Move GreenEmitter behind camera -> reflection ray misses all geometry -> must fall back to HDR env!
        auto& cardObj = m_scene.GetObjects()[1];
        cardObj.modelMatrix.reset();
        cardObj.transform.position = { 0.0f, 2.0f, 8.0f };
        cardObj.transform.scale = { 6.0f, 6.0f, 0.2f };
        if (!readReflection(reflData, auxData))
            return FailHybrid("rt-reflection", "readback open-sky env fallback");

        const glm::vec4 missRefl = ReadHalf4Pixel(reflData, extent.width, cx, cy);
        const glm::vec2 missAux = ReadHalf2Pixel(auxData, extent.width, cx, cy);
        const float maxChannel = std::max({ missRefl.r, missRefl.g, missRefl.b });
        if (missAux.x > 0.5f || maxChannel <= 0.005f)
        {
            std::cerr << "Miss fallback mismatch: hit=" << missAux.x << " maxEnv=" << maxChannel << '\n';
            return FailHybrid("rt-reflection", "HDR environment fallback on miss check");
        }
    }

    // ------------------------------------------------------------------
    // 2. Cornell Box Mirror & Multi-Roughness Reflections + Resize + Leak Check
    // ------------------------------------------------------------------
    reflReadback.Shutdown();
    auxReadback.Shutdown();
    if (!LoadDefaultCornellBoxScene())
        return FailHybrid("rt-reflection", "LoadDefaultCornellBoxScene");

    m_renderMode = RenderMode::HybridRayTracing;
    m_hybridConfig = {};
    m_camera.LookAt({ 1.45f, 1.78f, 3.25f }, { -1.25f, 1.38f, -0.05f }, { 0.0f, 1.0f, 0.0f });
    ResetTemporalHistory();
    for (int f = 0; f < 4; ++f)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return FailHybrid("rt-reflection", "Cornell Box reflection draw");
    }

    glfwSetWindowSize(m_window.GetNativeHandle(), 960, 540);
    m_window.PollEvents();
    if (!RecreateSwapchain())
        return FailHybrid("rt-reflection", "RecreateSwapchain");
    for (int f = 0; f < 3; ++f)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return FailHybrid("rt-reflection", "draw after resize");
    }

    Shutdown();
    std::cout << "Phase 11 rt.reflection hardware RT reflection test suite passed\n";
    return 0;
}

int Aether::AetherApplication::RunRtDenoiserTest()
{
    if (!Initialize(false))
        return FailHybrid("rt-denoise", "Initialize");

    if (!m_renderer.CanUseHybridRtMode())
    {
        std::cout << "SKIP: Hardware Ray Tracing / Hybrid RT is not ready on this device\n";
        Shutdown();
        return 77;
    }

    if (!LoadDefaultCornellBoxScene())
        return FailHybrid("rt-denoise", "LoadDefaultCornellBoxScene");

    const VkDevice device = m_vulkanContext.GetDevice();
    const VkExtent2D extent = m_vulkanContext.GetSwapchainExtent();
    const VkDeviceSize varConfBytes = VkDeviceSize(extent.width) * extent.height * 2 * sizeof(uint16_t);
    const VkDeviceSize compBytes = VkDeviceSize(extent.width) * extent.height * 4 * sizeof(uint16_t);

    VulkanBuffer varConfBuf, compBuf;
    if (varConfBuf.Initialize(
            device, m_memoryManager, varConfBytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS ||
        compBuf.Initialize(
            device, m_memoryManager, compBytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
        return FailHybrid("rt-denoise", "create readback buffers");

    auto readVarConf = [&](std::vector<uint16_t>& outVc) -> bool {
        m_renderer.m_rtVarConfReadbackTarget = varConfBuf.GetBuffer();
        m_window.PollEvents();
        const auto res = DrawScene();
        m_renderer.m_rtVarConfReadbackTarget = VK_NULL_HANDLE;
        if (res != VulkanRenderer::FrameResult::Rendered)
            return false;
        m_vulkanContext.WaitIdle();
        outVc.resize(static_cast<size_t>(extent.width) * extent.height * 2);
        return varConfBuf.ReadData(outVc.data(), varConfBytes) == VK_SUCCESS;
    };

    auto readComposite = [&](std::vector<uint16_t>& outComp) -> bool {
        m_renderer.m_rtCompositeReadbackTarget = compBuf.GetBuffer();
        m_window.PollEvents();
        const auto res = DrawScene();
        m_renderer.m_rtCompositeReadbackTarget = VK_NULL_HANDLE;
        if (res != VulkanRenderer::FrameResult::Rendered)
            return false;
        m_vulkanContext.WaitIdle();
        outComp.resize(static_cast<size_t>(extent.width) * extent.height * 4);
        return compBuf.ReadData(outComp.data(), compBytes) == VK_SUCCESS;
    };

    const auto meanConfidenceCenterPatch = [&](const std::vector<uint16_t>& vc) -> double {
        double sum = 0.0;
        int count = 0;
        const uint32_t cx = extent.width / 2;
        const uint32_t cy = extent.height / 2 + 60;
        for (int dy = -20; dy <= 20; dy += 2)
        {
            for (int dx = -20; dx <= 20; dx += 2)
            {
                const glm::vec2 sample = ReadHalf2Pixel(vc, extent.width, cx + dx, cy + dy);
                sum += sample.y;
                ++count;
            }
        }
        return sum / std::max(1, count);
    };

    m_renderMode = RenderMode::HybridRayTracing;
    m_hybridConfig = {};
    m_hybridConfig.denoiserEnabled = true;
    m_hybridConfig.shadowSamples = 1;
    m_hybridConfig.sunAngularRadiusDeg = 2.8f;
    m_camera.LookAt({ 0.0f, 2.1f, 0.4f }, { -3.0f, 2.1f, 0.4f }, { 0.0f, 1.0f, 0.0f });
    ResetTemporalHistory();

    // ------------------------------------------------------------------
    // 1. Temporal Convergence Over 16 Static Frames
    // ------------------------------------------------------------------
    std::vector<uint16_t> vc;
    if (!readVarConf(vc))
        return FailHybrid("rt-denoise", "frame 0 varConf readback");
    const double confFrame0 = meanConfidenceCenterPatch(vc);

    for (int f = 1; f < 15; ++f)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return FailHybrid("rt-denoise", "static convergence draw");
    }
    if (!readVarConf(vc))
        return FailHybrid("rt-denoise", "frame 15 varConf readback");
    const double confFrame15 = meanConfidenceCenterPatch(vc);
    if (!m_renderer.IsRtDenoiserHistoryValid() || confFrame0 > 0.15 || confFrame15 < 0.85)
    {
        std::cerr << "Temporal convergence failed: conf0=" << confFrame0 << " conf15=" << confFrame15 << '\n';
        return FailHybrid("rt-denoise", "temporal convergence check");
    }

    // ------------------------------------------------------------------
    // 2. Camera Jump Disocclusion Rejection & Recovery
    // ------------------------------------------------------------------
    m_camera.LookAt({ 0.0f, 2.1f, 0.4f }, { 3.0f, 2.1f, 0.4f }, { 0.0f, 1.0f, 0.0f });
    if (!readVarConf(vc))
        return FailHybrid("rt-denoise", "camera jump varConf readback");
    const double confJump = meanConfidenceCenterPatch(vc);
    if (confJump > 0.25)
    {
        std::cerr << "Disocclusion rejection failed on camera jump: confJump=" << confJump << '\n';
        return FailHybrid("rt-denoise", "camera jump disocclusion rejection");
    }

    for (int f = 0; f < 14; ++f)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return FailHybrid("rt-denoise", "post-jump recovery draw");
    }
    if (!readVarConf(vc))
        return FailHybrid("rt-denoise", "post-jump recovered varConf readback");
    const double confRecovered = meanConfidenceCenterPatch(vc);
    if (confRecovered < 0.70)
    {
        std::cerr << "Post-jump recovery failed: confRecovered=" << confRecovered << '\n';
        return FailHybrid("rt-denoise", "post-jump recovery check");
    }

    // ------------------------------------------------------------------
    // 3. Denoiser OFF vs ON Quantitative Spatial Noise Reduction
    // ------------------------------------------------------------------
    const CornellBoxCameraPreset cam = GetDefaultCornellBoxCamera();
    m_camera.LookAt(cam.eye, cam.target, cam.up);
    m_hybridConfig.denoiserEnabled = false;
    ResetTemporalHistory();
    m_window.PollEvents();
    if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
        return FailHybrid("rt-denoise", "denoiser OFF warmup");
    std::vector<uint16_t> compOff, compOn;
    if (!readComposite(compOff))
        return FailHybrid("rt-denoise", "denoiser OFF composite readback");

    m_hybridConfig.denoiserEnabled = true;
    ResetTemporalHistory();
    for (int f = 0; f < 16; ++f)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return FailHybrid("rt-denoise", "denoiser ON warmup");
    }
    if (!readComposite(compOn))
        return FailHybrid("rt-denoise", "denoiser ON composite readback");

    auto computeLocalRoughnessNoise = [&](const std::vector<uint16_t>& comp) -> double {
        double gradSqSum = 0.0;
        int n = 0;
        // Sample lower-middle floor / rough bronze sphere zone where soft shadows fall.
        const uint32_t xStart = extent.width / 2 - 80;
        const uint32_t xEnd = extent.width / 2 + 80;
        const uint32_t yStart = extent.height / 2 + 60;
        const uint32_t yEnd = extent.height / 2 + 160;
        for (uint32_t y = yStart; y < yEnd; ++y)
        {
            for (uint32_t x = xStart; x < xEnd; ++x)
            {
                const glm::vec3 c0 = glm::vec3(ReadHalf4Pixel(comp, extent.width, x, y));
                const glm::vec3 cx = glm::vec3(ReadHalf4Pixel(comp, extent.width, x + 1, y));
                const glm::vec3 cy = glm::vec3(ReadHalf4Pixel(comp, extent.width, x, y + 1));
                const float l0 = glm::dot(c0, { 0.2126f, 0.7152f, 0.0722f });
                const float lx = glm::dot(cx, { 0.2126f, 0.7152f, 0.0722f });
                const float ly = glm::dot(cy, { 0.2126f, 0.7152f, 0.0722f });
                const double dx = double(lx - l0);
                const double dy = double(ly - l0);
                gradSqSum += dx * dx + dy * dy;
                ++n;
            }
        }
        return gradSqSum / std::max(1, n);
    };
    const double noiseOff = computeLocalRoughnessNoise(compOff);
    const double noiseOn = computeLocalRoughnessNoise(compOn);
    if (noiseOff <= 0.0 || noiseOn >= noiseOff * 0.90)
    {
        std::cerr << "Denoiser noise reduction insufficient: noiseOff=" << noiseOff
                  << " noiseOn=" << noiseOn << '\n';
        return FailHybrid("rt-denoise", "denoiser OFF vs ON quantitative variance reduction");
    }

    // Mode switch must reset denoiser history cleanly.
    if (!ToggleRenderModeInteractive() || m_renderMode != RenderMode::Rasterization)
        return FailHybrid("rt-denoise", "Q switch back to Rasterization");
    if (m_renderer.IsRtDenoiserHistoryValid())
        return FailHybrid("rt-denoise", "denoiser history must reset on mode switch");

    varConfBuf.Shutdown();
    compBuf.Shutdown();
    Shutdown();

    std::cout << std::fixed << std::setprecision(5);
    std::cout << "Phase 12 rt.denoise passed (conf0=" << confFrame0
              << ", conf15=" << confFrame15
              << ", confJump=" << confJump
              << ", noiseOff=" << noiseOff
              << ", noiseOn=" << noiseOn << ")\n";
    return 0;
}

int Aether::AetherApplication::RunHybridRtTest()
{
    if (!Initialize(false))
        return FailHybrid("rt-hybrid", "Initialize");

    if (!m_renderer.CanUseHybridRtMode() || !m_renderer.CanUseRayQueryMode())
    {
        std::cout << "SKIP: Hardware Ray Tracing / Hybrid RT is not ready on this device\n";
        Shutdown();
        return 77;
    }

    if (!LoadDefaultCornellBoxScene())
        return FailHybrid("rt-hybrid", "LoadDefaultCornellBoxScene");

    // ------------------------------------------------------------------
    // 1. Cornell Box Q Toggle & All 12 G Debug Views & Scene Lifecycle
    // ------------------------------------------------------------------
    for (int f = 0; f < 3; ++f)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return FailHybrid("rt-hybrid", "initial Rasterization draw");
    }

    if (!ToggleRenderModeInteractive() || m_renderMode != RenderMode::HybridRayTracing)
        return FailHybrid("rt-hybrid", "Q switch Rasterization -> HybridRayTracing");

    for (uint32_t v = 0; v < kHybridDebugViewCount; ++v)
    {
        m_hybridConfig.debugView = static_cast<HybridDebugView>(v);
        for (int f = 0; f < 2; ++f)
        {
            m_window.PollEvents();
            if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
                return FailHybrid("rt-hybrid", "Hybrid debug view cycle draw");
        }
    }
    m_hybridConfig.debugView = HybridDebugView::FinalHybrid;

    if (!SelectStudioEnvironment(true) ||
        DrawScene() != VulkanRenderer::FrameResult::Rendered ||
        !SelectStudioEnvironment(false))
        return FailHybrid("rt-hybrid", "environment toggle in Hybrid mode");

    m_bloomConfig.enabled = false;
    if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
        return FailHybrid("rt-hybrid", "bloom OFF in Hybrid mode");
    m_bloomConfig.enabled = true;

    // ------------------------------------------------------------------
    // 2. Multi-Resolution GPU Timestamp Benchmark (1280x720, 1920x1080, 3840x2160)
    // ------------------------------------------------------------------
    struct ResSpec
    {
        uint32_t width;
        uint32_t height;
        const char* label;
    };
    const std::array<ResSpec, 3> resolutions = {{
        { 1280u,  720u, "1280x720 (720p)" },
        { 1920u, 1080u, "1920x1080 (1080p)" },
        { 3840u, 2160u, "3840x2160 (4K)" }
    }};

    constexpr int kWarmup = 8;
    constexpr int kMeasured = 24;

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "\n=== Aether RTX 5090 Multi-Resolution GPU Timestamp Benchmark (Warmup="
              << kWarmup << ", Measured=" << kMeasured << ") ===\n";

    for (const auto& res : resolutions)
    {
        if (!m_renderer.ResizeRenderTargetsForBenchmark({ res.width, res.height }))
            return FailHybrid("rt-hybrid", "ResizeRenderTargetsForBenchmark");
        m_camera.SetAspectRatio(static_cast<float>(res.width) / static_cast<float>(res.height));
        ResetTemporalHistory();

        std::vector<double> rasterFrame, rasterShadow, rasterTaa, rasterBloom;
        m_renderMode = RenderMode::Rasterization;
        m_taaConfig.enabled = true;
        m_shadowConfig.enabled = true;
        m_bloomConfig.enabled = true;
        ResetTemporalHistory();

        for (int f = 0; f < kWarmup + kMeasured; ++f)
        {
            m_window.PollEvents();
            if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
                return FailHybrid("rt-hybrid", "benchmark Rasterization draw");
            if (f >= kWarmup + 2)
            {
                rasterFrame.push_back(m_renderer.GetLastFrameGpuTimeMs());
                rasterShadow.push_back(m_renderer.GetLastShadowGpuTimeMs());
                rasterTaa.push_back(m_renderer.GetLastTaaTotalGpuTimeMs());
                rasterBloom.push_back(m_renderer.GetLastBloomGpuTimeMs());
            }
        }

        // Force one BLAS/TLAS dynamic update sequence cleanly for TLAS update timing at this resolution.
        m_renderMode = RenderMode::HybridRayTracing;
        m_hybridConfig = {};
        m_hybridConfig.debugView = HybridDebugView::FinalHybrid;
        m_hybridConfig.shadowSamples = 4;
        ResetTemporalHistory();

        std::vector<double> hybridFrame, gbuf, rtShadow, rtRefl, denTemp, denAtrous, comp, hyBloom, tlasUpd;
        for (int f = 0; f < kWarmup + kMeasured; ++f)
        {
            if (f == kWarmup - 2 || f == kWarmup - 1)
            {
                auto objs = m_scene.GetObjects();
                if (objs.size() > 15)
                {
                    if (objs[15].modelMatrix)
                        objs[15].modelMatrix = glm::rotate(*objs[15].modelMatrix, 0.05f, glm::vec3(0.0f, 1.0f, 0.0f));
                    else
                        objs[15].transform.rotation = glm::angleAxis(0.05f * static_cast<float>(f), glm::vec3(0.0f, 1.0f, 0.0f));
                }
            }
            m_window.PollEvents();
            if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
                return FailHybrid("rt-hybrid", "benchmark Hybrid draw");
            if (f >= kWarmup + 2)
            {
                hybridFrame.push_back(m_renderer.GetLastFrameGpuTimeMs());
                gbuf.push_back(m_renderer.GetLastHybridGBufferGpuTimeMs());
                rtShadow.push_back(m_renderer.GetLastRtShadowGpuTimeMs());
                rtRefl.push_back(m_renderer.GetLastRtReflectionGpuTimeMs());
                denTemp.push_back(m_renderer.GetLastRtTemporalDenoiseGpuTimeMs());
                denAtrous.push_back(m_renderer.GetLastRtAtrousDenoiseGpuTimeMs());
                comp.push_back(m_renderer.GetLastRtCompositeGpuTimeMs());
                hyBloom.push_back(m_renderer.GetLastBloomGpuTimeMs());
                tlasUpd.push_back(m_renderer.GetRayTracingScene().GetTimingStats().lastTlasUpdateGpuTimeMs);
            }
        }

        m_renderMode = RenderMode::RayQuery;
        m_rayQueryConfig.debugMode = RayQueryDebugMode::DiagnosticShading;
        ResetTemporalHistory();
        std::vector<double> rqDispatch;
        for (int f = 0; f < 6 + 12; ++f)
        {
            m_window.PollEvents();
            if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
                return FailHybrid("rt-hybrid", "benchmark RayQuery draw");
            if (f >= 8)
                rqDispatch.push_back(m_renderer.GetLastRayQueryGpuTimeMs());
        }

        const auto printRow = [](const char* name, const std::vector<double>& v) {
            const PassStats s = SummarizeSamples(v);
            std::cout << "  " << std::left << std::setw(26) << name
                      << " median=" << std::setw(8) << s.median
                      << " p95=" << std::setw(8) << s.p95
                      << " min=" << std::setw(8) << s.minVal
                      << " max=" << s.maxVal << " ms\n";
        };

        std::cout << "--- Resolution: " << res.label << " ---\n";
        printRow("Raster Total Frame", rasterFrame);
        printRow("Raster Shadow Map", rasterShadow);
        printRow("Raster TAA Total", rasterTaa);
        printRow("Raster Bloom", rasterBloom);
        printRow("Hybrid Total Frame", hybridFrame);
        printRow("TLAS Update (last)", tlasUpd);
        printRow("Hybrid G-Buffer", gbuf);
        printRow("RT Shadows (4 spp)", rtShadow);
        printRow("RT Reflections (VNDF)", rtRefl);
        printRow("Denoiser Temporal", denTemp);
        printRow("Denoiser A-Trous (3x)", denAtrous);
        printRow("Hybrid Composite", comp);
        printRow("Hybrid Bloom", hyBloom);
        printRow("RayQuery Diag Dispatch", rqDispatch);
    }

    if (!RecreateSwapchain())
        return FailHybrid("rt-hybrid", "restore swapchain after benchmark");

    // ------------------------------------------------------------------
    // 3. Empty Scene in Hybrid RT Mode & Software-Disabled Fallback
    // ------------------------------------------------------------------
    m_renderMode = RenderMode::HybridRayTracing;
    m_hybridConfig = {};
    m_scene.Clear();
    m_isCornellBoxScene = false;
    for (int f = 0; f < 3; ++f)
    {
        m_window.PollEvents();
        if (DrawScene() != VulkanRenderer::FrameResult::Rendered)
            return FailHybrid("rt-hybrid", "empty scene Hybrid draw");
    }

    Shutdown();

    {
        AetherApplication fallback;
        fallback.m_vulkanContext.m_disableRayTracingForTest = true;
        if (!fallback.Initialize(false) || !fallback.LoadDefaultCornellBoxScene())
            return FailHybrid("rt-hybrid", "software fallback init");
        if (fallback.m_renderer.CanUseHybridRtMode() ||
            fallback.ToggleRenderModeInteractive() ||
            fallback.m_renderMode != RenderMode::Rasterization)
            return FailHybrid("rt-hybrid", "fallback must stay in Rasterization");
        for (int f = 0; f < 3; ++f)
        {
            fallback.m_window.PollEvents();
            if (fallback.DrawScene() != VulkanRenderer::FrameResult::Rendered)
                return FailHybrid("rt-hybrid", "fallback Rasterization draw");
        }
        fallback.Shutdown();
    }

    std::cout << "Phase 12 rt.hybrid full integration & multi-res benchmark passed\n";
    return 0;
}
