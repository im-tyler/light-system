#include "vk_context.h"
#include "vk_helpers.h"
#include "shader_loader.h"
#include "math_utils.h"
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

#if MERIDIAN_VK_CONTEXT_HAS_VULKAN && MERIDIAN_VK_CONTEXT_HAS_GLFW

VkResult create_depth_resources(VkPhysicalDevice physical_device, VkDevice device,
                                const VkExtent2D& extent, DebugRenderContext& context) {
    context.depth_format = find_depth_format(physical_device);
    if (context.depth_format == VK_FORMAT_UNDEFINED) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent.width = extent.width;
    image_info.extent.height = extent.height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.format = context.depth_format;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateImage(device, &image_info, nullptr, &context.depth_image);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkMemoryRequirements memory_requirements{};
    vkGetImageMemoryRequirements(device, context.depth_image, &memory_requirements);

    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = memory_requirements.size;
    allocate_info.memoryTypeIndex =
        find_memory_type(physical_device, memory_requirements.memoryTypeBits,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocate_info.memoryTypeIndex == kInvalidQueueFamily) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }

    result = vkAllocateMemory(device, &allocate_info, nullptr, &context.depth_memory);
    if (result != VK_SUCCESS) {
        return result;
    }

    result = vkBindImageMemory(device, context.depth_image, context.depth_memory, 0);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = context.depth_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = context.depth_format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    return vkCreateImageView(device, &view_info, nullptr, &context.depth_view);
}

VkResult create_visibility_resources(VkPhysicalDevice physical_device, VkDevice device,
                                     const VkExtent2D& extent, DebugRenderContext& context) {
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent.width = extent.width;
    image_info.extent.height = extent.height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.format = context.visibility_format;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateImage(device, &image_info, nullptr, &context.visibility_image);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkMemoryRequirements memory_requirements{};
    vkGetImageMemoryRequirements(device, context.visibility_image, &memory_requirements);

    VkMemoryAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = memory_requirements.size;
    allocate_info.memoryTypeIndex =
        find_memory_type(physical_device, memory_requirements.memoryTypeBits,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocate_info.memoryTypeIndex == kInvalidQueueFamily) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }

    result = vkAllocateMemory(device, &allocate_info, nullptr, &context.visibility_memory);
    if (result != VK_SUCCESS) {
        return result;
    }

    result = vkBindImageMemory(device, context.visibility_image, context.visibility_memory, 0);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = context.visibility_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = context.visibility_format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    result = vkCreateImageView(device, &view_info, nullptr, &context.visibility_view);
    if (result != VK_SUCCESS) {
        return result;
    }

    const VkDeviceSize readback_size =
        static_cast<VkDeviceSize>(extent.width) * static_cast<VkDeviceSize>(extent.height) * sizeof(uint32_t) * 2;
    return create_uploaded_buffer(physical_device, device, nullptr, readback_size,
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT, context.visibility_readback_buffer);
}

