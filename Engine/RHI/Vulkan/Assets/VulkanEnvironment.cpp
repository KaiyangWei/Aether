#include "VulkanEnvironment.h"

namespace Aether
{
VkResult VulkanEnvironment::CreateLayout(VkDevice device, VkDescriptorSetLayout &layout)
{
    VkDescriptorSetLayoutBinding bindings[5]{};
    for (uint32_t i = 0; i < 5; ++i)
        bindings[i] = {i, i == 3 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                       VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = 5;
    info.pBindings = bindings;
    return vkCreateDescriptorSetLayout(device, &info, nullptr, &layout);
}

VkResult VulkanEnvironment::BindMaps()
{
    if (!device || layout || !irradiance.GetView() || !specular.GetView() || !lut.GetView() || !cube.GetView())
        return VK_ERROR_INITIALIZATION_FAILED;
    auto result = CreateLayout(device, layout);
    if (result != VK_SUCCESS)
        return result;
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.maxSets = 1;
    info.poolSizeCount = 2;
    info.pPoolSizes = sizes;
    result = vkCreateDescriptorPool(device, &info, nullptr, &pool);
    if (result != VK_SUCCESS)
        return result;
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &layout;
    result = vkAllocateDescriptorSets(device, &allocate, &set);
    if (result != VK_SUCCESS)
        return result;
    VkDescriptorImageInfo images[] = {
        {VK_NULL_HANDLE, irradiance.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, specular.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, lut.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {sampler},
        {VK_NULL_HANDLE, cube.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[5]{};
    for (uint32_t i = 0; i < 5; ++i)
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     nullptr,
                     set,
                     i,
                     0,
                     1,
                     i == 3 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                     images + i,
                     nullptr,
                     nullptr};
    vkUpdateDescriptorSets(device, 5, writes, 0, nullptr);
    return VK_SUCCESS;
}
} // namespace Aether
