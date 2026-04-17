#pragma once

#include "vgeo_builder.h"

#include "../thirdparty/meshoptimizer/src/meshoptimizer.h"
#include "../thirdparty/meshoptimizer/demo/clusterlod.h"
#include "../thirdparty/meshoptimizer/extern/cgltf.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace meridian::detail {

constexpr std::array<char, 4> kMagic = {'V', 'G', 'E', 'O'};
constexpr uint32_t kSchemaVersion = 2;
constexpr uint32_t kBuilderVersion = 2;
constexpr uint32_t kPageFlagLodPayload = 1u << 0;

struct FileHeader {
    char magic[4];
    uint32_t schema_version;
    uint32_t builder_version;
    uint32_t flags;
    uint32_t total_material_sections;
    uint32_t total_hierarchy_nodes;
    uint32_t total_clusters;
    uint32_t total_pages;
    uint32_t total_lod_groups;
    uint32_t total_lod_clusters;
    uint32_t total_node_lod_links;
    uint32_t total_page_dependencies;
    uint32_t total_cluster_geometry_bytes;
    uint32_t total_lod_geometry_bytes;
    uint32_t total_lod_group_base_runs;
    uint32_t reserved0;
    Bounds3f bounds;
    uint64_t metadata_offset;
    uint64_t material_table_offset;
    uint64_t hierarchy_table_offset;
    uint64_t page_dependency_table_offset;
    uint64_t node_lod_link_table_offset;
    uint64_t cluster_table_offset;
    uint64_t page_table_offset;
    uint64_t lod_group_table_offset;
    uint64_t lod_cluster_table_offset;
    uint64_t lod_group_base_run_table_offset;
    uint64_t cluster_geometry_payload_offset;
    uint64_t lod_geometry_payload_offset;
};

struct SummaryBlockDisk {
    char asset_id[64];
    char source_asset[256];
    uint32_t has_fallback;
    uint32_t source_vertex_count;
    uint32_t source_triangle_count;
};

struct MaterialSectionDisk {
    char name[64];
    uint32_t fallback_section;
    uint32_t flags;
};

struct HierarchyNodeDisk {
    uint32_t parent_index;
    uint32_t first_child_index;
    uint32_t child_count;
    uint32_t first_cluster_index;
    uint32_t cluster_count;
    uint32_t first_lod_link_index;
    uint32_t lod_link_count;
    Bounds3f bounds;
    float geometric_error;
    uint32_t min_resident_page;
    uint32_t max_resident_page;
    uint32_t flags;
};

struct ClusterRecordDisk {
    uint32_t owning_node_index;
    uint32_t local_vertex_count;
    uint32_t local_triangle_count;
    uint32_t geometry_payload_offset;
    uint32_t geometry_payload_size;
    uint32_t page_index;
    Bounds3f bounds;
    float normal_cone_axis[4];
    float local_error;
    uint32_t material_section_index;
    uint32_t flags;
};

struct PageRecordDisk {
    uint32_t page_index;
    uint64_t byte_offset;
    uint32_t compressed_byte_size;
    uint32_t uncompressed_byte_size;
    uint32_t first_cluster_index;
    uint32_t cluster_count;
    uint32_t first_lod_cluster_index;
    uint32_t lod_cluster_count;
    uint32_t dependency_page_start;
    uint32_t dependency_page_count;
    uint32_t flags;
};

struct LodClusterRecordDisk {
    int32_t refined_group_index;
    uint32_t group_index;
    uint32_t local_vertex_count;
    uint32_t local_triangle_count;
    uint32_t geometry_payload_offset;
    uint32_t geometry_payload_size;
    uint32_t page_index;
    Bounds3f bounds;
    float local_error;
    uint32_t material_section_index;
    uint32_t flags;
};

struct LodGroupRecordDisk {
    uint32_t depth;
    uint32_t first_lod_cluster_index;
    uint32_t lod_cluster_count;
    uint32_t material_section_index;
    Bounds3f bounds;
    float geometric_error;
    uint32_t flags;
    uint32_t first_base_run_index;
    uint32_t base_run_count;
};

struct NodeLodLinkDisk {
    uint32_t lod_group_index;
};

struct LodGroupBaseRunDisk {
    uint32_t first_cluster_index;
    uint32_t cluster_count;
};

struct MeshSection {
    uint32_t material_section_index = 0;
    std::vector<uint32_t> indices;
};

struct MeshData {
    std::vector<Vec3f> positions;
    std::vector<Vec3f> normals;
    std::vector<MeshSection> sections;
    std::vector<unsigned char> vertex_locks;
    struct VertexSeamAttributes {
        bool has_normal = false;
        Vec3f normal;
        bool has_texcoord0 = false;
        float texcoord0[2] = {0.0f, 0.0f};
    };
    std::vector<VertexSeamAttributes> seam_attributes;
    Bounds3f bounds;
};

struct TempHierarchyNode {
    uint32_t parent_index = 0xffffffffu;
    std::vector<uint32_t> child_indices;
    uint32_t leaf_cluster_index = 0xffffffffu;
    Bounds3f bounds;
    float geometric_error = 0.0f;
};

struct LodGroupBuildInfo {
    uint32_t material_section_index = 0;
    std::vector<uint32_t> source_cluster_ids;
};

struct PayloadHeader {
    uint32_t vertex_count;
    uint32_t triangle_count;
};

inline std::string trim(std::string_view input) {
    const auto begin = input.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = input.find_last_not_of(" \t\r\n");
    return std::string(input.substr(begin, end - begin + 1));
}

inline bool parse_bool(std::string_view value) {
    const std::string trimmed = trim(value);
    if (trimmed == "true" || trimmed == "1" || trimmed == "yes") {
        return true;
    }
    if (trimmed == "false" || trimmed == "0" || trimmed == "no") {
        return false;
    }
    throw BuilderError("invalid boolean value: " + std::string(value));
}

