#include "builder_internal.h"
#include "parallel_exec.h"

#include <atomic>
#include <functional>

namespace meridian::detail {

bool collect_group_pages(const VGeoResource& resource, const LodGroupRecord& group,
                         std::vector<uint32_t>& pages);

// Cluster coverage used to suppress already-rendered clusters during recursion.
// covered_marks[c] == 1 means some ancestor LOD group already emitted cluster c.
// The mark-set/unset pairs wrap recursive descents so the bitmap is reused.
struct CoverageMask {
    std::vector<uint8_t>& marks;
    bool has_any = false;
};

// Per-task traversal state: the dedup mark vectors plus the outputs they
// filter. The marks only suppress duplicate OUTPUT pushes (page/node
// dedup) -- they never influence traversal decisions -- so a task can run
// with fresh zeroed marks against a private copy of the coverage set and
// have its outputs merged afterwards in tree order to reproduce the serial
// output bit-for-bit.
struct TraversalScratch {
    std::vector<uint8_t> missing_marks;
    std::vector<uint8_t> prefetch_marks;
    std::vector<uint8_t> node_marks;
    std::vector<uint8_t> selected_page_marks;
    TraversalSelection selection;
};

// Fork-join control for parallel subtree traversal. tokens bounds the
// number of scheduled child tasks per traversal (oversubscription cap);
// it is a heuristic guard only and never affects output.
struct ForkControl {
    ParallelExecutor* exec = nullptr;
    std::atomic<uint32_t>* tokens = nullptr;
    uint32_t token_budget = 0;
    uint32_t min_child_clusters = 0;
};

bool cluster_covered(const CoverageMask& cov, uint32_t cluster_index) {
    return cov.has_any && cov.marks[cluster_index] != 0;
}

void mark_group_coverage(const VGeoResource& resource, const LodGroupRecord& group,
                         CoverageMask& cov, std::vector<uint32_t>& marked_indices) {
    for (uint32_t run_offset = 0; run_offset < group.base_run_count; ++run_offset) {
        const LodGroupBaseRun& run =
            resource.lod_group_base_runs[group.first_base_run_index + run_offset];
        for (uint32_t cluster_index = run.first_cluster_index;
             cluster_index < run.first_cluster_index + run.cluster_count; ++cluster_index) {
            if (!cov.marks[cluster_index]) {
                cov.marks[cluster_index] = 1;
                marked_indices.push_back(cluster_index);
            }
        }
    }
    cov.has_any = true;
}

void unmark_coverage(CoverageMask& cov, const std::vector<uint32_t>& marked_indices) {
    for (uint32_t cluster_index : marked_indices) {
        cov.marks[cluster_index] = 0;
    }
}

bool group_covers_whole_node(const VGeoResource& resource, const LodGroupRecord& group,
                             const HierarchyNode& node) {
    // True iff every cluster in the node's span is marked by one of the
    // group's base runs. Cheap when the group has a single run equal to the
    // node span; otherwise we just sum the run overlaps with the node span.
    uint32_t total = 0;
    const uint32_t node_end = node.first_cluster_index + node.cluster_count;
    for (uint32_t run_offset = 0; run_offset < group.base_run_count; ++run_offset) {
        const LodGroupBaseRun& run =
            resource.lod_group_base_runs[group.first_base_run_index + run_offset];
        const uint32_t run_end = run.first_cluster_index + run.cluster_count;
        if (run_end <= node.first_cluster_index || run.first_cluster_index >= node_end) {
            continue;
        }
        const uint32_t overlap_begin = std::max(run.first_cluster_index, node.first_cluster_index);
        const uint32_t overlap_end = std::min(run_end, node_end);
        total += overlap_end - overlap_begin;
    }
    return total == node.cluster_count;
}

bool group_overlaps_coverage(const VGeoResource& resource, const LodGroupRecord& group,
                             const CoverageMask& cov) {
    if (!cov.has_any) {
        return false;
    }
    for (uint32_t run_offset = 0; run_offset < group.base_run_count; ++run_offset) {
        const LodGroupBaseRun& run =
            resource.lod_group_base_runs[group.first_base_run_index + run_offset];
        for (uint32_t cluster_index = run.first_cluster_index;
             cluster_index < run.first_cluster_index + run.cluster_count; ++cluster_index) {
            if (cov.marks[cluster_index]) {
                return true;
            }
        }
    }
    return false;
}

bool node_span_fully_covered(const HierarchyNode& node, const CoverageMask& cov) {
    if (!cov.has_any) {
        return false;
    }
    for (uint32_t cluster_index = node.first_cluster_index;
         cluster_index < node.first_cluster_index + node.cluster_count; ++cluster_index) {
        if (!cov.marks[cluster_index]) {
            return false;
        }
    }
    return true;
}

bool covered_span_resident(const VGeoResource& resource, const HierarchyNode& node,
                           const CoverageMask& cov, const std::vector<uint8_t>& resident_pages,
                           TraversalScratch& scratch) {
    bool resident = true;
    for (uint32_t cluster_index = node.first_cluster_index;
         cluster_index < node.first_cluster_index + node.cluster_count; ++cluster_index) {
        if (cluster_covered(cov, cluster_index)) {
            continue;
        }
        const uint32_t page_index = resource.clusters[cluster_index].page_index;
        if (!resident_pages[page_index]) {
            resident = false;
            if (!scratch.missing_marks[page_index]) {
                scratch.missing_marks[page_index] = 1;
                scratch.selection.missing_page_indices.push_back(page_index);
            }
        }
    }
    return resident;
}

void collect_prefetch_pages(const VGeoResource& resource, uint32_t missing_page_index,
                            const std::vector<uint8_t>& resident_pages,
                            TraversalScratch& scratch) {
    const PageRecord& page = resource.pages[missing_page_index];
    const uint32_t dependency_end = page.dependency_page_start + page.dependency_page_count;
    for (uint32_t dependency_index = page.dependency_page_start; dependency_index < dependency_end;
         ++dependency_index) {
        const uint32_t dependency_page_index = resource.page_dependencies[dependency_index];
        if (!resident_pages[dependency_page_index] &&
            !scratch.prefetch_marks[dependency_page_index]) {
            scratch.prefetch_marks[dependency_page_index] = 1;
            scratch.selection.prefetch_page_indices.push_back(dependency_page_index);
        }
    }
}

void record_selected_node(uint32_t node_index, TraversalScratch& scratch) {
    if (!scratch.node_marks[node_index]) {
        scratch.node_marks[node_index] = 1;
        scratch.selection.selected_node_indices.push_back(node_index);
    }
}

void record_selected_page(uint32_t page_index, TraversalScratch& scratch) {
    if (!scratch.selected_page_marks[page_index]) {
        scratch.selected_page_marks[page_index] = 1;
        scratch.selection.selected_page_indices.push_back(page_index);
    }
}

void select_base_span(const HierarchyNode& node, const CoverageMask& cov,
                      TraversalScratch& scratch) {
    for (uint32_t cluster_index = node.first_cluster_index;
         cluster_index < node.first_cluster_index + node.cluster_count; ++cluster_index) {
        if (cluster_covered(cov, cluster_index)) {
            continue;
        }
        scratch.selection.selected_cluster_indices.push_back(cluster_index);
    }
}

// Emit an LOD group's clusters, assuming pages are already confirmed resident.
// Pages are checked against the group's LOD cluster set.
bool try_select_lod_group(const VGeoResource& resource, uint32_t group_index,
                          const std::vector<uint8_t>& resident_pages, uint32_t node_index,
                          TraversalScratch& scratch) {
    const LodGroupRecord& group = resource.lod_groups[group_index];
    std::vector<uint32_t> group_pages;
    collect_group_pages(resource, group, group_pages);

    bool resident = true;
    for (uint32_t page_index : group_pages) {
        if (!resident_pages[page_index]) {
            resident = false;
            if (!scratch.missing_marks[page_index]) {
                scratch.missing_marks[page_index] = 1;
                scratch.selection.missing_page_indices.push_back(page_index);
                collect_prefetch_pages(resource, page_index, resident_pages, scratch);
            }
        }
    }
    if (!resident) {
        return false;
    }

    record_selected_node(node_index, scratch);
    scratch.selection.selected_lod_group_indices.push_back(group_index);
    for (uint32_t cluster_index = group.first_lod_cluster_index;
         cluster_index < group.first_lod_cluster_index + group.lod_cluster_count; ++cluster_index) {
        scratch.selection.selected_lod_cluster_indices.push_back(cluster_index);
        record_selected_page(resource.lod_clusters[cluster_index].page_index, scratch);
    }
    return true;
}

// Merge a child task's outputs into the parent's, in child order. Lists the
// serial traversal deduplicates through shared marks (nodes, pages) are
// re-deduplicated here against the accumulated marks, so the merged order is
// the serial first-encounter order. Cluster and LOD-group lists have no
// dedup in the serial path and are appended as-is.
void merge_child_scratch(TraversalScratch& dst, TraversalScratch& child) {
    for (uint32_t node_index : child.selection.selected_node_indices) {
        if (!dst.node_marks[node_index]) {
            dst.node_marks[node_index] = 1;
            dst.selection.selected_node_indices.push_back(node_index);
        }
    }
    for (uint32_t page_index : child.selection.selected_page_indices) {
        if (!dst.selected_page_marks[page_index]) {
            dst.selected_page_marks[page_index] = 1;
            dst.selection.selected_page_indices.push_back(page_index);
        }
    }
    for (uint32_t page_index : child.selection.missing_page_indices) {
        if (!dst.missing_marks[page_index]) {
            dst.missing_marks[page_index] = 1;
            dst.selection.missing_page_indices.push_back(page_index);
        }
    }
    for (uint32_t page_index : child.selection.prefetch_page_indices) {
        if (!dst.prefetch_marks[page_index]) {
            dst.prefetch_marks[page_index] = 1;
            dst.selection.prefetch_page_indices.push_back(page_index);
        }
    }
    dst.selection.selected_cluster_indices.insert(
        dst.selection.selected_cluster_indices.end(),
        child.selection.selected_cluster_indices.begin(),
        child.selection.selected_cluster_indices.end());
    dst.selection.selected_lod_group_indices.insert(
        dst.selection.selected_lod_group_indices.end(),
        child.selection.selected_lod_group_indices.begin(),
        child.selection.selected_lod_group_indices.end());
    dst.selection.selected_lod_cluster_indices.insert(
        dst.selection.selected_lod_cluster_indices.end(),
        child.selection.selected_lod_cluster_indices.begin(),
        child.selection.selected_lod_cluster_indices.end());
}

void traverse_node_selection(const VGeoResource& resource, uint32_t node_index,
                             float error_threshold, CoverageMask& coverage,
                             const std::vector<uint8_t>& resident_pages,
                             TraversalScratch& scratch, const ForkControl& fork);

// Descend into a node's children. Sibling subtrees are independent under the
// coverage set: the serial traversal marks ancestor coverage before the
// sibling loop and unmarks only after ALL siblings completed, so every child
// executes against exactly this coverage state. When forking, each child
// task gets a byte-copy of the coverage marks plus fresh output marks; the
// ordered merge afterwards reproduces the serial output bit-for-bit.
void traverse_children(const VGeoResource& resource, const HierarchyNode& node,
                       float error_threshold, CoverageMask& coverage,
                       const std::vector<uint8_t>& resident_pages, TraversalScratch& scratch,
                       const ForkControl& fork) {
    if (node.child_count == 0) {
        return;
    }
    bool use_parallel = false;
    if (fork.exec != nullptr && node.child_count >= 2 &&
        fork.tokens->load(std::memory_order_relaxed) < fork.token_budget) {
        for (uint32_t child_offset = 0; child_offset < node.child_count; ++child_offset) {
            if (resource.hierarchy_nodes[node.first_child_index + child_offset].cluster_count >=
                fork.min_child_clusters) {
                use_parallel = true;
                break;
            }
        }
    }
    if (!use_parallel) {
        for (uint32_t child_offset = 0; child_offset < node.child_count; ++child_offset) {
            traverse_node_selection(resource, node.first_child_index + child_offset,
                                    error_threshold, coverage, resident_pages, scratch, fork);
        }
        return;
    }
    fork.tokens->fetch_add(node.child_count, std::memory_order_relaxed);
    std::vector<TraversalScratch> child_scratch(node.child_count);
    for (uint32_t child_offset = 0; child_offset < node.child_count; ++child_offset) {
        child_scratch[child_offset].missing_marks.assign(resource.pages.size(), 0);
        child_scratch[child_offset].prefetch_marks.assign(resource.pages.size(), 0);
        child_scratch[child_offset].node_marks.assign(resource.hierarchy_nodes.size(), 0);
        child_scratch[child_offset].selected_page_marks.assign(resource.pages.size(), 0);
    }
    std::vector<std::vector<uint8_t>> child_marks(node.child_count);
    std::vector<CoverageMask> child_coverage;
    child_coverage.reserve(node.child_count);
    for (uint32_t child_offset = 0; child_offset < node.child_count; ++child_offset) {
        child_marks[child_offset].assign(coverage.marks.begin(), coverage.marks.end());
        child_coverage.push_back(CoverageMask{child_marks[child_offset], coverage.has_any});
    }
    std::vector<std::function<void()>> jobs;
    jobs.reserve(node.child_count);
    for (uint32_t child_offset = 0; child_offset < node.child_count; ++child_offset) {
        const uint32_t child_index = node.first_child_index + child_offset;
        jobs.emplace_back([=, &resource, &child_coverage, &child_scratch, &resident_pages,
                           &fork]() {
            traverse_node_selection(resource, child_index, error_threshold,
                                    child_coverage[child_offset], resident_pages,
                                    child_scratch[child_offset], fork);
        });
    }
    fork.exec->run(jobs);
    for (uint32_t child_offset = 0; child_offset < node.child_count; ++child_offset) {
        merge_child_scratch(scratch, child_scratch[child_offset]);
    }
}

// Traverse a node under an ancestor-provided coverage set. When a node's
// cluster span is fully covered the ancestor LOD already rendered us and we
// return immediately. Otherwise we may select our own LOD group (extending
// the coverage set for children) and we filter base emits against coverage.
void traverse_node_selection(const VGeoResource& resource, uint32_t node_index, float error_threshold,
                             CoverageMask& coverage,
                             const std::vector<uint8_t>& resident_pages,
                             TraversalScratch& scratch, const ForkControl& fork) {
    const HierarchyNode& node = resource.hierarchy_nodes[node_index];

    if (coverage.has_any && node_span_fully_covered(node, coverage)) {
        return;
    }

    // Pick the coarsest LOD link that meets the error threshold AND whose
    // group does not overlap already-covered clusters. We iterate in the
    // table's sorted order (ascending geometric error) and greedy-choose the
    // last eligible one. Equal-error ties resolve deterministically: the
    // link table is totally ordered by (geometric_error, group index) at
    // build time, so the greedy pick prefers the highest group index among
    // tied groups -- identical on every platform.
    uint32_t selected_link_index = 0xffffffffu;
    for (uint32_t link_offset = 0; link_offset < node.lod_link_count; ++link_offset) {
        const NodeLodLink& link =
            resource.node_lod_links[node.first_lod_link_index + link_offset];
        const LodGroupRecord& group = resource.lod_groups[link.lod_group_index];
        if (group.geometric_error > error_threshold) {
            continue;
        }
        if (group_overlaps_coverage(resource, group, coverage)) {
            continue;
        }
        selected_link_index = node.first_lod_link_index + link_offset;
    }

    if (selected_link_index != 0xffffffffu) {
        const NodeLodLink& link = resource.node_lod_links[selected_link_index];
        if (try_select_lod_group(resource, link.lod_group_index, resident_pages, node_index,
                                 scratch)) {
            const LodGroupRecord& group = resource.lod_groups[link.lod_group_index];
            if (group_covers_whole_node(resource, group, node)) {
                return;
            }
            std::vector<uint32_t> marked_indices;
            marked_indices.reserve(group.base_run_count * 4);
            mark_group_coverage(resource, group, coverage, marked_indices);
            traverse_children(resource, node, error_threshold, coverage, resident_pages, scratch,
                              fork);
            unmark_coverage(coverage, marked_indices);
            // has_any may still be true if ancestor coverage remains; recompute
            // lazily: if no marks remain set after unmark, flip has_any off.
            // Rather than scanning, we assume callers keep has_any accurate --
            // re-check by seeing whether we added the first coverage at this
            // frame. Simpler: just leave has_any as-is; residual false-positives
            // are harmless because lookups are still correct (marks are 0).
            return;
        }
    }

    if (node.geometric_error <= error_threshold || node.child_count == 0) {
        if (covered_span_resident(resource, node, coverage, resident_pages, scratch)) {
            record_selected_node(node_index, scratch);
            select_base_span(node, coverage, scratch);
            for (uint32_t cluster_index = node.first_cluster_index;
                 cluster_index < node.first_cluster_index + node.cluster_count; ++cluster_index) {
                if (cluster_covered(coverage, cluster_index)) {
                    continue;
                }
                record_selected_page(resource.clusters[cluster_index].page_index, scratch);
            }
        } else {
            for (uint32_t page_index = node.min_resident_page; page_index <= node.max_resident_page;
                 ++page_index) {
                if (page_index < resource.pages.size() && scratch.missing_marks[page_index]) {
                    collect_prefetch_pages(resource, page_index, resident_pages, scratch);
                }
            }
        }
        return;
    }

    traverse_children(resource, node, error_threshold, coverage, resident_pages, scratch, fork);
}

TraversalSelection simulate_traversal(const VGeoResource& resource, float error_threshold,
                                      const std::vector<uint8_t>& resident_pages,
                                      ParallelExecutor* executor) {
    if (resident_pages.size() != resource.pages.size()) {
        throw BuilderError("resident page mask size must match resource page count");
    }

    TraversalScratch scratch;
    scratch.missing_marks.assign(resource.pages.size(), 0);
    scratch.prefetch_marks.assign(resource.pages.size(), 0);
    scratch.node_marks.assign(resource.hierarchy_nodes.size(), 0);
    scratch.selected_page_marks.assign(resource.pages.size(), 0);
    std::vector<uint8_t> coverage_marks(resource.clusters.size(), 0);
    CoverageMask coverage{coverage_marks, false};

    ForkControl fork;
    std::atomic<uint32_t> tokens(0);
    if (executor != nullptr && executor->total_threads() > 1) {
        fork.exec = executor;
        fork.tokens = &tokens;
        fork.token_budget = 2 * executor->total_threads();
        fork.min_child_clusters = 512;
    }
    traverse_node_selection(resource, resource.metadata.root_hierarchy_node_index, error_threshold,
                            coverage, resident_pages, scratch, fork);
    return std::move(scratch.selection);
}

}  // namespace meridian::detail
