#include "vk_context.h"
#include "vk_helpers.h"
#include "shader_loader.h"
#include "gpu_abi.h"

#include <algorithm>
#include <cstring>
#include <limits>
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
                                          VkQueue init_queue, uint32_t init_queue_family,
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
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                   context.output_draws);
    if (result != VK_SUCCESS) return result;

    // Two words: [0] = draw range for vkCmdDrawIndirectCount (input count;
    // rejected slots are zero-vertex tombstones), [1] = survivor count for
    // the diagnostic readback.
    const uint32_t zeros[2] = {0, 0};
    result = create_uploaded_buffer(physical_device, device, zeros, sizeof(zeros),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                    context.output_count);
    if (result != VK_SUCCESS) return result;

    // Descriptor pool
    VkDescriptorPoolSize pool_sizes[2] = {};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[0].descriptorCount = 12;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[1].descriptorCount = 2;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 2;
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

    // 1x1 far-depth fallback HZB for temporally invalid frames. Allocated
    // once, cleared once to max depth; binding it instead of the previous
    // frame's HZB makes every sample pass the depth test (min_depth >
    // FLT_MAX is never true), so the refine output keeps everything for one
    // conservative frame. Same NEAREST sampler as the real chain; a 1-level
    // view clamps any textureLod level to its only mip, and the shader's
    // footprint reduce degenerates to the single (0.5, 0.5) tap at 1x1.
    VkImageCreateInfo fallback_image_info{};
    fallback_image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    fallback_image_info.imageType = VK_IMAGE_TYPE_2D;
    fallback_image_info.format = VK_FORMAT_R32_SFLOAT;
    fallback_image_info.extent = {1, 1, 1};
    fallback_image_info.mipLevels = 1;
    fallback_image_info.arrayLayers = 1;
    fallback_image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    fallback_image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    fallback_image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    fallback_image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    fallback_image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    result = vkCreateImage(device, &fallback_image_info, nullptr, &context.fallback_hzb_image);
    if (result != VK_SUCCESS) return result;

    VkMemoryRequirements fallback_mem_req{};
    vkGetImageMemoryRequirements(device, context.fallback_hzb_image, &fallback_mem_req);
    VkMemoryAllocateInfo fallback_alloc_info{};
    fallback_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    fallback_alloc_info.allocationSize = fallback_mem_req.size;
    fallback_alloc_info.memoryTypeIndex = find_memory_type(physical_device,
                                                           fallback_mem_req.memoryTypeBits,
                                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (fallback_alloc_info.memoryTypeIndex == kInvalidQueueFamily) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    result = vkAllocateMemory(device, &fallback_alloc_info, nullptr, &context.fallback_hzb_memory);
    if (result != VK_SUCCESS) return result;
    result = vkBindImageMemory(device, context.fallback_hzb_image, context.fallback_hzb_memory, 0);
    if (result != VK_SUCCESS) return result;

    VkImageViewCreateInfo fallback_view_info{};
    fallback_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    fallback_view_info.image = context.fallback_hzb_image;
    fallback_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    fallback_view_info.format = VK_FORMAT_R32_SFLOAT;
    fallback_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    fallback_view_info.subresourceRange.levelCount = 1;
    fallback_view_info.subresourceRange.layerCount = 1;
    result = vkCreateImageView(device, &fallback_view_info, nullptr, &context.fallback_hzb_view);
    if (result != VK_SUCCESS) return result;

    // One-shot clear of the fallback on the caller's graphics queue/family
    // (same init pattern as the HZB context); failures propagate.
    if (init_queue == VK_NULL_HANDLE || init_queue_family == kInvalidQueueFamily) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    {
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = init_queue_family;
        VkCommandPool pool = VK_NULL_HANDLE;
        result = vkCreateCommandPool(device, &pool_info, nullptr, &pool);
        if (result != VK_SUCCESS) return result;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo cmd_alloc{};
        cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd_alloc.commandPool = pool;
        cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_alloc.commandBufferCount = 1;
        result = vkAllocateCommandBuffers(device, &cmd_alloc, &cmd);
        if (result != VK_SUCCESS) {
            vkDestroyCommandPool(device, pool, nullptr);
            return result;
        }
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(cmd, &begin);
        if (result != VK_SUCCESS) {
            vkDestroyCommandPool(device, pool, nullptr);
            return result;
        }
        const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier to_dst{};
        to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_dst.srcAccessMask = 0;
        to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.image = context.fallback_hzb_image;
        to_dst.subresourceRange = range;
        VkImageMemoryBarrier to_read{};
        to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_read.image = context.fallback_hzb_image;
        to_read.subresourceRange = range;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &to_dst);
        VkClearColorValue clear_value{};
        clear_value.float32[0] = std::numeric_limits<float>::max();
        vkCmdClearColorImage(cmd, context.fallback_hzb_image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_value, 1, &range);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &to_read);
        result = vkEndCommandBuffer(cmd);
        if (result == VK_SUCCESS) {
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            result = vkQueueSubmit(init_queue, 1, &submit, VK_NULL_HANDLE);
            if (result == VK_SUCCESS) {
                result = vkQueueWaitIdle(init_queue);
            }
        }
        vkDestroyCommandPool(device, pool, nullptr);
        if (result != VK_SUCCESS) return result;
    }

    VkDescriptorImageInfo hzb_info{};
    hzb_info.sampler = hzb.sampler;
    hzb_info.imageView = hzb_full_view;
    hzb_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    result = vkAllocateDescriptorSets(device, &alloc_info, &context.fallback_descriptor_set);
    if (result != VK_SUCCESS) return result;
    VkDescriptorImageInfo fallback_info{};
    fallback_info.sampler = hzb.sampler;
    fallback_info.imageView = context.fallback_hzb_view;
    fallback_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    const VkDescriptorSet hzb_sets[2] = {context.descriptor_set, context.fallback_descriptor_set};
    const VkDescriptorImageInfo* hzb_infos[2] = {&hzb_info, &fallback_info};
    for (int set_index = 0; set_index < 2; ++set_index) {
        VkWriteDescriptorSet writes[7] = {};
        // SSBOs: bindings 0,1,2,3,5,6
        const uint32_t ssbo_bindings[] = {0, 1, 2, 3, 5, 6};
        for (int i = 0; i < 6; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = hzb_sets[set_index];
            writes[i].dstBinding = ssbo_bindings[i];
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &buf_infos[i];
        }
        // HZB sampler: binding 4
        writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[6].dstSet = hzb_sets[set_index];
        writes[6].dstBinding = 4;
        writes[6].descriptorCount = 1;
        writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[6].pImageInfo = hzb_infos[set_index];
        vkUpdateDescriptorSets(device, 7, writes, 0, nullptr);
    }

    context.hzb_full_view = hzb_full_view;

    return VK_SUCCESS;
}

#endif  // MERIDIAN_VK_CONTEXT_HAS_VULKAN && MERIDIAN_VK_CONTEXT_HAS_GLFW && MERIDIAN_HAS_SHADERC

}  // namespace meridian
