#include "GltfImporter.h"

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <stdexcept>

namespace Aether
{
    namespace
    {
        template<class T, class Optional>
        std::optional<T> ConvertOptional(const Optional& value)
        {
            return value ? std::optional<T>(static_cast<T>(*value)) : std::nullopt;
        }

        void Require(bool condition, const std::string& message)
        {
            if (!condition)
                throw std::runtime_error(message);
        }

        void Check(fastgltf::Error error)
        {
            Require(error == fastgltf::Error::None, std::string(fastgltf::getErrorMessage(error)));
        }

        void CheckRange(size_t offset, size_t count, size_t stride, size_t width, size_t length)
        {
            Require(count && stride >= width && offset <= length && width <= length - offset &&
                count - 1 <= (length - offset - width) / stride, "Accessor byte range is invalid");
        }

        void ValidateAccessors(const fastgltf::Asset& asset)
        {
            const fastgltf::DefaultBufferDataAdapter buffer;
            for (const auto& item : asset.buffers)
            {
                const auto size = std::visit([](const auto& data) -> size_t {
                    if constexpr (requires { data.bytes.size(); })
                        return data.bytes.size();
                    else
                        return 0;
                }, item.data);
                Require(size >= item.byteLength, "Buffer data is missing or truncated");
            }
            for (const auto& view : asset.bufferViews)
            {
                const auto& source = asset.buffers.at(view.bufferIndex);
                Require(view.byteOffset <= source.byteLength &&
                    view.byteLength <= source.byteLength - view.byteOffset, "BufferView exceeds buffer");
            }
            for (const auto& accessor : asset.accessors)
            {
                const auto width = fastgltf::getElementByteSize(accessor.type, accessor.componentType);
                Require(width != 0 && accessor.count != 0, "Invalid accessor type or count");
                if (accessor.bufferViewIndex)
                {
                    const auto& view = asset.bufferViews.at(*accessor.bufferViewIndex);
                    CheckRange(accessor.byteOffset, accessor.count, view.byteStride.value_or(width),
                        width, view.byteLength);
                }
                if (!accessor.sparse)
                    continue;
                const auto& sparse = *accessor.sparse;
                const auto indexWidth = fastgltf::getComponentByteSize(sparse.indexComponentType);
                Require((sparse.indexComponentType == fastgltf::ComponentType::UnsignedByte ||
                         sparse.indexComponentType == fastgltf::ComponentType::UnsignedShort ||
                         sparse.indexComponentType == fastgltf::ComponentType::UnsignedInt) &&
                         sparse.count <= accessor.count, "Invalid sparse accessor indices");
                CheckRange(sparse.indicesByteOffset, sparse.count, indexWidth, indexWidth,
                    asset.bufferViews.at(sparse.indicesBufferView).byteLength);
                CheckRange(sparse.valuesByteOffset, sparse.count, width, width,
                    asset.bufferViews.at(sparse.valuesBufferView).byteLength);
                const auto bytes = buffer(asset, sparse.indicesBufferView);
                uint32_t previous = 0;
                for (size_t i = 0; i < sparse.count; ++i)
                {
                    uint32_t index = 0;
                    for (size_t j = 0; j < indexWidth; ++j)
                        index |= std::to_integer<uint32_t>(bytes[sparse.indicesByteOffset + i * indexWidth + j]) << (8 * j);
                    Require(index < accessor.count && (!i || index > previous), "Sparse indices must be ascending and in bounds");
                    previous = index;
                }
            }
        }

