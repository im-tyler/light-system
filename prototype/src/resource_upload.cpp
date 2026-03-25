#include "resource_upload.h"

namespace meridian {

namespace {

PageKind page_kind_from_record(const PageRecord& page) {
    return page.lod_cluster_count != 0 ? PageKind::lod_cluster : PageKind::base_cluster;
}

std::array<float, 4> to_float4(const Vec3f& value) {
    return {value.x, value.y, value.z, 0.0f};
}

}  // namespace

UploadableScene build_uploadable_scene(const VGeoResource& resource) {
    UploadableScene scene;
    scene.header.instance_count = 1;
    scene.header.hierarchy_node_count = static_cast<uint32_t>(resource.hierarchy_nodes.size());
    scene.header.cluster_count = static_cast<uint32_t>(resource.clusters.size());
    scene.header.lod_group_count = static_cast<uint32_t>(resource.lod_groups.size());
    scene.header.lod_cluster_count = static_cast<uint32_t>(resource.lod_clusters.size());
    scene.header.page_count = static_cast<uint32_t>(resource.pages.size());
    scene.header.page_dependency_count = static_cast<uint32_t>(resource.page_dependencies.size());
    scene.header.node_lod_link_count = static_cast<uint32_t>(resource.node_lod_links.size());
    scene.header.base_payload_bytes = static_cast<uint32_t>(resource.cluster_geometry_payload.size());
    scene.header.lod_payload_bytes = static_cast<uint32_t>(resource.lod_geometry_payload.size());
    scene.header.visibility_format_word_count = 2;
    scene.header.flags = resource.has_fallback ? 1u : 0u;

    GpuInstanceRecord instance;
    instance.bounds_min = to_float4(resource.bounds.min);
    instance.bounds_max = to_float4(resource.bounds.max);
    instance.resource_index = 0;
    instance.root_node_index = resource.metadata.root_hierarchy_node_index;
    scene.instances.push_back(instance);

    scene.hierarchy_nodes.reserve(resource.hierarchy_nodes.size());
    for (const HierarchyNode& node : resource.hierarchy_nodes) {
        GpuHierarchyNodeRecord record;
        record.parent_index = node.parent_index;
        record.first_child_index = node.first_child_index;
        record.child_count = node.child_count;
        record.first_cluster_index = node.first_cluster_index;
        record.cluster_count = node.cluster_count;
        record.first_lod_link_index = node.first_lod_link_index;
        record.lod_link_count = node.lod_link_count;
        record.min_resident_page = node.min_resident_page;
        record.max_resident_page = node.max_resident_page;
        record.geometric_error = node.geometric_error;
        record.flags = node.flags;
        record.bounds_min = to_float4(node.bounds.min);
        record.bounds_max = to_float4(node.bounds.max);
        scene.hierarchy_nodes.push_back(record);
    }

    scene.clusters.reserve(resource.clusters.size());
    for (const ClusterRecord& cluster : resource.clusters) {
        GpuClusterRecord record;
        record.owning_node_index = cluster.owning_node_index;
        record.local_vertex_count = cluster.local_vertex_count;
        record.local_triangle_count = cluster.local_triangle_count;
        record.page_index = cluster.page_index;
        record.payload_offset = cluster.geometry_payload_offset;
        record.payload_size = cluster.geometry_payload_size;
        record.material_section_index = cluster.material_section_index;
        record.flags = cluster.flags;
        record.local_error = cluster.local_error;
        record.bounds_min = to_float4(cluster.bounds.min);
        record.bounds_max = to_float4(cluster.bounds.max);
        record.normal_cone = {cluster.normal_cone_axis[0], cluster.normal_cone_axis[1],
                              cluster.normal_cone_axis[2], cluster.normal_cone_axis[3]};
        scene.clusters.push_back(record);
    }

    scene.lod_groups.reserve(resource.lod_groups.size());
    for (const LodGroupRecord& group : resource.lod_groups) {
        GpuLodGroupRecord record;
        record.depth = group.depth;
        record.first_lod_cluster_index = group.first_lod_cluster_index;
        record.lod_cluster_count = group.lod_cluster_count;
        record.material_section_index = group.material_section_index;
        record.geometric_error = group.geometric_error;
        record.flags = group.flags;
        record.bounds_min = to_float4(group.bounds.min);
        record.bounds_max = to_float4(group.bounds.max);
        scene.lod_groups.push_back(record);
    }

    scene.lod_clusters.reserve(resource.lod_clusters.size());
    for (const LodClusterRecord& cluster : resource.lod_clusters) {
        GpuLodClusterRecord record;
        record.refined_group_index = cluster.refined_group_index;
        record.group_index = cluster.group_index;
        record.local_vertex_count = cluster.local_vertex_count;
        record.local_triangle_count = cluster.local_triangle_count;
        record.page_index = cluster.page_index;
        record.payload_offset = cluster.geometry_payload_offset;
        record.payload_size = cluster.geometry_payload_size;
        record.material_section_index = cluster.material_section_index;
        record.local_error = cluster.local_error;
        record.flags = cluster.flags;
        record.bounds_min = to_float4(cluster.bounds.min);
        record.bounds_max = to_float4(cluster.bounds.max);
        scene.lod_clusters.push_back(record);
    }

    scene.node_lod_links.reserve(resource.node_lod_links.size());
    for (const NodeLodLink& link : resource.node_lod_links) {
        scene.node_lod_links.push_back(GpuNodeLodLinkRecord{link.lod_group_index});
    }

    scene.pages.reserve(resource.pages.size());
    for (const PageRecord& page : resource.pages) {
        GpuPageRecord record;
        record.kind = page_kind_from_record(page);
        record.byte_offset = static_cast<uint32_t>(page.byte_offset);
        record.byte_size = page.uncompressed_byte_size;
        record.first_cluster_index = page.first_cluster_index;
        record.cluster_count = page.cluster_count;
        record.first_lod_cluster_index = page.first_lod_cluster_index;
        record.lod_cluster_count = page.lod_cluster_count;
        record.dependency_page_start = page.dependency_page_start;
        record.dependency_page_count = page.dependency_page_count;
        record.flags = page.flags;
        scene.pages.push_back(record);
    }

    scene.page_dependencies = resource.page_dependencies;
    scene.page_residency.resize(resource.pages.size());
    scene.base_payload = resource.cluster_geometry_payload;
    scene.lod_payload = resource.lod_geometry_payload;
    return scene;
}

}  // namespace meridian