inline float parse_float(std::string_view value) {
    const std::string trimmed = trim(value);
    float parsed = 0.0f;
    const auto* begin = trimmed.data();
    const auto* end = trimmed.data() + trimmed.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc() || result.ptr != end) {
        throw BuilderError("invalid float value: " + trimmed);
    }
    return parsed;
}

inline uint32_t parse_u32(std::string_view value) {
    const std::string trimmed = trim(value);
    uint32_t parsed = 0;
    const auto* begin = trimmed.data();
    const auto* end = trimmed.data() + trimmed.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc() || result.ptr != end) {
        throw BuilderError("invalid u32 value: " + trimmed);
    }
    return parsed;
}

inline Vec3f parse_vec3(std::string_view value) {
    std::istringstream stream(trim(value));
    Vec3f result;
    std::string trailing;
    if (!(stream >> result.x >> result.y >> result.z) || (stream >> trailing)) {
        throw BuilderError("invalid vec3 value: " + std::string(value));
    }
    return result;
}

inline std::vector<std::string> parse_list(std::string_view value) {
    std::vector<std::string> result;
    std::string current;
    std::istringstream stream(trim(value));
    while (std::getline(stream, current, ',')) {
        const std::string item = trim(current);
        if (!item.empty()) {
            result.push_back(item);
        }
    }
    return result;
}

template <typename T>
inline void write_pod(std::ofstream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
inline void append_bytes(std::vector<std::byte>& output, const T& value) {
    const auto* begin = reinterpret_cast<const std::byte*>(&value);
    output.insert(output.end(), begin, begin + sizeof(T));
}

inline void update_bounds(Bounds3f& bounds, const Vec3f& point) {
    bounds.min.x = std::min(bounds.min.x, point.x);
    bounds.min.y = std::min(bounds.min.y, point.y);
    bounds.min.z = std::min(bounds.min.z, point.z);
    bounds.max.x = std::max(bounds.max.x, point.x);
    bounds.max.y = std::max(bounds.max.y, point.y);
    bounds.max.z = std::max(bounds.max.z, point.z);
}

inline Bounds3f make_empty_bounds() {
    return {
        {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
         std::numeric_limits<float>::max()},
        {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
         std::numeric_limits<float>::lowest()},
    };
}

inline Bounds3f apply_bounds_padding(Bounds3f bounds, float padding) {
    bounds.min.x -= padding;
    bounds.min.y -= padding;
    bounds.min.z -= padding;
    bounds.max.x += padding;
    bounds.max.y += padding;
    bounds.max.z += padding;
    return bounds;
}

inline Bounds3f resolve_resource_bounds(const BuildManifest& manifest, const Bounds3f& mesh_bounds) {
    const Bounds3f base_bounds = manifest.has_explicit_bounds ? manifest.explicit_bounds : mesh_bounds;
    return apply_bounds_padding(base_bounds, manifest.bounds_padding);
}

void validate_manifest(const BuildManifest& manifest);
MeshData load_mesh(const BuildManifest& manifest);
void compute_smooth_normals(MeshData& mesh);

clodConfig make_clod_config(const BuildManifest& manifest);
void build_section_base_clusters(const MeshData& mesh, const MeshSection& section,
                                 const clodConfig& config, std::vector<ClusterRecord>& source_clusters,
                                 std::vector<std::byte>& source_cluster_payload,
                                 std::vector<std::vector<unsigned int>>& cluster_global_indices);
Bounds3f merge_cluster_bounds(const std::vector<ClusterRecord>& clusters,
                              const std::vector<uint32_t>& cluster_ids);
uint32_t build_temp_hierarchy(std::vector<TempHierarchyNode>& nodes, const MeshData& mesh,
                              const std::vector<std::vector<unsigned int>>& cluster_global_indices,
                              const std::vector<ClusterRecord>& clusters,
                              const std::vector<uint32_t>& cluster_ids, uint32_t parent_index,
                              uint32_t partition_size);
void flatten_temp_hierarchy(const std::vector<TempHierarchyNode>& temp_nodes,
                            const std::vector<ClusterRecord>& source_clusters,
                            const std::vector<std::byte>& source_payload, VGeoResource& resource,
                            std::vector<uint32_t>& temp_to_runtime_node_indices,
                            std::vector<uint32_t>& source_to_runtime_cluster_indices,
                            uint32_t temp_node_index, uint32_t node_index, uint32_t parent_index);
std::vector<PageRecord> build_base_pages(const std::vector<ClusterRecord>& clusters,
                                         uint32_t page_cluster_limit);
std::vector<PageRecord> build_lod_pages(const std::vector<LodClusterRecord>& lod_clusters,
                                        uint32_t page_cluster_limit, uint32_t page_index_base);
void update_hierarchy_page_ranges(VGeoResource& resource);
void build_lod_metadata(VGeoResource& resource, const MeshData& mesh, const BuildManifest& manifest,
                        const std::vector<std::vector<unsigned int>>& source_cluster_global_indices,
                        const std::vector<uint32_t>& source_to_runtime_cluster_indices);
void build_page_dependencies(VGeoResource& resource);

void validate_resource(const VGeoResource& resource);
TraversalSelection simulate_traversal(const VGeoResource& resource, float error_threshold,
                                      const std::vector<uint8_t>& resident_pages);

ResourceSummary read_resource_summary(const std::filesystem::path& input_path);
void write_resource(const VGeoResource& resource, const std::filesystem::path& output_path);
void write_summary(const VGeoResource& resource, const std::filesystem::path& output_path);

}  // namespace meridian::detail
