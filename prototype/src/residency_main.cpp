#include "runtime_model.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void print_usage() {
    std::cerr << "Usage: meridian_residency --manifest <path> --error-threshold <value> [--frames <count>] [--resident-budget <count>] [--bootstrap-resident none|all|base-only|lod-only] [--eviction-grace <frames>]\n";
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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5 || argc % 2 == 0) {
        print_usage();
        return 1;
    }

    std::filesystem::path manifest_path;
    float error_threshold = 0.0f;
    uint32_t frame_count = 3;
    uint32_t resident_budget = 0xffffffffu;
    uint32_t eviction_grace = 2;
    std::string bootstrap_mode = "none";

    for (int i = 1; i < argc; i += 2) {
        const std::string_view flag = argv[i];
        if (i + 1 >= argc) {
            print_usage();
            return 1;
        }

        if (flag == "--manifest") {
            manifest_path = argv[i + 1];
        } else if (flag == "--error-threshold") {
            error_threshold = std::stof(argv[i + 1]);
        } else if (flag == "--frames") {
            frame_count = static_cast<uint32_t>(std::stoul(argv[i + 1]));
        } else if (flag == "--resident-budget") {
            resident_budget = static_cast<uint32_t>(std::stoul(argv[i + 1]));
        } else if (flag == "--bootstrap-resident") {
            bootstrap_mode = argv[i + 1];
        } else if (flag == "--eviction-grace") {
            eviction_grace = static_cast<uint32_t>(std::stoul(argv[i + 1]));
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

        meridian::ResidencyModel model = meridian::create_residency_model(resource);
        bootstrap_residency(model, resource, bootstrap_mode);

        std::cout << "asset_id=" << resource.asset_id << '\n';
        std::cout << "error_threshold=" << error_threshold << '\n';
        std::cout << "frames=" << frame_count << '\n';
        std::cout << "resident_budget=" << resident_budget << '\n';
        std::cout << "bootstrap_resident=" << bootstrap_mode << '\n';
        std::cout << "eviction_grace=" << eviction_grace << '\n';

        for (uint32_t frame_index = 0; frame_index < frame_count; ++frame_index) {
            std::vector<uint32_t> completed_pages;
            for (uint32_t page_index = 0; page_index < model.pages.size(); ++page_index) {
                if (model.pages[page_index].state == meridian::PageResidencyState::loading) {
                    completed_pages.push_back(page_index);
                }
            }

            const std::vector<uint8_t> resident_pages = meridian::build_resident_page_mask(model);
            const meridian::TraversalSelection selection =
                meridian::simulate_traversal(resource, error_threshold, resident_pages);

            meridian::ResidencyUpdateInput input;
            input.frame_index = frame_index;
            input.resident_budget = resident_budget;
            input.eviction_grace_frames = eviction_grace;
            input.selected_pages = selection.selected_page_indices;
            input.missing_pages = selection.missing_page_indices;
            input.prefetch_pages = selection.prefetch_page_indices;
            input.completed_pages = completed_pages;

            const meridian::ResidencyUpdateResult update = meridian::step_residency(model, input);

            std::cout << "frame=" << frame_index << '\n';
            std::cout << " selected_nodes=" << selection.selected_node_indices.size() << '\n';
            std::cout << " selected_pages=" << selection.selected_page_indices.size() << '\n';
            std::cout << " missing_pages=" << selection.missing_page_indices.size() << '\n';
            std::cout << " prefetch_pages=" << selection.prefetch_page_indices.size() << '\n';
            std::cout << " requested_pages=" << update.requested_pages.size() << '\n';
            std::cout << " loading_pages=" << update.loading_pages.size() << '\n';
            std::cout << " completed_pages=" << update.completed_pages.size() << '\n';
            std::cout << " eviction_candidates=" << update.eviction_candidate_pages.size() << '\n';
            std::cout << " evicted_pages=" << update.evicted_pages.size() << '\n';
            print_indices(" selected_page_list", selection.selected_page_indices);
            print_indices(" missing_page_list", selection.missing_page_indices);
            print_indices(" prefetch_page_list", selection.prefetch_page_indices);
            print_indices(" requested_page_list", update.requested_pages);
            print_indices(" loading_page_list", update.loading_pages);
            print_indices(" completed_page_list", update.completed_pages);
            print_indices(" evicted_page_list", update.evicted_pages);
        }

        return 0;
    } catch (const meridian::BuilderError& error) {
        std::cerr << "Residency error: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Unexpected error: " << error.what() << '\n';
        return 3;
    }
}
