#pragma once
#include <RHI/Vulkan/Resources/VulkanImage.h>
#include <Assets/HdrImage.h>

namespace Aether
{
class VulkanEnvironment
{
  public:
    VulkanEnvironment() = default;
    VulkanEnvironment(const VulkanEnvironment &) = delete;
    ~VulkanEnvironment();
    VkDescriptorSet Descriptor() const
    {
        return set;
    }
    VkResult BindMaps();
    static VkResult CreateLayout(VkDevice device, VkDescriptorSetLayout &layout);

  private:
    friend class VulkanRenderer;
    VkDevice device{};
    VulkanImage panorama, cube, irradiance, specular, lut;
    VkSampler sampler{};
    VkDescriptorSetLayout layout{};
    VkDescriptorPool pool{};
    VkDescriptorSet set{};
};
} // namespace Aether
