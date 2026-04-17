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

VkResult create_occlusion_refine_context(VkPhysicalDevice physical_device, VkDevice device,
                                          const ComputeSelectionContext& selection_ctx,
                                          const UploadedSceneBuffers& scene_buffers,
                                          const HzbContext& hzb,
                                          uint32_t max_draws,
                                          OcclusionRefineContext& context) {
    context.max_draws = max_draws;

    // Shader: for each draw entry, project cluster AABB to screen, test against HZB
    const std::string compute_source = load_shader_source(resolve_shader_path("occlusion_refine.comp"));

    const std::vector<uint32_t> spirv =
        compile_glsl_to_spirv(compute_source, shaderc_compute_shader, "occlusion_refine.comp");
    VkShaderModule module = create_shader_module(device, spirv);

    // 7 bindings: input draws, input count, clusters, lod_clusters, hzb sampler, output draws, output count
    VkDescriptorSetLayoutBinding bindings[7] = {};
    for (uint32_t i = 0; i < 7; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 7;
    set_layout_info.pBindings = bindings;
    VkResult result = vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                                   &context.descriptor_set_layout);
    if (result != VK_SUCCESS) { vkDestroyShaderModule(device, module, nullptr); return result; }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(OcclusionPushConstants);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &context.descriptor_set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    result = vkCreatePipelineLayout(device, &layout_info, nullptr, &context.pipeline_layout);
    if (result != VK_SUCCESS) { vkDestroyShaderModule(device, module, nullptr); return result; }

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
    const VkDeviceSize draw_size = static_cast<VkDeviceSize>(max_draws) * sizeof(GpuDrawEntry);
    result = create_uploaded_buffer(physical_device, device, nullptr,
                                   std::max(draw_size, static_cast<VkDeviceSize>(sizeof(GpuDrawEntry))),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   context.output_draws);
    if (result != VK_SUCCESS) return result;

    const uint32_t zero = 0;
    result = create_uploaded_buffer(physical_device, device, &zero, sizeof(uint32_t),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   context.output_count);
    if (result != VK_SUCCESS) return result;

    // Descriptor pool
    VkDescriptorPoolSize pool_sizes[2] = {};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[0].descriptorCount = 6;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    result = vkCreateDescriptorPool(device, &pool_info, nullptr, &context.descriptor_pool);
    if (result != VK_SUCCESS) return result;

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = context.descriptor_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &context.descriptor_set_layout;
    result = vkAllocateDescriptorSets(device, &alloc_info, &context.descriptor_set);
    if (result != VK_SUCCESS) return result;

    // Write descriptors
    VkDescriptorBufferInfo buf_infos[6] = {};
    buf_infos[0] = {selection_ctx.draw_list.buffer, 0, selection_ctx.draw_list.size};
    buf_infos[1] = {selection_ctx.draw_count.buffer, 0, selection_ctx.draw_count.size};
    buf_infos[2] = {scene_buffers.clusters.buffer, 0, scene_buffers.clusters.size};
    buf_infos[3] = {scene_buffers.lod_clusters.buffer, 0, scene_buffers.lod_clusters.size};
    buf_infos[4] = {context.output_draws.buffer, 0, context.output_draws.size};
    buf_infos[5] = {context.output_count.buffer, 0, context.output_count.size};

    // HZB sampler -- need a view of the full mip chain
    VkImageViewCreateInfo hzb_full_view_info{};
    hzb_full_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    hzb_full_view_info.image = hzb.image;
    hzb_full_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    hzb_full_view_info.format = VK_FORMAT_R32_SFLOAT;
    hzb_full_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    hzb_full_view_info.subresourceRange.levelCount = hzb.mip_count;
    hzb_full_view_info.subresourceRange.layerCount = 1;
    VkImageView hzb_full_view = VK_NULL_HANDLE;
    result = vkCreateImageView(device, &hzb_full_view_info, nullptr, &hzb_full_view);
    if (result != VK_SUCCESS) return result;

    VkDescriptorImageInfo hzb_info{};
    hzb_info.sampler = hzb.sampler;
    hzb_info.imageView = hzb_full_view;
    hzb_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[7] = {};
    // SSBOs: bindings 0,1,2,3,5,6
    const uint32_t ssbo_bindings[] = {0, 1, 2, 3, 5, 6};
    for (int i = 0; i < 6; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = context.descriptor_set;
        writes[i].dstBinding = ssbo_bindings[i];
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &buf_infos[i];
    }
    // HZB sampler: binding 4
    writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[6].dstSet = context.descriptor_set;
    writes[6].dstBinding = 4;
    writes[6].descriptorCount = 1;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[6].pImageInfo = &hzb_info;
    vkUpdateDescriptorSets(device, 7, writes, 0, nullptr);

    // TODO: track hzb_full_view for cleanup
    (void)hzb_full_view;

    return VK_SUCCESS;
}

#endif  // MERIDIAN_VK_CONTEXT_HAS_VULKAN && MERIDIAN_VK_CONTEXT_HAS_GLFW && MERIDIAN_HAS_SHADERC

}  // namespace meridian
