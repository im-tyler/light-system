#include "replay_script.h"
#include "resource_upload.h"
#include "runtime_model.h"

#include <array>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void print_usage() {
    std::cerr << "Usage: meridian_replay --manifest <path> --script <path> [--detail counts|verbose]\n";
}

bool page_is_lod(const meridian::PageRecord& page) {
    return page.lod_cluster_count != 0;
}

void bootstrap_residency(meridian::ResidencyModel& model, const meridian::VGeoResource& resource,
                         std::string_view mode) {
    if (mode == "none") {
        return;
    }

    for (uint32_t page_index = 0; page_index < resource.pages.size(); ++page_index) {
        const bool is_lod = page_is_lod(resource.pages[page_index]);
        const bool should_reside = mode == "all" || (mode == "base-only" && !is_lod) ||
                                   (mode == "lod-only" && is_lod);
        if (should_reside) {
            model.pages[page_index].state = meridian::PageResidencyState::resident;
            model.pages[page_index].last_touched_frame = 0;
        }
    }
}

void print_indices(std::string_view label, const std::vector<uint32_t>& values) {
    std::cout << label << '=';
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            std::cout << ',';
        }
        std::cout << values[i];
    }
    std::cout << '\n';
}

std::array<uint32_t, 5> count_page_states(const meridian::ResidencyModel& model) {
    std::array<uint32_t, 5> counts = {0, 0, 0, 0, 0};
    for (const meridian::PageResidencyEntry& entry : model.pages) {
        counts[static_cast<size_t>(entry.state)] += 1;
    }
    return counts;
}