        ImportedPrimitive ReadPrimitive(const fastgltf::Asset& asset, const fastgltf::Primitive& primitive)
        {
            using Type = fastgltf::AccessorType;
            using enum fastgltf::ComponentType;
            Require(primitive.type == fastgltf::PrimitiveType::Triangles && primitive.targets.empty(),
                "Only static triangle primitives are supported");
            const auto position = primitive.findAttribute("POSITION");
            Require(position != primitive.attributes.end(), "Missing POSITION");
            const auto count = asset.accessors.at(position->accessorIndex).count;
            Require(count <= UINT32_MAX, "Too many vertices");
            ImportedPrimitive output;
            output.material = ConvertOptional<size_t>(primitive.materialIndex);
            output.geometry.vertices.resize(count);
            for (auto& vertex : output.geometry.vertices)
                vertex.color = glm::vec3(1);

            for (const auto& attribute : primitive.attributes)
            {
                const auto& accessor = asset.accessors.at(attribute.accessorIndex);
                const auto& name = attribute.name;
                Require(accessor.count == count, "Mismatched attribute count for " + name);
                const bool floating = accessor.componentType == Float && !accessor.normalized;
                const bool unsignedNormalized = accessor.normalized &&
                    (accessor.componentType == UnsignedByte || accessor.componentType == UnsignedShort);
                if (name == "POSITION" || name == "NORMAL")
                {
                    Require(accessor.type == Type::Vec3 && floating, "Invalid " + name + " accessor");
                    const auto member = name == "POSITION" ? &Vertex::position : &Vertex::normal;
                    fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, accessor,
                        [&](glm::vec3 value, size_t i) { output.geometry.vertices[i].*member = value; });
                }
                else if (name == "TANGENT")
                {
                    Require(accessor.type == Type::Vec4 && floating, "Invalid TANGENT accessor");
                    fastgltf::iterateAccessorWithIndex<glm::vec4>(asset, accessor,
                        [&](glm::vec4 value, size_t i) { output.geometry.vertices[i].tangent = value; });
                }
                else if (name.starts_with("TEXCOORD_"))
                {
                    Require(accessor.type == Type::Vec2 && (floating || unsignedNormalized), "Invalid " + name + " accessor");
                    size_t set = 0;
                    const auto end = name.data() + name.size();
                    const auto result = std::from_chars(name.data() + 9, end, set);
                    Require(result.ec == std::errc{} && result.ptr == end, "Invalid UV set");
                    if (set == 0)
                        fastgltf::iterateAccessorWithIndex<glm::vec2>(asset, accessor,
                            [&](glm::vec2 value, size_t i) { output.geometry.vertices[i].uv = value; });
                    else
                    {
                        auto& values = output.extraTexCoords.emplace_back(set, std::vector<glm::vec2>(count)).second;
                        fastgltf::copyFromAccessor<glm::vec2>(asset, accessor, values.data());
                        for (auto uv : values)
                            Require(std::isfinite(uv.x) && std::isfinite(uv.y), "Non-finite UV attribute");
                    }
                }
                else if (name == "COLOR_0")
                {
                    Require((accessor.type == Type::Vec3 || accessor.type == Type::Vec4) && (floating || unsignedNormalized),
                        "Invalid COLOR_0 accessor");
                    if (accessor.type == Type::Vec3)
                        fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, accessor,
                            [&](glm::vec3 value, size_t i) { output.geometry.vertices[i].color = value; });
                    else
                        fastgltf::iterateAccessorWithIndex<glm::vec4>(asset, accessor,
                            [&](glm::vec4 value, size_t i) {
                                output.geometry.vertices[i].color = glm::vec3(value);
                                output.geometry.vertices[i].colorAlpha = value.w;
                            });
                }
                else
                    throw std::runtime_error("Unsupported vertex attribute " + name);
            }

