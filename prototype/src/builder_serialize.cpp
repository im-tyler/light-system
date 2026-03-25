#include "builder_internal.h"

namespace meridian::detail {

MaterialSectionDisk to_disk(const MaterialSection& section) {
    MaterialSectionDisk disk{};
    const auto name_size = std::min(section.name.size(), sizeof(disk.name) - 1);
    std::copy_n(section.name.data(), name_size, disk.name);
    disk.fallback_section = section.fallback_section;
    disk.flags = section.flags;
    return disk;
}

HierarchyNodeDisk to_disk(const HierarchyNode& node) {
    HierarchyNodeDisk disk{};
    disk.parent_index = node.parent_index;
    disk.first_child_index = node.first_child_index;
    disk.child_count = node.child_count;
    disk.first_cluster_index = node.first_cluster_index;
    disk.cluster_count = node.cluster_count;
    disk.first_lod_link_index = node.first_lod_link_index;
    disk.lod_link_count = node.lod_link_count;
    disk.bounds = node.bounds;
    disk.geometric_error = node.geometric_error;
    disk.min_resident_page = node.min_resident_page;
    disk.max_resident_page = node.max_resident_page;
    disk.flags = node.flags;
    return disk;
}

ClusterRecordDisk to_disk(const ClusterRecord& cluster) {
    ClusterRecordDisk disk{};
    disk.owning_node_index = cluster.owning_node_index;
    disk.local_vertex_count = cluster.local_vertex_count;
    disk.local_triangle_count = cluster.local_triangle_count;
    disk.geometry_payload_offset = cluster.geometry_payload_offset;
    disk.geometry_payload_size = cluster.geometry_payload_size;
    disk.page_index = cluster.page_index;
    disk.bounds = cluster.bounds;
    std::copy(std::begin(cluster.normal_cone_axis), std::end(cluster.normal_cone_axis),
              std::begin(disk.normal_cone_axis));
    disk.local_error = cluster.local_error;
    disk.material_section_index = cluster.material_section_index;
    disk.flags = cluster.flags;
    return disk;
}

PageRecordDisk to_disk(const PageRecord& page) {
    PageRecordDisk disk{};
    disk.page_index = page.page_index;
    disk.byte_offset = page.byte_offset;
    disk.compressed_byte_size = page.compressed_byte_size;
    disk.uncompressed_byte_size = page.uncompressed_byte_size;
    disk.first_cluster_index = page.first_cluster_index;
    disk.cluster_count = page.cluster_count;
    disk.first_lod_cluster_index = page.first_lod_cluster_index;
    disk.lod_cluster_count = page.lod_cluster_count;
    disk.dependency_page_start = page.dependency_page_start;
    disk.dependency_page_count = page.dependency_page_count;
    disk.flags = page.flags;
    return disk;
}

LodClusterRecordDisk to_disk(const LodClusterRecord& cluster) {
    LodClusterRecordDisk disk{};
    disk.refined_group_index = cluster.refined_group_index;
    disk.group_index = cluster.group_index;
    disk.local_vertex_count = cluster.local_vertex_count;
    disk.local_triangle_count = cluster.local_triangle_count;
    disk.geometry_payload_offset = cluster.geometry_payload_offset;
    disk.geometry_payload_size = cluster.geometry_payload_size;
    disk.page_index = cluster.page_index;
    disk.bounds = cluster.bounds;
    disk.local_error = cluster.local_error;
    disk.material_section_index = cluster.material_section_index;
    disk.flags = cluster.flags;
    return disk;
}

LodGroupRecordDisk to_disk(const LodGroupRecord& group) {
    LodGroupRecordDisk disk{};
    disk.depth = group.depth;
    disk.first_lod_cluster_index = group.first_lod_cluster_index;
    disk.lod_cluster_count = group.lod_cluster_count;
    disk.material_section_index = group.material_section_index;
    disk.bounds = group.bounds;
    disk.geometric_error = group.geometric_error;
    disk.flags = group.flags;
    return disk;
}

NodeLodLinkDisk to_disk(const NodeLodLink& link) {
    NodeLodLinkDisk disk{};
    disk.lod_group_index = link.lod_group_index;
    return disk;
}

ResourceSummary read_resource_summary(const std::filesystem::path& input_path) {
    std::ifstream input(input_path, std::ios::binary);
    if (!input) {
        throw BuilderError("failed to open input file: " + input_path.string());
    }

    FileHeader header{};
    SummaryBlockDisk summary_disk{};
    input.read(reinterpret_cast<char*>(&header), sizeof(header));
    input.read(reinterpret_cast<char*>(&summary_disk), sizeof(summary_disk));
    if (!input) {
        throw BuilderError("failed to read summary from input file: " + input_path.string());
    }
    if (!std::equal(std::begin(header.magic), std::end(header.magic), kMagic.begin())) {
        throw BuilderError("input file does not have a valid VGEO header: " + input_path.string());
    }

    ResourceSummary summary;
    summary.asset_id = std::string(summary_disk.asset_id);
    summary.source_asset = std::string(summary_disk.source_asset);
    summary.has_fallback = summary_disk.has_fallback != 0;
    summary.source_vertex_count = summary_disk.source_vertex_count;
    summary.source_triangle_count = summary_disk.source_triangle_count;
    summary.bounds = header.bounds;
    summary.material_section_count = header.total_material_sections;
    summary.hierarchy_node_count = header.total_hierarchy_nodes;
    summary.cluster_count = header.total_clusters;
    summary.page_count = header.total_pages;
    summary.lod_group_count = header.total_lod_groups;
    summary.lod_cluster_count = header.total_lod_clusters;
    summary.node_lod_link_count = header.total_node_lod_links;
    summary.page_dependency_count = header.total_page_dependencies;
    summary.cluster_geometry_bytes = header.total_cluster_geometry_bytes;
    summary.lod_geometry_bytes = header.total_lod_geometry_bytes;
    return summary;
}

void write_resource(const VGeoResource& resource, const std::filesystem::path& output_path) {
    if (!output_path.parent_path().empty()) {
        std::filesystem::create_directories(output_path.parent_path());
    }

    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        throw BuilderError("failed to open output file: " + output_path.string());
    }

