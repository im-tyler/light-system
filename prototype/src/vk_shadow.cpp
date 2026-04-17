#include "vk_context.h"
#include "vk_helpers.h"
#include "shader_loader.h"
#include "math_utils.h"
#include "gpu_abi.h"
#include "resource_upload.h"
#include "vgeo_builder.h"

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

VkResult create_shadow_context(VkPhysicalDevice physical_device, VkDevice device,
                               const UploadedSceneBuffers& scene_buffers,
                               const UploadedBuffer& frame_ubo,
                               uint32_t max_draws_per_cascade,
                               const VGeoResource& resource,
                               uint32_t shadow_resolution,
                               ShadowContext& context) {
    context.resolution = shadow_resolution;
    context.max_draws_per_cascade = max_draws_per_cascade;

    // 2D array depth image, one layer per cascade.
    VkFormat depth_format = find_depth_format(physical_device);
    if (depth_format == VK_FORMAT_UNDEFINED) return VK_ERROR_FORMAT_NOT_SUPPORTED;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = depth_format;
    image_info.extent = {shadow_resolution, shadow_resolution, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = kShadowCascadeCount;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkResult result = vkCreateImage(device, &image_info, nullptr, &context.depth_image);
    if (result != VK_SUCCESS) return result;

    VkMemoryRequirements mem_req{};
    vkGetImageMemoryRequirements(device, context.depth_image, &mem_req);
    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_req.size;
    alloc_info.memoryTypeIndex = find_memory_type(physical_device, mem_req.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    result = vkAllocateMemory(device, &alloc_info, nullptr, &context.depth_memory);
    if (result != VK_SUCCESS) return result;
    result = vkBindImageMemory(device, context.depth_image, context.depth_memory, 0);
    if (result != VK_SUCCESS) return result;

    // Full 2D_ARRAY view for sampling in the main pass as sampler2DArrayShadow.
    {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = context.depth_image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        view_info.format = depth_format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = kShadowCascadeCount;
        result = vkCreateImageView(device, &view_info, nullptr, &context.depth_array_view);
        if (result != VK_SUCCESS) return result;
    }

    // Per-layer 2D views for framebuffer attachments (render pass writes one layer).
    for (uint32_t i = 0; i < kShadowCascadeCount; ++i) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = context.depth_image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = depth_format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = i;
        view_info.subresourceRange.layerCount = 1;
        result = vkCreateImageView(device, &view_info, nullptr, &context.cascade_views[i]);
        if (result != VK_SUCCESS) return result;
    }

    // Shadow sampler. MoltenVK's translation of sampler2DArrayShadow samples
    // at zero cost on Apple silicon (Metal depth2d_array.sample_compare does
    // not always route correctly through SPIRV-Cross), so we sample the raw
    // depth with a regular sampler and do the comparison in the fragment shader.
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sampler_info.compareEnable = VK_FALSE;
    result = vkCreateSampler(device, &sampler_info, nullptr, &context.sampler);
    if (result != VK_SUCCESS) return result;

    // Depth-only render pass (identical for all cascades).
    VkAttachmentDescription depth_attach{};
    depth_attach.format = depth_format;
    depth_attach.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_attach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth_attach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth_attach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_attach.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference depth_ref{};
    depth_ref.attachment = 0;
    depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depth_ref;

    VkRenderPassCreateInfo rp_info{};
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_info.attachmentCount = 1;
    rp_info.pAttachments = &depth_attach;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    result = vkCreateRenderPass(device, &rp_info, nullptr, &context.render_pass);
    if (result != VK_SUCCESS) return result;

    // One framebuffer per cascade, bound to that cascade's layer view.
    for (uint32_t i = 0; i < kShadowCascadeCount; ++i) {
        VkFramebufferCreateInfo fb_info{};
        fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_info.renderPass = context.render_pass;
        fb_info.attachmentCount = 1;
        fb_info.pAttachments = &context.cascade_views[i];
        fb_info.width = shadow_resolution;
        fb_info.height = shadow_resolution;
        fb_info.layers = 1;
        result = vkCreateFramebuffer(device, &fb_info, nullptr, &context.framebuffers[i]);
        if (result != VK_SUCCESS) return result;
    }

    // Shadow vertex shader: vertex pulling via indirect draw list SSBO.
    const std::string vert_source = load_shader_source(resolve_shader_path("shadow.vert"));
    const std::vector<uint32_t> vert_spirv =
        compile_glsl_to_spirv(vert_source, shaderc_vertex_shader, "shadow.vert");
    VkShaderModule vert_mod = create_shader_module(device, vert_spirv);

    // Descriptor set: payload SSBOs + frame UBO + draw list SSBO.
    VkDescriptorSetLayoutBinding ds_bindings[4] = {};
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
    ds_bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    ds_bindings[3].binding = 3;
    ds_bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ds_bindings[3].descriptorCount = 1;
    ds_bindings[3].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo ds_layout_info{};
    ds_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ds_layout_info.bindingCount = 4;
    ds_layout_info.pBindings = ds_bindings;
    result = vkCreateDescriptorSetLayout(device, &ds_layout_info, nullptr, &context.descriptor_set_layout);
    if (result != VK_SUCCESS) { vkDestroyShaderModule(device, vert_mod, nullptr); return result; }

    // Push constant range: the shadow vertex shader reads the cascade index
    // to pick which light_vp to transform through.
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(uint32_t);

    VkPipelineLayoutCreateInfo pl_info{};
    pl_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_info.setLayoutCount = 1;
    pl_info.pSetLayouts = &context.descriptor_set_layout;
    pl_info.pushConstantRangeCount = 1;
    pl_info.pPushConstantRanges = &push_range;
    result = vkCreatePipelineLayout(device, &pl_info, nullptr, &context.pipeline_layout);
    if (result != VK_SUCCESS) { vkDestroyShaderModule(device, vert_mod, nullptr); return result; }

    // Depth-only graphics pipeline (no fragment shader).
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = vert_mod;
    stage.pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{};
    viewport.width = static_cast<float>(shadow_resolution);
    viewport.height = static_cast<float>(shadow_resolution);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = {shadow_resolution, shadow_resolution};

    VkPipelineViewportStateCreateInfo vp_state{};
    vp_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp_state.viewportCount = 1;
    vp_state.pViewports = &viewport;
    vp_state.scissorCount = 1;
    vp_state.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rast{};
    rast.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.lineWidth = 1.0f;
    rast.cullMode = VK_CULL_MODE_NONE;
    rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rast.depthBiasEnable = VK_TRUE;
    rast.depthBiasConstantFactor = 1.5f;
    rast.depthBiasSlopeFactor = 1.75f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;

    VkGraphicsPipelineCreateInfo gp_info{};
    gp_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp_info.stageCount = 1;
    gp_info.pStages = &stage;
    gp_info.pVertexInputState = &vi;
    gp_info.pInputAssemblyState = &ia;
    gp_info.pViewportState = &vp_state;
    gp_info.pRasterizationState = &rast;
    gp_info.pMultisampleState = &ms;
    gp_info.pDepthStencilState = &ds;
    gp_info.pColorBlendState = &cb;
    gp_info.layout = context.pipeline_layout;
    gp_info.renderPass = context.render_pass;
    result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp_info, nullptr, &context.pipeline);
    vkDestroyShaderModule(device, vert_mod, nullptr);
    if (result != VK_SUCCESS) return result;

    // Per-cascade draw list + draw count buffers. The CPU fills these each
    // frame with the subset of the main draw list that falls inside each
    // cascade's orthographic frustum. Sizing each buffer to the full cluster
    // count is cheap (32 B / draw) and means the worst-case fallback where
    // every cluster lands in every cascade still fits.
    const VkDeviceSize per_cascade_list_bytes =
        std::max<VkDeviceSize>(
            static_cast<VkDeviceSize>(max_draws_per_cascade) * sizeof(GpuDrawEntry),
            sizeof(GpuDrawEntry));
    for (uint32_t c = 0; c < kShadowCascadeCount; ++c) {
        result = create_uploaded_buffer(
            physical_device, device, nullptr, per_cascade_list_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
            context.cascade_draw_lists[c]);
        if (result != VK_SUCCESS) return result;

        const uint32_t zero = 0;
        result = create_uploaded_buffer(
            physical_device, device, &zero, sizeof(uint32_t),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
            context.cascade_draw_counts[c]);
        if (result != VK_SUCCESS) return result;
    }

    // Descriptor pool sized for 3 sets, each holding 3 storage buffers (two
    // payload SSBOs + one per-cascade draw-list SSBO) + 1 uniform (frame UBO).
    VkDescriptorPoolSize shadow_pool_sizes[2] = {};
    shadow_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    shadow_pool_sizes[0].descriptorCount = 3 * kShadowCascadeCount;
    shadow_pool_sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    shadow_pool_sizes[1].descriptorCount = kShadowCascadeCount;
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = kShadowCascadeCount;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = shadow_pool_sizes;
    result = vkCreateDescriptorPool(device, &pool_info, nullptr, &context.descriptor_pool);
    if (result != VK_SUCCESS) return result;

    VkDescriptorSetLayout layouts[kShadowCascadeCount] = {};
    for (uint32_t c = 0; c < kShadowCascadeCount; ++c) layouts[c] = context.descriptor_set_layout;
    VkDescriptorSetAllocateInfo ds_alloc{};
    ds_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ds_alloc.descriptorPool = context.descriptor_pool;
    ds_alloc.descriptorSetCount = kShadowCascadeCount;
    ds_alloc.pSetLayouts = layouts;
    result = vkAllocateDescriptorSets(device, &ds_alloc, context.cascade_descriptor_sets);
    if (result != VK_SUCCESS) return result;

    // Write bindings for each cascade set. Bindings 0-2 are shared (payloads,
    // frame UBO); binding 3 is the cascade-specific draw list.
    for (uint32_t c = 0; c < kShadowCascadeCount; ++c) {
        VkDescriptorBufferInfo buf_infos[4] = {};
        VkWriteDescriptorSet ds_writes[4] = {};
        uint32_t wc = 0;
        if (scene_buffers.base_payload.buffer != VK_NULL_HANDLE) {
            buf_infos[wc] = {scene_buffers.base_payload.buffer, 0, scene_buffers.base_payload.size};
            ds_writes[wc].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            ds_writes[wc].dstSet = context.cascade_descriptor_sets[c];
            ds_writes[wc].dstBinding = 0;
            ds_writes[wc].descriptorCount = 1;
            ds_writes[wc].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            ds_writes[wc].pBufferInfo = &buf_infos[wc];
            wc++;
        }
        if (scene_buffers.lod_payload.buffer != VK_NULL_HANDLE) {
            buf_infos[wc] = {scene_buffers.lod_payload.buffer, 0, scene_buffers.lod_payload.size};
            ds_writes[wc].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            ds_writes[wc].dstSet = context.cascade_descriptor_sets[c];
            ds_writes[wc].dstBinding = 1;
            ds_writes[wc].descriptorCount = 1;
            ds_writes[wc].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            ds_writes[wc].pBufferInfo = &buf_infos[wc];
            wc++;
        }
        buf_infos[wc] = {frame_ubo.buffer, 0, sizeof(FrameUBO)};
        ds_writes[wc].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ds_writes[wc].dstSet = context.cascade_descriptor_sets[c];
        ds_writes[wc].dstBinding = 2;
        ds_writes[wc].descriptorCount = 1;
        ds_writes[wc].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ds_writes[wc].pBufferInfo = &buf_infos[wc];
        wc++;
        buf_infos[wc] = {context.cascade_draw_lists[c].buffer, 0,
                         context.cascade_draw_lists[c].size};
        ds_writes[wc].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ds_writes[wc].dstSet = context.cascade_descriptor_sets[c];
        ds_writes[wc].dstBinding = 3;
        ds_writes[wc].descriptorCount = 1;
        ds_writes[wc].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ds_writes[wc].pBufferInfo = &buf_infos[wc];
        wc++;
        vkUpdateDescriptorSets(device, wc, ds_writes, 0, nullptr);
    }

    // Cache scene radius so the per-frame CSM fit can size the caster-extent
    // push-back consistently across frames.
    const Vec3f extents = {
        resource.bounds.max.x - resource.bounds.min.x,
        resource.bounds.max.y - resource.bounds.min.y,
        resource.bounds.max.z - resource.bounds.min.z,
    };
    context.scene_radius = std::max({extents.x, extents.y, extents.z, 1.0f}) * 0.75f;

    return VK_SUCCESS;
}

#endif  // MERIDIAN_VK_CONTEXT_HAS_VULKAN && MERIDIAN_VK_CONTEXT_HAS_GLFW && MERIDIAN_HAS_SHADERC

}  // namespace meridian
