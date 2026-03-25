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

}  // namespace meridian
