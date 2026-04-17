#include "vk_context.h"
#include "vk_helpers.h"
#include "shader_loader.h"

#include <algorithm>
#include <cstring>
#include <vector>

#if __has_include(<shaderc/shaderc.hpp>)
#include <shaderc/shaderc.hpp>
#define MERIDIAN_HAS_SHADERC 1
#else
#define MERIDIAN_HAS_SHADERC 0
#endif

namespace meridian {

#if MERIDIAN_VK_CONTEXT_HAS_VULKAN && MERIDIAN_VK_CONTEXT_HAS_GLFW && MERIDIAN_HAS_SHADERC

VkResult create_compute_cull_context(VkPhysicalDevice physical_device, VkDevice device,
                                     const UploadedSceneBuffers& scene_buffers,
                                     uint32_t instance_count,
                                     ComputeCullContext& context) {
    context.max_instances = instance_count;

    const std::string compute_source = load_shader_source(resolve_shader_path("instance_cull.comp"));

    const std::vector<uint32_t> spirv =
        compile_glsl_to_spirv(compute_source, shaderc_compute_shader, "instance_cull.comp");
    VkShaderModule module = create_shader_module(device, spirv);

    // Descriptor set layout: instances (in), visible list (out), counter (out)
    VkDescriptorSetLayoutBinding bindings[3] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 3;
    set_layout_info.pBindings = bindings;
    VkResult result = vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                                   &context.descriptor_set_layout);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, module, nullptr);
        return result;
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(CullPushConstants);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &context.descriptor_set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    result = vkCreatePipelineLayout(device, &layout_info, nullptr, &context.pipeline_layout);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, module, nullptr);
        return result;
    }

    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = module;
    pipeline_info.stage.pName = "main";
    pipeline_info.layout = context.pipeline_layout;
    result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                       &context.pipeline);
    vkDestroyShaderModule(device, module, nullptr);
    if (result != VK_SUCCESS) {
        return result;
    }

    // Output buffers
    const VkDeviceSize visible_buffer_size =
        static_cast<VkDeviceSize>(instance_count) * sizeof(uint32_t);
    result = create_uploaded_buffer(physical_device, device, nullptr, std::max(visible_buffer_size,
                                   static_cast<VkDeviceSize>(4)),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   context.visible_instances);
    if (result != VK_SUCCESS) return result;

    const uint32_t zero = 0;
    result = create_uploaded_buffer(physical_device, device, &zero, sizeof(uint32_t),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   context.counter);
    if (result != VK_SUCCESS) return result;

    // Descriptor pool and set
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 3;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    result = vkCreateDescriptorPool(device, &pool_info, nullptr, &context.descriptor_pool);
    if (result != VK_SUCCESS) return result;

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = context.descriptor_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &context.descriptor_set_layout;
    result = vkAllocateDescriptorSets(device, &alloc_info, &context.descriptor_set);
    if (result != VK_SUCCESS) return result;

    VkDescriptorBufferInfo buf_infos[3] = {};
    buf_infos[0].buffer = scene_buffers.instances.buffer;
    buf_infos[0].range = scene_buffers.instances.size;
    buf_infos[1].buffer = context.visible_instances.buffer;
    buf_infos[1].range = context.visible_instances.size;
    buf_infos[2].buffer = context.counter.buffer;
    buf_infos[2].range = context.counter.size;

    VkWriteDescriptorSet writes[3] = {};
    for (int i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = context.descriptor_set;
        writes[i].dstBinding = static_cast<uint32_t>(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &buf_infos[i];
    }
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

    return VK_SUCCESS;
}

#endif  // MERIDIAN_VK_CONTEXT_HAS_VULKAN && MERIDIAN_VK_CONTEXT_HAS_GLFW && MERIDIAN_HAS_SHADERC

}  // namespace meridian
