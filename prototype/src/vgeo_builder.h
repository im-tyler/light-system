#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace meridian {

struct Vec3f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Bounds3f {
    Vec3f min;
    Vec3f max;
};

struct MaterialSection {
    std::string name;
    uint32_t fallback_section = 0;
    uint32_t flags = 0;
};

struct HierarchyNode {
    uint32_t parent_index = 0xffffffffu;
    uint32_t first_child_index = 0;
    uint32_t child_count = 0;
    uint32_t first_cluster_index = 0;
    uint32_t cluster_count = 0;
    uint32_t first_lod_link_index = 0;
    uint32_t lod_link_count = 0;
    Bounds3f bounds;
    float geometric_error = 0.0f;
    uint32_t min_resident_page = 0xffffffffu;
    uint32_t max_resident_page = 0xffffffffu;
    uint32_t flags = 0;
};

struct ClusterRecord {
    uint32_t owning_node_index = 0xffffffffu;
    uint32_t local_vertex_count = 0;
    uint32_t local_triangle_count = 0;
    uint32_t geometry_payload_offset = 0;
    uint32_t geometry_payload_size = 0;
    uint32_t page_index = 0;
    Bounds3f bounds;
    float normal_cone_axis[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    float local_error = 0.0f;
    uint32_t material_section_index = 0;
    uint32_t flags = 0;
};

struct PageRecord {
    uint32_t page_index = 0;
    uint64_t byte_offset = 0;
    uint32_t compressed_byte_size = 0;
    uint32_t uncompressed_byte_size = 0;
    uint32_t first_cluster_index = 0;
    uint32_t cluster_count = 0;
    uint32_t first_lod_cluster_index = 0;
    uint32_t lod_cluster_count = 0;
    uint32_t dependency_page_start = 0;
    uint32_t dependency_page_count = 0;
    uint32_t flags = 0;
};

struct LodClusterRecord {
    int32_t refined_group_index = -1;
    uint32_t group_index = 0;
    uint32_t local_vertex_count = 0;
    uint32_t local_triangle_count = 0;
    uint32_t geometry_payload_offset = 0;
    uint32_t geometry_payload_size = 0;
    uint32_t page_index = 0;
    Bounds3f bounds;
    float local_error = 0.0f;
    uint32_t material_section_index = 0;
    uint32_t flags = 0;
};

struct LodGroupRecord {
    uint32_t depth = 0;
    uint32_t first_lod_cluster_index = 0;
    uint32_t lod_cluster_count = 0;
    uint32_t material_section_index = 0;
    Bounds3f bounds;
    float geometric_error = 0.0f;
    uint32_t flags = 0;
};

struct NodeLodLink {
    uint32_t lod_group_index = 0;
};

struct ResourceMetadata {
    uint32_t root_hierarchy_node_index = 0xffffffffu;
    uint64_t page_table_offset = 0;
    uint64_t page_dependency_table_offset = 0;
    uint64_t node_lod_link_table_offset = 0;
    uint64_t cluster_table_offset = 0;
    uint64_t lod_group_table_offset = 0;
    uint64_t lod_cluster_table_offset = 0;
    uint64_t cluster_geometry_payload_offset = 0;
    uint64_t lod_geometry_payload_offset = 0;
    uint64_t material_mapping_offset = 0;
    uint64_t debug_info_offset = 0;
};

struct BuildManifest {
    std::string asset_id;
    std::filesystem::path source_asset;
    std::filesystem::path output_path;
    Bounds3f explicit_bounds;
    bool has_explicit_bounds = false;
    float bounds_padding = 0.0f;
    bool emit_fallback = true;
    uint32_t cluster_vertex_limit = 64;
    uint32_t cluster_triangle_limit = 124;
    uint32_t page_cluster_limit = 8;
    uint32_t hierarchy_partition_size = 8;
    std::vector<std::string> material_slots;
};

struct VGeoResource {
    std::string asset_id;
    std::filesystem::path source_asset;
    Bounds3f bounds;
    bool has_fallback = true;
    uint32_t source_vertex_count = 0;
    uint32_t source_triangle_count = 0;
    uint32_t seam_locked_vertex_count = 0;
    ResourceMetadata metadata;
    std::vector<MaterialSection> material_sections;
    std::vector<HierarchyNode> hierarchy_nodes;
    std::vector<ClusterRecord> clusters;
    std::vector<PageRecord> pages;
    std::vector<uint32_t> page_dependencies;
    std::vector<LodGroupRecord> lod_groups;
    std::vector<LodClusterRecord> lod_clusters;
    std::vector<NodeLodLink> node_lod_links;
    std::vector<std::byte> cluster_geometry_payload;
    std::vector<std::byte> lod_geometry_payload;
};

struct ResourceSummary {
    std::string asset_id;
    std::filesystem::path source_asset;
    bool has_fallback = false;
    Bounds3f bounds;
    uint32_t source_vertex_count = 0;
    uint32_t source_triangle_count = 0;
    uint32_t material_section_count = 0;
    uint32_t hierarchy_node_count = 0;
    uint32_t cluster_count = 0;
    uint32_t page_count = 0;
    uint32_t lod_group_count = 0;
    uint32_t lod_cluster_count = 0;
    uint32_t node_lod_link_count = 0;
    uint32_t page_dependency_count = 0;
    uint32_t cluster_geometry_bytes = 0;
    uint32_t lod_geometry_bytes = 0;
};

struct TraversalSelection {
    std::vector<uint32_t> selected_node_indices;
    std::vector<uint32_t> selected_page_indices;
    std::vector<uint32_t> selected_cluster_indices;
    std::vector<uint32_t> selected_lod_group_indices;
    std::vector<uint32_t> selected_lod_cluster_indices;
    std::vector<uint32_t> missing_page_indices;
    std::vector<uint32_t> prefetch_page_indices;
};

class BuilderError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

BuildManifest load_manifest(const std::filesystem::path& manifest_path);
VGeoResource create_stub_resource(const BuildManifest& manifest);
VGeoResource build_resource(const BuildManifest& manifest);
void validate_resource(const VGeoResource& resource);
ResourceSummary read_resource_summary(const std::filesystem::path& input_path);
TraversalSelection simulate_traversal(const VGeoResource& resource, float error_threshold,
                                      const std::vector<uint8_t>& resident_pages);
void write_resource(const VGeoResource& resource, const std::filesystem::path& output_path);
void write_summary(const VGeoResource& resource, const std::filesystem::path& output_path);

}  // namespace meridian
