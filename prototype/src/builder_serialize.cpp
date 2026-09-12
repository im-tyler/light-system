#include "builder_internal.h"

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>

namespace meridian::detail {

namespace {

// Fixed char arrays are not guaranteed NUL-terminated on disk; construct
// bounded and reject an unterminated field instead of reading past it.
std::string fixed_char_field(const char* field, size_t size, const char* field_name) {
    const size_t length = strnlen(field, size);
    if (length == size) {
        throw BuilderError(std::string(field_name) + " is not NUL-terminated");
    }
    return std::string(field, length);
}

// Crash-persistent publication (POSIX, macOS/Linux clean): after a file is
// renamed into place, fsync the containing directory so the directory
// entry itself survives a crash. The file is already fsynced by then; an
// unfsynced rename can lose the published name.
void fsync_parent_directory(const std::filesystem::path& published_path) {
    const std::filesystem::path parent = published_path.parent_path().empty()
                                              ? std::filesystem::path(".")
                                              : published_path.parent_path();
    const int dir_fd = ::open(parent.c_str(), O_RDONLY);
    if (dir_fd < 0) {
        throw BuilderError("failed to open output directory for fsync: " + parent.string());
    }
    const int dir_fsync_result = ::fsync(dir_fd);
    ::close(dir_fd);
    if (dir_fsync_result != 0) {
        throw BuilderError("failed to fsync output directory: " + parent.string());
    }
}

// ONE canonical list of the summary sidecar's scalar metadata fields (the
// LS-23 fix family). write_summary emits exactly these keys in this order,
// and compute_content_fingerprint mixes every key and canonical value into
// the .vgeo fingerprint -- both consume this single table, so the sidecar
// writer, the fingerprint, and (by key name) the Godot importer
// (addons/meridian_importer/vgeo_import_plugin.gd) cannot drift apart
// again: a published sidecar field that the fingerprint does not cover
// cannot exist, because there is no second list to forget to update.
//
// Canonical value forms (what the fingerprint hashes, independent of the
// sidecar's lossy 6-significant-digit float text): u32/count/size/payload
// fields and booleans hash as one u64 (a); text fields hash the length
// (a) plus the bytes; bounds triples hash the three raw float bit
// patterns (a, b, c). Fields that are not triples leave b/c at 0.
// content_fingerprint itself is deliberately absent: it is the digest of
// this table and cannot be an input to itself.
struct SidecarField {
    const char* key;
    uint64_t a = 0;
    uint64_t b = 0;
    uint64_t c = 0;
    std::string text;
};

uint64_t float_to_bits(float value) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float must be 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::string render_vec3(const Vec3f& value) {
    std::ostringstream stream;
    stream << value.x << ' ' << value.y << ' ' << value.z;
    return stream.str();
}

uint32_t count_uv_clusters(const std::vector<ClusterRecord>& clusters) {
    uint32_t count = 0;
    for (const ClusterRecord& cluster : clusters) {
        if ((cluster.flags & kClusterFlagHasUv) != 0) {
            count += 1;
        }
    }
    return count;
}

uint32_t count_uv_lod_clusters(const std::vector<LodClusterRecord>& clusters) {
    uint32_t count = 0;
    for (const LodClusterRecord& cluster : clusters) {
        if ((cluster.flags & kClusterFlagHasUv) != 0) {
            count += 1;
        }
    }
    return count;
}

std::vector<SidecarField> collect_sidecar_fields(const VGeoResource& resource) {
    std::vector<SidecarField> fields;
    const auto text_field = [&fields](const char* key, const std::string& value) {
        SidecarField field;
        field.key = key;
        field.a = value.size();
        field.text = value;
        fields.push_back(std::move(field));
    };
    const auto u32_field = [&fields](const char* key, uint32_t value) {
        SidecarField field;
        field.key = key;
        field.a = value;
        field.text = std::to_string(value);
        fields.push_back(std::move(field));
    };
    const auto size_field = [&u32_field](const char* key, size_t value) {
        u32_field(key, narrow_payload_u32(value, key));
    };
    const auto bool_field = [&fields](const char* key, bool value) {
        SidecarField field;
        field.key = key;
        field.a = value ? 1ull : 0ull;
        field.text = value ? "true" : "false";
        fields.push_back(std::move(field));
    };
    const auto vec3_field = [&fields](const char* key, const Vec3f& value) {
        SidecarField field;
        field.key = key;
        field.a = float_to_bits(value.x);
        field.b = float_to_bits(value.y);
        field.c = float_to_bits(value.z);
        field.text = render_vec3(value);
        fields.push_back(std::move(field));
    };

    text_field("asset_id", resource.asset_id);
    text_field("source_asset", resource.source_asset.string());
    bool_field("has_fallback", resource.has_fallback);
    u32_field("source_vertices", resource.source_vertex_count);
    u32_field("source_triangles", resource.source_triangle_count);
    u32_field("seam_locked_vertices", resource.seam_locked_vertex_count);
    vec3_field("bounds_min", resource.bounds.min);
    vec3_field("bounds_max", resource.bounds.max);
    size_field("material_sections", resource.material_sections.size());
    size_field("hierarchy_nodes", resource.hierarchy_nodes.size());
    size_field("clusters", resource.clusters.size());
    size_field("pages", resource.pages.size());
    size_field("lod_groups", resource.lod_groups.size());
    size_field("lod_clusters", resource.lod_clusters.size());
    size_field("node_lod_links", resource.node_lod_links.size());
    size_field("page_dependencies", resource.page_dependencies.size());
    size_field("cluster_geometry_bytes", resource.cluster_geometry_payload.size());
    size_field("lod_geometry_bytes", resource.lod_geometry_payload.size());
    text_field("texture", resource.texture_payload.empty() ? "none" : "checker");
    u32_field("texture_width", resource.texture_width);
    u32_field("texture_height", resource.texture_height);
    size_field("texture_bytes", resource.texture_payload.size());
    u32_field("uv_clusters", count_uv_clusters(resource.clusters));
    u32_field("uv_lod_clusters", count_uv_lod_clusters(resource.lod_clusters));
    return fields;
}

}  // namespace

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
    std::copy(std::begin(cluster.cull_sphere), std::end(cluster.cull_sphere),
              std::begin(disk.cull_sphere));
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
    std::copy(std::begin(cluster.normal_cone_axis), std::end(cluster.normal_cone_axis),
              std::begin(disk.normal_cone_axis));
    std::copy(std::begin(cluster.cull_sphere), std::end(cluster.cull_sphere),
              std::begin(disk.cull_sphere));
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
    disk.first_base_run_index = group.first_base_run_index;
    disk.base_run_count = group.base_run_count;
    return disk;
}

