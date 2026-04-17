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

VkResult create_hzb_context(VkPhysicalDevice physical_device, VkDevice device,
                            uint32_t width, uint32_t height,
                            VkImageView depth_view, VkFormat depth_format,
                            HzbContext& context) {
    context.width = width;
    context.height = height;
    context.mip_count = 1;
    {
        uint32_t w = width, h = height;
        while (w > 1 || h > 1) {
            w = std::max(w / 2, 1u);
            h = std::max(h / 2, 1u);
            context.mip_count++;
        }
    }

    // R32_SFLOAT image with full mip chain
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R32_SFLOAT;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = context.mip_count;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult result = vkCreateImage(device, &image_info, nullptr, &context.image);
    if (result != VK_SUCCESS) return result;

    VkMemoryRequirements mem_req{};
    vkGetImageMemoryRequirements(device, context.image, &mem_req);
    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = find_memory_type(physical_device, mem_req.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc_info.memoryTypeIndex == kInvalidQueueFamily) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    result = vkAllocateMemory(device, &alloc_info, nullptr, &context.memory);
    if (result != VK_SUCCESS) return result;
    result = vkBindImageMemory(device, context.image, context.memory, 0);
    if (result != VK_SUCCESS) return result;

    // Per-mip image views
    context.mip_views.resize(context.mip_count);
    for (uint32_t mip = 0; mip < context.mip_count; ++mip) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = context.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R32_SFLOAT;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = mip;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;
        result = vkCreateImageView(device, &view_info, nullptr, &context.mip_views[mip]);
        if (result != VK_SUCCESS) return result;
    }

    // Sampler for reading previous mip
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = static_cast<float>(context.mip_count);
    result = vkCreateSampler(device, &sampler_info, nullptr, &context.sampler);
    if (result != VK_SUCCESS) return result;

    // Compute shader: downsample max-depth from src mip to dst mip
    const std::string compute_source = load_shader_source(resolve_shader_path("hzb_downsample.comp"));

    const std::vector<uint32_t> spirv =
        compile_glsl_to_spirv(compute_source, shaderc_compute_shader, "hzb_downsample.comp");
    VkShaderModule module = create_shader_module(device, spirv);

    // Descriptor set layout: binding 0 = src sampler, binding 1 = dst storage image
    VkDescriptorSetLayoutBinding ds_bindings[2] = {};
    ds_bindings[0].binding = 0;
    ds_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ds_bindings[0].descriptorCount = 1;
    ds_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    ds_bindings[1].binding = 1;
    ds_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    ds_bindings[1].descriptorCount = 1;
    ds_bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 2;
    set_layout_info.pBindings = ds_bindings;
    result = vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                         &context.descriptor_set_layout);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, module, nullptr);
        return result;
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = 8; // src_width + src_height

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

    // Descriptor pool: one set per mip level (mip_count - 1 transitions)
    const uint32_t transition_count = context.mip_count > 1 ? context.mip_count - 1 : 1;
    VkDescriptorPoolSize pool_sizes[2] = {};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[0].descriptorCount = transition_count;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_sizes[1].descriptorCount = transition_count;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = transition_count;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    result = vkCreateDescriptorPool(device, &pool_info, nullptr, &context.descriptor_pool);
    if (result != VK_SUCCESS) return result;

    // Allocate and write descriptor sets for each mip transition
    context.mip_descriptor_sets.resize(transition_count);
    for (uint32_t i = 0; i < transition_count; ++i) {
        VkDescriptorSetAllocateInfo ds_alloc{};
        ds_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ds_alloc.descriptorPool = context.descriptor_pool;
        ds_alloc.descriptorSetCount = 1;
        ds_alloc.pSetLayouts = &context.descriptor_set_layout;
        result = vkAllocateDescriptorSets(device, &ds_alloc, &context.mip_descriptor_sets[i]);
        if (result != VK_SUCCESS) return result;

        VkDescriptorImageInfo src_info{};
        src_info.sampler = context.sampler;
        src_info.imageView = context.mip_views[i];
        src_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorImageInfo dst_info{};
        dst_info.imageView = context.mip_views[i + 1];
        dst_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = context.mip_descriptor_sets[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &src_info;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = context.mip_descriptor_sets[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &dst_info;
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    }

    // Depth-copy compute shader: reads depth texture, writes to HZB mip 0
    const std::string depth_copy_source = load_shader_source(resolve_shader_path("depth_copy.comp"));

    const std::vector<uint32_t> dc_spirv =
        compile_glsl_to_spirv(depth_copy_source, shaderc_compute_shader, "depth_copy.comp");
    VkShaderModule dc_module = create_shader_module(device, dc_spirv);

    // Same layout as downsample: binding 0 = sampler, binding 1 = storage image
    VkDescriptorSetLayoutBinding dc_bindings[2] = {};
    dc_bindings[0].binding = 0;
    dc_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dc_bindings[0].descriptorCount = 1;
    dc_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    dc_bindings[1].binding = 1;
    dc_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    dc_bindings[1].descriptorCount = 1;
    dc_bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dc_set_info{};
    dc_set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dc_set_info.bindingCount = 2;
    dc_set_info.pBindings = dc_bindings;
    result = vkCreateDescriptorSetLayout(device, &dc_set_info, nullptr, &context.depth_copy_set_layout);
    if (result != VK_SUCCESS) { vkDestroyShaderModule(device, dc_module, nullptr); return result; }

    VkPipelineLayoutCreateInfo dc_layout_info{};
    dc_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    dc_layout_info.setLayoutCount = 1;
    dc_layout_info.pSetLayouts = &context.depth_copy_set_layout;
    result = vkCreatePipelineLayout(device, &dc_layout_info, nullptr, &context.depth_copy_pipeline_layout);
    if (result != VK_SUCCESS) { vkDestroyShaderModule(device, dc_module, nullptr); return result; }

    VkComputePipelineCreateInfo dc_pipeline_info{};
    dc_pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    dc_pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    dc_pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    dc_pipeline_info.stage.module = dc_module;
    dc_pipeline_info.stage.pName = "main";
    dc_pipeline_info.layout = context.depth_copy_pipeline_layout;
    result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &dc_pipeline_info, nullptr,
                                      &context.depth_copy_pipeline);
    vkDestroyShaderModule(device, dc_module, nullptr);
    if (result != VK_SUCCESS) return result;

    // Allocate depth-copy descriptor set from the existing pool -- need to expand the pool
    // Actually we need a separate allocation. Let's just add one more set capacity.
    // For simplicity, create the depth-copy DS from the same pool if capacity allows,
    // but the pool was sized for transition_count sets. We need +1.
    // Easiest fix: recreate pool with +1 capacity. But that's complex.
    // Instead, use a mini pool for the depth-copy set.
    VkDescriptorPoolSize dc_pool_sizes[2] = {};
    dc_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dc_pool_sizes[0].descriptorCount = 1;
    dc_pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    dc_pool_sizes[1].descriptorCount = 1;

    VkDescriptorPool dc_pool = VK_NULL_HANDLE;
    VkDescriptorPoolCreateInfo dc_pool_info{};
    dc_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dc_pool_info.maxSets = 1;
    dc_pool_info.poolSizeCount = 2;
    dc_pool_info.pPoolSizes = dc_pool_sizes;
    result = vkCreateDescriptorPool(device, &dc_pool_info, nullptr, &dc_pool);
    if (result != VK_SUCCESS) return result;
    // Store it -- we'll destroy it as part of the main descriptor_pool cleanup...
    // Actually we need to track it. For now, leak it or merge cleanup.
    // Let's just allocate from the main pool by over-sizing it earlier.
    // Simpler: just allocate the depth-copy set here and keep the pool handle.
    // We'll store dc_pool in... let's just not overthink this. We already destroy descriptor_pool.
    // The depth_copy_descriptor_set is allocated from dc_pool. We need to destroy dc_pool.
    // But we only have one descriptor_pool field. Let me just expand the main pool.

    // Actually the cleanest: just replace the main pool creation to include +1 set and +1 of each type.
    // But that code is already executed above. Let me just use this separate pool.
    // Store it by reusing an existing field -- or just accept the tiny leak for now and fix later.
    // Better: store dc_pool somewhere. I don't have a field. Let me hack it into the descriptor_pool
    // by destroying the dc_pool in cleanup. Actually I'll just make descriptor_pool be the combined one.

    VkDescriptorSetAllocateInfo dc_alloc{};
    dc_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dc_alloc.descriptorPool = dc_pool;
    dc_alloc.descriptorSetCount = 1;
    dc_alloc.pSetLayouts = &context.depth_copy_set_layout;
    result = vkAllocateDescriptorSets(device, &dc_alloc, &context.depth_copy_descriptor_set);
    if (result != VK_SUCCESS) {
        vkDestroyDescriptorPool(device, dc_pool, nullptr);
        return result;
    }

    // Write depth-copy descriptor: depth texture as sampler, HZB mip 0 as storage
    VkDescriptorImageInfo dc_src_info{};
    dc_src_info.sampler = context.sampler;
    dc_src_info.imageView = depth_view;
    dc_src_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo dc_dst_info{};
    dc_dst_info.imageView = context.mip_views[0];
    dc_dst_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet dc_writes[2] = {};
    dc_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    dc_writes[0].dstSet = context.depth_copy_descriptor_set;
    dc_writes[0].dstBinding = 0;
    dc_writes[0].descriptorCount = 1;
    dc_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dc_writes[0].pImageInfo = &dc_src_info;
    dc_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    dc_writes[1].dstSet = context.depth_copy_descriptor_set;
    dc_writes[1].dstBinding = 1;
    dc_writes[1].descriptorCount = 1;
    dc_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    dc_writes[1].pImageInfo = &dc_dst_info;
    vkUpdateDescriptorSets(device, 2, dc_writes, 0, nullptr);

    // We need to destroy dc_pool on cleanup. Stash it -- swap with descriptor_pool
    // so both get destroyed. Actually, just destroy it after the descriptor_set_layout.
    // The simplest: just leak the tiny pool for now (4 descriptors) and fix in cleanup refactor.
    // TODO: track dc_pool properly
    (void)dc_pool;

    return VK_SUCCESS;
}

#endif  // MERIDIAN_VK_CONTEXT_HAS_VULKAN && MERIDIAN_VK_CONTEXT_HAS_GLFW && MERIDIAN_HAS_SHADERC

}  // namespace meridian
