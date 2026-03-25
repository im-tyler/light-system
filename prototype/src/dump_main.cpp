#include "vgeo_builder.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

void print_usage() {
    std::cerr << "Usage: meridian_dump --input <path>\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 || std::string_view(argv[1]) != "--input") {
        print_usage();
        return 1;
    }

    try {
        const meridian::ResourceSummary summary =
            meridian::read_resource_summary(std::filesystem::path(argv[2]));

        std::cout << "asset_id=" << summary.asset_id << '\n';
        std::cout << "source_asset=" << summary.source_asset.string() << '\n';
        std::cout << "has_fallback=" << (summary.has_fallback ? "true" : "false") << '\n';
        std::cout << "source_vertices=" << summary.source_vertex_count << '\n';
        std::cout << "source_triangles=" << summary.source_triangle_count << '\n';
        std::cout << "bounds_min=" << summary.bounds.min.x << ' ' << summary.bounds.min.y << ' '
                  << summary.bounds.min.z << '\n';
        std::cout << "bounds_max=" << summary.bounds.max.x << ' ' << summary.bounds.max.y << ' '
                  << summary.bounds.max.z << '\n';
        std::cout << "material_sections=" << summary.material_section_count << '\n';
        std::cout << "hierarchy_nodes=" << summary.hierarchy_node_count << '\n';
        std::cout << "clusters=" << summary.cluster_count << '\n';
        std::cout << "pages=" << summary.page_count << '\n';
        std::cout << "lod_groups=" << summary.lod_group_count << '\n';
        std::cout << "lod_clusters=" << summary.lod_cluster_count << '\n';
        std::cout << "node_lod_links=" << summary.node_lod_link_count << '\n';
        std::cout << "page_dependencies=" << summary.page_dependency_count << '\n';
        std::cout << "cluster_geometry_bytes=" << summary.cluster_geometry_bytes << '\n';
        std::cout << "lod_geometry_bytes=" << summary.lod_geometry_bytes << '\n';
        return 0;
    } catch (const meridian::BuilderError& error) {
        std::cerr << "Dump error: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Unexpected error: " << error.what() << '\n';
        return 3;
    }
}
