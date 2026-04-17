#include "vk_bootstrap.h"
#include "visibility_format.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

void print_usage() {
    std::cerr << "Usage: meridian_vk_bootstrap --manifest <path> [--interactive] [--screenshot <path>] [--budget <pages>] [--demand-streaming]\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path manifest_path;
    std::string screenshot_path;
    uint32_t resident_budget = 0xffffffffu;
    bool interactive = false;
    bool demand_streaming = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--manifest" && i + 1 < argc) {
            manifest_path = argv[++i];
        } else if (arg == "--screenshot" && i + 1 < argc) {
            screenshot_path = argv[++i];
        } else if (arg == "--budget" && i + 1 < argc) {
            resident_budget = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (arg == "--interactive") {
            interactive = true;
        } else if (arg == "--demand-streaming") {
            demand_streaming = true;
        } else {
            print_usage();
            return 1;
        }
    }
    if (manifest_path.empty()) {
        print_usage();
        return 1;
    }

    try {
        const meridian::BuildManifest manifest = meridian::load_manifest(manifest_path);
        const meridian::VGeoResource resource = meridian::build_resource(manifest);
        meridian::validate_resource(resource);

        meridian::VkBootstrapConfig config{};
        if (interactive) {
            config.interactive = true;
            config.visible_window = true;
            config.present_frame_count = 0xffffffffu;
        }
        config.screenshot_path = screenshot_path;
        config.resident_budget = resident_budget;
        config.demand_streaming = demand_streaming;
        const meridian::VkBootstrapReport report =
            meridian::build_vk_bootstrap_report(resource, config);

        std::cout << "asset_id=" << resource.asset_id << '\n';
        std::cout << "compiled_with_vulkan=" << (report.compiled_with_vulkan ? "true" : "false")
                  << '\n';
        std::cout << "instance_created=" << (report.instance_created ? "true" : "false") << '\n';
        std::cout << "window_created=" << (report.window_created ? "true" : "false") << '\n';
        std::cout << "surface_created=" << (report.surface_created ? "true" : "false") << '\n';
        std::cout << "device_created=" << (report.device_created ? "true" : "false") << '\n';
        std::cout << "swapchain_created=" << (report.swapchain_created ? "true" : "false") << '\n';
        std::cout << "scene_buffers_uploaded=" << (report.scene_buffers_uploaded ? "true" : "false")
                  << '\n';
        std::cout << "debug_pipeline_created=" << (report.debug_pipeline_created ? "true" : "false")
                  << '\n';
        std::cout << "debug_geometry_uploaded=" << (report.debug_geometry_uploaded ? "true" : "false")
                  << '\n';
        std::cout << "visibility_attachment_created="
                  << (report.visibility_attachment_created ? "true" : "false") << '\n';
        std::cout << "visibility_readback_ready="
                  << (report.visibility_readback_ready ? "true" : "false") << '\n';
        std::cout << "debug_draw_submitted=" << (report.debug_draw_submitted ? "true" : "false")
                  << '\n';
        std::cout << "present_loop_completed=" << (report.present_loop_completed ? "true" : "false")
                  << '\n';
        std::cout << "status=" << report.status << '\n';
        std::cout << "selected_device=" << report.selected_device << '\n';
        std::cout << "graphics_queue_family=" << report.graphics_queue_family << '\n';
        std::cout << "present_queue_family=" << report.present_queue_family << '\n';
        std::cout << "swapchain_images=" << report.swapchain_image_count << '\n';
        std::cout << "uploaded_buffer_count=" << report.uploaded_buffer_count << '\n';
        std::cout << "uploaded_buffer_bytes=" << report.uploaded_buffer_bytes << '\n';
        std::cout << "debug_selected_nodes=" << report.debug_selected_node_count << '\n';
        std::cout << "debug_rendered_clusters=" << report.debug_rendered_cluster_count << '\n';
        std::cout << "debug_rendered_lod_clusters=" << report.debug_rendered_lod_cluster_count << '\n';
        std::cout << "replay_selected_nodes=" << report.replay_selected_node_count << '\n';
        std::cout << "replay_selected_clusters=" << report.replay_selected_cluster_count << '\n';
        std::cout << "replay_selected_lod_clusters=" << report.replay_selected_lod_cluster_count << '\n';
        std::cout << "replay_selected_pages=" << report.replay_selected_page_count << '\n';
        std::cout << "replay_runtime_parity=" << (report.replay_runtime_parity ? "true" : "false")
                  << '\n';
        std::cout << "runtime_missing_pages=" << report.runtime_missing_page_count << '\n';
        std::cout << "runtime_prefetch_pages=" << report.runtime_prefetch_page_count << '\n';
        std::cout << "runtime_requested_pages=" << report.runtime_requested_page_count << '\n';
        std::cout << "runtime_loading_pages=" << report.runtime_loading_page_count << '\n';
        std::cout << "runtime_completed_pages=" << report.runtime_completed_page_count << '\n';
        std::cout << "runtime_resident_pages=" << report.runtime_resident_page_count << '\n';
        std::cout << "visibility_valid_pixels=" << report.visibility_valid_pixels << '\n';
        std::cout << "visibility_unique_base_geometry=" << report.visibility_unique_base_geometry << '\n';
        std::cout << "visibility_unique_lod_geometry=" << report.visibility_unique_lod_geometry << '\n';
        std::cout << "visibility_invalid_ids=" << report.visibility_invalid_ids << '\n';
        std::cout << "visibility_visible_selected_base_geometry="
                  << report.visibility_visible_selected_base_geometry << '\n';
        std::cout << "visibility_visible_selected_lod_geometry="
                  << report.visibility_visible_selected_lod_geometry << '\n';
        std::cout << "visibility_invisible_selected_base_geometry="
                  << report.visibility_invisible_selected_base_geometry << '\n';
        std::cout << "visibility_invisible_selected_lod_geometry="
                  << report.visibility_invisible_selected_lod_geometry << '\n';
        std::cout << "visibility_selection_subset="
                  << (report.visibility_selection_subset ? "true" : "false") << '\n';
        std::cout << "compute_cull_visible_instances=" << report.compute_cull_visible_instances << '\n';
        std::cout << "compute_selection_draw_count=" << report.compute_selection_draw_count << '\n';
        std::cout << "compute_occlusion_surviving=" << report.compute_occlusion_surviving_draws << '\n';
        std::cout << "debug_triangles=" << report.debug_triangle_count << '\n';
        std::cout << "debug_vertices=" << report.debug_vertex_count << '\n';
        std::cout << "debug_camera_distance=" << report.debug_camera_distance << '\n';
        std::cout << "presented_frames=" << report.presented_frame_count << '\n';
        std::cout << "swapchain_width=" << report.swapchain_width << '\n';
        std::cout << "swapchain_height=" << report.swapchain_height << '\n';
        std::cout << "gpu_instances=" << report.uploadable_scene.instances.size() << '\n';
        std::cout << "gpu_nodes=" << report.uploadable_scene.hierarchy_nodes.size() << '\n';
        std::cout << "gpu_clusters=" << report.uploadable_scene.clusters.size() << '\n';
        std::cout << "gpu_lod_groups=" << report.uploadable_scene.lod_groups.size() << '\n';
        std::cout << "gpu_lod_clusters=" << report.uploadable_scene.lod_clusters.size() << '\n';
        std::cout << "gpu_pages=" << report.uploadable_scene.pages.size() << '\n';
        std::cout << "gpu_page_dependencies=" << report.uploadable_scene.page_dependencies.size()
                  << '\n';
        std::cout << "base_payload_bytes=" << report.uploadable_scene.base_payload.size() << '\n';
        std::cout << "lod_payload_bytes=" << report.uploadable_scene.lod_payload.size() << '\n';

        const meridian::VisibilityPixel example =
            meridian::encode_visibility(0, meridian::GeometryKind::base_cluster, 1, 2);
        std::cout << "visibility_words=" << report.uploadable_scene.header.visibility_format_word_count
                  << '\n';
        std::cout << "visibility_example_word0=" << example.word0 << '\n';
        std::cout << "visibility_example_word1=" << example.word1 << '\n';
        std::cout << "physical_devices=" << report.physical_devices.size() << '\n';
        for (size_t device_index = 0; device_index < report.physical_devices.size(); ++device_index) {
            std::cout << "physical_device[" << device_index << "]="
                      << report.physical_devices[device_index] << '\n';
        }
        return 0;
    } catch (const meridian::BuilderError& error) {
        std::cerr << "Bootstrap error: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Unexpected error: " << error.what() << '\n';
        return 3;
    }
}