NodeLodLinkDisk to_disk(const NodeLodLink& link) {
    NodeLodLinkDisk disk{};
    disk.lod_group_index = link.lod_group_index;
    return disk;
}

LodGroupBaseRunDisk to_disk(const LodGroupBaseRun& run) {
    LodGroupBaseRunDisk disk{};
    disk.first_cluster_index = run.first_cluster_index;
    disk.cluster_count = run.cluster_count;
    return disk;
}

ResourceSummary read_resource_summary(const std::filesystem::path& input_path) {
    std::ifstream input(input_path, std::ios::binary);
    if (!input) {
        throw BuilderError("failed to open input file: " + input_path.string());
    }

    // Read only the fixed prefix (magic + schema_version) first: older
    // schema versions have smaller headers, so reading sizeof(FileHeader)
    // up front would misalign on anything but the current version. Only
    // the current version is decodable; legacy versions are rejected.
    FileHeader header{};
    SummaryBlockDisk summary_disk{};
    input.read(reinterpret_cast<char*>(&header), 8);
    if (!input) {
        throw BuilderError("failed to read summary from input file: " + input_path.string());
    }
    if (!std::equal(std::begin(header.magic), std::end(header.magic), kMagic.begin())) {
        throw BuilderError("input file does not have a valid VGEO header: " + input_path.string());
    }
    if (header.schema_version != kSchemaVersion) {
        throw BuilderError("unsupported VGEO schema version " +
                           std::to_string(header.schema_version) + " (supported: " +
                           std::to_string(kSchemaVersion) + "): " + input_path.string());
    }
    input.read(reinterpret_cast<char*>(&header) + 8, sizeof(header) - 8);
    input.read(reinterpret_cast<char*>(&summary_disk), sizeof(summary_disk));
    if (!input) {
        throw BuilderError("failed to read summary from input file: " + input_path.string());
    }
    const bool file_is_textured = (header.flags & kFileFlagTextured) != 0;

    ResourceSummary summary;
    summary.asset_id = fixed_char_field(summary_disk.asset_id, sizeof(summary_disk.asset_id),
                                        "asset_id");
    summary.source_asset = fixed_char_field(summary_disk.source_asset,
                                            sizeof(summary_disk.source_asset), "source_asset");
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
    if (file_is_textured) {
        summary.texture_bytes = header.total_texture_bytes;
        ResourceMetadata metadata{};
        input.seekg(static_cast<std::streamoff>(header.metadata_offset), std::ios::beg);
        input.read(reinterpret_cast<char*>(&metadata), sizeof(metadata));
        if (input) {
            summary.texture_width = metadata.texture_width;
            summary.texture_height = metadata.texture_height;
        }
    }
    return summary;
}

