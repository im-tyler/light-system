#pragma once

#include "vgeo_builder.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace meridian {

enum class PageResidencyState : uint8_t {
    unloaded = 0,
    requested = 1,
    loading = 2,
    resident = 3,
    eviction_candidate = 4,
};

// Initial residency set for create_residency_model. `none` starts with
// zero pages resident; `base_only`/`lod_only` start with exactly that
// page kind resident (the complement unloaded); `all` keeps the original
// everything-resident behavior used by the viewer runtime.
enum class ResidencyBootstrapMode : uint8_t {
    none = 0,
    all,
    base_only,
    lod_only,
};

struct PageResidencyEntry {
    PageResidencyState state = PageResidencyState::unloaded;
    uint32_t last_touched_frame = 0xffffffffu;
    uint32_t request_priority = 0;
};

struct ResidencyModel {
    std::vector<PageResidencyEntry> pages;
};

struct ResidencyUpdateInput {
    uint32_t frame_index = 0;
    uint32_t resident_budget = 0xffffffffu;
    uint32_t eviction_grace_frames = 2;
    std::vector<uint32_t> selected_pages;
    std::vector<uint32_t> missing_pages;
    std::vector<uint32_t> prefetch_pages;
    std::vector<uint32_t> completed_pages;
};

struct ResidencyUpdateResult {
    std::vector<uint32_t> touched_pages;
    std::vector<uint32_t> requested_pages;
    std::vector<uint32_t> loading_pages;
    std::vector<uint32_t> completed_pages;
    std::vector<uint32_t> eviction_candidate_pages;
    std::vector<uint32_t> evicted_pages;
};

ResidencyBootstrapMode parse_residency_bootstrap_mode(std::string_view mode);
ResidencyModel create_residency_model(
    const VGeoResource& resource,
    ResidencyBootstrapMode bootstrap = ResidencyBootstrapMode::all);
std::vector<uint8_t> build_resident_page_mask(const ResidencyModel& model);
ResidencyUpdateResult step_residency(ResidencyModel& model, const ResidencyUpdateInput& input);
const char* to_string(PageResidencyState state);

}  // namespace meridian