            auto& indices = output.geometry.indices;
            if (primitive.indicesAccessor)
            {
                const auto& accessor = asset.accessors.at(*primitive.indicesAccessor);
                Require(accessor.type == Type::Scalar && !accessor.normalized && accessor.count <= UINT32_MAX &&
                    (accessor.componentType == UnsignedByte || accessor.componentType == UnsignedShort ||
                     accessor.componentType == UnsignedInt), "Invalid index accessor");
                indices.resize(accessor.count);
                fastgltf::copyFromAccessor<uint32_t>(asset, accessor, indices.data());
            }
            else
            {
                indices.resize(count);
                for (size_t i = 0; i < count; ++i)
                    indices[i] = static_cast<uint32_t>(i);
            }
            Require(output.geometry.IsValid(), "Invalid triangle geometry");
            return output;
        }

        glm::mat4 ReadMatrix(const fastgltf::Node& node)
        {
            if (const auto* matrix = std::get_if<fastgltf::math::nmat4x4>(&node.transform))
            {
                glm::mat4 result(1);
                for (int col = 0; col < 4; ++col)
                    for (int row = 0; row < 4; ++row)
                        result[col][row] = (*matrix)[col][row];
                return result;
            }
            const auto& trs = std::get<fastgltf::TRS>(node.transform);
            return glm::translate(glm::mat4(1), {trs.translation[0], trs.translation[1], trs.translation[2]}) *
                glm::mat4_cast(glm::quat(trs.rotation[3], trs.rotation[0], trs.rotation[1], trs.rotation[2])) *
                glm::scale(glm::mat4(1), {trs.scale[0], trs.scale[1], trs.scale[2]});
        }

        void ReadNodes(const fastgltf::Asset& source, ImportedAsset& asset)
        {
            for (const auto& node : source.nodes)
            {
                Require(!node.skinIndex && !node.cameraIndex && node.weights.empty(), "Unsupported node feature");
                asset.nodes.push_back({node.name, ConvertOptional<size_t>(node.meshIndex), {}, node.children, ReadMatrix(node)});
            }
            for (size_t i = 0; i < asset.nodes.size(); ++i)
                for (auto child : asset.nodes[i].children)
                {
                    auto& node = asset.nodes.at(child);
                    Require(!node.parent, "Node has multiple parents");
                    node.parent = i;
                }

            std::vector<size_t> pending;
            for (size_t i = 0; i < asset.nodes.size(); ++i)
                if (!asset.nodes[i].parent)
                    pending.push_back(i);
            if (source.scenes.empty())
                asset.roots = pending;
            else
                asset.roots = source.scenes.at(source.defaultScene.value_or(0)).nodeIndices;
            std::vector<bool> selected(asset.nodes.size());
            for (auto root : asset.roots)
            {
                Require(!asset.nodes.at(root).parent && !selected.at(root), "Invalid or duplicate scene root");
                selected[root] = true;
            }

            for (size_t i = 0; i < pending.size(); ++i)
            {
                auto& node = asset.nodes[pending[i]];
                node.world = node.parent ? asset.nodes[*node.parent].world * node.local : node.local;
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        Require(std::isfinite(node.world[c][r]), "Non-finite node transform");
                pending.insert(pending.end(), node.children.begin(), node.children.end());
            }
            Require(pending.size() == asset.nodes.size(), "Node hierarchy contains a cycle");
        }

        ImportedImage ReadImage(const fastgltf::Asset& asset, const fastgltf::Image& image)
        {
            ImportedImage result;
            const auto copy = [&](auto bytes, fastgltf::MimeType mime) {
                result.bytes.assign(bytes.begin(), bytes.end());
                using enum fastgltf::MimeType;
                if (mime == PNG)
                    result.mimeType = "image/png";
                else if (mime == JPEG)
                    result.mimeType = "image/jpeg";
                else if (mime != None)
                    throw std::runtime_error("Unsupported image MIME type");
            };
            if (const auto* view = std::get_if<fastgltf::sources::BufferView>(&image.data))
                copy(fastgltf::DefaultBufferDataAdapter{}(asset, view->bufferViewIndex), view->mimeType);
            else if (const auto* bytes = std::get_if<fastgltf::sources::Array>(&image.data))
                copy(bytes->bytes, bytes->mimeType);
            else
                throw std::runtime_error("Unable to load image " + image.name);

            const auto& b = result.bytes;
            const bool png = b.size() >= 8 && b[0] == std::byte{0x89} && b[1] == std::byte{0x50} &&
                b[2] == std::byte{0x4e} && b[3] == std::byte{0x47} && b[4] == std::byte{0x0d} &&
                b[5] == std::byte{0x0a} && b[6] == std::byte{0x1a} && b[7] == std::byte{0x0a};
            const bool jpeg = b.size() >= 3 && b[0] == std::byte{0xff} && b[1] == std::byte{0xd8} && b[2] == std::byte{0xff};
            const std::string detected = png ? "image/png" : jpeg ? "image/jpeg" : "";
            Require(!detected.empty() && (result.mimeType.empty() || result.mimeType == detected),
                "Unsupported or mismatched image signature");
            result.mimeType = detected;
            return result;
        }

        ImportedMaterial ReadMaterial(const fastgltf::Material& value)
        {
            ImportedMaterial result;
            const auto& pbr = value.pbrData;
            result.baseColor = {pbr.baseColorFactor[0], pbr.baseColorFactor[1], pbr.baseColorFactor[2], pbr.baseColorFactor[3]};
            result.emissive = {value.emissiveFactor[0], value.emissiveFactor[1], value.emissiveFactor[2]};
            result.metallic = pbr.metallicFactor;
            result.roughness = pbr.roughnessFactor;
            result.alphaCutoff = value.alphaCutoff;
            result.doubleSided = value.doubleSided;
            if (value.alphaMode == fastgltf::AlphaMode::Mask)
                result.alphaMode = ImportedMaterial::AlphaMode::Mask;
            else if (value.alphaMode == fastgltf::AlphaMode::Blend)
                result.alphaMode = ImportedMaterial::AlphaMode::Blend;

            const auto slot = [](const auto& info, float strength = 1) -> std::optional<ImportedTextureSlot> {
                if (!info)
                    return std::nullopt;
                return ImportedTextureSlot{info->textureIndex, info->texCoordIndex, strength};
            };
            result.baseColorTexture = slot(pbr.baseColorTexture);
            result.metallicRoughnessTexture = slot(pbr.metallicRoughnessTexture);
            result.normalTexture = slot(value.normalTexture, value.normalTexture ? value.normalTexture->scale : 1);
            result.occlusionTexture = slot(value.occlusionTexture, value.occlusionTexture ? value.occlusionTexture->strength : 1);
            result.emissiveTexture = slot(value.emissiveTexture);
            return result;
        }
    }

    GltfImportResult ImportGltf(const std::filesystem::path& path)
    {
        try
        {
            const auto absolute = std::filesystem::absolute(path);
            auto data = fastgltf::GltfDataBuffer::FromPath(absolute);
            Check(data.error());
            fastgltf::Parser parser;
            auto parsed = parser.loadGltf(data.get(), absolute.parent_path(),
                fastgltf::Options::LoadExternalBuffers | fastgltf::Options::LoadExternalImages);
            Check(parsed.error());
            const auto& source = parsed.get();
            if (!source.extensionsUsed.empty())
                throw std::runtime_error("Unsupported glTF extension " + source.extensionsUsed.front());
            Require(source.animations.empty() && source.skins.empty(), "Animations and skins are unsupported");
            ValidateAccessors(source);
            Check(fastgltf::validate(source));

            ImportedAsset asset;
            ReadNodes(source, asset);
            for (size_t m = 0; m < source.meshes.size(); ++m)
            {
                auto& mesh = asset.meshes.emplace_back();
                for (const auto& primitive : source.meshes[m].primitives)
                {
                    try { mesh.push_back(ReadPrimitive(source, primitive)); }
                    catch (const std::exception& error)
                    {
                        throw std::runtime_error("mesh " + std::to_string(m) + " primitive " +
                            std::to_string(mesh.size()) + ": " + error.what());
                    }
                }
            }
            for (const auto& material : source.materials)
                asset.materials.push_back(ReadMaterial(material));
            for (const auto& sampler : source.samplers)
                asset.samplers.push_back({ConvertOptional<int>(sampler.minFilter), ConvertOptional<int>(sampler.magFilter),
                    static_cast<int>(sampler.wrapS), static_cast<int>(sampler.wrapT)});
            for (const auto& texture : source.textures)
            {
                Require(texture.imageIndex.has_value(), "Texture has no image");
                asset.textures.push_back({*texture.imageIndex, ConvertOptional<size_t>(texture.samplerIndex)});
            }
            for (const auto& image : source.images)
                asset.images.push_back(ReadImage(source, image));
            return {std::move(asset), {}};
        }
        catch (const std::exception& error)
        {
            const auto utf8 = path.u8string();
            return {{}, std::string(utf8.begin(), utf8.end()) + ": " + error.what()};
        }
    }
}
