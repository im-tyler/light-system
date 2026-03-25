#pragma once

#include "resource_upload.h"

#include <string>
#include <vector>

namespace meridian {

struct VkBootstrapConfig {
    bool enable_validation = false;
    bool visible_window = false;
    bool interactive = false;
    uint32_t window_width = 1280;
    uint32_t window_height = 720;
    uint32_t present_frame_count = 3;
    float debug_error_threshold = 0.001f;
    uint32_t resident_budget = 0xffffffffu;
    uint32_t eviction_grace_frames = 1;
};

struct VkBootstrapReport {
    bool compiled_with_vulkan = false;
    bool instance_created = false;
    bool window_created = false;
    bool surface_created = false;
    bool device_created = false;
    bool swapchain_created = false;
    bool scene_buffers_uploaded = false;
    bool debug_pipeline_created = false;
    bool debug_geometry_uploaded = false;
    bool visibility_attachment_created = false;
    bool visibility_readback_ready = false;
    bool debug_draw_submitted = false;
    bool present_loop_completed = false;
    std::string status;
    std::string selected_device;
    uint32_t graphics_queue_family = 0xffffffffu;
    uint32_t present_queue_family = 0xffffffffu;
    uint32_t swapchain_image_count = 0;
    uint32_t uploaded_buffer_count = 0;
    uint64_t uploaded_buffer_bytes = 0;
    uint32_t debug_selected_node_count = 0;
    uint32_t debug_rendered_cluster_count = 0;
    uint32_t debug_rendered_lod_cluster_count = 0;
    uint32_t replay_selected_node_count = 0;
    uint32_t replay_selected_cluster_count = 0;
    uint32_t replay_selected_lod_cluster_count = 0;
    uint32_t replay_selected_page_count = 0;
    bool replay_runtime_parity = false;
    uint32_t runtime_missing_page_count = 0;
    uint32_t runtime_prefetch_page_count = 0;
    uint32_t runtime_requested_page_count = 0;
    uint32_t runtime_loading_page_count = 0;
    uint32_t runtime_completed_page_count = 0;
    uint32_t runtime_resident_page_count = 0;
    uint32_t visibility_valid_pixels = 0;
    uint32_t visibility_unique_base_geometry = 0;
    uint32_t visibility_unique_lod_geometry = 0;
    uint32_t visibility_invalid_ids = 0;
    uint32_t visibility_visible_selected_base_geometry = 0;
    uint32_t visibility_visible_selected_lod_geometry = 0;
    uint32_t visibility_invisible_selected_base_geometry = 0;
    uint32_t visibility_invisible_selected_lod_geometry = 0;
    bool visibility_selection_subset = false;
    uint32_t compute_cull_visible_instances = 0;
    uint32_t compute_selection_draw_count = 0;
    uint32_t compute_occlusion_surviving_draws = 0;
    uint32_t debug_triangle_count = 0;
    uint32_t debug_vertex_count = 0;
    float debug_camera_distance = 0.0f;
    uint32_t presented_frame_count = 0;
    uint32_t swapchain_width = 0;
    uint32_t swapchain_height = 0;
    std::vector<std::string> physical_devices;
    UploadableScene uploadable_scene;
};

VkBootstrapReport build_vk_bootstrap_report(const VGeoResource& resource,
                                            const VkBootstrapConfig& config);

}  // namespace meridian