    const uint64_t metadata_offset = sizeof(FileHeader) + sizeof(SummaryBlockDisk);
    const uint64_t material_table_offset = metadata_offset + sizeof(ResourceMetadata);
    const uint64_t hierarchy_table_offset =
        material_table_offset + (resource.material_sections.size() * sizeof(MaterialSectionDisk));
    const uint64_t page_dependency_table_offset =
        hierarchy_table_offset + (resource.hierarchy_nodes.size() * sizeof(HierarchyNodeDisk));
    const uint64_t node_lod_link_table_offset =
        page_dependency_table_offset + (resource.page_dependencies.size() * sizeof(uint32_t));
    const uint64_t cluster_table_offset =
        node_lod_link_table_offset + (resource.node_lod_links.size() * sizeof(NodeLodLinkDisk));
    const uint64_t page_table_offset =
        cluster_table_offset + (resource.clusters.size() * sizeof(ClusterRecordDisk));
    const uint64_t lod_group_table_offset =
        page_table_offset + (resource.pages.size() * sizeof(PageRecordDisk));
    const uint64_t lod_cluster_table_offset =
        lod_group_table_offset + (resource.lod_groups.size() * sizeof(LodGroupRecordDisk));
    const uint64_t cluster_geometry_payload_offset =
        lod_cluster_table_offset + (resource.lod_clusters.size() * sizeof(LodClusterRecordDisk));
    const uint64_t lod_geometry_payload_offset =
        cluster_geometry_payload_offset + resource.cluster_geometry_payload.size();

    FileHeader header{};
    std::copy(kMagic.begin(), kMagic.end(), std::begin(header.magic));
    header.schema_version = kSchemaVersion;
    header.builder_version = kBuilderVersion;
    header.flags = resource.has_fallback ? 0x1u : 0x0u;
    header.total_material_sections = static_cast<uint32_t>(resource.material_sections.size());
    header.total_hierarchy_nodes = static_cast<uint32_t>(resource.hierarchy_nodes.size());
    header.total_clusters = static_cast<uint32_t>(resource.clusters.size());
    header.total_pages = static_cast<uint32_t>(resource.pages.size());
    header.total_lod_groups = static_cast<uint32_t>(resource.lod_groups.size());
    header.total_lod_clusters = static_cast<uint32_t>(resource.lod_clusters.size());
    header.total_node_lod_links = static_cast<uint32_t>(resource.node_lod_links.size());
    header.total_page_dependencies = static_cast<uint32_t>(resource.page_dependencies.size());
    header.total_cluster_geometry_bytes = static_cast<uint32_t>(resource.cluster_geometry_payload.size());
    header.total_lod_geometry_bytes = static_cast<uint32_t>(resource.lod_geometry_payload.size());
    header.bounds = resource.bounds;
    header.metadata_offset = metadata_offset;
    header.material_table_offset = material_table_offset;
    header.hierarchy_table_offset = hierarchy_table_offset;
    header.page_dependency_table_offset = page_dependency_table_offset;
    header.node_lod_link_table_offset = node_lod_link_table_offset;
    header.cluster_table_offset = cluster_table_offset;
    header.page_table_offset = page_table_offset;
    header.lod_group_table_offset = lod_group_table_offset;
    header.lod_cluster_table_offset = lod_cluster_table_offset;
    header.cluster_geometry_payload_offset = cluster_geometry_payload_offset;
    header.lod_geometry_payload_offset = lod_geometry_payload_offset;

