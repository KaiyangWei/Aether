#include "VulkanRenderer.h"
#include "VulkanContext.h"
#include <RHI/Vulkan/Resources/VulkanBuffer.h>
#include <RHI/Vulkan/Pipeline/VulkanShader.h>
#include <array>

namespace Aether
{
namespace
{
struct ComputeResources
{
    VkDevice device;
    VkPipeline pipeline{};
    VkPipelineLayout layout{};
    VkDescriptorPool pool{};
    VkDescriptorSetLayout descriptors{};
    ~ComputeResources()
    {
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);
        vkDestroyDescriptorPool(device, pool, nullptr);
        vkDestroyDescriptorSetLayout(device, descriptors, nullptr);
    }
};
} // namespace

VkResult VulkanRenderer::PrecomputeEnvironment(VulkanEnvironment &env, std::vector<glm::vec4> *verification)
{
    const auto device = m_context->GetDevice();
    ComputeResources resources{device};
    constexpr uint32_t diffuseSize = 32, specSize = 128, lutSize = 128, levels = 8;
    constexpr uint32_t specStart = 6 * diffuseSize * diffuseSize;
    uint32_t lutStart = specStart;
    for (uint32_t side = specSize; side; side /= 2)
        lutStart += 6 * side * side;
    const uint32_t count = lutStart + lutSize * lutSize;
    const auto create = [&](VulkanImage &image, uint32_t side, uint32_t mips, bool cube) {
        return image.Initialize(device, *m_memoryManager, {side, side}, VK_FORMAT_R32G32B32A32_SFLOAT,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, mips, cube);
    };
    auto r = create(env.irradiance, diffuseSize, 1, true);
    if (r == VK_SUCCESS)
        r = create(env.specular, specSize, levels, true);
    if (r == VK_SUCCESS)
        r = create(env.lut, lutSize, 1, false);
    VulkanBuffer buffer;
    const VkDeviceSize bytes = VkDeviceSize(count) * sizeof(glm::vec4);
    if (r == VK_SUCCESS)
        r = buffer.Initialize(
            device, *m_memoryManager, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            verification ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (r != VK_SUCCESS)
        return r;
    const VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER,
                                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    VkDescriptorSetLayoutBinding bindings[3]{};
    VkDescriptorPoolSize sizes[3]{};
    for (uint32_t i = 0; i < 3; ++i)
    {
        bindings[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        sizes[i] = {types[i], 1};
    }
    VkDescriptorSetLayoutCreateInfo desc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    desc.bindingCount = 3;
    desc.pBindings = bindings;
    r = vkCreateDescriptorSetLayout(device, &desc, nullptr, &resources.descriptors);
    if (r != VK_SUCCESS)
        return r;
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 3;
    pool.pPoolSizes = sizes;
    r = vkCreateDescriptorPool(device, &pool, nullptr, &resources.pool);
    if (r != VK_SUCCESS)
        return r;
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = resources.pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &resources.descriptors;
    VkDescriptorSet set{};
    r = vkAllocateDescriptorSets(device, &alloc, &set);
    if (r != VK_SUCCESS)
        return r;
    VkDescriptorImageInfo image{env.sampler, env.cube.GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo storage{buffer.GetBuffer(), 0, bytes};
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; ++i)
        writes[i] = {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, i, 0, 1, types[i], &image, &storage, nullptr};
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &resources.descriptors;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    r = vkCreatePipelineLayout(device, &layout, nullptr, &resources.layout);
    if (r != VK_SUCCESS)
        return r;
    VulkanShader shader;
    if (!shader.Initialize(device, std::string(AETHER_SHADER_DIR) + "/ibl.comp.spv"))
        return VK_ERROR_INITIALIZATION_FAILED;
    VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.layout = resources.layout;
    pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      nullptr,
                      0,
                      VK_SHADER_STAGE_COMPUTE_BIT,
                      shader.GetShaderModule(),
                      "main"};
    r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &resources.pipeline);
    if (r != VK_SUCCESS)
        return r;
    r = EnvironmentCommands([&](VkCommandBuffer cmd) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resources.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, resources.layout, 0, 1, &set, 0, nullptr);
        uint32_t offset = 0;
        auto dispatch = [&](uint32_t side, uint32_t layers, uint32_t mode, float roughness) {
            struct
            {
                uint32_t side, offset, mode;
                float roughness;
            } params{side, offset, mode, roughness};
            vkCmdPushConstants(cmd, resources.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
            vkCmdDispatch(cmd, (side + 7) / 8, (side + 7) / 8, layers);
            offset += side * side * layers;
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask =
                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                                     VK_PIPELINE_STAGE_HOST_BIT,
                                 0, 1, &barrier, 0, nullptr, 0, nullptr);
        };
        dispatch(diffuseSize, 6, 0, 0);
        for (uint32_t mip = 0; mip < levels; ++mip)
            dispatch(specSize >> mip, 6, 1, float(mip) / (levels - 1));
        dispatch(lutSize, 1, 2, 0);
        CopyEnvironmentMap(cmd, env.irradiance, buffer.GetBuffer(), 6, 0);
        CopyEnvironmentMap(cmd, env.specular, buffer.GetBuffer(), 6, specStart * sizeof(glm::vec4));
        CopyEnvironmentMap(cmd, env.lut, buffer.GetBuffer(), 1, lutStart * sizeof(glm::vec4));
    });
    if (r == VK_SUCCESS && verification)
    {
        verification->resize(count);
        r = buffer.ReadData(verification->data(), bytes);
    }
    return r;
}
} // namespace Aether
