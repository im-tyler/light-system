#include "streaming_scheduler.h"

#include <algorithm>
#include <cmath>

namespace meridian {

StreamingScheduler create_streaming_scheduler(const VGeoResource& resource,
                                              const StreamingConfig& config) {
    StreamingScheduler scheduler;
    scheduler.config = config;
    scheduler.page_count = static_cast<uint32_t>(resource.pages.size());
    scheduler.page_priorities.resize(scheduler.page_count, 0.0f);
    scheduler.last_selected_frame.resize(scheduler.page_count, 0xffffffffu);
    return scheduler;
}

ResidencyUpdateInput update_streaming_scheduler(StreamingScheduler& scheduler,
                                                const ResidencyModel& model,
                                                const TraversalSelection& selection,
                                                uint32_t frame_index) {
    const auto& config = scheduler.config;
    auto& priorities = scheduler.page_priorities;

    // Reset priorities
    for (uint32_t i = 0; i < scheduler.page_count; ++i) {
        priorities[i] = 0.0f;
    }

    // Selected pages: highest priority
    for (uint32_t page : selection.selected_page_indices) {
        if (page < scheduler.page_count) {
            priorities[page] = 1.0f;
            scheduler.last_selected_frame[page] = frame_index;
        }
    }

    // Prefetch pages: medium priority
    for (uint32_t page : selection.prefetch_page_indices) {
        if (page < scheduler.page_count && priorities[page] < 0.5f) {
            priorities[page] = 0.5f;
        }
    }

    // Recently visible pages: decaying priority
    for (uint32_t i = 0; i < scheduler.page_count; ++i) {
        if (priorities[i] > 0.0f) {
            continue;  // already scored by selection or prefetch
        }
        uint32_t last = scheduler.last_selected_frame[i];
        if (last == 0xffffffffu) {
            continue;  // never selected
        }
        uint32_t age = frame_index - last;
        if (age <= config.eviction_grace_frames) {
            // Decay from 0.3 toward 0 over the grace period
            float t = static_cast<float>(age) / static_cast<float>(config.eviction_grace_frames);
            priorities[i] = 0.3f * (1.0f - t);
        }
        // else: priority stays 0, eligible for eviction
    }

    // Count resident and loading pages
    scheduler.total_resident = 0;
    scheduler.total_loading = 0;
    for (uint32_t i = 0; i < scheduler.page_count; ++i) {
        PageResidencyState state = model.pages[i].state;
        if (state == PageResidencyState::resident ||
            state == PageResidencyState::eviction_candidate) {
            scheduler.total_resident++;
        } else if (state == PageResidencyState::loading) {
            scheduler.total_loading++;
        }
    }

    // Build load queue: collect non-resident pages that have priority > 0,
    // sort by priority descending, cap at max_loads_per_frame
    scheduler.load_queue.clear();
    for (uint32_t i = 0; i < scheduler.page_count; ++i) {
        if (priorities[i] > 0.0f) {
            PageResidencyState state = model.pages[i].state;
            if (state == PageResidencyState::unloaded ||
                state == PageResidencyState::eviction_candidate) {
                scheduler.load_queue.push_back(i);
            }
        }
    }
    std::sort(scheduler.load_queue.begin(), scheduler.load_queue.end(),
              [&](uint32_t a, uint32_t b) {
                  return priorities[a] > priorities[b];
              });
    if (scheduler.load_queue.size() > config.max_loads_per_frame) {
        scheduler.load_queue.resize(config.max_loads_per_frame);
    }

    // Build evict queue: when over budget, pick lowest-priority resident pages
    // that have zero priority (not touched in grace period)
    scheduler.evict_queue.clear();
    uint32_t effective_resident = scheduler.total_resident + static_cast<uint32_t>(scheduler.load_queue.size());
    if (effective_resident > config.max_resident_pages) {
        uint32_t need_to_evict = effective_resident - config.max_resident_pages;

        // Collect eviction candidates: zero-priority resident/eviction_candidate pages
        std::vector<uint32_t> candidates;
        for (uint32_t i = 0; i < scheduler.page_count; ++i) {
            PageResidencyState state = model.pages[i].state;
            if ((state == PageResidencyState::resident ||
                 state == PageResidencyState::eviction_candidate) &&
                priorities[i] == 0.0f) {
                candidates.push_back(i);
            }
        }

        // Sort by last_touched_frame ascending (oldest first)
        std::sort(candidates.begin(), candidates.end(),
                  [&](uint32_t a, uint32_t b) {
                      return model.pages[a].last_touched_frame < model.pages[b].last_touched_frame;
                  });

        uint32_t evict_count = std::min(need_to_evict, static_cast<uint32_t>(candidates.size()));
        scheduler.evict_queue.assign(candidates.begin(), candidates.begin() + evict_count);
    }

    // Produce the ResidencyUpdateInput that drives step_residency()
    ResidencyUpdateInput input;
    input.frame_index = frame_index;
    input.resident_budget = config.max_resident_pages;
    input.eviction_grace_frames = config.eviction_grace_frames;
    input.selected_pages = selection.selected_page_indices;
    input.missing_pages = selection.missing_page_indices;
    input.prefetch_pages = selection.prefetch_page_indices;
    // completed_pages left empty -- the caller fills this in when actual I/O completes

    return input;
}

}  // namespace meridian
