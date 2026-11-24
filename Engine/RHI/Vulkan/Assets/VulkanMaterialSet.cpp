#include "VulkanMaterialSet.h"

#include <Assets/ImageDecoder.h>
#include <RHI/Vulkan/VulkanRenderer.h>
#include <limits>
#include <map>
#include <stdexcept>

namespace Aether
{
    namespace
    {
        void Check(VkResult result, const char* operation)
        {
            if (result != VK_SUCCESS)
                throw std::runtime_error(std::string(operation) + " failed (" + std::to_string(result) + ")");
        }

        VkSamplerAddressMode Wrap(int mode)
        {
            if (mode == 33071)
                return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            if (mode == 33648)
                return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }

        TextureSampling Sampling(const ImportedTexture& texture, const ImportedAsset& asset)
        {
            TextureSampling sampling;
            if (!texture.sampler)
                return sampling;

            const auto& source = asset.samplers.at(*texture.sampler);
            const int minFilter = source.minFilter.value_or(9987);
            sampling.minFilter = minFilter == 9728 || minFilter == 9984 || minFilter == 9986 ?
                VK_FILTER_NEAREST : VK_FILTER_LINEAR;
            sampling.magFilter = source.magFilter == 9728 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
            sampling.mipMode = minFilter == 9984 || minFilter == 9985 ?
                VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
            sampling.mipmapped = minFilter != 9728 && minFilter != 9729;
            sampling.wrapU = Wrap(source.wrapS);
            sampling.wrapV = Wrap(source.wrapT);
            return sampling;
        }

        struct MaterialConstants
        {
            glm::vec4 baseColor;
            glm::vec4 emissiveMetallic;
            glm::vec4 surface;
            glm::vec4 flags;
        };
        static_assert(sizeof(MaterialConstants) == 64);
    }

    VulkanMaterialSet::~VulkanMaterialSet()
    {
        if (m_pool)
            vkDestroyDescriptorPool(m_device, m_pool, nullptr);
        if (m_layout)
            vkDestroyDescriptorSetLayout(m_device, m_layout, nullptr);
    }

    VkResult VulkanMaterialSet::CreateLayout(VkDevice device, VkDescriptorSetLayout& layout)
    {
        std::array<VkDescriptorSetLayoutBinding, 11> bindings{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        for (uint32_t i = 0; i < 5; ++i)
        {
            bindings[1 + i] = {1 + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            bindings[6 + i] = {6 + i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        }
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        return vkCreateDescriptorSetLayout(device, &info, nullptr, &layout);
    }

    void VulkanMaterialSet::Initialize(VkDevice device, const VulkanMemoryManager& memory,
                                       VulkanRenderer& renderer, const ImportedAsset& asset)
    {
        if (m_device || asset.materials.size() >= std::numeric_limits<uint32_t>::max() / 5)
            throw std::runtime_error("Invalid material set size or state");
        m_device = device;
        Check(CreateLayout(device, m_layout), "Material layout creation");
        const auto count = static_cast<uint32_t>(asset.materials.size() + 1);
        const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, count},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, count * 5},
            {VK_DESCRIPTOR_TYPE_SAMPLER, count * 5}
        };
        VkDescriptorPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets = count;
        pool.poolSizeCount = 3;
        pool.pPoolSizes = sizes;
        Check(vkCreateDescriptorPool(device, &pool, nullptr, &m_pool), "Material descriptor pool creation");

        const std::array white{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
        auto fallback = std::make_unique<VulkanTexture>();
        Check(renderer.UploadTexture(*fallback, {1, 1}, false, white, {}), "Fallback texture upload");
        m_textures.push_back(std::move(fallback));
        std::map<std::pair<size_t, bool>, const VulkanTexture*> textureCache;
        const auto resolve = [&](const std::optional<ImportedTextureSlot>& slot, bool srgb) {
            if (!slot)
                return static_cast<const VulkanTexture*>(m_textures.front().get());

            const auto key = std::pair(slot->texture, srgb);
            if (auto cached = textureCache.find(key); cached != textureCache.end())
                return cached->second;

            const auto& source = asset.textures.at(slot->texture);
            try
            {
                auto decoded = DecodeImage(asset.images.at(source.image).bytes);
                auto texture = std::make_unique<VulkanTexture>();
                Check(renderer.UploadTexture(*texture, {decoded.width, decoded.height}, srgb, decoded.rgba,
                    Sampling(source, asset)), "Texture upload");
                const VulkanTexture* result = texture.get();
                m_textures.push_back(std::move(texture));
                textureCache.emplace(key, result);
                return result;
            }
            catch (const std::exception& error)
            {
                throw std::runtime_error("Texture " + std::to_string(slot->texture) + ": " + error.what());
            }
        };

        for (size_t i = 0; i < count; ++i)
        {
            const ImportedMaterial fallbackMaterial;
            const auto& source = i < asset.materials.size() ? asset.materials[i] : fallbackMaterial;
            const std::array textures{
                resolve(source.baseColorTexture, true), resolve(source.metallicRoughnessTexture, false),
                resolve(source.normalTexture, false), resolve(source.occlusionTexture, false),
                resolve(source.emissiveTexture, true)
            };
            const MaterialConstants parameters{
                source.baseColor, glm::vec4(source.emissive, source.metallic),
                {source.roughness, source.normalTexture ? source.normalTexture->strength : 1.0f,
                 source.occlusionTexture ? source.occlusionTexture->strength : 1.0f, source.alphaCutoff},
                {static_cast<float>(source.alphaMode), source.normalTexture ? 1.0f : 0.0f, 0, 0}
            };
            auto material = std::make_unique<Material>();
            material->baseColorTexture = textures[0];
            Check(material->constants.Initialize(device, memory, sizeof(parameters), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT), "Material buffer allocation");
            Check(material->constants.UploadData(&parameters, sizeof(parameters)), "Material constants upload");

            VkDescriptorSetAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocate.descriptorPool = m_pool;
            allocate.descriptorSetCount = 1;
            allocate.pSetLayouts = &m_layout;
            Check(vkAllocateDescriptorSets(device, &allocate, &material->set), "Material descriptor allocation");

            VkDescriptorBufferInfo constants{material->constants.GetBuffer(), 0, sizeof(parameters)};
            std::array<VkDescriptorImageInfo, 5> images{};
            std::array<VkWriteDescriptorSet, 11> writes{};
            writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, material->set, 0, 0, 1,
                         VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &constants, nullptr};
            for (uint32_t slot = 0; slot < textures.size(); ++slot)
            {
                images[slot] = {textures[slot]->GetSampler(), textures[slot]->GetImage().GetView(),
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                writes[1 + slot] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, material->set, 1 + slot,
                    0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &images[slot], nullptr, nullptr};
                writes[6 + slot] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, material->set, 6 + slot,
                    0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &images[slot], nullptr, nullptr};
            }
            vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
            m_materials.push_back(std::move(material));
        }
    }

    VkDescriptorSet VulkanMaterialSet::GetSet(std::optional<size_t> material) const
    {
        return m_materials.at(material.value_or(m_materials.size() - 1))->set;
    }

    const VulkanTexture* VulkanMaterialSet::GetBaseColorTexture(std::optional<size_t> material) const
    {
        if (m_materials.empty())
            return nullptr;
        const size_t idx = material && *material < m_materials.size() ? *material : m_materials.size() - 1;
        return m_materials[idx]->baseColorTexture;
    }
}
