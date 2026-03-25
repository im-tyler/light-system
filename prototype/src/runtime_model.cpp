#include "runtime_model.h"

#include <algorithm>

namespace meridian {

namespace {

bool is_resident_like(PageResidencyState state) {
    return state == PageResidencyState::resident || state == PageResidencyState::eviction_candidate;
}

void request_page(PageResidencyEntry& entry, uint32_t priority, uint32_t page_index,
                  ResidencyUpdateResult& result) {
    if (entry.state == PageResidencyState::unloaded ||
        entry.state == PageResidencyState::eviction_candidate) {
        entry.state = PageResidencyState::requested;
        entry.request_priority = std::max(entry.request_priority, priority);
        result.requested_pages.push_back(page_index);
    } else if (entry.state == PageResidencyState::requested) {
        entry.request_priority = std::max(entry.request_priority, priority);
    }
}

}  // namespace

ResidencyModel create_residency_model(const VGeoResource& resource) {
    ResidencyModel model;
    model.pages.resize(resource.pages.size());
    // Initialize all pages as resident so geometry is available immediately
    for (auto& page : model.pages) {
        page.state = PageResidencyState::resident;
        page.last_touched_frame = 0;
    }
    return model;
}

std::vector<uint8_t> build_resident_page_mask(const ResidencyModel& model) {
    std::vector<uint8_t> resident_pages(model.pages.size(), 0);
    for (uint32_t page_index = 0; page_index < model.pages.size(); ++page_index) {
        resident_pages[page_index] = is_resident_like(model.pages[page_index].state) ? 1 : 0;
    }
    return resident_pages;
}

ResidencyUpdateResult step_residency(ResidencyModel& model, const ResidencyUpdateInput& input) {
    ResidencyUpdateResult result;
    std::vector<PageResidencyState> original_states(model.pages.size());
    for (uint32_t page_index = 0; page_index < model.pages.size(); ++page_index) {
        original_states[page_index] = model.pages[page_index].state;
    }

    for (const uint32_t page_index : input.completed_pages) {
        if (page_index >= model.pages.size()) {
            continue;
        }
        PageResidencyEntry& entry = model.pages[page_index];
        if (entry.state == PageResidencyState::requested || entry.state == PageResidencyState::loading) {
            entry.state = PageResidencyState::resident;
            entry.last_touched_frame = input.frame_index;
            result.completed_pages.push_back(page_index);
        }
    }

    for (const uint32_t page_index : input.selected_pages) {
        if (page_index >= model.pages.size()) {
            continue;
        }
        PageResidencyEntry& entry = model.pages[page_index];
        if (entry.state == PageResidencyState::resident ||
            entry.state == PageResidencyState::eviction_candidate) {
            entry.state = PageResidencyState::resident;
            entry.last_touched_frame = input.frame_index;
            result.touched_pages.push_back(page_index);
        }
    }

    for (const uint32_t page_index : input.missing_pages) {
        if (page_index < model.pages.size()) {
            request_page(model.pages[page_index], 2, page_index, result);
        }
    }
    for (const uint32_t page_index : input.prefetch_pages) {
        if (page_index < model.pages.size()) {
            request_page(model.pages[page_index], 1, page_index, result);
        }
    }

    for (uint32_t page_index = 0; page_index < model.pages.size(); ++page_index) {
        PageResidencyEntry& entry = model.pages[page_index];
        if (original_states[page_index] == PageResidencyState::requested &&
            entry.state == PageResidencyState::requested) {
            entry.state = PageResidencyState::loading;
            result.loading_pages.push_back(page_index);
        }
    }

    for (uint32_t page_index = 0; page_index < model.pages.size(); ++page_index) {
        PageResidencyEntry& entry = model.pages[page_index];
        if (entry.state == PageResidencyState::resident &&
            entry.last_touched_frame != 0xffffffffu &&
            input.frame_index > entry.last_touched_frame + input.eviction_grace_frames) {
            entry.state = PageResidencyState::eviction_candidate;
            result.eviction_candidate_pages.push_back(page_index);
        }
    }

    std::vector<uint32_t> resident_like_pages;
    for (uint32_t page_index = 0; page_index < model.pages.size(); ++page_index) {
        if (is_resident_like(model.pages[page_index].state)) {
            resident_like_pages.push_back(page_index);
        }
    }

    if (resident_like_pages.size() > input.resident_budget) {
        std::sort(resident_like_pages.begin(), resident_like_pages.end(), [&](uint32_t lhs, uint32_t rhs) {
            const PageResidencyEntry& left = model.pages[lhs];
            const PageResidencyEntry& right = model.pages[rhs];
            if (left.state != right.state) {
                return left.state == PageResidencyState::eviction_candidate;
            }
            return left.last_touched_frame < right.last_touched_frame;
        });

        while (resident_like_pages.size() > input.resident_budget) {
            const uint32_t page_index = resident_like_pages.front();
            resident_like_pages.erase(resident_like_pages.begin());
            PageResidencyEntry& entry = model.pages[page_index];
            if (entry.state == PageResidencyState::resident) {
                entry.state = PageResidencyState::eviction_candidate;
                result.eviction_candidate_pages.push_back(page_index);
                resident_like_pages.push_back(page_index);
                continue;
            }
            entry.state = PageResidencyState::unloaded;
            entry.request_priority = 0;
            result.evicted_pages.push_back(page_index);
        }
    }

    return result;
}

const char* to_string(PageResidencyState state) {
    switch (state) {
        case PageResidencyState::unloaded:
            return "unloaded";
        case PageResidencyState::requested:
            return "requested";
        case PageResidencyState::loading:
            return "loading";
        case PageResidencyState::resident:
            return "resident";
        case PageResidencyState::eviction_candidate:
            return "eviction_candidate";
    }
    return "unknown";
}

}  // namespace meridian
