#include "vk_context.h"
#include "vk_helpers.h"
#include "shader_loader.h"
#include "gpu_abi.h"

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

VkResult create_compute_selection_context(VkPhysicalDevice physical_device, VkDevice device,
                                          const UploadedSceneBuffers& scene_buffers,
                                          const ComputeCullContext& cull_context,
                                          uint32_t max_clusters,
                                          ComputeSelectionContext& context) {
    context.max_draws = max_clusters;

    // Shader uses raw uint[] SSBOs to avoid GLSL struct alignment mismatches with C++.
    // Field offsets are hardcoded to match the GPU ABI structs in gpu_abi.h.
    const std::string compute_source = load_shader_source(resolve_shader_path("cluster_select.comp"));

    const std::vector<uint32_t> spirv =
        compile_glsl_to_spirv(compute_source, shaderc_compute_shader, "cluster_select.comp");
    VkShaderModule module = create_shader_module(device, spirv);

    // 11 SSBO bindings
    VkDescriptorSetLayoutBinding bindings[11] = {};
    for (uint32_t i = 0; i < 11; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 11;
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
    push_range.size = sizeof(SelectionPushConstants);

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
    if (result != VK_SUCCESS) return result;

    // Output buffers
    const VkDeviceSize draw_list_size =
        static_cast<VkDeviceSize>(max_clusters) * sizeof(GpuDrawEntry);
    result = create_uploaded_buffer(physical_device, device, nullptr,
                                   std::max(draw_list_size, static_cast<VkDeviceSize>(sizeof(GpuDrawEntry))),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                   context.draw_list);
    if (result != VK_SUCCESS) return result;

    const uint32_t zero = 0;
    result = create_uploaded_buffer(physical_device, device, &zero, sizeof(uint32_t),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                   context.draw_count);
    if (result != VK_SUCCESS) return result;

    // Descriptor pool and set
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 11;

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

    // Bind all 11 buffers
    struct BufferBinding { VkBuffer buffer; VkDeviceSize size; };
    const BufferBinding buffer_map[11] = {
        {cull_context.visible_instances.buffer, cull_context.visible_instances.size},
        {cull_context.counter.buffer, cull_context.counter.size},
        {scene_buffers.hierarchy_nodes.buffer, scene_buffers.hierarchy_nodes.size},
        {scene_buffers.clusters.buffer, scene_buffers.clusters.size},
        {scene_buffers.lod_groups.buffer, scene_buffers.lod_groups.size},
        {scene_buffers.lod_clusters.buffer, scene_buffers.lod_clusters.size},
        {scene_buffers.node_lod_links.buffer, scene_buffers.node_lod_links.size},
        {scene_buffers.page_residency.buffer, scene_buffers.page_residency.size},
        {scene_buffers.instances.buffer, scene_buffers.instances.size},
        {context.draw_list.buffer, context.draw_list.size},
        {context.draw_count.buffer, context.draw_count.size},
    };

    VkDescriptorBufferInfo buf_infos[11] = {};
    VkWriteDescriptorSet writes[11] = {};
    uint32_t write_count = 0;
    for (uint32_t i = 0; i < 11; ++i) {
        if (buffer_map[i].buffer == VK_NULL_HANDLE) continue;
        buf_infos[i].buffer = buffer_map[i].buffer;
        buf_infos[i].range = buffer_map[i].size;
        writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write_count].dstSet = context.descriptor_set;
        writes[write_count].dstBinding = i;
        writes[write_count].descriptorCount = 1;
        writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[write_count].pBufferInfo = &buf_infos[i];
        write_count++;
    }
    if (write_count > 0) {
        vkUpdateDescriptorSets(device, write_count, writes, 0, nullptr);
    }

    return VK_SUCCESS;
}

#endif  // MERIDIAN_VK_CONTEXT_HAS_VULKAN && MERIDIAN_VK_CONTEXT_HAS_GLFW && MERIDIAN_HAS_SHADERC

}  // namespace meridian
