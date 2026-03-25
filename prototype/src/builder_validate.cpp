#include "builder_internal.h"

namespace meridian::detail {

void validate_manifest(const BuildManifest& manifest) {
    if (manifest.asset_id.empty()) {
        throw BuilderError("manifest is missing asset_id");
    }
    if (manifest.source_asset.empty()) {
        throw BuilderError("manifest is missing source_asset");
    }
    if (manifest.output_path.empty()) {
        throw BuilderError("manifest is missing output_path");
    }
    if (manifest.material_slots.empty()) {
        throw BuilderError("manifest must define at least one material slot");
    }
    std::unordered_map<std::string, uint32_t> material_names;
    for (const std::string& material_slot : manifest.material_slots) {
        if (material_slot.empty()) {
            throw BuilderError("manifest material_slots must not contain empty names");
        }
        if (!material_names.emplace(material_slot, 1).second) {
            throw BuilderError("manifest material_slots must be unique");
        }
    }
    if (manifest.cluster_triangle_limit == 0) {
        throw BuilderError("cluster_triangle_limit must be greater than zero");
    }
    if (manifest.cluster_vertex_limit == 0) {
        throw BuilderError("cluster_vertex_limit must be greater than zero");
    }
    if (manifest.page_cluster_limit == 0) {
        throw BuilderError("page_cluster_limit must be greater than zero");
    }
    if (manifest.hierarchy_partition_size == 0) {
        throw BuilderError("hierarchy_partition_size must be greater than zero");
    }
}

void validate_resource(const VGeoResource& resource) {
    if (resource.asset_id.empty()) {
        throw BuilderError("resource asset_id is empty");
    }
    if (resource.hierarchy_nodes.empty()) {
        throw BuilderError("resource must contain at least one hierarchy node");
    }
    if (resource.hierarchy_nodes[0].parent_index != 0xffffffffu) {
        throw BuilderError("root hierarchy node must use invalid parent marker");
    }
    if (!resource.clusters.empty() && resource.pages.empty()) {
        throw BuilderError("resource with clusters must define pages");
    }

    for (size_t cluster_index = 0; cluster_index < resource.clusters.size(); ++cluster_index) {
        const ClusterRecord& cluster = resource.clusters[cluster_index];
        if (cluster.owning_node_index == 0 || cluster.owning_node_index >= resource.hierarchy_nodes.size()) {
            throw BuilderError("cluster has invalid owning node index");
        }
        if (cluster.material_section_index >= resource.material_sections.size()) {
            throw BuilderError("cluster has invalid material section index");
        }
        if (cluster.page_index >= resource.pages.size()) {
            throw BuilderError("cluster has invalid page index");
        }
        if (cluster.geometry_payload_offset + cluster.geometry_payload_size >
            resource.cluster_geometry_payload.size()) {
            throw BuilderError("cluster payload range exceeds cluster geometry payload");
        }
        const HierarchyNode& owning_node = resource.hierarchy_nodes[cluster.owning_node_index];
        if (owning_node.child_count != 0) {
            throw BuilderError("cluster owning node must be a leaf");
        }
        if (owning_node.first_cluster_index != cluster_index || owning_node.cluster_count != 1) {
            throw BuilderError("cluster owning node must map to exactly one cluster");
        }
    }

    for (const PageRecord& page : resource.pages) {
        if (page.dependency_page_start + page.dependency_page_count > resource.page_dependencies.size()) {
            throw BuilderError("page dependency range exceeds page dependency table");
        }
        const bool is_lod_page = (page.flags & kPageFlagLodPayload) != 0;
        if (is_lod_page) {
            if (page.cluster_count != 0) {
                throw BuilderError("lod page must not reference base cluster ranges");
            }
            if (page.first_lod_cluster_index + page.lod_cluster_count > resource.lod_clusters.size()) {
                throw BuilderError("page lod cluster range exceeds lod cluster table");
            }
            if (page.byte_offset + page.uncompressed_byte_size > resource.lod_geometry_payload.size()) {
                throw BuilderError("lod page payload range exceeds lod geometry payload");
            }
            const uint32_t page_end = page.first_lod_cluster_index + page.lod_cluster_count;
            for (uint32_t cluster_index = page.first_lod_cluster_index; cluster_index < page_end;
                 ++cluster_index) {
                const LodClusterRecord& cluster = resource.lod_clusters[cluster_index];
                if (cluster.page_index != page.page_index) {
                    throw BuilderError("lod page cluster does not point back to owning page");
                }
                if (cluster.geometry_payload_offset < page.byte_offset ||
                    cluster.geometry_payload_offset + cluster.geometry_payload_size >
                        page.byte_offset + page.uncompressed_byte_size) {
                    throw BuilderError("lod cluster payload falls outside owning page payload range");
                }
            }
        } else {
            if (page.lod_cluster_count != 0) {
                throw BuilderError("base page must not reference lod cluster ranges");
            }
            if (page.first_cluster_index + page.cluster_count > resource.clusters.size()) {
                throw BuilderError("page cluster range exceeds cluster table");
            }
            if (page.byte_offset + page.uncompressed_byte_size > resource.cluster_geometry_payload.size()) {
                throw BuilderError("page payload range exceeds cluster geometry payload");
            }
            const uint32_t page_end = page.first_cluster_index + page.cluster_count;
            for (uint32_t cluster_index = page.first_cluster_index; cluster_index < page_end;
                 ++cluster_index) {
                const ClusterRecord& cluster = resource.clusters[cluster_index];
                if (cluster.page_index != page.page_index) {
                    throw BuilderError("page cluster does not point back to owning page");
                }
                if (cluster.geometry_payload_offset < page.byte_offset ||
                    cluster.geometry_payload_offset + cluster.geometry_payload_size >
                        page.byte_offset + page.uncompressed_byte_size) {
                    throw BuilderError("cluster payload falls outside owning page payload range");
                }
            }
        }

        const uint32_t dependency_end = page.dependency_page_start + page.dependency_page_count;
        for (uint32_t dependency_index = page.dependency_page_start; dependency_index < dependency_end;
             ++dependency_index) {
            const uint32_t referenced_page_index = resource.page_dependencies[dependency_index];
            if (referenced_page_index >= resource.pages.size()) {
                throw BuilderError("page dependency references invalid page index");
            }
            if (referenced_page_index == page.page_index) {
                throw BuilderError("page dependency must not reference the page itself");
            }
        }
    }

    for (size_t node_index = 0; node_index < resource.hierarchy_nodes.size(); ++node_index) {
        const HierarchyNode& node = resource.hierarchy_nodes[node_index];
        if (node.parent_index != 0xffffffffu && node.parent_index >= resource.hierarchy_nodes.size()) {
            throw BuilderError("hierarchy node has invalid parent index");
        }
        if (node.first_lod_link_index + node.lod_link_count > resource.node_lod_links.size()) {
            throw BuilderError("hierarchy node lod link range exceeds node lod link table");
        }
        if (node.child_count > 0) {
            if (node.first_child_index + node.child_count > resource.hierarchy_nodes.size()) {
                throw BuilderError("hierarchy node child range exceeds node table");
            }
            for (uint32_t i = 0; i < node.child_count; ++i) {
                const HierarchyNode& child = resource.hierarchy_nodes[node.first_child_index + i];
                if (child.parent_index != node_index) {
                    throw BuilderError("hierarchy child does not point back to parent");
                }
                if (i == 0) {
                    if (child.first_cluster_index != node.first_cluster_index) {
                        throw BuilderError("hierarchy first child must begin at parent cluster range");
                    }
                } else {
                    const HierarchyNode& previous_child =
                        resource.hierarchy_nodes[node.first_child_index + i - 1];
                    if (child.first_cluster_index !=
                        previous_child.first_cluster_index + previous_child.cluster_count) {
                        throw BuilderError("hierarchy child cluster ranges must be contiguous");
                    }
                }
            }
            const HierarchyNode& last_child =
                resource.hierarchy_nodes[node.first_child_index + node.child_count - 1];
            if (last_child.first_cluster_index + last_child.cluster_count !=
                node.first_cluster_index + node.cluster_count) {
                throw BuilderError("hierarchy child cluster ranges must fully cover parent range");
            }
        } else if (node.cluster_count != 1) {
            throw BuilderError("hierarchy leaf node must own exactly one cluster");
        }
        if (node.first_cluster_index + node.cluster_count > resource.clusters.size()) {
            throw BuilderError("hierarchy node cluster range exceeds cluster table");
        }
        if (node.cluster_count > 0 && (node.min_resident_page > node.max_resident_page ||
                                       node.max_resident_page >= resource.pages.size())) {
            throw BuilderError("hierarchy node has invalid resident page range");
        }
        if (node.lod_link_count > 0) {
            if (node.cluster_count == 0) {
                throw BuilderError("hierarchy node with lod links must own clusters");
            }
            const uint32_t node_material_section_index =
                resource.clusters[node.first_cluster_index].material_section_index;
            for (uint32_t cluster_index = node.first_cluster_index + 1;
                 cluster_index < node.first_cluster_index + node.cluster_count; ++cluster_index) {
                if (resource.clusters[cluster_index].material_section_index != node_material_section_index) {
                    throw BuilderError(
                        "hierarchy node with lod links must cover a single material section");
                }
            }

            float previous_error = -std::numeric_limits<float>::infinity();
            for (uint32_t link_offset = 0; link_offset < node.lod_link_count; ++link_offset) {
                const NodeLodLink& link =
                    resource.node_lod_links[node.first_lod_link_index + link_offset];
                if (link.lod_group_index >= resource.lod_groups.size()) {
                    throw BuilderError("node lod link has invalid lod group index");
                }
                const LodGroupRecord& group = resource.lod_groups[link.lod_group_index];
                if (group.material_section_index != node_material_section_index) {
                    throw BuilderError("node lod link group material does not match node material");
                }
                if (group.geometric_error < previous_error) {
                    throw BuilderError("node lod links must be sorted by increasing geometric error");
                }
                previous_error = group.geometric_error;
            }
        }
    }

    for (const LodClusterRecord& cluster : resource.lod_clusters) {
        if (cluster.group_index >= resource.lod_groups.size()) {
            throw BuilderError("lod cluster has invalid group index");
        }
        if (cluster.refined_group_index >= static_cast<int32_t>(resource.lod_groups.size())) {
            throw BuilderError("lod cluster has invalid refined group index");
        }
        if (cluster.material_section_index >= resource.material_sections.size()) {
            throw BuilderError("lod cluster has invalid material section index");
        }
        if (cluster.page_index >= resource.pages.size()) {
            throw BuilderError("lod cluster has invalid page index");
        }
        if (cluster.geometry_payload_offset + cluster.geometry_payload_size >
            resource.lod_geometry_payload.size()) {
            throw BuilderError("lod cluster payload range exceeds lod geometry payload");
        }
    }

    for (size_t group_index = 0; group_index < resource.lod_groups.size(); ++group_index) {
        const LodGroupRecord& group = resource.lod_groups[group_index];
        if (group.first_lod_cluster_index + group.lod_cluster_count > resource.lod_clusters.size()) {
            throw BuilderError("lod group cluster range exceeds lod cluster table");
        }
        if (group.material_section_index >= resource.material_sections.size()) {
            throw BuilderError("lod group has invalid material section index");
        }
        const uint32_t group_end = group.first_lod_cluster_index + group.lod_cluster_count;
        for (uint32_t cluster_index = group.first_lod_cluster_index; cluster_index < group_end;
             ++cluster_index) {
            if (resource.lod_clusters[cluster_index].group_index >= resource.lod_groups.size()) {
                throw BuilderError("lod cluster in group range has invalid group index");
            }
            if (resource.lod_clusters[cluster_index].group_index != group_index) {
                throw BuilderError("lod group range contains cluster from a different group");
            }
            if (resource.lod_clusters[cluster_index].material_section_index !=
                group.material_section_index) {
                throw BuilderError("lod cluster material does not match owning lod group");
            }
        }
    }
}

}  // namespace meridian::detail