// 64-bit FNV-1a over the canonical sidecar field table (every scalar
// metadata field the summary publishes -- see collect_sidecar_fields), the
// page layout, the base-run count, and the payload bytes (fields hashed
// individually: PageRecord contains alignment padding between page_index
// and byte_offset that would leak uninitialized bytes into a raw-struct
// hash). Hashing the field table is what ties the fingerprint to the
// sidecar: a rebuild that changed only sidecar metadata (e.g. only
// seam_locked_vertices) produces a different fingerprint, so a stale
// sidecar can never pair with a new .vgeo. The header's table offsets are
// pure functions of the totals and payload sizes hashed here (see the
// layout math in write_resource) and are not hashed separately: the
// viewer's rebuild comparison computes this from an in-memory resource
// that has not derived its offsets yet. Changing this input set changes
// every fingerprint; pre-change .vgeo+sidecar pairs then fail the pairing
// check on purpose (regeneration required).
uint64_t compute_content_fingerprint(const VGeoResource& resource) {
    uint64_t hash = 1469598103934665603ull;
    const auto mix_bytes = [&hash](const std::byte* data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            hash ^= static_cast<uint64_t>(data[i]);
            hash *= 1099511628211ull;
        }
    };
    const auto mix_u64 = [&hash](uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            hash ^= (value >> (i * 8)) & 0xffull;
            hash *= 1099511628211ull;
        }
    };
    const auto mix_string = [&mix_bytes, &mix_u64](const std::string& value) {
        mix_u64(value.size());
        if (!value.empty()) {
            mix_bytes(reinterpret_cast<const std::byte*>(value.data()), value.size());
        }
    };
    const auto mix_payload = [&](const std::vector<std::byte>& payload) {
        mix_u64(payload.size());
        if (!payload.empty()) {
            mix_bytes(payload.data(), payload.size());
        }
    };
    for (const SidecarField& field : collect_sidecar_fields(resource)) {
        mix_string(field.key);
        mix_u64(field.a);
        mix_u64(field.b);
        mix_u64(field.c);
        if (!field.text.empty()) {
            mix_bytes(reinterpret_cast<const std::byte*>(field.text.data()), field.text.size());
        }
    }
    // Layout inputs beyond the sidecar table: the base-run table has no
    // sidecar scalar, and the page records + payload bytes are the content
    // the sidecar's counts summarize.
    mix_u64(resource.lod_group_base_runs.size());
    for (const PageRecord& page : resource.pages) {
        mix_u64(page.page_index);
        mix_u64(page.byte_offset);
        mix_u64(page.compressed_byte_size);
        mix_u64(page.uncompressed_byte_size);
        mix_u64(page.first_cluster_index);
        mix_u64(page.cluster_count);
        mix_u64(page.first_lod_cluster_index);
        mix_u64(page.lod_cluster_count);
        mix_u64(page.dependency_page_start);
        mix_u64(page.dependency_page_count);
        mix_u64(page.flags);
    }
    mix_payload(resource.cluster_geometry_payload);
    mix_payload(resource.lod_geometry_payload);
    mix_payload(resource.texture_payload);
    return hash;
}

