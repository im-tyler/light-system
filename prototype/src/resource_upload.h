#pragma once

#include "gpu_abi.h"
#include "vgeo_builder.h"

#include <cstddef>
#include <vector>

namespace meridian {

struct UploadableScene {
    GpuSceneHeader header;
    std::vector<GpuInstanceRecord> instances;
    std::vector<GpuHierarchyNodeRecord> hierarchy_nodes;
    std::vector<GpuClusterRecord> clusters;
    std::vector<GpuLodGroupRecord> lod_groups;
    std::vector<GpuLodClusterRecord> lod_clusters;
    std::vector<GpuNodeLodLinkRecord> node_lod_links;
    std::vector<GpuPageRecord> pages;
    std::vector<uint32_t> page_dependencies;
    std::vector<GpuPageResidencyEntry> page_residency;
    std::vector<std::byte> base_payload;
    std::vector<std::byte> lod_payload;
    // Embedded RGBA8 base-color texture; empty when untextured.
    std::vector<std::byte> texture_payload;
    uint32_t texture_width = 0;
    uint32_t texture_height = 0;
};

UploadableScene build_uploadable_scene(const VGeoResource& resource);

}  // namespace meridian