VkResult create_debug_render_context(VkPhysicalDevice physical_device, VkDevice device,
                                     const SwapchainContext& swapchain,
                                     const UploadedSceneBuffers& scene_buffers,
                                     const UploadedBuffer& draw_list,
                                     DebugRenderContext& context) {
#if !MERIDIAN_HAS_SHADERC
    (void)physical_device;
    (void)device;
    (void)swapchain;
    (void)scene_buffers;
    (void)draw_list;
    (void)context;
    return VK_ERROR_FEATURE_NOT_PRESENT;
#else
    VkAttachmentDescription color_attachment{};
    color_attachment.format = swapchain.surface_format.format;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depth_attachment{};
    depth_attachment.format = context.depth_format;
    depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription visibility_attachment{};
    visibility_attachment.format = context.visibility_format;
    visibility_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    visibility_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    visibility_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    visibility_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    visibility_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    visibility_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    visibility_attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkAttachmentReference color_attachment_ref{};
    color_attachment_ref.attachment = 0;
    color_attachment_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depth_attachment_ref{};
    depth_attachment_ref.attachment = 1;
    depth_attachment_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference visibility_attachment_ref{};
    visibility_attachment_ref.attachment = 2;
    visibility_attachment_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_attachment_ref;
    subpass.pDepthStencilAttachment = &depth_attachment_ref;
    subpass.colorAttachmentCount = 2;
    const VkAttachmentReference color_attachments[] = {color_attachment_ref, visibility_attachment_ref};
    subpass.pColorAttachments = color_attachments;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    const VkAttachmentDescription attachments[] = {color_attachment, depth_attachment, visibility_attachment};

    VkResult result = create_depth_resources(physical_device, device, swapchain.extent, context);
    if (result != VK_SUCCESS) {
        return result;
    }
    result = create_visibility_resources(physical_device, device, swapchain.extent, context);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 3;
    render_pass_info.pAttachments = attachments;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    render_pass_info.dependencyCount = 1;
    render_pass_info.pDependencies = &dependency;
    result = vkCreateRenderPass(device, &render_pass_info, nullptr, &context.render_pass);
    if (result != VK_SUCCESS) {
        return result;
    }

    const std::string vertex_shader_source = load_shader_source(resolve_shader_path("main_geometry.vert"));
    const std::string fragment_shader_source = load_shader_source(resolve_shader_path("main_geometry.frag"));

    const std::vector<uint32_t> vert_spirv =
        compile_glsl_to_spirv(vertex_shader_source, shaderc_vertex_shader, "pull_geometry.vert");
    const std::vector<uint32_t> frag_spirv =
        compile_glsl_to_spirv(fragment_shader_source, shaderc_fragment_shader, "pull_geometry.frag");

    VkShaderModule vert_module = create_shader_module(device, vert_spirv);
    VkShaderModule frag_module = create_shader_module(device, frag_spirv);

    VkPipelineShaderStageCreateInfo shader_stages[2] = {};
    shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shader_stages[0].module = vert_module;
    shader_stages[0].pName = "main";
    shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shader_stages[1].module = frag_module;
    shader_stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input_info{};
    vertex_input_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{};
    viewport.width = static_cast<float>(swapchain.extent.width);
    viewport.height = static_cast<float>(swapchain.extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.extent = swapchain.extent;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState color_blend_attachments[2] = {};
    color_blend_attachments[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    color_blend_attachments[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = 2;
    color_blending.pAttachments = color_blend_attachments;

    // Descriptor set layout: 0=base SSBO, 1=lod SSBO, 2=frame UBO, 3=shadow sampler, 4=draw list SSBO
    VkDescriptorSetLayoutBinding ds_bindings[5] = {};
    ds_bindings[0].binding = 0;
    ds_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ds_bindings[0].descriptorCount = 1;
    ds_bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    ds_bindings[1].binding = 1;
    ds_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ds_bindings[1].descriptorCount = 1;
    ds_bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    ds_bindings[2].binding = 2;
    ds_bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ds_bindings[2].descriptorCount = 1;
    ds_bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    ds_bindings[3].binding = 3;
    ds_bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ds_bindings[3].descriptorCount = 1;
    ds_bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    ds_bindings[4].binding = 4;
    ds_bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ds_bindings[4].descriptorCount = 1;
    ds_bindings[4].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = 5;
    set_layout_info.pBindings = ds_bindings;
    result = vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                         &context.descriptor_set_layout);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, frag_module, nullptr);
        vkDestroyShaderModule(device, vert_module, nullptr);
        return result;
    }

    // Create frame UBO buffer
    FrameUBO initial_ubo{};
    result = create_uploaded_buffer(physical_device, device, &initial_ubo, sizeof(FrameUBO),
                                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   context.frame_ubo);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, frag_module, nullptr);
        vkDestroyShaderModule(device, vert_module, nullptr);
        return result;
    }

    // Create 1x1 placeholder depth image for shadow sampler binding until real shadow map is ready
    {
        VkFormat ph_fmt = find_depth_format(physical_device);
        VkImageCreateInfo ph_img{};
        ph_img.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ph_img.imageType = VK_IMAGE_TYPE_2D;
        ph_img.format = ph_fmt;
        ph_img.extent = {1, 1, 1};
        ph_img.mipLevels = 1;
        ph_img.arrayLayers = 1;
        ph_img.samples = VK_SAMPLE_COUNT_1_BIT;
        ph_img.tiling = VK_IMAGE_TILING_OPTIMAL;
        ph_img.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        vkCreateImage(device, &ph_img, nullptr, &context.placeholder_depth_image);
        VkMemoryRequirements ph_req{};
        vkGetImageMemoryRequirements(device, context.placeholder_depth_image, &ph_req);
        VkMemoryAllocateInfo ph_alloc{};
        ph_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ph_alloc.allocationSize = ph_req.size;
        ph_alloc.memoryTypeIndex = find_memory_type(physical_device, ph_req.memoryTypeBits,
                                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(device, &ph_alloc, nullptr, &context.placeholder_depth_memory);
        vkBindImageMemory(device, context.placeholder_depth_image, context.placeholder_depth_memory, 0);
        VkImageViewCreateInfo ph_view{};
        ph_view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ph_view.image = context.placeholder_depth_image;
        ph_view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ph_view.format = ph_fmt;
        ph_view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        ph_view.subresourceRange.levelCount = 1;
        ph_view.subresourceRange.layerCount = 1;
        vkCreateImageView(device, &ph_view, nullptr, &context.placeholder_depth_view);
        VkSamplerCreateInfo ph_samp{};
        ph_samp.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ph_samp.magFilter = VK_FILTER_NEAREST;
        ph_samp.minFilter = VK_FILTER_NEAREST;
        ph_samp.compareEnable = VK_TRUE;
        ph_samp.compareOp = VK_COMPARE_OP_LESS;
        ph_samp.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        ph_samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        ph_samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        vkCreateSampler(device, &ph_samp, nullptr, &context.placeholder_sampler);
    }

    VkDescriptorPoolSize pool_sizes[3] = {};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[0].descriptorCount = 3;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[1].descriptorCount = 1;
    pool_sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[2].descriptorCount = 1;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 3;
    pool_info.pPoolSizes = pool_sizes;
    result = vkCreateDescriptorPool(device, &pool_info, nullptr, &context.descriptor_pool);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, frag_module, nullptr);
        vkDestroyShaderModule(device, vert_module, nullptr);
        return result;
    }

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = context.descriptor_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &context.descriptor_set_layout;
    result = vkAllocateDescriptorSets(device, &alloc_info, &context.descriptor_set);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, frag_module, nullptr);
        vkDestroyShaderModule(device, vert_module, nullptr);
        return result;
    }

    VkDescriptorBufferInfo buffer_infos[4] = {};
    buffer_infos[0].buffer = scene_buffers.base_payload.buffer;
    buffer_infos[0].range = scene_buffers.base_payload.size > 0 ? scene_buffers.base_payload.size : VK_WHOLE_SIZE;
    buffer_infos[1].buffer = scene_buffers.lod_payload.buffer;
    buffer_infos[1].range = scene_buffers.lod_payload.size > 0 ? scene_buffers.lod_payload.size : VK_WHOLE_SIZE;
    buffer_infos[2].buffer = context.frame_ubo.buffer;
    buffer_infos[2].range = sizeof(FrameUBO);
    buffer_infos[3].buffer = draw_list.buffer;
    buffer_infos[3].range = draw_list.size > 0 ? draw_list.size : VK_WHOLE_SIZE;

    VkWriteDescriptorSet writes[4] = {};
    uint32_t write_count = 0;
    if (scene_buffers.base_payload.buffer != VK_NULL_HANDLE) {
        writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write_count].dstSet = context.descriptor_set;
        writes[write_count].dstBinding = 0;
        writes[write_count].descriptorCount = 1;
        writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[write_count].pBufferInfo = &buffer_infos[0];
        write_count++;
    }
    if (scene_buffers.lod_payload.buffer != VK_NULL_HANDLE) {
        writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write_count].dstSet = context.descriptor_set;
        writes[write_count].dstBinding = 1;
        writes[write_count].descriptorCount = 1;
        writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[write_count].pBufferInfo = &buffer_infos[1];
        write_count++;
    }
    writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[write_count].dstSet = context.descriptor_set;
    writes[write_count].dstBinding = 2;
    writes[write_count].descriptorCount = 1;
    writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[write_count].pBufferInfo = &buffer_infos[2];
    write_count++;
    if (draw_list.buffer != VK_NULL_HANDLE) {
        writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write_count].dstSet = context.descriptor_set;
        writes[write_count].dstBinding = 4;
        writes[write_count].descriptorCount = 1;
        writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[write_count].pBufferInfo = &buffer_infos[3];
        write_count++;
    }
    vkUpdateDescriptorSets(device, write_count, writes, 0, nullptr);

    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &context.descriptor_set_layout;
    result = vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &context.pipeline_layout);
    if (result != VK_SUCCESS) {
        vkDestroyShaderModule(device, frag_module, nullptr);
        vkDestroyShaderModule(device, vert_module, nullptr);
        return result;
    }

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = shader_stages;
    pipeline_info.pVertexInputState = &vertex_input_info;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisampling;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blending;
    pipeline_info.layout = context.pipeline_layout;
    pipeline_info.renderPass = context.render_pass;
    pipeline_info.subpass = 0;
    result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                       &context.pipeline);
    vkDestroyShaderModule(device, frag_module, nullptr);
    vkDestroyShaderModule(device, vert_module, nullptr);
    if (result != VK_SUCCESS) {
        return result;
    }

    context.framebuffers.resize(swapchain.image_views.size());
    for (size_t image_index = 0; image_index < swapchain.image_views.size(); ++image_index) {
        VkImageView framebuffer_attachments[] = {swapchain.image_views[image_index], context.depth_view,
                                                 context.visibility_view};
        VkFramebufferCreateInfo framebuffer_info{};
        framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer_info.renderPass = context.render_pass;
        framebuffer_info.attachmentCount = 3;
        framebuffer_info.pAttachments = framebuffer_attachments;
        framebuffer_info.width = swapchain.extent.width;
        framebuffer_info.height = swapchain.extent.height;
        framebuffer_info.layers = 1;
        result = vkCreateFramebuffer(device, &framebuffer_info, nullptr,
                                     &context.framebuffers[image_index]);
        if (result != VK_SUCCESS) {
            return result;
        }
    }

    return VK_SUCCESS;
#endif
}

#endif  // MERIDIAN_VK_CONTEXT_HAS_VULKAN && MERIDIAN_VK_CONTEXT_HAS_GLFW

}  // namespace meridian
