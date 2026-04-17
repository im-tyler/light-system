#pragma once

#include "vgeo_builder.h"
#include "runtime_model.h"

#include <cstdint>
#include <vector>

namespace meridian {

struct StreamingConfig {
    uint32_t max_resident_pages = 256;
    uint32_t max_loads_per_frame = 4;
    uint32_t eviction_grace_frames = 30;
    uint32_t prefetch_distance = 2;
};

struct StreamingScheduler {
    StreamingConfig config;
    std::vector<float> page_priorities;
    std::vector<uint32_t> last_selected_frame;  // frame when page was last in selection
    std::vector<uint32_t> load_queue;
    std::vector<uint32_t> evict_queue;
    uint32_t total_resident = 0;
    uint32_t total_loading = 0;
    uint32_t page_count = 0;
};

// Initialize scheduler for a resource
StreamingScheduler create_streaming_scheduler(const VGeoResource& resource,
                                              const StreamingConfig& config);

// Per-frame update: given current traversal selection and residency state,
// compute which pages to load and which to evict.
// Returns a ResidencyUpdateInput suitable for passing to step_residency().
ResidencyUpdateInput update_streaming_scheduler(StreamingScheduler& scheduler,
                                                const ResidencyModel& model,
                                                const TraversalSelection& selection,
                                                uint32_t frame_index);

}  // namespace meridian