    ResourceMetadata metadata = resource.metadata;
    metadata.material_mapping_offset = material_table_offset;
    metadata.page_table_offset = page_table_offset;
    metadata.page_dependency_table_offset = page_dependency_table_offset;
    metadata.node_lod_link_table_offset = node_lod_link_table_offset;
    metadata.cluster_table_offset = cluster_table_offset;
    metadata.lod_group_table_offset = lod_group_table_offset;
    metadata.lod_cluster_table_offset = lod_cluster_table_offset;
    metadata.cluster_geometry_payload_offset = cluster_geometry_payload_offset;
    metadata.lod_geometry_payload_offset = lod_geometry_payload_offset;

    SummaryBlockDisk summary_disk{};
    const auto asset_id_size = std::min(resource.asset_id.size(), sizeof(summary_disk.asset_id) - 1);
    std::copy_n(resource.asset_id.data(), asset_id_size, summary_disk.asset_id);
    const std::string source_asset = resource.source_asset.string();
    const auto source_asset_size = std::min(source_asset.size(), sizeof(summary_disk.source_asset) - 1);
    std::copy_n(source_asset.data(), source_asset_size, summary_disk.source_asset);
    summary_disk.has_fallback = resource.has_fallback ? 1u : 0u;
    summary_disk.source_vertex_count = resource.source_vertex_count;
    summary_disk.source_triangle_count = resource.source_triangle_count;

    write_pod(output, header);
    write_pod(output, summary_disk);
    write_pod(output, metadata);
    for (const auto& section : resource.material_sections) write_pod(output, to_disk(section));
    for (const auto& node : resource.hierarchy_nodes) write_pod(output, to_disk(node));
    for (const uint32_t dependency_page_index : resource.page_dependencies) write_pod(output, dependency_page_index);
    for (const auto& link : resource.node_lod_links) write_pod(output, to_disk(link));
    for (const auto& cluster : resource.clusters) {
        ClusterRecord adjusted_cluster = cluster;
        adjusted_cluster.geometry_payload_offset += static_cast<uint32_t>(cluster_geometry_payload_offset);
        write_pod(output, to_disk(adjusted_cluster));
    }
    for (const auto& page : resource.pages) {
        PageRecord adjusted_page = page;
        adjusted_page.byte_offset += (page.flags & kPageFlagLodPayload) != 0
                                         ? lod_geometry_payload_offset
                                         : cluster_geometry_payload_offset;
        write_pod(output, to_disk(adjusted_page));
    }
    for (const auto& group : resource.lod_groups) write_pod(output, to_disk(group));
    for (const auto& cluster : resource.lod_clusters) {
        LodClusterRecord adjusted_cluster = cluster;
        adjusted_cluster.geometry_payload_offset += static_cast<uint32_t>(lod_geometry_payload_offset);
        write_pod(output, to_disk(adjusted_cluster));
    }

    if (!resource.cluster_geometry_payload.empty()) {
        output.write(reinterpret_cast<const char*>(resource.cluster_geometry_payload.data()),
                     static_cast<std::streamsize>(resource.cluster_geometry_payload.size()));
    }
    if (!resource.lod_geometry_payload.empty()) {
        output.write(reinterpret_cast<const char*>(resource.lod_geometry_payload.data()),
                     static_cast<std::streamsize>(resource.lod_geometry_payload.size()));
    }
}