void write_resource(const VGeoResource& resource, const std::filesystem::path& output_path) {
    if (!output_path.parent_path().empty()) {
        std::filesystem::create_directories(output_path.parent_path());
    }

    // Publish by atomic rename, never by truncating the destination in
    // place: a concurrent reader (the viewer mmaps the published .vgeo and
    // streams pages from it) holding the old file keeps seeing the complete
    // old generation -- an in-place truncate hands it a shrinking file whose
    // header is gone mid-read. Generations are immutable once published.
    const std::filesystem::path temp_path =
        output_path.parent_path() /
        (output_path.filename().string() + ".tmp-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::ofstream output(temp_path, std::ios::binary);
    if (!output) {
        throw BuilderError("failed to open output file: " + temp_path.string());
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
    const uint64_t lod_group_base_run_table_offset =
        lod_cluster_table_offset + (resource.lod_clusters.size() * sizeof(LodClusterRecordDisk));
    const uint64_t cluster_geometry_payload_offset =
        lod_group_base_run_table_offset +
        (resource.lod_group_base_runs.size() * sizeof(LodGroupBaseRunDisk));
    const uint64_t lod_geometry_payload_offset =
        cluster_geometry_payload_offset + resource.cluster_geometry_payload.size();
    const uint64_t texture_payload_offset =
        lod_geometry_payload_offset + resource.lod_geometry_payload.size();

    FileHeader header{};
    std::copy(kMagic.begin(), kMagic.end(), std::begin(header.magic));
    header.schema_version = kSchemaVersion;
    header.builder_version = kBuilderVersion;
    header.flags = (resource.has_fallback ? kFileFlagHasFallback : 0x0u) |
                   (!resource.texture_payload.empty() ? kFileFlagTextured : 0x0u);
    header.total_material_sections = static_cast<uint32_t>(resource.material_sections.size());
    header.total_hierarchy_nodes = static_cast<uint32_t>(resource.hierarchy_nodes.size());
    header.total_clusters = static_cast<uint32_t>(resource.clusters.size());
    header.total_pages = static_cast<uint32_t>(resource.pages.size());
    header.total_lod_groups = static_cast<uint32_t>(resource.lod_groups.size());
    header.total_lod_clusters = static_cast<uint32_t>(resource.lod_clusters.size());
    header.total_node_lod_links = static_cast<uint32_t>(resource.node_lod_links.size());
    header.total_page_dependencies = static_cast<uint32_t>(resource.page_dependencies.size());
    header.total_cluster_geometry_bytes =
        narrow_payload_u32(resource.cluster_geometry_payload.size(), "geometry payload");
    header.total_lod_geometry_bytes =
        narrow_payload_u32(resource.lod_geometry_payload.size(), "LOD geometry payload");
    header.total_lod_group_base_runs = static_cast<uint32_t>(resource.lod_group_base_runs.size());
    header.total_texture_bytes = narrow_payload_u32(resource.texture_payload.size(), "texture payload");
    // Cluster payload offsets are 32-bit in the schema: the payload bases
    // plus each payload's own extent must stay addressable, otherwise the
    // per-cluster adjusted offsets below would wrap.
    if (cluster_geometry_payload_offset + resource.cluster_geometry_payload.size() >
        0xffffffffull) {
        throw BuilderError("geometry payload exceeds 32-bit offset limit");
    }
    if (lod_geometry_payload_offset + resource.lod_geometry_payload.size() > 0xffffffffull) {
        throw BuilderError("LOD geometry payload exceeds 32-bit offset limit");
    }
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
    header.lod_group_base_run_table_offset = lod_group_base_run_table_offset;
    header.cluster_geometry_payload_offset = cluster_geometry_payload_offset;
    header.lod_geometry_payload_offset = lod_geometry_payload_offset;
    header.content_fingerprint = compute_content_fingerprint(resource);

    ResourceMetadata metadata = resource.metadata;
    metadata.material_mapping_offset = material_table_offset;
    metadata.page_table_offset = page_table_offset;
    metadata.page_dependency_table_offset = page_dependency_table_offset;
    metadata.node_lod_link_table_offset = node_lod_link_table_offset;
    metadata.cluster_table_offset = cluster_table_offset;
    metadata.lod_group_table_offset = lod_group_table_offset;
    metadata.lod_cluster_table_offset = lod_cluster_table_offset;
    metadata.lod_group_base_run_table_offset = lod_group_base_run_table_offset;
    metadata.cluster_geometry_payload_offset = cluster_geometry_payload_offset;
    metadata.lod_geometry_payload_offset = lod_geometry_payload_offset;
    metadata.texture_payload_offset = texture_payload_offset;
    metadata.texture_width = resource.texture_width;
    metadata.texture_height = resource.texture_height;

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
        adjusted_cluster.geometry_payload_offset =
            narrow_payload_u32(cluster_geometry_payload_offset + cluster.geometry_payload_offset,
                               "geometry payload");
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
        adjusted_cluster.geometry_payload_offset =
            narrow_payload_u32(lod_geometry_payload_offset + cluster.geometry_payload_offset,
                               "LOD geometry payload");
        write_pod(output, to_disk(adjusted_cluster));
    }
    for (const auto& run : resource.lod_group_base_runs) write_pod(output, to_disk(run));

    if (!resource.cluster_geometry_payload.empty()) {
        output.write(reinterpret_cast<const char*>(resource.cluster_geometry_payload.data()),
                     static_cast<std::streamsize>(resource.cluster_geometry_payload.size()));
    }
    if (!resource.lod_geometry_payload.empty()) {
        output.write(reinterpret_cast<const char*>(resource.lod_geometry_payload.data()),
                     static_cast<std::streamsize>(resource.lod_geometry_payload.size()));
    }
    if (!resource.texture_payload.empty()) {
        output.write(reinterpret_cast<const char*>(resource.texture_payload.data()),
                     static_cast<std::streamsize>(resource.texture_payload.size()));
    }
    output.close();
    if (!output) {
        std::error_code remove_ec;
        std::filesystem::remove(temp_path, remove_ec);
        throw BuilderError("failed to write output file: " + temp_path.string());
    }
    // Durability before visibility: fsync the temp file so the published
    // name never resolves to a generation a crash could truncate, then
    // rename() over the destination (atomic replace on POSIX), then fsync
    // the parent directory so the rename itself is crash-persistent. A
    // failed reopen or fsync is a write failure -- never rename a file
    // whose bytes may not be on disk.
    const int fd = ::open(temp_path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::error_code remove_ec;
        std::filesystem::remove(temp_path, remove_ec);
        throw BuilderError("failed to reopen output file for fsync: " + temp_path.string());
    }
    const int fsync_result = ::fsync(fd);
    ::close(fd);
    if (fsync_result != 0) {
        std::error_code remove_ec;
        std::filesystem::remove(temp_path, remove_ec);
        throw BuilderError("failed to fsync output file: " + temp_path.string());
    }
    if (::rename(temp_path.c_str(), output_path.c_str()) != 0) {
        std::error_code remove_ec;
        std::filesystem::remove(temp_path, remove_ec);
        throw BuilderError("failed to publish output file: " + output_path.string());
    }
    fsync_parent_directory(output_path);
}

void write_summary(const VGeoResource& resource, const std::filesystem::path& output_path) {
    // Publish by atomic rename like the .vgeo itself (write_resource): the
    // summary is the Godot importer's sidecar, and overwriting it in place
    // can hand a concurrent reader a half-written file. The content
    // fingerprint ties the sidecar to exactly this .vgeo generation, so the
    // importer can reject a stale summary instead of defaulting its fields.
    const std::filesystem::path temp_path =
        output_path.parent_path() /
        (output_path.filename().string() + ".tmp-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::ofstream output(temp_path);
    if (!output) {
        throw BuilderError("failed to open summary file: " + temp_path.string());
    }

    // The scalar metadata section comes from the same canonical field
    // table the fingerprint hashes (collect_sidecar_fields); the pairing
    // key is inserted at its historical position right after source_asset.
    const std::vector<SidecarField> sidecar_fields = collect_sidecar_fields(resource);
    for (const SidecarField& field : sidecar_fields) {
        output << field.key << '=' << field.text << '\n';
        if (std::string_view(field.key) == "source_asset") {
            output << "content_fingerprint=" << compute_content_fingerprint(resource) << '\n';
        }
    }

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
        const NodeLodLink& link = resource.node_lod_links[index];
        output << "node_lod_link[" << index << "]=" << " group=" << link.lod_group_index << '\n';
    }
    for (size_t index = 0; index < resource.lod_group_base_runs.size(); ++index) {
        const LodGroupBaseRun& run = resource.lod_group_base_runs[index];
        output << "lod_group_base_run[" << index << "]="
               << " first_cluster=" << run.first_cluster_index
               << " cluster_count=" << run.cluster_count << '\n';
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

    output.close();
    if (!output) {
        std::error_code remove_ec;
        std::filesystem::remove(temp_path, remove_ec);
        throw BuilderError("failed to write summary file: " + temp_path.string());
    }
    // Same publication discipline as the .vgeo itself (write_resource):
    // fsync the temp file, rename, then fsync the parent directory.
    const int fd = ::open(temp_path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::error_code remove_ec;
        std::filesystem::remove(temp_path, remove_ec);
        throw BuilderError("failed to reopen summary file for fsync: " + temp_path.string());
    }
    const int fsync_result = ::fsync(fd);
    ::close(fd);
    if (fsync_result != 0) {
        std::error_code remove_ec;
        std::filesystem::remove(temp_path, remove_ec);
        throw BuilderError("failed to fsync summary file: " + temp_path.string());
    }
    if (::rename(temp_path.c_str(), output_path.c_str()) != 0) {
        std::error_code remove_ec;
        std::filesystem::remove(temp_path, remove_ec);
        throw BuilderError("failed to publish summary file: " + output_path.string());
    }
    fsync_parent_directory(output_path);
}

}  // namespace meridian::detail
