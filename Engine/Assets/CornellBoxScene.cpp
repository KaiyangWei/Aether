#include "CornellBoxScene.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <string>

namespace Aether
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;

        MeshData MakeUnitBoxMesh()
        {
            MeshData mesh;
            const auto addFace = [&](glm::vec3 n, glm::vec3 t,
                                     glm::vec3 p0, glm::vec3 p1, glm::vec3 p2, glm::vec3 p3) {
                const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
                const glm::vec4 tangent(t, 1.0f);
                const glm::vec3 white(1.0f);
                mesh.vertices.push_back({ p0, white, n, { 0.0f, 0.0f }, tangent });
                mesh.vertices.push_back({ p1, white, n, { 1.0f, 0.0f }, tangent });
                mesh.vertices.push_back({ p2, white, n, { 1.0f, 1.0f }, tangent });
                mesh.vertices.push_back({ p3, white, n, { 0.0f, 1.0f }, tangent });
                mesh.indices.insert(mesh.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
            };

            constexpr float h = 0.5f;
            // +Z front
            addFace({ 0, 0, 1 }, { 1, 0, 0 },
                { -h, -h,  h }, {  h, -h,  h }, {  h,  h,  h }, { -h,  h,  h });
            // -Z back
            addFace({ 0, 0, -1 }, { -1, 0, 0 },
                {  h, -h, -h }, { -h, -h, -h }, { -h,  h, -h }, {  h,  h, -h });
            // +X right
            addFace({ 1, 0, 0 }, { 0, 0, -1 },
                {  h, -h,  h }, {  h, -h, -h }, {  h,  h, -h }, {  h,  h,  h });
            // -X left
            addFace({ -1, 0, 0 }, { 0, 0, 1 },
                { -h, -h, -h }, { -h, -h,  h }, { -h,  h,  h }, { -h,  h, -h });
            // +Y top
            addFace({ 0, 1, 0 }, { 1, 0, 0 },
                { -h,  h,  h }, {  h,  h,  h }, {  h,  h, -h }, { -h,  h, -h });
            // -Y bottom
            addFace({ 0, -1, 0 }, { 1, 0, 0 },
                { -h, -h, -h }, {  h, -h, -h }, {  h, -h,  h }, { -h, -h,  h });
            return mesh;
        }

        MeshData MakeUnitSphereMesh(unsigned rings = 28, unsigned segments = 56)
        {
            MeshData mesh;
            for (unsigned j = 0; j <= rings; ++j)
            {
                const float v = static_cast<float>(j) / static_cast<float>(rings);
                const float polar = v * kPi;
                const float sinP = std::sin(polar);
                const float cosP = std::cos(polar);
                for (unsigned i = 0; i <= segments; ++i)
                {
                    const float u = static_cast<float>(i) / static_cast<float>(segments);
                    const float azimuth = u * 2.0f * kPi;
                    const float sinA = std::sin(azimuth);
                    const float cosA = std::cos(azimuth);
                    const glm::vec3 normal(sinP * cosA, cosP, sinP * sinA);
                    const glm::vec3 tangentDir = glm::normalize(glm::vec3(-sinA, 0.0f, cosA));
                    Vertex vert{};
                    vert.position = normal;
                    vert.color = glm::vec3(1.0f);
                    vert.normal = normal;
                    vert.uv = { u, v };
                    vert.tangent = glm::vec4(tangentDir, 1.0f);
                    mesh.vertices.push_back(vert);
                    if (j < rings && i < segments)
                    {
                        const uint32_t a = j * (segments + 1) + i;
                        const uint32_t b = a + segments + 1;
                        mesh.indices.insert(mesh.indices.end(), { a, a + 1, b, a + 1, b + 1, b });
                    }
                }
            }
            return mesh;
        }

        MeshData MakeUnitCylinderMesh(unsigned segments = 32)
        {
            MeshData mesh;
            constexpr float halfH = 0.5f;
            const glm::vec3 white(1.0f);

            // Side wall
            const uint32_t sideBase = static_cast<uint32_t>(mesh.vertices.size());
            for (unsigned i = 0; i <= segments; ++i)
            {
                const float u = static_cast<float>(i) / static_cast<float>(segments);
                const float angle = u * 2.0f * kPi;
                const float c = std::cos(angle);
                const float s = std::sin(angle);
                const glm::vec3 n(c, 0.0f, s);
                const glm::vec4 t(glm::normalize(glm::vec3(-s, 0.0f, c)), 1.0f);
                mesh.vertices.push_back({ { c, -halfH, s }, white, n, { u, 0.0f }, t });
                mesh.vertices.push_back({ { c,  halfH, s }, white, n, { u, 1.0f }, t });
                if (i < segments)
                {
                    const uint32_t b0 = sideBase + i * 2;
                    const uint32_t b1 = b0 + 2;
                    mesh.indices.insert(mesh.indices.end(), { b0, b0 + 1, b1, b0 + 1, b1 + 1, b1 });
                }
            }

            // Top cap (+Y)
            const uint32_t topCenter = static_cast<uint32_t>(mesh.vertices.size());
            mesh.vertices.push_back({ { 0.0f, halfH, 0.0f }, white, { 0, 1, 0 }, { 0.5f, 0.5f }, { 1, 0, 0, 1 } });
            for (unsigned i = 0; i <= segments; ++i)
            {
                const float angle = static_cast<float>(i) / static_cast<float>(segments) * 2.0f * kPi;
                const float c = std::cos(angle);
                const float s = std::sin(angle);
                mesh.vertices.push_back({ { c, halfH, s }, white, { 0, 1, 0 }, { 0.5f + 0.5f * c, 0.5f + 0.5f * s }, { 1, 0, 0, 1 } });
                if (i < segments)
                {
                    mesh.indices.insert(mesh.indices.end(), { topCenter, topCenter + 1 + i + 1, topCenter + 1 + i });
                }
            }

            // Bottom cap (-Y)
            const uint32_t botCenter = static_cast<uint32_t>(mesh.vertices.size());
            mesh.vertices.push_back({ { 0.0f, -halfH, 0.0f }, white, { 0, -1, 0 }, { 0.5f, 0.5f }, { 1, 0, 0, 1 } });
            for (unsigned i = 0; i <= segments; ++i)
            {
                const float angle = static_cast<float>(i) / static_cast<float>(segments) * 2.0f * kPi;
                const float c = std::cos(angle);
                const float s = std::sin(angle);
                mesh.vertices.push_back({ { c, -halfH, s }, white, { 0, -1, 0 }, { 0.5f + 0.5f * c, 0.5f + 0.5f * s }, { 1, 0, 0, 1 } });
                if (i < segments)
                {
                    mesh.indices.insert(mesh.indices.end(), { botCenter, botCenter + 1 + i, botCenter + 1 + i + 1 });
                }
            }

            return mesh;
        }

        MeshData MakeTorusMesh(float majorRadius = 0.52f, float minorRadius = 0.17f,
                               unsigned majorSegments = 40, unsigned minorSegments = 24)
        {
            MeshData mesh;
            const glm::vec3 white(1.0f);
            for (unsigned i = 0; i <= majorSegments; ++i)
            {
                const float u = static_cast<float>(i) / static_cast<float>(majorSegments);
                const float theta = u * 2.0f * kPi;
                const float cosT = std::cos(theta);
                const float sinT = std::sin(theta);
                const glm::vec3 tangentDir(-sinT, 0.0f, cosT);
                for (unsigned j = 0; j <= minorSegments; ++j)
                {
                    const float v = static_cast<float>(j) / static_cast<float>(minorSegments);
                    const float phi = v * 2.0f * kPi;
                    const float cosP = std::cos(phi);
                    const float sinP = std::sin(phi);
                    const float r = majorRadius + minorRadius * cosP;
                    const glm::vec3 pos(r * cosT, minorRadius * sinP, r * sinT);
                    const glm::vec3 normal(cosP * cosT, sinP, cosP * sinT);
                    mesh.vertices.push_back({ pos, white, normal, { u, v }, glm::vec4(tangentDir, 1.0f) });
                    if (i < majorSegments && j < minorSegments)
                    {
                        const uint32_t a = i * (minorSegments + 1) + j;
                        const uint32_t b = a + minorSegments + 1;
                        mesh.indices.insert(mesh.indices.end(), { a, a + 1, b, a + 1, b + 1, b });
                    }
                }
            }
            return mesh;
        }

        glm::mat4 ComposeTransform(
            const glm::vec3& position,
            const glm::vec3& scale,
            const glm::vec3& eulerDegrees = glm::vec3(0.0f))
        {
            const glm::quat q =
                glm::angleAxis(glm::radians(eulerDegrees.y), glm::vec3(0.0f, 1.0f, 0.0f)) *
                glm::angleAxis(glm::radians(eulerDegrees.x), glm::vec3(1.0f, 0.0f, 0.0f)) *
                glm::angleAxis(glm::radians(eulerDegrees.z), glm::vec3(0.0f, 0.0f, 1.0f));
            return glm::translate(glm::mat4(1.0f), position) *
                   glm::mat4_cast(q) *
                   glm::scale(glm::mat4(1.0f), scale);
        }
    }

    CornellBoxCameraPreset GetDefaultCornellBoxCamera()
    {
        return CornellBoxCameraPreset{};
    }

    ImportedAsset MakeCornellBoxScene()
    {
        ImportedAsset asset;

        const MeshData unitBox = MakeUnitBoxMesh();
        const MeshData unitSphere = MakeUnitSphereMesh();
        const MeshData unitCylinder = MakeUnitCylinderMesh();
        const MeshData torus = MakeTorusMesh();

        auto addMaterial = [&](const glm::vec4& baseColor,
                               float metallic,
                               float roughness,
                               const glm::vec3& emissive = glm::vec3(0.0f),
                               bool doubleSided = false) -> size_t {
            ImportedMaterial mat{};
            mat.baseColor = baseColor;
            mat.metallic = metallic;
            mat.roughness = roughness;
            mat.emissive = emissive;
            mat.doubleSided = doubleSided;
            mat.alphaMode = ImportedMaterial::AlphaMode::Opaque;
            const size_t id = asset.materials.size();
            asset.materials.push_back(mat);
            return id;
        };

        // Material palette covering rough dielectrics, colored diffuse walls, glossy floor,
        // mirror-like chrome, mid-rough metals, and warm/cool HDR emissive neon fixtures.
        const size_t matFloorSlate     = addMaterial({ 0.14f, 0.15f, 0.18f, 1.0f }, 0.08f, 0.16f);
        const size_t matFloorInlay     = addMaterial({ 0.25f, 0.26f, 0.30f, 1.0f }, 0.15f, 0.28f);
        const size_t matLeftRed        = addMaterial({ 0.76f, 0.11f, 0.09f, 1.0f }, 0.00f, 0.86f);
        const size_t matLeftRib        = addMaterial({ 0.46f, 0.07f, 0.06f, 1.0f }, 0.00f, 0.80f);
        const size_t matRightGreen     = addMaterial({ 0.08f, 0.56f, 0.24f, 1.0f }, 0.00f, 0.86f);
        const size_t matRightAlcove    = addMaterial({ 0.06f, 0.34f, 0.16f, 1.0f }, 0.00f, 0.78f);
        const size_t matBackPlaster    = addMaterial({ 0.78f, 0.76f, 0.72f, 1.0f }, 0.00f, 0.88f);
        const size_t matDarkGunmetal   = addMaterial({ 0.14f, 0.15f, 0.18f, 1.0f }, 0.88f, 0.30f);
        const size_t matIvoryMatte     = addMaterial({ 0.86f, 0.83f, 0.78f, 1.0f }, 0.00f, 0.76f);
        const size_t matMirrorChrome   = addMaterial({ 0.96f, 0.97f, 0.99f, 1.0f }, 1.00f, 0.02f);
        const size_t matBrushedGold    = addMaterial({ 0.95f, 0.74f, 0.28f, 1.0f }, 0.96f, 0.28f);
        const size_t matAnodizedCopper = addMaterial({ 0.90f, 0.40f, 0.22f, 1.0f }, 0.92f, 0.18f);
        const size_t matRoughBronze    = addMaterial({ 0.72f, 0.48f, 0.30f, 1.0f }, 0.92f, 0.62f);
        const size_t matCeilingWarm    = addMaterial({ 1.00f, 0.96f, 0.90f, 1.0f }, 0.00f, 0.20f, { 5.2f, 4.6f, 3.6f });
        const size_t matNeonCyan       = addMaterial({ 0.75f, 0.96f, 1.00f, 1.0f }, 0.00f, 0.15f, { 0.6f, 4.8f, 8.5f });
        const size_t matNeonAmber      = addMaterial({ 1.00f, 0.82f, 0.65f, 1.0f }, 0.00f, 0.15f, { 8.0f, 2.2f, 0.65f });

        // Reuse (geometry, material) pairs as shared meshes so BLAS reuse is exercised cleanly.
        std::vector<std::pair<int, size_t>> meshKeys;
        auto getOrCreateMesh = [&](int shapeKind, const MeshData& geom, size_t materialId) -> size_t {
            for (size_t i = 0; i < meshKeys.size(); ++i)
            {
                if (meshKeys[i].first == shapeKind && meshKeys[i].second == materialId)
                    return i;
            }
            const size_t meshIndex = asset.meshes.size();
            asset.meshes.push_back({ ImportedPrimitive{ geom, materialId, {} } });
            meshKeys.emplace_back(shapeKind, materialId);
            return meshIndex;
        };

        auto addBox = [&](const std::string& name, size_t mat,
                          glm::vec3 pos, glm::vec3 scale, glm::vec3 eulerDeg = glm::vec3(0.0f)) {
            ImportedNode node{};
            node.name = name;
            node.mesh = getOrCreateMesh(0, unitBox, mat);
            node.local = node.world = ComposeTransform(pos, scale, eulerDeg);
            asset.roots.push_back(asset.nodes.size());
            asset.nodes.push_back(std::move(node));
        };

        auto addSphere = [&](const std::string& name, size_t mat,
                             glm::vec3 pos, float radius) {
            ImportedNode node{};
            node.name = name;
            node.mesh = getOrCreateMesh(1, unitSphere, mat);
            node.local = node.world = ComposeTransform(pos, glm::vec3(radius));
            asset.roots.push_back(asset.nodes.size());
            asset.nodes.push_back(std::move(node));
        };

        auto addCylinder = [&](const std::string& name, size_t mat,
                               glm::vec3 pos, float radius, float height,
                               glm::vec3 eulerDeg = glm::vec3(0.0f)) {
            ImportedNode node{};
            node.name = name;
            node.mesh = getOrCreateMesh(2, unitCylinder, mat);
            node.local = node.world = ComposeTransform(pos, { radius, height, radius }, eulerDeg);
            asset.roots.push_back(asset.nodes.size());
            asset.nodes.push_back(std::move(node));
        };

        auto addTorus = [&](const std::string& name, size_t mat,
                            glm::vec3 pos, float scale, glm::vec3 eulerDeg) {
            ImportedNode node{};
            node.name = name;
            node.mesh = getOrCreateMesh(3, torus, mat);
            node.local = node.world = ComposeTransform(pos, glm::vec3(scale), eulerDeg);
            asset.roots.push_back(asset.nodes.size());
            asset.nodes.push_back(std::move(node));
        };

        // ------------------------------------------------------------------
        // 1. Enclosed Stylized Cornell Box Room Shell (6.0 x 4.6 x 6.0 m)
        // ------------------------------------------------------------------
        // Floor + central architectural inlay slab
        addBox("FloorMain",  matFloorSlate, { 0.0f, -0.10f, 0.0f }, { 6.4f, 0.20f, 6.4f });
        addBox("FloorInlay", matFloorInlay, { 0.0f,  0.01f, -0.25f }, { 4.2f, 0.02f, 4.0f });

        // Left crimson Cornell wall + vertical acoustic architectural ribs (asymmetric rhythm)
        addBox("LeftWall", matLeftRed, { -3.10f, 2.30f, 0.0f }, { 0.20f, 4.60f, 6.4f });
        for (int i = 0; i < 4; ++i)
        {
            const float zPos = -2.25f + static_cast<float>(i) * 1.15f;
            addBox("LeftWallRib_" + std::to_string(i), matLeftRib,
                   { -2.93f, 2.30f, zPos }, { 0.14f, 4.20f, 0.16f });
        }

        // Right emerald Cornell wall + asymmetric stepped gallery alcove shelf
        addBox("RightWall", matRightGreen, { 3.10f, 2.30f, 0.0f }, { 0.20f, 4.60f, 6.4f });
        addBox("RightAlcoveBacking", matRightAlcove, { 2.94f, 2.55f, -1.05f }, { 0.12f, 2.40f, 2.50f });
        addBox("RightAlcoveShelf",   matDarkGunmetal, { 2.74f, 1.38f, -1.05f }, { 0.52f, 0.09f, 2.55f });

        // Back neutral plaster wall + recessed asymmetric frame & upper lintel
        addBox("BackWall",        matBackPlaster, { 0.0f, 2.30f, -3.10f }, { 6.4f, 4.60f, 0.20f });
        addBox("BackWallPillarL", matBackPlaster, { -2.45f, 2.30f, -2.92f }, { 0.45f, 4.60f, 0.18f });
        addBox("BackWallLintel",  matDarkGunmetal, { 0.0f, 4.30f, -2.86f }, { 6.0f, 0.22f, 0.28f });

        // Coffered Ceiling with open central skylight (x in [-1.6, 1.6], z in [-1.5, 1.5])
        // so directional sunlight enters cleanly and casts crisp architectural & hero shadows!
        addBox("CeilingLeft",  matBackPlaster, { -2.35f, 4.70f,  0.00f }, { 1.70f, 0.20f, 6.40f });
        addBox("CeilingRight", matBackPlaster, {  2.35f, 4.70f,  0.00f }, { 1.70f, 0.20f, 6.40f });
        addBox("CeilingBack",  matBackPlaster, {  0.00f, 4.70f, -2.35f }, { 3.20f, 0.20f, 1.70f });
        addBox("CeilingFront", matBackPlaster, {  0.00f, 4.70f,  2.35f }, { 3.20f, 0.20f, 1.70f });
        // Structural skylight cross-beams casting recognizable shadow lines
        addBox("SkylightBeamA", matDarkGunmetal, { -0.55f, 4.56f, 0.00f }, { 0.12f, 0.14f, 3.10f });
        addBox("SkylightBeamB", matDarkGunmetal, {  0.55f, 4.56f, 0.00f }, { 0.12f, 0.14f, 3.10f });

        // ------------------------------------------------------------------
        // 2. HDR Emissive & Neon Accent Lighting (Bloom / HDR Validation)
        // ------------------------------------------------------------------
        // Warm overhead skylight perimeter light strips
        addBox("CeilingLightBack",  matCeilingWarm, {  0.00f, 4.46f, -1.52f }, { 3.10f, 0.07f, 0.14f });
        addBox("CeilingLightFront", matCeilingWarm, {  0.00f, 4.46f,  1.52f }, { 3.10f, 0.07f, 0.14f });

        // Cool Cyan vertical corner neon tube + horizontal back-wall cyan bar
        addCylinder("NeonTubeCyanCorner", matNeonCyan, { -2.76f, 2.25f, -2.76f }, 0.055f, 3.90f);
        addBox("NeonBarCyanBack",         matNeonCyan, { -1.15f, 3.65f, -2.91f }, { 1.85f, 0.08f, 0.08f });

        // Warm Amber-Orange asymmetric neon L-frame on right-back wall + corner tube
        addBox("NeonFrameAmberTop",   matNeonAmber, { 1.65f, 3.45f, -2.91f }, { 1.65f, 0.08f, 0.08f });
        addBox("NeonFrameAmberSide",  matNeonAmber, { 2.44f, 2.75f, -2.91f }, { 0.08f, 1.48f, 0.08f });
        addCylinder("NeonTubeAmberCorner", matNeonAmber, { 2.76f, 2.25f, -2.76f }, 0.055f, 3.90f);

        // ------------------------------------------------------------------
        // 3. Elevated Central Pedestal + Chrome Mirror Hero Sphere
        // ------------------------------------------------------------------
        addBox("PedestalBase",      matDarkGunmetal, { -0.22f, 0.18f, -0.55f }, { 1.50f, 0.36f, 1.50f });
        addBox("PedestalGlowRim",   matNeonCyan,     { -0.22f, 0.38f, -0.55f }, { 1.40f, 0.045f, 1.40f });
        addBox("PedestalPlinth",    matIvoryMatte,   { -0.22f, 0.66f, -0.55f }, { 1.24f, 0.52f, 1.24f });
        addSphere("HeroChromeSphere", matMirrorChrome, { -0.22f, 1.48f, -0.55f }, 0.56f);

        // ------------------------------------------------------------------
        // 4. Tall Rotated Architectural Monolith (Classic Cornell Tall Box)
        // ------------------------------------------------------------------
        addBox("TallMonolithBody",   matIvoryMatte,   { -1.48f, 1.22f, -1.42f }, { 0.98f, 2.44f, 0.98f }, { 0.0f, 24.0f, 0.0f });
        addBox("TallMonolithCap",    matDarkGunmetal, { -1.48f, 2.48f, -1.42f }, { 1.04f, 0.08f, 1.04f }, { 0.0f, 24.0f, 0.0f });

        // ------------------------------------------------------------------
        // 5. Large Angled Mirror Panel on Dark Gunmetal Easel (Reflection Target)
        // ------------------------------------------------------------------
        addBox("MirrorFrame",   matDarkGunmetal, { -2.22f, 1.38f, 0.55f }, { 0.10f, 2.36f, 1.52f }, { 0.0f, -28.0f, 0.0f });
        addBox("MirrorSurface", matMirrorChrome, { -2.16f, 1.38f, 0.58f }, { 0.04f, 2.18f, 1.34f }, { 0.0f, -28.0f, 0.0f });
        addBox("MirrorStandL",  matDarkGunmetal, { -2.32f, 0.14f, 0.50f }, { 0.52f, 0.28f, 1.30f }, { 0.0f, -28.0f, 0.0f });

        // ------------------------------------------------------------------
        // 6. Mixed Material Comparison Objects (Dielectric, Brushed Gold, Copper Torus)
        // ------------------------------------------------------------------
        // Foreground-left rough dielectric ceramic sphere
        addSphere("MatteCeramicSphere", matIvoryMatte, { -1.18f, 0.44f, 1.15f }, 0.44f);

        // Foreground-center rough metallic bronze sphere (rough metal vs mirror/mid-rough contrast)
        addSphere("RoughBronzeSphere", matRoughBronze, { 0.18f, 0.36f, 1.32f }, 0.36f);

        // Foreground-right brushed gold metallic sphere on low cylindrical gunmetal plinth
        addCylinder("GoldSpherePlinth", matDarkGunmetal, { 1.32f, 0.14f, 0.78f }, 0.58f, 0.28f);
        addSphere("BrushedGoldSphere",  matBrushedGold,  { 1.32f, 0.76f, 0.78f }, 0.48f);

        // Right-back stepped gallery block + sculptural anodized copper torus
        addBox("RightStepLow",  matDarkGunmetal,   { 1.48f, 0.28f, -1.25f }, { 1.22f, 0.56f, 1.22f }, { 0.0f, -18.0f, 0.0f });
        addBox("RightStepHigh", matIvoryMatte,     { 1.48f, 0.72f, -1.25f }, { 0.92f, 0.32f, 0.92f }, { 0.0f, -18.0f, 0.0f });
        addTorus("AnodizedCopperTorus", matAnodizedCopper, { 1.48f, 1.42f, -1.25f }, 0.95f, { 52.0f, -26.0f, 14.0f });

        // ------------------------------------------------------------------
        // 7. High-Frequency Architectural Louver Bars (TAA & Ray Query Edge Stress)
        // ------------------------------------------------------------------
        for (int i = 0; i < 5; ++i)
        {
            const float zPos = -0.35f + static_cast<float>(i) * 0.34f;
            addBox("RightLouver_" + std::to_string(i), matDarkGunmetal,
                   { 2.52f, 2.25f, zPos }, { 0.06f, 2.95f, 0.12f }, { 0.0f, 22.0f, 0.0f });
        }

        // Small decorative spheres on right alcove shelf for scale & specular variety
        addSphere("AlcoveAccentChrome", matMirrorChrome, { 2.70f, 1.62f, -1.65f }, 0.19f);
        addSphere("AlcoveAccentGold",   matBrushedGold,  { 2.70f, 1.60f, -0.95f }, 0.17f);
        addSphere("AlcoveAccentCopper", matAnodizedCopper, { 2.70f, 1.58f, -0.35f }, 0.15f);

        return asset;
    }

    CornellBoxValidationSummary InspectCornellBoxAsset(const ImportedAsset& asset)
    {
        CornellBoxValidationSummary summary{};
        summary.meshCount = asset.meshes.size();
        summary.nodeCount = asset.nodes.size();
        summary.materialCount = asset.materials.size();
        summary.allMeshesValid = !asset.meshes.empty();

        for (const auto& meshPrimitives : asset.meshes)
        {
            if (meshPrimitives.empty())
            {
                summary.allMeshesValid = false;
                continue;
            }
            for (const auto& prim : meshPrimitives)
            {
                if (!prim.geometry.IsValid())
                    summary.allMeshesValid = false;
            }
        }

        for (const auto& mat : asset.materials)
        {
            if (mat.emissive.r > 0.5f || mat.emissive.g > 0.5f || mat.emissive.b > 0.5f)
                ++summary.emissiveMaterialCount;
            if (mat.metallic >= 0.95f && mat.roughness <= 0.05f)
                ++summary.mirrorLikeMaterialCount;
            if (mat.metallic <= 0.05f && mat.roughness >= 0.65f)
                ++summary.roughDielectricMaterialCount;
            if (mat.metallic >= 0.70f && mat.roughness >= 0.15f && mat.roughness <= 0.45f)
                ++summary.midRoughMetallicMaterialCount;
            if (mat.metallic >= 0.70f && mat.roughness >= 0.50f)
                ++summary.roughMetallicMaterialCount;
        }

        return summary;
    }

    std::vector<RtGpuLight> GetCornellBoxLights()
    {
        std::vector<RtGpuLight> lights;
        lights.reserve(4);

        // 1. Overhead warm skylight rectangular area emitter (soft area shadows inside room)
        RtGpuLight skylight{};
        skylight.posAndType = { 0.0f, 4.42f, 0.0f, static_cast<float>(RtLightType::RectArea) };
        skylight.dirAndRange = { 0.0f, -1.0f, 0.0f, 16.0f };
        skylight.colorAndIntensity = { 1.0f, 0.95f, 0.86f, 3.4f };
        skylight.areaRightAndHalfW = { 1.0f, 0.0f, 0.0f, 1.35f };
        skylight.areaUpAndHalfH = { 0.0f, 0.0f, 1.0f, 1.35f };
        lights.push_back(skylight);

        // 2. Back-left cool cyan neon bar / tube strip rectangular area light
        RtGpuLight cyanNeon{};
        cyanNeon.posAndType = { -1.35f, 3.30f, -2.72f, static_cast<float>(RtLightType::RectArea) };
        cyanNeon.dirAndRange = { 0.0f, 0.0f, 1.0f, 12.0f };
        cyanNeon.colorAndIntensity = { 0.12f, 0.82f, 1.0f, 5.2f };
        cyanNeon.areaRightAndHalfW = { 1.0f, 0.0f, 0.0f, 0.90f };
        cyanNeon.areaUpAndHalfH = { 0.0f, 1.0f, 0.0f, 0.30f };
        lights.push_back(cyanNeon);

        // 3. Back-right warm amber neon accent frame rectangular area light
        RtGpuLight amberNeon{};
        amberNeon.posAndType = { 1.85f, 3.15f, -2.72f, static_cast<float>(RtLightType::RectArea) };
        amberNeon.dirAndRange = { 0.0f, 0.0f, 1.0f, 12.0f };
        amberNeon.colorAndIntensity = { 1.0f, 0.34f, 0.09f, 5.0f };
        amberNeon.areaRightAndHalfW = { 1.0f, 0.0f, 0.0f, 0.85f };
        amberNeon.areaUpAndHalfH = { 0.0f, 1.0f, 0.0f, 0.35f };
        lights.push_back(amberNeon);

        // 4. Subtle cyan pedestal rim accent point light
        RtGpuLight pedestalRim{};
        pedestalRim.posAndType = { -0.22f, 0.52f, 0.42f, static_cast<float>(RtLightType::Point) };
        pedestalRim.dirAndRange = { 0.0f, -1.0f, 0.0f, 3.2f };
        pedestalRim.colorAndIntensity = { 0.14f, 0.84f, 1.0f, 1.4f };
        pedestalRim.areaRightAndHalfW = { 1.0f, 0.0f, 0.0f, 0.06f };
        pedestalRim.areaUpAndHalfH = { 0.0f, 1.0f, 0.0f, 0.0f };
        lights.push_back(pedestalRim);

        return lights;
    }
}