void write_summary(const VGeoResource& resource, const std::filesystem::path& output_path) {
    std::ofstream output(output_path);
    if (!output) {
        throw BuilderError("failed to open summary file: " + output_path.string());
    }

    output << "asset_id=" << resource.asset_id << '\n';
    output << "source_asset=" << resource.source_asset.string() << '\n';
    output << "has_fallback=" << (resource.has_fallback ? "true" : "false") << '\n';
    output << "source_vertices=" << resource.source_vertex_count << '\n';
    output << "source_triangles=" << resource.source_triangle_count << '\n';
    output << "seam_locked_vertices=" << resource.seam_locked_vertex_count << '\n';
    output << "bounds_min=" << resource.bounds.min.x << ' ' << resource.bounds.min.y << ' '
           << resource.bounds.min.z << '\n';
    output << "bounds_max=" << resource.bounds.max.x << ' ' << resource.bounds.max.y << ' '
           << resource.bounds.max.z << '\n';
    output << "material_sections=" << resource.material_sections.size() << '\n';
    output << "hierarchy_nodes=" << resource.hierarchy_nodes.size() << '\n';
    output << "clusters=" << resource.clusters.size() << '\n';
    output << "pages=" << resource.pages.size() << '\n';
    output << "lod_groups=" << resource.lod_groups.size() << '\n';
    output << "lod_clusters=" << resource.lod_clusters.size() << '\n';
    output << "node_lod_links=" << resource.node_lod_links.size() << '\n';
    output << "page_dependencies=" << resource.page_dependencies.size() << '\n';
    output << "cluster_geometry_bytes=" << resource.cluster_geometry_payload.size() << '\n';
    output << "lod_geometry_bytes=" << resource.lod_geometry_payload.size() << '\n';

    for (size_t index = 0; index < resource.material_sections.size(); ++index) {
        output << "material[" << index << "]=" << resource.material_sections[index].name << '\n';
    }
    for (size_t index = 0; index < resource.clusters.size(); ++index) {
        const ClusterRecord& cluster = resource.clusters[index];
        output << "cluster[" << index << "]="
               << " tris=" << cluster.local_triangle_count
               << " verts=" << cluster.local_vertex_count
               << " page=" << cluster.page_index
               << " material=" << cluster.material_section_index
               << " payload_offset=" << cluster.geometry_payload_offset
               << " payload_size=" << cluster.geometry_payload_size << '\n';
    }
    for (size_t index = 0; index < resource.pages.size(); ++index) {
        const PageRecord& page = resource.pages[index];
        output << "page[" << index << "]="
               << " kind=" << (((page.flags & kPageFlagLodPayload) != 0) ? "lod" : "base")
               << " first_cluster=" << page.first_cluster_index
               << " cluster_count=" << page.cluster_count
               << " first_lod_cluster=" << page.first_lod_cluster_index
               << " lod_cluster_count=" << page.lod_cluster_count
               << " dep_start=" << page.dependency_page_start
               << " dep_count=" << page.dependency_page_count
               << " byte_offset=" << page.byte_offset
               << " byte_size=" << page.uncompressed_byte_size << '\n';
    }
    for (size_t index = 0; index < resource.page_dependencies.size(); ++index) {
        output << "page_dependency[" << index << "]=" << resource.page_dependencies[index] << '\n';
    }
    for (size_t index = 0; index < resource.hierarchy_nodes.size(); ++index) {
        const HierarchyNode& node = resource.hierarchy_nodes[index];
        output << "node[" << index << "]="
               << " parent=" << node.parent_index
               << " first_child=" << node.first_child_index
               << " child_count=" << node.child_count
               << " first_cluster=" << node.first_cluster_index
               << " cluster_count=" << node.cluster_count
               << " first_lod_link=" << node.first_lod_link_index
               << " lod_link_count=" << node.lod_link_count
               << " min_page=" << node.min_resident_page
               << " max_page=" << node.max_resident_page
               << " error=" << node.geometric_error << '\n';
    }
    for (size_t index = 0; index < resource.node_lod_links.size(); ++index) {
        output << "node_lod_link[" << index << "]="
               << " group=" << resource.node_lod_links[index].lod_group_index << '\n';
    }
    for (size_t index = 0; index < resource.lod_groups.size(); ++index) {
        const LodGroupRecord& group = resource.lod_groups[index];
        output << "lod_group[" << index << "]="
               << " depth=" << group.depth
               << " first_cluster=" << group.first_lod_cluster_index
               << " cluster_count=" << group.lod_cluster_count
               << " material=" << group.material_section_index
               << " error=" << group.geometric_error << '\n';
    }
    for (size_t index = 0; index < resource.lod_clusters.size(); ++index) {
        const LodClusterRecord& cluster = resource.lod_clusters[index];
        output << "lod_cluster[" << index << "]="
               << " group=" << cluster.group_index
               << " refined_group=" << cluster.refined_group_index
               << " tris=" << cluster.local_triangle_count
               << " verts=" << cluster.local_vertex_count
               << " page=" << cluster.page_index
               << " material=" << cluster.material_section_index
               << " error=" << cluster.local_error
               << " payload_offset=" << cluster.geometry_payload_offset
               << " payload_size=" << cluster.geometry_payload_size << '\n';
    }
}

}  // namespace meridian::detail
