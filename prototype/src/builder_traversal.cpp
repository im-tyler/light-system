#include "builder_internal.h"

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
                           std::vector<uint8_t>& missing_marks,
                           TraversalSelection& selection) {
    bool resident = true;
    for (uint32_t cluster_index = node.first_cluster_index;
         cluster_index < node.first_cluster_index + node.cluster_count; ++cluster_index) {
        if (cluster_covered(cov, cluster_index)) {
            continue;
        }
        const uint32_t page_index = resource.clusters[cluster_index].page_index;
        if (!resident_pages[page_index]) {
            resident = false;
            if (!missing_marks[page_index]) {
                missing_marks[page_index] = 1;
                selection.missing_page_indices.push_back(page_index);
            }
        }
    }
    return resident;
}

void collect_prefetch_pages(const VGeoResource& resource, uint32_t missing_page_index,
                            const std::vector<uint8_t>& resident_pages,
                            std::vector<uint8_t>& prefetch_marks,
                            TraversalSelection& selection) {
    const PageRecord& page = resource.pages[missing_page_index];
    const uint32_t dependency_end = page.dependency_page_start + page.dependency_page_count;
    for (uint32_t dependency_index = page.dependency_page_start; dependency_index < dependency_end;
         ++dependency_index) {
        const uint32_t dependency_page_index = resource.page_dependencies[dependency_index];
        if (!resident_pages[dependency_page_index] && !prefetch_marks[dependency_page_index]) {
            prefetch_marks[dependency_page_index] = 1;
            selection.prefetch_page_indices.push_back(dependency_page_index);
        }
    }
}

void record_selected_node(uint32_t node_index, std::vector<uint8_t>& node_marks,
                          TraversalSelection& selection) {
    if (!node_marks[node_index]) {
        node_marks[node_index] = 1;
        selection.selected_node_indices.push_back(node_index);
    }
}

void record_selected_page(uint32_t page_index, std::vector<uint8_t>& selected_page_marks,
                          TraversalSelection& selection) {
    if (!selected_page_marks[page_index]) {
        selected_page_marks[page_index] = 1;
        selection.selected_page_indices.push_back(page_index);
    }
}

void select_base_span(const HierarchyNode& node, const CoverageMask& cov,
                      TraversalSelection& selection) {
    for (uint32_t cluster_index = node.first_cluster_index;
         cluster_index < node.first_cluster_index + node.cluster_count; ++cluster_index) {
        if (cluster_covered(cov, cluster_index)) {
            continue;
        }
        selection.selected_cluster_indices.push_back(cluster_index);
    }
}

// Emit an LOD group's clusters, assuming pages are already confirmed resident.
// Pages are checked against the group's LOD cluster set.
bool try_select_lod_group(const VGeoResource& resource, uint32_t group_index,
                          const std::vector<uint8_t>& resident_pages,
                          std::vector<uint8_t>& missing_marks,
                          std::vector<uint8_t>& prefetch_marks,
                          std::vector<uint8_t>& node_marks,
                          std::vector<uint8_t>& selected_page_marks,
                          uint32_t node_index,
                          TraversalSelection& selection) {
    const LodGroupRecord& group = resource.lod_groups[group_index];
    std::vector<uint32_t> group_pages;
    collect_group_pages(resource, group, group_pages);

    bool resident = true;
    for (uint32_t page_index : group_pages) {
        if (!resident_pages[page_index]) {
            resident = false;
            if (!missing_marks[page_index]) {
                missing_marks[page_index] = 1;
                selection.missing_page_indices.push_back(page_index);
                collect_prefetch_pages(resource, page_index, resident_pages, prefetch_marks,
                                      selection);
            }
        }
    }
    if (!resident) {
        return false;
    }

    record_selected_node(node_index, node_marks, selection);
    selection.selected_lod_group_indices.push_back(group_index);
    for (uint32_t cluster_index = group.first_lod_cluster_index;
         cluster_index < group.first_lod_cluster_index + group.lod_cluster_count; ++cluster_index) {
        selection.selected_lod_cluster_indices.push_back(cluster_index);
        record_selected_page(resource.lod_clusters[cluster_index].page_index, selected_page_marks,
                             selection);
    }
    return true;
}

