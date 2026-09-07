#pragma once

#include <cstdint>

namespace meridian {

constexpr uint32_t kInvalidIndex = 0xffffffffu;

enum class PageKind : uint32_t {
    base_cluster = 0,
    lod_cluster = 1,
};

enum class GeometryKind : uint32_t {
    base_cluster = 0,
    lod_cluster = 1,
};

// Cluster/lod-cluster record flag: the cluster payload carries per-vertex
// UVs (2 floats per vertex, between the normal block and the index block).
constexpr uint32_t kClusterFlagHasUv = 1u << 0;

// GpuDrawEntry.geometry_kind packs the payload domain in the low 16 bits
// (GeometryKind) and kGeometryKindHasUv above it so the vertex-pulling
// shaders can select the payload layout without reading cluster records.
constexpr uint32_t kGeometryKindDomainMask = 0xffffu;
constexpr uint32_t kGeometryKindHasUv = 1u << 16;

}  // namespace meridian
