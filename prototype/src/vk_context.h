#pragma once

#include "math_utils.h"

#include <cstdint>
#include <vector>

#if __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>
#if defined(__APPLE__) && __has_include(<vulkan/vulkan_metal.h>)
#include <vulkan/vulkan_metal.h>
#endif
#define MERIDIAN_VK_CONTEXT_HAS_VULKAN 1
#else
#define MERIDIAN_VK_CONTEXT_HAS_VULKAN 0
#endif

#if __has_include(<GLFW/glfw3.h>)
#include <GLFW/glfw3.h>
#define MERIDIAN_VK_CONTEXT_HAS_GLFW 1
#else
#define MERIDIAN_VK_CONTEXT_HAS_GLFW 0
#endif

namespace meridian {

constexpr uint32_t kInvalidQueueFamily = 0xffffffffu;

#if MERIDIAN_VK_CONTEXT_HAS_VULKAN && MERIDIAN_VK_CONTEXT_HAS_GLFW

struct QueueFamilySelection {
    uint32_t graphics_family = kInvalidQueueFamily;
    uint32_t present_family = kInvalidQueueFamily;

    bool complete() const {
        return graphics_family != kInvalidQueueFamily && present_family != kInvalidQueueFamily;
    }
};

struct DeviceSelection {
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    QueueFamilySelection queues;
    bool enable_portability_subset = false;
    bool has_draw_indirect_count = false;
};

struct SwapchainContext {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkSurfaceFormatKHR surface_format{};
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> image_views;
};

struct FrameContext {
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkSemaphore image_available = VK_NULL_HANDLE;
    VkSemaphore render_finished = VK_NULL_HANDLE;
    VkFence in_flight = VK_NULL_HANDLE;
};

struct UploadedBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

struct UploadedSceneBuffers {
    UploadedBuffer header;
    UploadedBuffer instances;
    UploadedBuffer hierarchy_nodes;
    UploadedBuffer clusters;
    UploadedBuffer lod_groups;
    UploadedBuffer lod_clusters;
    UploadedBuffer node_lod_links;
    UploadedBuffer pages;
    UploadedBuffer page_dependencies;
    UploadedBuffer page_residency;
    UploadedBuffer base_payload;
    UploadedBuffer lod_payload;
};

struct DebugRenderContext {
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;
    VkImage depth_image = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    VkImage visibility_image = VK_NULL_HANDLE;
    VkDeviceMemory visibility_memory = VK_NULL_HANDLE;
    VkImageView visibility_view = VK_NULL_HANDLE;
    VkFormat visibility_format = VK_FORMAT_R32G32_UINT;
    UploadedBuffer visibility_readback_buffer;
    UploadedBuffer frame_ubo;
    VkImage placeholder_depth_image = VK_NULL_HANDLE;
    VkDeviceMemory placeholder_depth_memory = VK_NULL_HANDLE;
    VkImageView placeholder_depth_view = VK_NULL_HANDLE;
    VkSampler placeholder_sampler = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
};

struct CullPushConstants {
    float frustum_planes[6][4];
    uint32_t instance_count;
    uint32_t pad[3];
};

struct ComputeCullContext {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    UploadedBuffer visible_instances;
    UploadedBuffer counter;
    uint32_t max_instances = 0;
};

struct SelectionPushConstants {
    float error_threshold;
    float camera_pos[3];
    uint32_t pad[4];
};

struct HzbContext {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::vector<VkImageView> mip_views;
    VkSampler sampler = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline depth_copy_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout depth_copy_pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout depth_copy_set_layout = VK_NULL_HANDLE;
    VkDescriptorSet depth_copy_descriptor_set = VK_NULL_HANDLE;
    VkDescriptorPool depth_copy_descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> mip_descriptor_sets;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mip_count = 0;
};

struct ComputeSelectionContext {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    UploadedBuffer draw_list;
    UploadedBuffer draw_count;
    uint32_t max_draws = 0;
};

struct OcclusionPushConstants {
    float view_projection[16];
    uint32_t hzb_width;
    uint32_t hzb_height;
    uint32_t pad[2];
};

struct OcclusionRefineContext {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkImageView hzb_full_view = VK_NULL_HANDLE;
    UploadedBuffer output_draws;
    UploadedBuffer output_count;
    uint32_t max_draws = 0;
};

struct ShadowContext {
    VkImage depth_image = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory = VK_NULL_HANDLE;
    VkImageView depth_array_view = VK_NULL_HANDLE;
    VkImageView cascade_views[kShadowCascadeCount] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkFramebuffer framebuffers[kShadowCascadeCount] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkSampler sampler = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    // Per-cascade descriptor sets + draw buffers. Each cascade samples the
    // same payload + frame UBO but pulls its draw list from its own buffer,
    // which the CPU fills with clusters that pass that cascade's frustum test.
    VkDescriptorSet cascade_descriptor_sets[kShadowCascadeCount] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    UploadedBuffer cascade_draw_lists[kShadowCascadeCount];
    UploadedBuffer cascade_draw_counts[kShadowCascadeCount];
    uint32_t max_draws_per_cascade = 0;
    uint32_t resolution = 0;
    CascadeLightSetup cascades{};
    float scene_radius = 1.0f;
};

void destroy_uploaded_buffer(VkDevice device, UploadedBuffer& uploaded_buffer);
void destroy_uploaded_scene_buffers(VkDevice device, UploadedSceneBuffers& buffers);
void destroy_debug_render_context(VkDevice device, DebugRenderContext& context);
void destroy_frame_context(VkDevice device, FrameContext& frame);
void destroy_swapchain(VkDevice device, SwapchainContext& swapchain);
void destroy_compute_cull_context(VkDevice device, ComputeCullContext& context);
void destroy_hzb_context(VkDevice device, HzbContext& context);
void destroy_compute_selection_context(VkDevice device, ComputeSelectionContext& context);
void destroy_occlusion_refine_context(VkDevice device, OcclusionRefineContext& context);
void destroy_shadow_context(VkDevice device, ShadowContext& context);

// Create functions (implementations in per-subsystem .cpp files)
struct VGeoResource;

VkResult create_compute_cull_context(VkPhysicalDevice physical_device, VkDevice device,
                                     const UploadedSceneBuffers& scene_buffers,
                                     uint32_t instance_count,
                                     ComputeCullContext& context);

VkResult create_compute_selection_context(VkPhysicalDevice physical_device, VkDevice device,
                                          const UploadedSceneBuffers& scene_buffers,
                                          const ComputeCullContext& cull_context,
                                          uint32_t max_clusters,
                                          ComputeSelectionContext& context);

VkResult create_hzb_context(VkPhysicalDevice physical_device, VkDevice device,
                            uint32_t width, uint32_t height,
                            VkImageView depth_view, VkFormat depth_format,
                            HzbContext& context);

VkResult create_occlusion_refine_context(VkPhysicalDevice physical_device, VkDevice device,
                                          const ComputeSelectionContext& selection_ctx,
                                          const UploadedSceneBuffers& scene_buffers,
                                          const HzbContext& hzb,
                                          uint32_t max_draws,
                                          OcclusionRefineContext& context);

VkResult create_shadow_context(VkPhysicalDevice physical_device, VkDevice device,
                               const UploadedSceneBuffers& scene_buffers,
                               const UploadedBuffer& frame_ubo,
                               uint32_t max_draws_per_cascade,
                               const VGeoResource& resource,
                               uint32_t shadow_resolution,
                               ShadowContext& context);

VkResult create_depth_resources(VkPhysicalDevice physical_device, VkDevice device,
                                const VkExtent2D& extent, DebugRenderContext& context);

VkResult create_visibility_resources(VkPhysicalDevice physical_device, VkDevice device,
                                     const VkExtent2D& extent, DebugRenderContext& context);

VkResult create_debug_render_context(VkPhysicalDevice physical_device, VkDevice device,
                                     const SwapchainContext& swapchain,
                                     const UploadedSceneBuffers& scene_buffers,
                                     const UploadedBuffer& draw_list,
                                     DebugRenderContext& context);

#endif  // MERIDIAN_VK_CONTEXT_HAS_VULKAN && MERIDIAN_VK_CONTEXT_HAS_GLFW

}  // namespace meridian
