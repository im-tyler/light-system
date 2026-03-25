#include "vgeo_builder.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void print_usage() {
    std::cerr << "Usage: meridian_trace --manifest <path> --error-threshold <value> [--resident-pages all|base-only|lod-only] [--detail counts|verbose]\n";
}

bool page_is_lod(const meridian::PageRecord& page) {
    return page.lod_cluster_count != 0;
}

std::vector<uint8_t> build_resident_mask(const meridian::VGeoResource& resource,
                                         std::string_view mode) {
    std::vector<uint8_t> resident(resource.pages.size(), 0);

    if (mode == "all") {
        std::fill(resident.begin(), resident.end(), 1);
        return resident;
    }

    if (mode == "base-only") {
        for (size_t page_index = 0; page_index < resource.pages.size(); ++page_index) {
            resident[page_index] = page_is_lod(resource.pages[page_index]) ? 0 : 1;
        }
        return resident;
    }

    if (mode == "lod-only") {
        for (size_t page_index = 0; page_index < resource.pages.size(); ++page_index) {
            resident[page_index] = page_is_lod(resource.pages[page_index]) ? 1 : 0;
        }
        return resident;
    }

    throw meridian::BuilderError("invalid resident page mode: " + std::string(mode));
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
    std::string resident_mode = "all";
    std::string detail_mode = "counts";

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
        } else if (flag == "--resident-pages") {
            resident_mode = argv[i + 1];
        } else if (flag == "--detail") {
            detail_mode = argv[i + 1];
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

        const std::vector<uint8_t> resident_pages = build_resident_mask(resource, resident_mode);
        const meridian::TraversalSelection selection =
            meridian::simulate_traversal(resource, error_threshold, resident_pages);

        std::cout << "asset_id=" << resource.asset_id << '\n';
        std::cout << "error_threshold=" << error_threshold << '\n';
        std::cout << "resident_pages_mode=" << resident_mode << '\n';
        std::cout << "selected_clusters=" << selection.selected_cluster_indices.size() << '\n';
        std::cout << "selected_lod_groups=" << selection.selected_lod_group_indices.size() << '\n';
        std::cout << "selected_lod_clusters=" << selection.selected_lod_cluster_indices.size() << '\n';
        std::cout << "missing_pages=" << selection.missing_page_indices.size() << '\n';
        std::cout << "prefetch_pages=" << selection.prefetch_page_indices.size() << '\n';
        if (detail_mode == "verbose") {
            print_indices("selected_node_list", selection.selected_node_indices);
            print_indices("selected_page_list", selection.selected_page_indices);
            print_indices("selected_cluster_list", selection.selected_cluster_indices);
            print_indices("selected_lod_group_list", selection.selected_lod_group_indices);
            print_indices("selected_lod_cluster_list", selection.selected_lod_cluster_indices);
            print_indices("missing_page_list", selection.missing_page_indices);
            print_indices("prefetch_page_list", selection.prefetch_page_indices);
        } else if (detail_mode != "counts") {
            throw meridian::BuilderError("invalid detail mode: " + detail_mode);
        }
        return 0;
    } catch (const meridian::BuilderError& error) {
        std::cerr << "Trace error: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Unexpected error: " << error.what() << '\n';
        return 3;
    }
}
