#define CLUSTERLOD_IMPLEMENTATION
#include "builder_internal.h"

namespace meridian::detail {

std::vector<unsigned int> extract_meshlet_global_indices(
    const meshopt_Meshlet& meshlet, const std::vector<unsigned int>& meshlet_vertices,
    const std::vector<unsigned char>& meshlet_triangles) {
    std::vector<unsigned int> global_indices;
    global_indices.reserve(meshlet.triangle_count * 3);
    for (size_t i = 0; i < meshlet.triangle_count * 3; ++i) {
        global_indices.push_back(
            meshlet_vertices[meshlet.vertex_offset + meshlet_triangles[meshlet.triangle_offset + i]]);
    }
    return global_indices;
}

ClusterRecord append_meshlet_payload(const MeshData& mesh, const meshopt_Meshlet& meshlet,
                                     const std::vector<unsigned int>& meshlet_vertices,
                                     const std::vector<unsigned char>& meshlet_triangles,
                                     uint32_t material_section_index,
                                     std::vector<std::byte>& payload) {
    std::vector<Vec3f> local_positions;
    std::vector<uint32_t> local_indices;
    local_positions.reserve(meshlet.vertex_count);
    local_indices.reserve(meshlet.triangle_count * 3);

    Bounds3f bounds = make_empty_bounds();
    for (size_t i = 0; i < meshlet.vertex_count; ++i) {
        const uint32_t vertex_index = meshlet_vertices[meshlet.vertex_offset + i];
        const Vec3f& position = mesh.positions[vertex_index];
        local_positions.push_back(position);
        update_bounds(bounds, position);
    }
    for (size_t i = 0; i < meshlet.triangle_count * 3; ++i) {
        local_indices.push_back(meshlet_triangles[meshlet.triangle_offset + i]);
    }

    std::vector<Vec3f> local_normals;
    local_normals.reserve(meshlet.vertex_count);
    for (size_t i = 0; i < meshlet.vertex_count; ++i) {
        const uint32_t vertex_index = meshlet_vertices[meshlet.vertex_offset + i];
        local_normals.push_back(mesh.normals[vertex_index]);
    }

    const uint32_t payload_offset = static_cast<uint32_t>(payload.size());
    const PayloadHeader payload_header{meshlet.vertex_count, meshlet.triangle_count};
    append_bytes(payload, payload_header);
    for (const Vec3f& position : local_positions) {
        append_bytes(payload, position);
    }
    for (const Vec3f& normal : local_normals) {
        append_bytes(payload, normal);
    }
    for (const uint32_t index : local_indices) {
        append_bytes(payload, index);
    }

    ClusterRecord cluster;
    cluster.owning_node_index = 0xffffffffu;
    cluster.local_vertex_count = meshlet.vertex_count;
    cluster.local_triangle_count = meshlet.triangle_count;
    cluster.geometry_payload_offset = payload_offset;
    cluster.geometry_payload_size = static_cast<uint32_t>(payload.size()) - payload_offset;
    cluster.page_index = 0;
    cluster.bounds = bounds;
    const meshopt_Bounds meshlet_bounds = meshopt_computeMeshletBounds(
        &meshlet_vertices[meshlet.vertex_offset], &meshlet_triangles[meshlet.triangle_offset],
        meshlet.triangle_count, reinterpret_cast<const float*>(mesh.positions.data()),
        mesh.positions.size(), sizeof(Vec3f));
    cluster.normal_cone_axis[0] = meshlet_bounds.cone_axis[0];
    cluster.normal_cone_axis[1] = meshlet_bounds.cone_axis[1];
    cluster.normal_cone_axis[2] = meshlet_bounds.cone_axis[2];
    cluster.normal_cone_axis[3] = meshlet_bounds.cone_cutoff;
    cluster.local_error = meshlet_bounds.radius;
    cluster.material_section_index = material_section_index;
    return cluster;
}

Bounds3f merge_cluster_bounds(const std::vector<ClusterRecord>& clusters,
                              const std::vector<uint32_t>& cluster_ids) {
    Bounds3f bounds = make_empty_bounds();
    for (const uint32_t cluster_id : cluster_ids) {
        update_bounds(bounds, clusters[cluster_id].bounds.min);
        update_bounds(bounds, clusters[cluster_id].bounds.max);
    }
    return bounds;
}

std::vector<std::vector<uint32_t>> partition_cluster_ids(
    const MeshData& mesh, const std::vector<std::vector<unsigned int>>& cluster_global_indices,
    const std::vector<uint32_t>& cluster_ids, uint32_t target_partition_size) {
    if (cluster_ids.size() <= target_partition_size) {
        return {cluster_ids};
    }

    size_t total_index_count = 0;
    for (const uint32_t cluster_id : cluster_ids) {
        total_index_count += cluster_global_indices[cluster_id].size();
    }

    std::vector<unsigned int> flat_indices;
    std::vector<unsigned int> cluster_counts(cluster_ids.size());
    flat_indices.reserve(total_index_count);
    for (size_t i = 0; i < cluster_ids.size(); ++i) {
        const uint32_t cluster_id = cluster_ids[i];
        cluster_counts[i] = static_cast<unsigned int>(cluster_global_indices[cluster_id].size());
        flat_indices.insert(flat_indices.end(), cluster_global_indices[cluster_id].begin(),
                            cluster_global_indices[cluster_id].end());
    }

    std::vector<unsigned int> partition_ids(cluster_ids.size());
    const size_t partition_count = meshopt_partitionClusters(
        partition_ids.data(), flat_indices.data(), flat_indices.size(), cluster_counts.data(),
        cluster_counts.size(), reinterpret_cast<const float*>(mesh.positions.data()),
        mesh.positions.size(), sizeof(Vec3f), target_partition_size);

    if (partition_count <= 1) {
        std::vector<std::vector<uint32_t>> fallback;
        for (size_t i = 0; i < cluster_ids.size(); i += target_partition_size) {
            const size_t end = std::min(cluster_ids.size(), i + target_partition_size);
            fallback.emplace_back(cluster_ids.begin() + static_cast<std::ptrdiff_t>(i),
                                  cluster_ids.begin() + static_cast<std::ptrdiff_t>(end));
        }
        return fallback;
    }

    std::vector<std::vector<uint32_t>> partitions(partition_count);
    for (size_t i = 0; i < cluster_ids.size(); ++i) {
        partitions[partition_ids[i]].push_back(cluster_ids[i]);
    }

    std::vector<std::vector<uint32_t>> compacted;
    compacted.reserve(partitions.size());
    for (auto& partition : partitions) {
        if (!partition.empty()) {
            compacted.push_back(std::move(partition));
        }
    }
    return compacted;
}

uint32_t build_temp_hierarchy(std::vector<TempHierarchyNode>& nodes, const MeshData& mesh,
                              const std::vector<std::vector<unsigned int>>& cluster_global_indices,
                              const std::vector<ClusterRecord>& clusters,
                              const std::vector<uint32_t>& cluster_ids, uint32_t parent_index,
                              uint32_t partition_size) {
    const uint32_t node_index = static_cast<uint32_t>(nodes.size());
    TempHierarchyNode node;
    node.parent_index = parent_index;
    node.bounds = merge_cluster_bounds(clusters, cluster_ids);
    node.geometric_error = 0.0f;
    for (const uint32_t cluster_id : cluster_ids) {
        node.geometric_error = std::max(node.geometric_error, clusters[cluster_id].local_error);
    }
    nodes.push_back(node);

    if (cluster_ids.size() == 1) {
        nodes[node_index].leaf_cluster_index = cluster_ids[0];
        return node_index;
    }

    std::vector<std::vector<uint32_t>> partitions =
        partition_cluster_ids(mesh, cluster_global_indices, cluster_ids, partition_size);
    if (partitions.size() == 1 && partitions[0].size() == cluster_ids.size()) {
        partitions.clear();
        partitions.reserve(cluster_ids.size());
        for (const uint32_t cluster_id : cluster_ids) {
            partitions.push_back({cluster_id});
        }
    }

    nodes[node_index].child_indices.reserve(partitions.size());
    for (const std::vector<uint32_t>& partition : partitions) {
        nodes[node_index].child_indices.push_back(build_temp_hierarchy(
            nodes, mesh, cluster_global_indices, clusters, partition, node_index, partition_size));
    }
    return node_index;
}

uint32_t append_reordered_cluster(const ClusterRecord& source_cluster,
                                  const std::vector<std::byte>& source_payload,
                                  VGeoResource& resource) {
    const uint32_t new_cluster_index = static_cast<uint32_t>(resource.clusters.size());
    ClusterRecord cluster = source_cluster;
    cluster.geometry_payload_offset = static_cast<uint32_t>(resource.cluster_geometry_payload.size());
    const auto begin = source_payload.begin() + source_cluster.geometry_payload_offset;
    const auto end = begin + source_cluster.geometry_payload_size;
    resource.cluster_geometry_payload.insert(resource.cluster_geometry_payload.end(), begin, end);
    cluster.geometry_payload_size = source_cluster.geometry_payload_size;
    resource.clusters.push_back(cluster);
    return new_cluster_index;
}

void flatten_temp_hierarchy(const std::vector<TempHierarchyNode>& temp_nodes,
                            const std::vector<ClusterRecord>& source_clusters,
                            const std::vector<std::byte>& source_payload, VGeoResource& resource,
                            std::vector<uint32_t>& temp_to_runtime_node_indices,
                            std::vector<uint32_t>& source_to_runtime_cluster_indices,
                            uint32_t temp_node_index, uint32_t node_index, uint32_t parent_index) {
    const TempHierarchyNode& temp_node = temp_nodes[temp_node_index];
    HierarchyNode node;
    temp_to_runtime_node_indices[temp_node_index] = node_index;
    node.parent_index = parent_index;
    node.bounds = temp_node.bounds;
    node.geometric_error = temp_node.geometric_error;
    node.min_resident_page = 0xffffffffu;
    node.max_resident_page = 0xffffffffu;

    const uint32_t cluster_start = static_cast<uint32_t>(resource.clusters.size());
    if (temp_node.child_indices.empty()) {
        node.first_child_index = 0;
        node.child_count = 0;
        node.first_cluster_index = cluster_start;
        node.cluster_count = 1;
        const uint32_t new_cluster_index =
            append_reordered_cluster(source_clusters[temp_node.leaf_cluster_index], source_payload, resource);
        source_to_runtime_cluster_indices[temp_node.leaf_cluster_index] = new_cluster_index;
        resource.clusters[new_cluster_index].owning_node_index = node_index;
        resource.hierarchy_nodes[node_index] = node;
        return;
    }

    node.first_child_index = static_cast<uint32_t>(resource.hierarchy_nodes.size());
    node.child_count = static_cast<uint32_t>(temp_node.child_indices.size());
    resource.hierarchy_nodes.resize(resource.hierarchy_nodes.size() + temp_node.child_indices.size());

    for (size_t child_offset = 0; child_offset < temp_node.child_indices.size(); ++child_offset) {
        const uint32_t child_index = node.first_child_index + static_cast<uint32_t>(child_offset);
        flatten_temp_hierarchy(temp_nodes, source_clusters, source_payload, resource,
                               temp_to_runtime_node_indices, source_to_runtime_cluster_indices,
                               temp_node.child_indices[child_offset], child_index, node_index);
    }

    node.first_cluster_index = cluster_start;
    node.cluster_count = static_cast<uint32_t>(resource.clusters.size()) - cluster_start;
    resource.hierarchy_nodes[node_index] = node;
}

void update_hierarchy_page_ranges(VGeoResource& resource) {
    for (HierarchyNode& node : resource.hierarchy_nodes) {
        if (node.cluster_count == 0) {
            node.min_resident_page = 0xffffffffu;
            node.max_resident_page = 0xffffffffu;
            continue;
        }

        node.min_resident_page = std::numeric_limits<uint32_t>::max();
        node.max_resident_page = 0;
        const uint32_t cluster_end = node.first_cluster_index + node.cluster_count;
        for (uint32_t cluster_index = node.first_cluster_index; cluster_index < cluster_end; ++cluster_index) {
            node.min_resident_page =
                std::min(node.min_resident_page, resource.clusters[cluster_index].page_index);
            node.max_resident_page =
                std::max(node.max_resident_page, resource.clusters[cluster_index].page_index);
        }
    }
}

std::vector<PageRecord> build_base_pages(const std::vector<ClusterRecord>& clusters,
                                         uint32_t page_cluster_limit) {
    std::vector<PageRecord> pages;
    if (clusters.empty()) {
        return pages;
    }

    const uint32_t cluster_count = static_cast<uint32_t>(clusters.size());
    for (uint32_t page_start = 0, page_index = 0; page_start < cluster_count;
         page_start += page_cluster_limit, ++page_index) {
        const uint32_t page_end = std::min(cluster_count, page_start + page_cluster_limit);
        const uint32_t first_offset = clusters[page_start].geometry_payload_offset;
        const ClusterRecord& last_cluster = clusters[page_end - 1];
        const uint32_t last_end =
            last_cluster.geometry_payload_offset + last_cluster.geometry_payload_size;

        PageRecord page;
        page.page_index = page_index;
        page.byte_offset = first_offset;
        page.compressed_byte_size = last_end - first_offset;
        page.uncompressed_byte_size = page.compressed_byte_size;
        page.first_cluster_index = page_start;
        page.cluster_count = page_end - page_start;
        page.first_lod_cluster_index = 0;
        page.lod_cluster_count = 0;
        page.dependency_page_start = 0;
        page.dependency_page_count = 0;
        page.flags = 0;
        pages.push_back(page);
    }
    return pages;
}

std::vector<PageRecord> build_lod_pages(const std::vector<LodClusterRecord>& lod_clusters,
                                        uint32_t page_cluster_limit, uint32_t page_index_base) {
    std::vector<PageRecord> pages;
    if (lod_clusters.empty()) {
        return pages;
    }

    const uint32_t cluster_count = static_cast<uint32_t>(lod_clusters.size());
    for (uint32_t page_start = 0, local_page_index = 0; page_start < cluster_count;
         page_start += page_cluster_limit, ++local_page_index) {
        const uint32_t page_end = std::min(cluster_count, page_start + page_cluster_limit);
        const uint32_t first_offset = lod_clusters[page_start].geometry_payload_offset;
        const LodClusterRecord& last_cluster = lod_clusters[page_end - 1];
        const uint32_t last_end =
            last_cluster.geometry_payload_offset + last_cluster.geometry_payload_size;

        PageRecord page;
        page.page_index = page_index_base + local_page_index;
        page.byte_offset = first_offset;
        page.compressed_byte_size = last_end - first_offset;
        page.uncompressed_byte_size = page.compressed_byte_size;
        page.first_cluster_index = 0;
        page.cluster_count = 0;
        page.first_lod_cluster_index = page_start;
        page.lod_cluster_count = page_end - page_start;
        page.dependency_page_start = 0;
        page.dependency_page_count = 0;
        page.flags = kPageFlagLodPayload;
        pages.push_back(page);
    }
    return pages;
}

Bounds3f sphere_bounds_to_aabb(const clodBounds& bounds) {
    Bounds3f box;
    box.min = {bounds.center[0] - bounds.radius, bounds.center[1] - bounds.radius,
               bounds.center[2] - bounds.radius};
    box.max = {bounds.center[0] + bounds.radius, bounds.center[1] + bounds.radius,
               bounds.center[2] + bounds.radius};
    return box;
}

clodConfig make_clod_config(const BuildManifest& manifest) {
    clodConfig config = clodDefaultConfig(manifest.cluster_triangle_limit);
    config.max_vertices = manifest.cluster_vertex_limit;
    config.max_triangles = manifest.cluster_triangle_limit;
    config.min_triangles =
        std::max<size_t>(1, std::min<size_t>(config.min_triangles, config.max_triangles));
    config.partition_size = manifest.hierarchy_partition_size;
    config.optimize_bounds = true;
    config.optimize_clusters = true;
    config.simplify_permissive = false;
    config.simplify_fallback_permissive = false;
    return config;
}

std::string make_index_signature(const unsigned int* indices, size_t index_count) {
    std::string signature(index_count * sizeof(unsigned int), '\0');
    std::memcpy(&signature[0], indices, signature.size());
    return signature;
}

void build_section_base_clusters(const MeshData& mesh, const MeshSection& section,
                                 const clodConfig& config, std::vector<ClusterRecord>& source_clusters,
                                 std::vector<std::byte>& source_cluster_payload,
                                 std::vector<std::vector<unsigned int>>& cluster_global_indices) {
    const size_t max_meshlets = meshopt_buildMeshletsBound(section.indices.size(), config.max_vertices,
                                                           config.min_triangles);
    std::vector<meshopt_Meshlet> meshlets(max_meshlets);
    std::vector<unsigned int> meshlet_vertices(section.indices.size());
    std::vector<unsigned char> meshlet_triangles(section.indices.size());

    const size_t meshlet_count = config.cluster_spatial
                                     ? meshopt_buildMeshletsSpatial(
                                           meshlets.data(), meshlet_vertices.data(),
                                           meshlet_triangles.data(), section.indices.data(),
                                           section.indices.size(),
                                           reinterpret_cast<const float*>(mesh.positions.data()),
                                           mesh.positions.size(), sizeof(Vec3f), config.max_vertices,
                                           config.min_triangles, config.max_triangles,
                                           config.cluster_fill_weight)
                                     : meshopt_buildMeshletsFlex(
                                           meshlets.data(), meshlet_vertices.data(),
                                           meshlet_triangles.data(), section.indices.data(),
                                           section.indices.size(),
                                           reinterpret_cast<const float*>(mesh.positions.data()),
                                           mesh.positions.size(), sizeof(Vec3f), config.max_vertices,
                                           config.min_triangles, config.max_triangles, 0.0f,
                                           config.cluster_split_factor);

    meshlets.resize(meshlet_count);
    for (meshopt_Meshlet& meshlet : meshlets) {
        if (config.optimize_clusters) {
            meshopt_optimizeMeshlet(&meshlet_vertices[meshlet.vertex_offset],
                                    &meshlet_triangles[meshlet.triangle_offset],
                                    meshlet.triangle_count, meshlet.vertex_count);
        }
        cluster_global_indices.push_back(
            extract_meshlet_global_indices(meshlet, meshlet_vertices, meshlet_triangles));
        source_clusters.push_back(append_meshlet_payload(
            mesh, meshlet, meshlet_vertices, meshlet_triangles, section.material_section_index,
            source_cluster_payload));
    }
}

// Attach each LOD group to the deepest hierarchy node whose cluster span
// contains the full set of base clusters the group covers. Base-cluster
// coverage is stored as a flat list of (first_cluster_index, cluster_count)
// runs on each LOD group -- most groups have a single run, scenes whose
// clusterlod grouping doesn't align with the hierarchy partitioner have more.
// Subset attachment is made safe by threading a "covered" set of cluster
// ranges through the traversal: when an LOD group is selected at a node,
// its runs become the covered set for that subtree; descendants whose
// clusters fall in the covered set are skipped and base-cluster emits
// filter out already-covered clusters.
//
// Historical note: before f018cf1 this function required an exact node/group
// span match. That was semantically safe but scenes whose clusterlod groupings
// don't line up with the hierarchy partitioner (e.g. massive_city: 6230 groups
// -> 8 links) lost almost all LOD coverage and fell through to 31k base-leaf
// emits. Multi-run subset attachment + covered-set threading keeps correctness
// and recovers most of the lost coverage.
void build_node_lod_links(VGeoResource& resource,
                          const std::vector<LodGroupBuildInfo>& lod_group_infos,
                          const std::vector<uint32_t>& source_to_runtime_cluster_indices) {
    resource.node_lod_links.clear();
    resource.lod_group_base_runs.clear();

    // For each hierarchy node, the cluster span's material section (for
    // groups that cover the whole node). We only attach groups to nodes whose
    // entire span is single-material -- LOD groups are always single-material.
    std::vector<uint32_t> node_material_section(resource.hierarchy_nodes.size(), 0xffffffffu);
    std::vector<uint8_t> node_single_material(resource.hierarchy_nodes.size(), 0);
    for (uint32_t node_index = 0; node_index < resource.hierarchy_nodes.size(); ++node_index) {
        const HierarchyNode& node = resource.hierarchy_nodes[node_index];
        if (node.cluster_count == 0) {
            continue;
        }
        const uint32_t material_section_index =
            resource.clusters[node.first_cluster_index].material_section_index;
        bool single_material = true;
        for (uint32_t cluster_index = node.first_cluster_index + 1;
             cluster_index < node.first_cluster_index + node.cluster_count; ++cluster_index) {
            if (resource.clusters[cluster_index].material_section_index != material_section_index) {
                single_material = false;
                break;
            }
        }
        node_material_section[node_index] = material_section_index;
        node_single_material[node_index] = single_material ? 1u : 0u;
    }

    // cluster_owning_node[c] = the leaf hierarchy node that owns cluster c.
    std::vector<uint32_t> cluster_owning_node(resource.clusters.size(), 0xffffffffu);
    for (uint32_t node_index = 0; node_index < resource.hierarchy_nodes.size(); ++node_index) {
        const HierarchyNode& node = resource.hierarchy_nodes[node_index];
        if (node.child_count != 0 || node.cluster_count == 0) {
            continue;
        }
        for (uint32_t offset = 0; offset < node.cluster_count; ++offset) {
            cluster_owning_node[node.first_cluster_index + offset] = node_index;
        }
    }

    // Bucket candidate group -> node attachments; we resolve order and write
    // out the final link table after processing all groups.
    std::vector<std::vector<uint32_t>> links_by_node(resource.hierarchy_nodes.size());

    for (uint32_t group_index = 0; group_index < lod_group_infos.size(); ++group_index) {
        const LodGroupBuildInfo& info = lod_group_infos[group_index];
        if (info.source_cluster_ids.empty()) {
            continue;
        }

        std::vector<uint32_t> runtime_cluster_indices;
        runtime_cluster_indices.reserve(info.source_cluster_ids.size());
        for (uint32_t source_cluster_index : info.source_cluster_ids) {
            if (source_cluster_index >= source_to_runtime_cluster_indices.size()) {
                throw BuilderError("lod group provenance references invalid source cluster index");
            }
            runtime_cluster_indices.push_back(source_to_runtime_cluster_indices[source_cluster_index]);
        }

        std::sort(runtime_cluster_indices.begin(), runtime_cluster_indices.end());
        runtime_cluster_indices.erase(
            std::unique(runtime_cluster_indices.begin(), runtime_cluster_indices.end()),
            runtime_cluster_indices.end());

        // Compact consecutive cluster indices into runs of (first, count).
        std::vector<LodGroupBaseRun> runs;
        for (size_t i = 0; i < runtime_cluster_indices.size();) {
            uint32_t run_first = runtime_cluster_indices[i];
            uint32_t run_count = 1;
            while (i + run_count < runtime_cluster_indices.size() &&
                   runtime_cluster_indices[i + run_count] == run_first + run_count) {
                ++run_count;
            }
            runs.push_back(LodGroupBaseRun{run_first, run_count});
            i += run_count;
        }

        const uint32_t group_first = runtime_cluster_indices.front();
        const uint32_t group_end = runtime_cluster_indices.back() + 1;

        // Walk up from the leaf owning group_first to find the deepest
        // ancestor whose cluster span contains [group_first, group_end). The
        // group's runs must all live inside this span (they're all >= group_first
        // and < group_end by construction).
        if (group_first >= cluster_owning_node.size()) {
            continue;
        }
        uint32_t candidate = cluster_owning_node[group_first];
        uint32_t best_node = 0xffffffffu;
        while (candidate != 0xffffffffu) {
            const HierarchyNode& node = resource.hierarchy_nodes[candidate];
            const uint32_t node_first = node.first_cluster_index;
            const uint32_t node_end = node_first + node.cluster_count;
            if (node_first > group_first || node_end < group_end) {
                candidate = node.parent_index;
                continue;
            }
            if (!node_single_material[candidate] ||
                node_material_section[candidate] != info.material_section_index) {
                candidate = node.parent_index;
                continue;
            }

            best_node = candidate;
            uint32_t containing_child = 0xffffffffu;
            for (uint32_t child_offset = 0; child_offset < node.child_count; ++child_offset) {
                const HierarchyNode& child =
                    resource.hierarchy_nodes[node.first_child_index + child_offset];
                const uint32_t child_first = child.first_cluster_index;
                const uint32_t child_end = child_first + child.cluster_count;
                if (child_first <= group_first && child_end >= group_end) {
                    containing_child = node.first_child_index + child_offset;
                    break;
                }
            }
            if (containing_child != 0xffffffffu) {
                candidate = containing_child;
                continue;
            }
            break;
        }

        if (best_node == 0xffffffffu) {
            continue;
        }

        // Persist the group's base runs.
        LodGroupRecord& group_record = resource.lod_groups[group_index];
        group_record.first_base_run_index =
            static_cast<uint32_t>(resource.lod_group_base_runs.size());
        group_record.base_run_count = static_cast<uint32_t>(runs.size());
        resource.lod_group_base_runs.insert(resource.lod_group_base_runs.end(), runs.begin(),
                                            runs.end());

        links_by_node[best_node].push_back(group_index);
    }

    for (uint32_t node_index = 0; node_index < resource.hierarchy_nodes.size(); ++node_index) {
        std::vector<uint32_t>& linked = links_by_node[node_index];
        std::sort(linked.begin(), linked.end(), [&](uint32_t lhs, uint32_t rhs) {
            return resource.lod_groups[lhs].geometric_error <
                   resource.lod_groups[rhs].geometric_error;
        });
        linked.erase(std::unique(linked.begin(), linked.end()), linked.end());

        HierarchyNode& node = resource.hierarchy_nodes[node_index];
        node.first_lod_link_index = static_cast<uint32_t>(resource.node_lod_links.size());
        node.lod_link_count = static_cast<uint32_t>(linked.size());
        for (uint32_t group_index : linked) {
            resource.node_lod_links.push_back(NodeLodLink{group_index});
        }
    }
}

LodClusterRecord append_lod_cluster_payload(const MeshData& mesh, const unsigned int* global_indices,
                                            size_t index_count, uint32_t group_index,
                                            int32_t refined_group_index,
                                            uint32_t material_section_index,
                                            std::vector<std::byte>& payload,
                                            const clodBounds& cluster_bounds,
                                            size_t vertex_count_hint) {
    std::vector<unsigned int> local_vertices(vertex_count_hint > 0 ? vertex_count_hint : index_count);
    std::vector<unsigned char> local_triangles(index_count);
    const size_t local_vertex_count =
        clodLocalIndices(local_vertices.data(), local_triangles.data(), global_indices, index_count);
    local_vertices.resize(local_vertex_count);
    local_triangles.resize(index_count);

    const uint32_t payload_offset = static_cast<uint32_t>(payload.size());
    const PayloadHeader payload_header{static_cast<uint32_t>(local_vertices.size()),
                                       static_cast<uint32_t>(index_count / 3)};
    append_bytes(payload, payload_header);
    for (const uint32_t vertex_index : local_vertices) {
        append_bytes(payload, mesh.positions[vertex_index]);
    }
    for (const uint32_t vertex_index : local_vertices) {
        append_bytes(payload, mesh.normals[vertex_index]);
    }
    for (const unsigned char index : local_triangles) {
        const uint32_t widened = index;
        append_bytes(payload, widened);
    }

    LodClusterRecord cluster;
    cluster.refined_group_index = refined_group_index;
    cluster.group_index = group_index;
    cluster.local_vertex_count = static_cast<uint32_t>(local_vertices.size());
    cluster.local_triangle_count = static_cast<uint32_t>(index_count / 3);
    cluster.geometry_payload_offset = payload_offset;
    cluster.geometry_payload_size = static_cast<uint32_t>(payload.size()) - payload_offset;
    cluster.bounds = sphere_bounds_to_aabb(cluster_bounds);
    cluster.local_error = cluster_bounds.error;
    cluster.material_section_index = material_section_index;
    return cluster;
}

void build_lod_metadata(VGeoResource& resource, const MeshData& mesh, const BuildManifest& manifest,
                        const std::vector<std::vector<unsigned int>>& source_cluster_global_indices,
                        const std::vector<uint32_t>& source_to_runtime_cluster_indices) {
    const clodConfig config = make_clod_config(manifest);
    std::vector<LodGroupBuildInfo> lod_group_infos;
    std::vector<std::vector<uint32_t>> group_provenance_by_index(resource.lod_groups.size());

    for (const MeshSection& section : mesh.sections) {
        if (section.indices.empty()) {
            continue;
        }

        std::unordered_map<std::string, uint32_t> original_cluster_lookup;
        for (uint32_t source_cluster_index = 0; source_cluster_index < source_cluster_global_indices.size();
             ++source_cluster_index) {
            if (resource.clusters[source_to_runtime_cluster_indices[source_cluster_index]]
                    .material_section_index != section.material_section_index) {
                continue;
            }
            const std::vector<unsigned int>& global_indices = source_cluster_global_indices[source_cluster_index];
            original_cluster_lookup.emplace(
                make_index_signature(global_indices.data(), global_indices.size()), source_cluster_index);
        }

        clodMesh clod_mesh{};
        clod_mesh.indices = section.indices.data();
        clod_mesh.index_count = section.indices.size();
        clod_mesh.vertex_count = mesh.positions.size();
        clod_mesh.vertex_positions = reinterpret_cast<const float*>(mesh.positions.data());
        clod_mesh.vertex_positions_stride = sizeof(Vec3f);
        clod_mesh.vertex_attributes = nullptr;
        clod_mesh.vertex_attributes_stride = 0;
        clod_mesh.vertex_lock = mesh.vertex_locks.data();
        clod_mesh.attribute_weights = nullptr;
        clod_mesh.attribute_count = 0;
        clod_mesh.attribute_protect_mask = 0;

        clodBuild(config, clod_mesh,
                  [&](clodGroup group, const clodCluster* clusters, size_t cluster_count) -> int {
                      std::vector<uint32_t> group_source_cluster_ids;
                      for (size_t i = 0; i < cluster_count; ++i) {
                          if (clusters[i].refined == -1) {
                              const auto found = original_cluster_lookup.find(
                                  make_index_signature(clusters[i].indices, clusters[i].index_count));
                              if (found == original_cluster_lookup.end()) {
                                  throw BuilderError(
                                      "failed to resolve original cluster provenance for lod group");
                              }
                              group_source_cluster_ids.push_back(found->second);
                          } else {
                              if (clusters[i].refined < 0 ||
                                  static_cast<size_t>(clusters[i].refined) >= group_provenance_by_index.size()) {
                                  throw BuilderError("lod cluster refined group index is out of provenance range");
                              }
                              const std::vector<uint32_t>& refined_provenance =
                                  group_provenance_by_index[static_cast<size_t>(clusters[i].refined)];
                              group_source_cluster_ids.insert(group_source_cluster_ids.end(),
                                                              refined_provenance.begin(),
                                                              refined_provenance.end());
                          }
                      }
                      std::sort(group_source_cluster_ids.begin(), group_source_cluster_ids.end());
                      group_source_cluster_ids.erase(
                          std::unique(group_source_cluster_ids.begin(), group_source_cluster_ids.end()),
                          group_source_cluster_ids.end());

                      LodGroupRecord lod_group;
                      lod_group.depth = static_cast<uint32_t>(group.depth);
                      lod_group.first_lod_cluster_index =
                          static_cast<uint32_t>(resource.lod_clusters.size());
                      lod_group.lod_cluster_count = static_cast<uint32_t>(cluster_count);
                      lod_group.material_section_index = section.material_section_index;
                      lod_group.bounds = sphere_bounds_to_aabb(group.simplified);
                      lod_group.geometric_error = group.simplified.error;

                      const uint32_t group_index = static_cast<uint32_t>(resource.lod_groups.size());
                      resource.lod_groups.push_back(lod_group);
                      group_provenance_by_index.push_back(group_source_cluster_ids);
                      lod_group_infos.push_back(LodGroupBuildInfo{section.material_section_index,
                                                                  group_source_cluster_ids});

                      for (size_t i = 0; i < cluster_count; ++i) {
                          resource.lod_clusters.push_back(append_lod_cluster_payload(
                              mesh, clusters[i].indices, clusters[i].index_count, group_index,
                              clusters[i].refined, section.material_section_index,
                              resource.lod_geometry_payload, clusters[i].bounds,
                              clusters[i].vertex_count));
                      }

                      return static_cast<int>(group_index);
                  });
    }

    build_node_lod_links(resource, lod_group_infos, source_to_runtime_cluster_indices);
}

bool collect_group_pages(const VGeoResource& resource, const LodGroupRecord& group,
                         std::vector<uint32_t>& pages) {
    pages.clear();
    for (uint32_t cluster_index = group.first_lod_cluster_index;
         cluster_index < group.first_lod_cluster_index + group.lod_cluster_count; ++cluster_index) {
        const uint32_t page_index = resource.lod_clusters[cluster_index].page_index;
        if (pages.empty() || pages.back() != page_index) {
            pages.push_back(page_index);
        }
    }
    return !pages.empty();
}

void collect_node_base_pages(const VGeoResource& resource, const HierarchyNode& node,
                             std::vector<uint32_t>& pages) {
    pages.clear();
    for (uint32_t cluster_index = node.first_cluster_index;
         cluster_index < node.first_cluster_index + node.cluster_count; ++cluster_index) {
        const uint32_t page_index = resource.clusters[cluster_index].page_index;
        if (pages.empty() || pages.back() != page_index) {
            pages.push_back(page_index);
        }
    }
}

void collect_cluster_span_pages(const VGeoResource& resource, uint32_t first_cluster_index,
                                uint32_t cluster_count, std::vector<uint32_t>& pages) {
    pages.clear();
    for (uint32_t cluster_index = first_cluster_index;
         cluster_index < first_cluster_index + cluster_count; ++cluster_index) {
        const uint32_t page_index = resource.clusters[cluster_index].page_index;
        if (pages.empty() || pages.back() != page_index) {
            pages.push_back(page_index);
        }
    }
}

void link_adjacent_page_sets(std::vector<std::vector<uint32_t>>& adjacency,
                             const std::vector<uint32_t>& lhs, const std::vector<uint32_t>& rhs) {
    for (const uint32_t lhs_page : lhs) {
        std::vector<uint32_t>& lhs_adjacency = adjacency[lhs_page];
        lhs_adjacency.insert(lhs_adjacency.end(), rhs.begin(), rhs.end());
    }
    for (const uint32_t rhs_page : rhs) {
        std::vector<uint32_t>& rhs_adjacency = adjacency[rhs_page];
        rhs_adjacency.insert(rhs_adjacency.end(), lhs.begin(), lhs.end());
    }
}

void build_page_dependencies(VGeoResource& resource) {
    std::vector<std::vector<uint32_t>> adjacency(resource.pages.size());
    std::vector<uint32_t> current_pages;
    std::vector<uint32_t> next_pages;

    for (const HierarchyNode& node : resource.hierarchy_nodes) {
        if (node.lod_link_count == 0 || node.cluster_count == 0) {
            continue;
        }

        // Each link's adjacent replacement-level dependency is between the
        // pages holding the group's covered base clusters and the pages
        // holding the group's LOD clusters.
        std::vector<uint32_t> link_base_pages;
        for (uint32_t link_offset = 0; link_offset < node.lod_link_count; ++link_offset) {
            const NodeLodLink& link =
                resource.node_lod_links[node.first_lod_link_index + link_offset];
            const LodGroupRecord& group = resource.lod_groups[link.lod_group_index];
            link_base_pages.clear();
            for (uint32_t run_offset = 0; run_offset < group.base_run_count; ++run_offset) {
                const LodGroupBaseRun& run =
                    resource.lod_group_base_runs[group.first_base_run_index + run_offset];
                for (uint32_t cluster_index = run.first_cluster_index;
                     cluster_index < run.first_cluster_index + run.cluster_count; ++cluster_index) {
                    const uint32_t page_index = resource.clusters[cluster_index].page_index;
                    if (link_base_pages.empty() || link_base_pages.back() != page_index) {
                        link_base_pages.push_back(page_index);
                    }
                }
            }
            collect_group_pages(resource, group, next_pages);
            if (!link_base_pages.empty() && !next_pages.empty()) {
                link_adjacent_page_sets(adjacency, link_base_pages, next_pages);
            }
        }
        (void)current_pages;
    }

    resource.page_dependencies.clear();
    for (uint32_t page_index = 0; page_index < resource.pages.size(); ++page_index) {
        std::vector<uint32_t>& page_adjacency = adjacency[page_index];
        std::sort(page_adjacency.begin(), page_adjacency.end());
        page_adjacency.erase(std::remove(page_adjacency.begin(), page_adjacency.end(), page_index),
                             page_adjacency.end());
        page_adjacency.erase(std::unique(page_adjacency.begin(), page_adjacency.end()),
                             page_adjacency.end());

        PageRecord& page = resource.pages[page_index];
        page.dependency_page_start = static_cast<uint32_t>(resource.page_dependencies.size());
        page.dependency_page_count = static_cast<uint32_t>(page_adjacency.size());
        resource.page_dependencies.insert(resource.page_dependencies.end(), page_adjacency.begin(),
                                          page_adjacency.end());
    }
}

}  // namespace meridian::detail
