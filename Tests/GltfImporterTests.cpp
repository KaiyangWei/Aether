#include <Assets/GltfImporter.h>

#include <cmath>
#include <fstream>
#include <iostream>
#include <string_view>

namespace
{
    void Write(const std::filesystem::path& path, std::string_view data)
    {
        std::ofstream file(path, std::ios::binary);
        file.write(data.data(), data.size());
        if (!file)
            throw std::runtime_error("Cannot write test fixture");
    }

    void U32(std::string& data, uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
            data.push_back(static_cast<char>(value >> (8 * i)));
    }

    void WriteGlb(const std::filesystem::path& path, std::string json, std::string bin)
    {
        while (json.size() % 4)
            json.push_back(' ');
        while (bin.size() % 4)
            bin.push_back(0);
        std::string data("glTF");
        U32(data, 2);
        U32(data, static_cast<uint32_t>(28 + json.size() + bin.size()));
        U32(data, static_cast<uint32_t>(json.size()));
        data += "JSON";
        data += json;
        U32(data, static_cast<uint32_t>(bin.size()));
        data.append("BIN\0", 4);
        data += bin;
        Write(path, data);
    }

    void Check(bool ok, const char* message)
    {
        if (!ok)
            throw std::runtime_error(message);
    }
}

int main()
{
    try
    {
        const auto dir = std::filesystem::current_path() / L"asset-fixtures";
        std::filesystem::create_directories(dir);
        std::string bin;
        // Interleaved POSITION and normalized u16 UV.
        for (auto row : {0, 1, 2})
        {
            U32(bin, row == 1 ? 0x3f800000 : 0);
            U32(bin, row == 2 ? 0x3f800000 : 0);
            U32(bin, 0);
            U32(bin, row == 2 ? 0xffff8000 : 0);
        }
        bin.append("\0\1\2\0", 4);
        bin.append("\0\0\1\0\2\0\0\0", 8);
        U32(bin, 0); U32(bin, 1); U32(bin, 2);
        bin.append("\1\0\0\0", 4);
        U32(bin, 0x40000000); U32(bin, 0); U32(bin, 0);
        const std::string png("\x89PNG\r\n\x1a\n"
            "\0\0\0\x0dIHDR\0\0\0\1\0\0\0\1\x08\x06\0\0\0\x1f\x15\xc4\x89"
            "\0\0\0\x0dIDAT\x08\xd7\x63\xf8\xcf\xc0\xf0\x1f\0\x05\0\x01\xff\x89\x99\x3d\x1d"
            "\0\0\0\0IEND\xae\x42\x60\x82", 70);

        const auto minimal = dir / "minimal.glb";
        WriteGlb(minimal, R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":48}],
            "bufferViews":[{"buffer":0,"byteLength":48,"byteStride":16}],
            "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],
            "meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}]})",
            bin.substr(0,48));
        auto basic = Aether::ImportGltf(minimal);
        if (!basic.asset)
            throw std::runtime_error(basic.error);
        Check(basic.asset->meshes[0][0].geometry.indices == std::vector<uint32_t>{0,1,2}, "Generated indices");

        const std::string body = R"(,
            "bufferViews":[{"buffer":0,"byteLength":48,"byteStride":16},
              {"buffer":0,"byteOffset":48,"byteLength":3},{"buffer":0,"byteOffset":52,"byteLength":6},
              {"buffer":0,"byteOffset":60,"byteLength":12},{"buffer":0,"byteOffset":72,"byteLength":1},
              {"buffer":0,"byteOffset":76,"byteLength":12},{"buffer":0,"byteOffset":88,"byteLength":70},
              {"buffer":0,"byteOffset":160,"byteLength":36},{"buffer":0,"byteOffset":196,"byteLength":48}],
            "accessors":[
              {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[2,1,0],
                "sparse":{"count":1,"indices":{"bufferView":4,"componentType":5121},"values":{"bufferView":5}}},
              {"bufferView":0,"byteOffset":12,"componentType":5123,"normalized":true,"count":3,"type":"VEC2"},
              {"bufferView":1,"componentType":5121,"count":3,"type":"SCALAR"},
              {"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"},
              {"bufferView":3,"componentType":5125,"count":3,"type":"SCALAR"},
              {"bufferView":7,"componentType":5126,"count":3,"type":"VEC3"},
              {"bufferView":8,"componentType":5126,"count":3,"type":"VEC4"}],
            "meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1,"TEXCOORD_1":1,"NORMAL":5,"TANGENT":6},"indices":2,"material":0},
                        {"attributes":{"POSITION":0},"indices":3}]},
                      {"primitives":[{"attributes":{"POSITION":0},"indices":4}]}],
            "nodes":[{"scale":[2,1,1],"children":[1]},{"mesh":0,"translation":[1,0,0],"rotation":[0,0,0.38268343,0.92387953]},
                     {"mesh":0},{"mesh":1,"matrix":[-1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]}],
            "scenes":[{"nodes":[0,2,3]}],"scene":0,
            "samplers":[{"wrapS":33071,"minFilter":9987}],"textures":[{"source":0,"sampler":0}],
            "materials":[{"pbrMetallicRoughness":{"baseColorFactor":[0.2,0.3,0.4,0.5],"metallicFactor":0.6,
                         "roughnessFactor":0.7,"baseColorTexture":{"index":0,"texCoord":1},"metallicRoughnessTexture":{"index":0}},
                         "normalTexture":{"index":0,"scale":0.8},"occlusionTexture":{"index":0,"strength":0.4},
                         "emissiveFactor":[0.1,0.2,0.3],"emissiveTexture":{"index":0},
                         "alphaMode":"MASK","alphaCutoff":0.25,"doubleSided":true}])";
        bin += png;
        bin.append(2, '\0');
        for (int i = 0; i < 3; ++i)
        {
            U32(bin, 0); U32(bin, 0); U32(bin, 0x3f800000);
        }
        for (int i = 0; i < 3; ++i)
        {
            U32(bin, 0x3f800000); U32(bin, 0); U32(bin, 0); U32(bin, 0xbf800000);
        }
        const auto embedded = dir / "embedded.glb";
        WriteGlb(embedded, R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":244}])" + body +
            R"(,"images":[{"bufferView":6,"mimeType":"image/png"}]})", bin);

        const auto external = dir / L"\u6a21\u578b.gltf";
        const std::string externalPrefix = R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":244,"uri":"mesh.bin"}])";
        Write(dir / "mesh.bin", bin);
        Write(dir / "pixel.png", png);
        Write(external, externalPrefix + body + R"(,"images":[{"uri":"pixel.png"}]})");
        for (const auto& path : {embedded, external})
        {
            const auto result = Aether::ImportGltf(path);
            if (!result.asset)
                throw std::runtime_error(result.error);
            const auto& asset = *result.asset;
            Check(asset.meshes.size() == 2 && asset.meshes[0].size() == 2 && asset.nodes.size() == 4 &&
                asset.nodes[1].mesh == asset.nodes[2].mesh && asset.nodes[1].parent == 0, "Mesh and node graph");
            const auto& world = asset.nodes[1].world;
            Check(std::abs(world[3].x - 2) < 1e-5f && std::abs(glm::dot(glm::vec3(world[0]), glm::vec3(world[1]))) > 1,
                "World transform preserves shear");
            for (const auto& mesh : asset.meshes)
                for (const auto& primitive : mesh)
                    Check(primitive.geometry.indices == std::vector<uint32_t>{0,1,2} &&
                        primitive.geometry.vertices[1].position.x == 2, "Index widths and sparse override");
            const auto& vertex = asset.meshes[0][0].geometry.vertices[2];
            Check(vertex.normal == glm::vec3(0,0,1) && vertex.tangent == glm::vec4(1,0,0,-1), "Normal and tangent");
            const auto uv = vertex.uv;
            Check(std::abs(uv.x - 0.5f) < 2e-5f && uv.y == 1, "Interleaved normalized UV");
            const auto& material = asset.materials.at(0);
            Check(material.baseColor.w == 0.5f && material.metallic == 0.6f && material.roughness == 0.7f &&
                material.emissive.z == 0.3f && material.alphaCutoff == 0.25f && material.doubleSided &&
                material.alphaMode == Aether::ImportedMaterial::AlphaMode::Mask &&
                material.baseColorTexture && material.baseColorTexture->texture == 0 && material.baseColorTexture->uvSet == 1 &&
                asset.meshes[0][0].extraTexCoords.at(0).second[2] == uv && material.metallicRoughnessTexture &&
                material.normalTexture->strength == 0.8f && material.occlusionTexture->strength == 0.4f &&
                material.emissiveTexture && asset.textures[0].image == 0 && asset.textures[0].sampler == 0 &&
                asset.samplers[0].wrapS == 33071 && asset.images[0].mimeType == "image/png" &&
                asset.images[0].bytes.size() == png.size(), "PBR and image metadata");
        }

        Write(dir / "missing.gltf", externalPrefix + body + R"(,"images":[{"uri":"absent.png"}]})");
        Write(dir / "broken.glb", "glTF");
        bin[72] = 9;
        WriteGlb(dir / "bad-sparse.glb", R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":244}])" +
            body + R"(,"images":[{"bufferView":6,"mimeType":"image/png"}]})", bin);
        for (const auto* file : {"missing.gltf", "broken.glb", "bad-sparse.glb"})
        {
            const auto result = Aether::ImportGltf(dir / file);
            Check(!result.asset && result.error.find(file) != std::string::npos, "Invalid input diagnostic");
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
