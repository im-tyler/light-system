#include "runtime_model.h"

#include <algorithm>
#include <cassert>

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

ResidencyBootstrapMode parse_residency_bootstrap_mode(std::string_view mode) {
    if (mode == "none") {
        return ResidencyBootstrapMode::none;
    }
    if (mode == "all") {
        return ResidencyBootstrapMode::all;
    }
    if (mode == "base-only") {
        return ResidencyBootstrapMode::base_only;
    }
    if (mode == "lod-only") {
        return ResidencyBootstrapMode::lod_only;
    }
    throw BuilderError("invalid residency bootstrap mode: " + std::string(mode));
}

ResidencyModel create_residency_model(const VGeoResource& resource,
                                      ResidencyBootstrapMode bootstrap) {
    ResidencyModel model;
    model.pages.resize(resource.pages.size());
    // Establish exactly the advertised initial set: pages in the set start
    // resident (available immediately), the complement starts unloaded.
    // `all` preserves the original everything-resident default the viewer
    // runtime relies on.
    for (uint32_t page_index = 0; page_index < resource.pages.size(); ++page_index) {
        PageResidencyEntry& page = model.pages[page_index];
        const bool is_lod = resource.pages[page_index].lod_cluster_count != 0;
        const bool should_reside = bootstrap == ResidencyBootstrapMode::all ||
                                   (bootstrap == ResidencyBootstrapMode::base_only && !is_lod) ||
                                   (bootstrap == ResidencyBootstrapMode::lod_only && is_lod);
        if (should_reside) {
            page.state = PageResidencyState::resident;
            page.last_touched_frame = 0;
        } else {
            page.state = PageResidencyState::unloaded;
            page.last_touched_frame = 0xffffffffu;
        }
    }
#ifndef NDEBUG
    if (bootstrap == ResidencyBootstrapMode::none) {
        for (const PageResidencyEntry& page : model.pages) {
            assert(!is_resident_like(page.state) &&
                   "bootstrap mode 'none' must start with zero pages resident");
        }
    }
#endif
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

    // Pages in this call's selection set must stay resident: at frame 0 a
    // freshly-touched page's last_touched_frame (0) equals every untouched
    // page's, so without protection a tight budget could evict pages that
    // were selected this very frame.
    std::vector<uint8_t> selected_this_call(model.pages.size(), 0);
    for (const uint32_t page_index : input.selected_pages) {
        if (page_index < model.pages.size()) {
            selected_this_call[page_index] = 1;
        }
    }

    std::vector<uint32_t> resident_like_pages;
    for (uint32_t page_index = 0; page_index < model.pages.size(); ++page_index) {
        if (is_resident_like(model.pages[page_index].state)) {
            resident_like_pages.push_back(page_index);
        }
    }

    if (resident_like_pages.size() > input.resident_budget) {
        // Total order: state (eviction candidates first), then
        // last_touched_frame, then page index -- the page-index tiebreak
        // keeps the sort deterministic when timestamps collide (e.g. every
        // page initialized at frame 0).
        std::sort(resident_like_pages.begin(), resident_like_pages.end(), [&](uint32_t lhs, uint32_t rhs) {
            const PageResidencyEntry& left = model.pages[lhs];
            const PageResidencyEntry& right = model.pages[rhs];
            if (left.state != right.state) {
                return left.state == PageResidencyState::eviction_candidate;
            }
            if (left.last_touched_frame != right.last_touched_frame) {
                return left.last_touched_frame < right.last_touched_frame;
            }
            return lhs < rhs;
        });

        size_t evicted = 0;
        const size_t to_remove = resident_like_pages.size() - input.resident_budget;
        for (const uint32_t page_index : resident_like_pages) {
            if (evicted >= to_remove) {
                break;
            }
            if (selected_this_call[page_index] != 0) {
                continue;
            }
            PageResidencyEntry& entry = model.pages[page_index];
            if (entry.state == PageResidencyState::resident) {
                // Demote first; the page keeps one more pass as a candidate
                // and is only unloaded on a later call if it stays cold.
                entry.state = PageResidencyState::eviction_candidate;
                result.eviction_candidate_pages.push_back(page_index);
                continue;
            }
            entry.state = PageResidencyState::unloaded;
            entry.request_priority = 0;
            result.evicted_pages.push_back(page_index);
            evicted += 1;
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
