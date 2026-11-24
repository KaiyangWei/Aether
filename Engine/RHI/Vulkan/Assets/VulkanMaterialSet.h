#pragma once

#include <Assets/GltfImporter.h>
#include <RHI/Vulkan/Resources/VulkanBuffer.h>
#include <RHI/Vulkan/Resources/VulkanTexture.h>
#include <array>
#include <memory>

namespace Aether
{
    class VulkanRenderer;
    class VulkanMemoryManager;

    class VulkanMaterialSet
    {
    public:
        VulkanMaterialSet() = default;
        ~VulkanMaterialSet();
        VulkanMaterialSet(const VulkanMaterialSet&) = delete;
        VulkanMaterialSet& operator=(const VulkanMaterialSet&) = delete;

        void Initialize(VkDevice device, const VulkanMemoryManager& memory,
                        VulkanRenderer& renderer, const ImportedAsset& asset);
        static VkResult CreateLayout(VkDevice device, VkDescriptorSetLayout& layout);
        VkDescriptorSet GetSet(std::optional<size_t> material) const;
        const VulkanTexture* GetBaseColorTexture(std::optional<size_t> material) const;
        size_t GetTextureCount() const { return m_textures.size(); }

    private:
        struct Material
        {
            VulkanBuffer constants;
            VkDescriptorSet set = VK_NULL_HANDLE;
            const VulkanTexture* baseColorTexture = nullptr;
        };
        VkDevice m_device = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_layout = VK_NULL_HANDLE;
        VkDescriptorPool m_pool = VK_NULL_HANDLE;
        std::vector<std::unique_ptr<VulkanTexture>> m_textures;
        std::vector<std::unique_ptr<Material>> m_materials;
    };
}