// Traverse a node under an ancestor-provided coverage set. When a node's
// cluster span is fully covered the ancestor LOD already rendered us and we
// return immediately. Otherwise we may select our own LOD group (extending
// the coverage set for children) and we filter base emits against coverage.
void traverse_node_selection(const VGeoResource& resource, uint32_t node_index, float error_threshold,
                             CoverageMask& coverage,
                             const std::vector<uint8_t>& resident_pages,
                             std::vector<uint8_t>& missing_marks,
                             std::vector<uint8_t>& prefetch_marks,
                             std::vector<uint8_t>& node_marks,
                             std::vector<uint8_t>& selected_page_marks,
                             TraversalSelection& selection) {
    const HierarchyNode& node = resource.hierarchy_nodes[node_index];

    if (coverage.has_any && node_span_fully_covered(node, coverage)) {
        return;
    }

    // Pick the coarsest LOD link that meets the error threshold AND whose
    // group does not overlap already-covered clusters. We iterate in the
    // table's sorted order (ascending geometric error) and greedy-choose the
    // last eligible one.
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
        if (try_select_lod_group(resource, link.lod_group_index, resident_pages, missing_marks,
                                 prefetch_marks, node_marks, selected_page_marks, node_index,
                                 selection)) {
            const LodGroupRecord& group = resource.lod_groups[link.lod_group_index];
            if (group_covers_whole_node(resource, group, node)) {
                return;
            }
            std::vector<uint32_t> marked_indices;
            marked_indices.reserve(group.base_run_count * 4);
            mark_group_coverage(resource, group, coverage, marked_indices);
            for (uint32_t child_offset = 0; child_offset < node.child_count; ++child_offset) {
                traverse_node_selection(resource, node.first_child_index + child_offset,
                                        error_threshold, coverage, resident_pages, missing_marks,
                                        prefetch_marks, node_marks, selected_page_marks, selection);
            }
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
        if (covered_span_resident(resource, node, coverage, resident_pages, missing_marks,
                                  selection)) {
            record_selected_node(node_index, node_marks, selection);
            select_base_span(node, coverage, selection);
            for (uint32_t cluster_index = node.first_cluster_index;
                 cluster_index < node.first_cluster_index + node.cluster_count; ++cluster_index) {
                if (cluster_covered(coverage, cluster_index)) {
                    continue;
                }
                record_selected_page(resource.clusters[cluster_index].page_index, selected_page_marks,
                                     selection);
            }
        } else {
            for (uint32_t page_index = node.min_resident_page; page_index <= node.max_resident_page;
                 ++page_index) {
                if (page_index < resource.pages.size() && missing_marks[page_index]) {
                    collect_prefetch_pages(resource, page_index, resident_pages, prefetch_marks,
                                          selection);
                }
            }
        }
        return;
    }

    for (uint32_t child_offset = 0; child_offset < node.child_count; ++child_offset) {
        traverse_node_selection(resource, node.first_child_index + child_offset, error_threshold,
                                coverage, resident_pages, missing_marks, prefetch_marks, node_marks,
                                selected_page_marks, selection);
    }
}

TraversalSelection simulate_traversal(const VGeoResource& resource, float error_threshold,
                                      const std::vector<uint8_t>& resident_pages) {
    if (resident_pages.size() != resource.pages.size()) {
        throw BuilderError("resident page mask size must match resource page count");
    }

    TraversalSelection selection;
    std::vector<uint8_t> missing_marks(resource.pages.size(), 0);
    std::vector<uint8_t> prefetch_marks(resource.pages.size(), 0);
    std::vector<uint8_t> node_marks(resource.hierarchy_nodes.size(), 0);
    std::vector<uint8_t> selected_page_marks(resource.pages.size(), 0);
    std::vector<uint8_t> coverage_marks(resource.clusters.size(), 0);
    CoverageMask coverage{coverage_marks, false};
    traverse_node_selection(resource, resource.metadata.root_hierarchy_node_index, error_threshold,
                            coverage, resident_pages, missing_marks, prefetch_marks, node_marks,
                            selected_page_marks, selection);
    return selection;
}

}  // namespace meridian::detail
