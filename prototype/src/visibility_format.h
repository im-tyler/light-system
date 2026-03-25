#pragma once

#include "runtime_contract.h"

#include <cstdint>

namespace meridian {

struct VisibilityPixel {
    uint32_t word0 = 0;
    uint32_t word1 = 0;
};

constexpr uint32_t kVisibilityValidBit = 1u << 31;
constexpr uint32_t kVisibilityGeometryKindBit = 1u << 30;
constexpr uint32_t kVisibilityGeometryIndexShift = 8;
constexpr uint32_t kVisibilityGeometryIndexMask = 0x3fffffu;
constexpr uint32_t kVisibilityLocalTriangleMask = 0xffu;

inline VisibilityPixel encode_visibility(uint32_t instance_index, GeometryKind geometry_kind,
                                         uint32_t geometry_index, uint32_t local_triangle_index) {
    VisibilityPixel pixel;
    pixel.word0 = instance_index;
    pixel.word1 = kVisibilityValidBit |
                  ((geometry_kind == GeometryKind::lod_cluster ? 1u : 0u)
                   << 30) |
                  ((geometry_index & kVisibilityGeometryIndexMask)
                   << kVisibilityGeometryIndexShift) |
                  (local_triangle_index & kVisibilityLocalTriangleMask);
    return pixel;
}

inline bool visibility_valid(const VisibilityPixel& pixel) {
    return (pixel.word1 & kVisibilityValidBit) != 0;
}

inline GeometryKind decode_visibility_geometry_kind(const VisibilityPixel& pixel) {
    return (pixel.word1 & kVisibilityGeometryKindBit) != 0 ? GeometryKind::lod_cluster
                                                           : GeometryKind::base_cluster;
}

inline uint32_t decode_visibility_geometry_index(const VisibilityPixel& pixel) {
    return (pixel.word1 >> kVisibilityGeometryIndexShift) & kVisibilityGeometryIndexMask;
}

inline uint32_t decode_visibility_local_triangle_index(const VisibilityPixel& pixel) {
    return pixel.word1 & kVisibilityLocalTriangleMask;
}

}  // namespace meridian