void snapshot_page_residency(meridian::UploadableScene& scene, const meridian::ResidencyModel& model) {
    scene.page_residency.resize(model.pages.size());
    for (uint32_t page_index = 0; page_index < model.pages.size(); ++page_index) {
        scene.page_residency[page_index].state = static_cast<uint32_t>(model.pages[page_index].state);
        scene.page_residency[page_index].last_touched_frame = model.pages[page_index].last_touched_frame;
        scene.page_residency[page_index].request_priority = model.pages[page_index].request_priority;
        scene.page_residency[page_index].flags = 0;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5 || argc % 2 == 0) {
        print_usage();
        return 1;
    }

    std::filesystem::path manifest_path;
    std::filesystem::path script_path;
    std::string detail_mode = "counts";

    for (int i = 1; i < argc; i += 2) {
        const std::string_view flag = argv[i];
        if (i + 1 >= argc) {
            print_usage();
            return 1;
        }

        if (flag == "--manifest") {
            manifest_path = argv[i + 1];
        } else if (flag == "--script") {
            script_path = argv[i + 1];
        } else if (flag == "--detail") {
            detail_mode = argv[i + 1];
        } else {
            print_usage();
            return 1;
        }
    }

    if (manifest_path.empty() || script_path.empty()) {
        print_usage();
        return 1;
    }

    try {
        const meridian::BuildManifest manifest = meridian::load_manifest(manifest_path);
        const meridian::ReplayScript script = meridian::load_replay_script(script_path);
        const meridian::VGeoResource resource = meridian::build_resource(manifest);
        meridian::validate_resource(resource);

        meridian::UploadableScene uploadable_scene = meridian::build_uploadable_scene(resource);
        meridian::ResidencyModel model = meridian::create_residency_model(resource);
        bootstrap_residency(model, resource, script.bootstrap_resident);

        std::cout << "asset_id=" << resource.asset_id << '\n';
        std::cout << "replay_name=" << script.name << '\n';
        std::cout << "frame_count=" << script.frame_count << '\n';
        std::cout << "resident_budget=" << script.resident_budget << '\n';
        std::cout << "eviction_grace_frames=" << script.eviction_grace_frames << '\n';
        std::cout << "bootstrap_resident=" << script.bootstrap_resident << '\n';
        std::cout << "gpu_instances=" << uploadable_scene.instances.size() << '\n';
        std::cout << "gpu_nodes=" << uploadable_scene.hierarchy_nodes.size() << '\n';
        std::cout << "gpu_clusters=" << uploadable_scene.clusters.size() << '\n';
        std::cout << "gpu_lod_groups=" << uploadable_scene.lod_groups.size() << '\n';
        std::cout << "gpu_lod_clusters=" << uploadable_scene.lod_clusters.size() << '\n';
        std::cout << "gpu_pages=" << uploadable_scene.pages.size() << '\n';
        std::cout << "gpu_page_dependencies=" << uploadable_scene.page_dependencies.size() << '\n';
        std::cout << "base_payload_bytes=" << uploadable_scene.base_payload.size() << '\n';
        std::cout << "lod_payload_bytes=" << uploadable_scene.lod_payload.size() << '\n';

        for (uint32_t frame_index = 0; frame_index < script.frame_count; ++frame_index) {
            std::vector<uint32_t> completed_pages;
            for (uint32_t page_index = 0; page_index < model.pages.size(); ++page_index) {
                if (model.pages[page_index].state == meridian::PageResidencyState::loading) {
                    completed_pages.push_back(page_index);
                }
            }

            const std::vector<uint8_t> resident_pages = meridian::build_resident_page_mask(model);
            const meridian::TraversalSelection selection = meridian::simulate_traversal(
                resource, script.error_thresholds[frame_index], resident_pages);

            meridian::ResidencyUpdateInput input;
            input.frame_index = frame_index;
            input.resident_budget = script.resident_budget;
            input.eviction_grace_frames = script.eviction_grace_frames;
            input.selected_pages = selection.selected_page_indices;
            input.missing_pages = selection.missing_page_indices;
            input.prefetch_pages = selection.prefetch_page_indices;
            input.completed_pages = completed_pages;

            const meridian::ResidencyUpdateResult update = meridian::step_residency(model, input);
            snapshot_page_residency(uploadable_scene, model);
            const std::array<uint32_t, 5> state_counts = count_page_states(model);

            std::cout << "frame=" << frame_index << '\n';
            std::cout << " error_threshold=" << script.error_thresholds[frame_index] << '\n';
            std::cout << " selected_nodes=" << selection.selected_node_indices.size() << '\n';
            std::cout << " selected_pages=" << selection.selected_page_indices.size() << '\n';
            std::cout << " selected_clusters=" << selection.selected_cluster_indices.size() << '\n';
            std::cout << " selected_lod_groups=" << selection.selected_lod_group_indices.size() << '\n';
            std::cout << " selected_lod_clusters=" << selection.selected_lod_cluster_indices.size() << '\n';
            std::cout << " missing_pages=" << selection.missing_page_indices.size() << '\n';
            std::cout << " prefetch_pages=" << selection.prefetch_page_indices.size() << '\n';
            std::cout << " requested_pages=" << update.requested_pages.size() << '\n';
            std::cout << " loading_pages=" << update.loading_pages.size() << '\n';
            std::cout << " completed_pages=" << update.completed_pages.size() << '\n';
            std::cout << " eviction_candidates=" << update.eviction_candidate_pages.size() << '\n';
            std::cout << " evicted_pages=" << update.evicted_pages.size() << '\n';
            std::cout << " residency_unloaded=" << state_counts[0] << '\n';
            std::cout << " residency_requested=" << state_counts[1] << '\n';
            std::cout << " residency_loading=" << state_counts[2] << '\n';
            std::cout << " residency_resident=" << state_counts[3] << '\n';
            std::cout << " residency_eviction_candidate=" << state_counts[4] << '\n';

            if (detail_mode == "verbose") {
                print_indices(" selected_node_list", selection.selected_node_indices);
                print_indices(" selected_page_list", selection.selected_page_indices);
                print_indices(" selected_cluster_list", selection.selected_cluster_indices);
                print_indices(" selected_lod_group_list", selection.selected_lod_group_indices);
                print_indices(" selected_lod_cluster_list", selection.selected_lod_cluster_indices);
                print_indices(" missing_page_list", selection.missing_page_indices);
                print_indices(" prefetch_page_list", selection.prefetch_page_indices);
                print_indices(" requested_page_list", update.requested_pages);
                print_indices(" loading_page_list", update.loading_pages);
                print_indices(" completed_page_list", update.completed_pages);
                print_indices(" evicted_page_list", update.evicted_pages);
            } else if (detail_mode != "counts") {
                throw meridian::BuilderError("invalid detail mode: " + detail_mode);
            }
        }

        return 0;
    } catch (const meridian::BuilderError& error) {
        std::cerr << "Replay error: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Unexpected error: " << error.what() << '\n';
        return 3;
    }
}
