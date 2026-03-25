#pragma once

#include "runtime_contract.h"

#include <array>
#include <cstdint>
#include <type_traits>

namespace meridian {

struct GpuSceneHeader {
    uint32_t instance_count = 0;
    uint32_t hierarchy_node_count = 0;
    uint32_t cluster_count = 0;
    uint32_t lod_group_count = 0;
    uint32_t lod_cluster_count = 0;
    uint32_t page_count = 0;
    uint32_t page_dependency_count = 0;
    uint32_t node_lod_link_count = 0;
    uint32_t base_payload_bytes = 0;
    uint32_t lod_payload_bytes = 0;
    uint32_t visibility_format_word_count = 0;
    uint32_t flags = 0;
};

struct GpuInstanceRecord {
    std::array<float, 16> object_to_world = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    std::array<float, 4> bounds_min = {0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> bounds_max = {0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t resource_index = 0;
    uint32_t root_node_index = kInvalidIndex;
    uint32_t flags = 0;
    uint32_t reserved = 0;
};

struct GpuHierarchyNodeRecord {
    uint32_t parent_index = kInvalidIndex;
    uint32_t first_child_index = 0;
    uint32_t child_count = 0;
    uint32_t first_cluster_index = 0;
    uint32_t cluster_count = 0;
    uint32_t first_lod_link_index = 0;
    uint32_t lod_link_count = 0;
    uint32_t min_resident_page = kInvalidIndex;
    uint32_t max_resident_page = kInvalidIndex;
    float geometric_error = 0.0f;
    uint32_t flags = 0;
    std::array<float, 4> bounds_min = {0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> bounds_max = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct GpuClusterRecord {
    uint32_t owning_node_index = kInvalidIndex;
    uint32_t local_vertex_count = 0;
    uint32_t local_triangle_count = 0;
    uint32_t page_index = kInvalidIndex;
    uint32_t payload_offset = 0;
    uint32_t payload_size = 0;
    uint32_t material_section_index = 0;
    uint32_t flags = 0;
    float local_error = 0.0f;
    std::array<float, 3> padding0 = {0.0f, 0.0f, 0.0f};
    std::array<float, 4> bounds_min = {0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> bounds_max = {0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> normal_cone = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct GpuLodGroupRecord {
    uint32_t depth = 0;
    uint32_t first_lod_cluster_index = 0;
    uint32_t lod_cluster_count = 0;
    uint32_t material_section_index = 0;
    float geometric_error = 0.0f;
    uint32_t flags = 0;
    std::array<float, 2> padding0 = {0.0f, 0.0f};
    std::array<float, 4> bounds_min = {0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> bounds_max = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct GpuLodClusterRecord {
    int32_t refined_group_index = -1;
    uint32_t group_index = 0;
    uint32_t local_vertex_count = 0;
    uint32_t local_triangle_count = 0;
    uint32_t page_index = kInvalidIndex;
    uint32_t payload_offset = 0;
    uint32_t payload_size = 0;
    uint32_t material_section_index = 0;
    float local_error = 0.0f;
    uint32_t flags = 0;
    std::array<float, 2> padding0 = {0.0f, 0.0f};
    std::array<float, 4> bounds_min = {0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> bounds_max = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct GpuNodeLodLinkRecord {
    uint32_t lod_group_index = 0;
};

struct GpuPageRecord {
    PageKind kind = PageKind::base_cluster;
    uint32_t byte_offset = 0;
    uint32_t byte_size = 0;
    uint32_t first_cluster_index = 0;
    uint32_t cluster_count = 0;
    uint32_t first_lod_cluster_index = 0;
    uint32_t lod_cluster_count = 0;
    uint32_t dependency_page_start = 0;
    uint32_t dependency_page_count = 0;
    uint32_t flags = 0;
};

struct GpuPageResidencyEntry {
    uint32_t state = 0;
    uint32_t last_touched_frame = kInvalidIndex;
    uint32_t request_priority = 0;
    uint32_t flags = 0;
};

static_assert(std::is_standard_layout<GpuSceneHeader>::value);
static_assert(std::is_standard_layout<GpuInstanceRecord>::value);
static_assert(std::is_standard_layout<GpuHierarchyNodeRecord>::value);
static_assert(std::is_standard_layout<GpuClusterRecord>::value);
static_assert(std::is_standard_layout<GpuLodGroupRecord>::value);
static_assert(std::is_standard_layout<GpuLodClusterRecord>::value);
static_assert(std::is_standard_layout<GpuNodeLodLinkRecord>::value);
static_assert(std::is_standard_layout<GpuPageRecord>::value);
static_assert(std::is_standard_layout<GpuPageResidencyEntry>::value);

// Word counts for raw-uint SSBO access in shaders (sizeof / 4).
static_assert(sizeof(GpuHierarchyNodeRecord) == 76);
static_assert(sizeof(GpuClusterRecord) == 96);
static_assert(sizeof(GpuLodGroupRecord) == 64);
static_assert(sizeof(GpuLodClusterRecord) == 80);
static_assert(sizeof(GpuNodeLodLinkRecord) == 4);
static_assert(sizeof(GpuPageResidencyEntry) == 16);
static_assert(sizeof(GpuInstanceRecord) == 112);

constexpr uint32_t kNodeWords = sizeof(GpuHierarchyNodeRecord) / 4;
constexpr uint32_t kClusterWords = sizeof(GpuClusterRecord) / 4;
constexpr uint32_t kLodGroupWords = sizeof(GpuLodGroupRecord) / 4;
constexpr uint32_t kLodClusterWords = sizeof(GpuLodClusterRecord) / 4;
constexpr uint32_t kPageResidencyWords = sizeof(GpuPageResidencyEntry) / 4;
constexpr uint32_t kInstanceWords = sizeof(GpuInstanceRecord) / 4;

struct GpuDrawEntry {
    // VkDrawIndirectCommand (must be first 16 bytes)
    uint32_t draw_vertex_count = 0;  // = triangle_count * 3
    uint32_t draw_instance_count = 1;
    uint32_t draw_first_vertex = 0;
    uint32_t draw_first_instance = 0; // = draw index, becomes gl_InstanceIndex
    // Per-draw metadata (read by vertex shader via gl_InstanceIndex)
    uint32_t cluster_index = 0;
    uint32_t geometry_kind = 0;
    uint32_t payload_offset = 0;
    uint32_t local_vertex_count = 0;
};
static_assert(sizeof(GpuDrawEntry) == 32);

}  // namespace meridian
