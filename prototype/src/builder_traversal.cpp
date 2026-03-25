#include "builder_internal.h"

namespace meridian::detail {

bool collect_group_pages(const VGeoResource& resource, const LodGroupRecord& group,
                         std::vector<uint32_t>& pages);

bool base_span_resident(const VGeoResource& resource, const HierarchyNode& node,
                        const std::vector<uint8_t>& resident_pages,
                        std::vector<uint8_t>& missing_marks,
                        TraversalSelection& selection) {
    bool resident = true;
    for (uint32_t cluster_index = node.first_cluster_index;
         cluster_index < node.first_cluster_index + node.cluster_count; ++cluster_index) {
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

void select_base_span(const HierarchyNode& node, TraversalSelection& selection) {
    for (uint32_t cluster_index = node.first_cluster_index;
         cluster_index < node.first_cluster_index + node.cluster_count; ++cluster_index) {
        selection.selected_cluster_indices.push_back(cluster_index);
    }
}

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

void traverse_node_selection(const VGeoResource& resource, uint32_t node_index, float error_threshold,
                             const std::vector<uint8_t>& resident_pages,
                             std::vector<uint8_t>& missing_marks,
                             std::vector<uint8_t>& prefetch_marks,
                             std::vector<uint8_t>& node_marks,
                             std::vector<uint8_t>& selected_page_marks,
                             TraversalSelection& selection) {
    const HierarchyNode& node = resource.hierarchy_nodes[node_index];

    uint32_t selected_group_index = 0xffffffffu;
    for (uint32_t link_offset = 0; link_offset < node.lod_link_count; ++link_offset) {
        const uint32_t group_index =
            resource.node_lod_links[node.first_lod_link_index + link_offset].lod_group_index;
        if (resource.lod_groups[group_index].geometric_error <= error_threshold) {
            selected_group_index = group_index;
        }
    }
    if (selected_group_index != 0xffffffffu &&
        try_select_lod_group(resource, selected_group_index, resident_pages, missing_marks,
                             prefetch_marks, node_marks, selected_page_marks, node_index,
                             selection)) {
        return;
    }

    if (node.geometric_error <= error_threshold || node.child_count == 0) {
        if (base_span_resident(resource, node, resident_pages, missing_marks, selection)) {
            record_selected_node(node_index, node_marks, selection);
            select_base_span(node, selection);
            for (uint32_t cluster_index = node.first_cluster_index;
                 cluster_index < node.first_cluster_index + node.cluster_count; ++cluster_index) {
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
                                resident_pages, missing_marks, prefetch_marks, node_marks,
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
    traverse_node_selection(resource, resource.metadata.root_hierarchy_node_index, error_threshold,
                            resident_pages, missing_marks, prefetch_marks, node_marks,
                            selected_page_marks, selection);
    return selection;
}

}  // namespace meridian::detail
