#define CGLTF_IMPLEMENTATION
#include "builder_internal.h"

namespace meridian::detail {

std::optional<uint32_t> parse_obj_index_token(std::string_view token, size_t vertex_count) {
    const auto slash = token.find('/');
    const std::string_view vertex_token = token.substr(0, slash);
    if (vertex_token.empty()) {
        return std::nullopt;
    }

    int parsed = 0;
    const auto* begin = vertex_token.data();
    const auto* end = vertex_token.data() + vertex_token.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc() || result.ptr != end || parsed == 0) {
        return std::nullopt;
    }

    if (parsed > 0) {
        return static_cast<uint32_t>(parsed - 1);
    }

    const int resolved = static_cast<int>(vertex_count) + parsed;
    if (resolved < 0) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(resolved);
}

uint32_t resolve_material_slot(const std::unordered_map<std::string, uint32_t>& material_lookup,
                               const std::string& material_name, size_t line_number) {
    const auto found = material_lookup.find(material_name);
    if (found == material_lookup.end()) {
        throw BuilderError("unknown material '" + material_name + "' at line " +
                           std::to_string(line_number));
    }

    return found->second;
}

std::unordered_map<std::string, uint32_t> build_material_lookup(const BuildManifest& manifest) {
    std::unordered_map<std::string, uint32_t> material_lookup;
    material_lookup.reserve(manifest.material_slots.size());
    for (uint32_t material_index = 0; material_index < manifest.material_slots.size(); ++material_index) {
        material_lookup.emplace(manifest.material_slots[material_index], material_index);
    }
    return material_lookup;
}

MeshData make_empty_mesh(const BuildManifest& manifest) {
    MeshData mesh;
    mesh.bounds = make_empty_bounds();
    mesh.sections.resize(manifest.material_slots.size());
    for (uint32_t material_index = 0; material_index < mesh.sections.size(); ++material_index) {
        mesh.sections[material_index].material_section_index = material_index;
    }
    return mesh;
}

Vec3f transform_point(const float matrix[16], const Vec3f& point) {
    return Vec3f{
        matrix[0] * point.x + matrix[4] * point.y + matrix[8] * point.z + matrix[12],
        matrix[1] * point.x + matrix[5] * point.y + matrix[9] * point.z + matrix[13],
        matrix[2] * point.x + matrix[6] * point.y + matrix[10] * point.z + matrix[14],
    };
}

bool nearly_equal(float lhs, float rhs, float epsilon = 1e-5f) {
    return std::fabs(lhs - rhs) <= epsilon;
}

bool same_seam_attributes(const MeshData::VertexSeamAttributes& lhs,
                          const MeshData::VertexSeamAttributes& rhs) {
    if (lhs.has_normal != rhs.has_normal || lhs.has_texcoord0 != rhs.has_texcoord0) {
        return false;
    }
    if (lhs.has_normal &&
        (!nearly_equal(lhs.normal.x, rhs.normal.x) || !nearly_equal(lhs.normal.y, rhs.normal.y) ||
         !nearly_equal(lhs.normal.z, rhs.normal.z))) {
        return false;
    }
    if (lhs.has_texcoord0 &&
        (!nearly_equal(lhs.texcoord0[0], rhs.texcoord0[0]) ||
         !nearly_equal(lhs.texcoord0[1], rhs.texcoord0[1]))) {
        return false;
    }
    return true;
}

cgltf_result decompress_meshopt_buffer_views(cgltf_data* data) {
    for (cgltf_size buffer_view_index = 0; buffer_view_index < data->buffer_views_count;
         ++buffer_view_index) {
        cgltf_buffer_view& buffer_view = data->buffer_views[buffer_view_index];
        if (!buffer_view.has_meshopt_compression) {
            continue;
        }

        cgltf_meshopt_compression* meshopt = &buffer_view.meshopt_compression;
        const unsigned char* source = static_cast<const unsigned char*>(meshopt->buffer->data);
        if (source == nullptr) {
            return cgltf_result_invalid_gltf;
        }
        source += meshopt->offset;

        void* decoded = std::malloc(meshopt->count * meshopt->stride);
        if (decoded == nullptr) {
            return cgltf_result_out_of_memory;
        }
        buffer_view.data = decoded;

        int decode_result = -1;
        switch (meshopt->mode) {
            case cgltf_meshopt_compression_mode_attributes:
                decode_result = meshopt_decodeVertexBuffer(decoded, meshopt->count, meshopt->stride,
                                                           source, meshopt->size);
                break;
            case cgltf_meshopt_compression_mode_triangles:
                decode_result = meshopt_decodeIndexBuffer(decoded, meshopt->count, meshopt->stride,
                                                          source, meshopt->size);
                break;
            case cgltf_meshopt_compression_mode_indices:
                decode_result = meshopt_decodeIndexSequence(decoded, meshopt->count,
                                                            meshopt->stride, source,
                                                            meshopt->size);
                break;
            default:
                return cgltf_result_invalid_gltf;
        }

        if (decode_result != 0) {
            return cgltf_result_io_error;
        }

        switch (meshopt->filter) {
            case cgltf_meshopt_compression_filter_octahedral:
                meshopt_decodeFilterOct(decoded, meshopt->count, meshopt->stride);
                break;
            case cgltf_meshopt_compression_filter_quaternion:
                meshopt_decodeFilterQuat(decoded, meshopt->count, meshopt->stride);
                break;
            case cgltf_meshopt_compression_filter_exponential:
                meshopt_decodeFilterExp(decoded, meshopt->count, meshopt->stride);
                break;
            case cgltf_meshopt_compression_filter_color:
                meshopt_decodeFilterColor(decoded, meshopt->count, meshopt->stride);
                break;
            default:
                break;
        }
    }

    return cgltf_result_success;
}

uint32_t resolve_gltf_material_slot(const cgltf_primitive& primitive,
                                    const cgltf_data& data,
                                    const std::unordered_map<std::string, uint32_t>& material_lookup,
                                    const BuildManifest& manifest) {
    if (primitive.material != nullptr && primitive.material->name != nullptr) {
        const auto found = material_lookup.find(primitive.material->name);
        if (found == material_lookup.end()) {
            throw BuilderError("glTF primitive references unknown material slot: " +
                               std::string(primitive.material->name));
        }
        return found->second;
    }

    if (primitive.material != nullptr && manifest.material_slots.size() == data.materials_count) {
        const cgltf_size material_index = cgltf_material_index(&data, primitive.material);
        if (material_index < manifest.material_slots.size()) {
            return static_cast<uint32_t>(material_index);
        }
    }

    if (manifest.material_slots.size() == 1) {
        return 0;
    }

    throw BuilderError(
        "glTF primitive material could not be mapped; provide matching material names or matching material slot count");
}

const cgltf_accessor* find_attribute_accessor(const cgltf_primitive& primitive,
                                              cgltf_attribute_type attribute_type,
                                              cgltf_int attribute_set_index = 0) {
    for (cgltf_size attribute_offset = 0; attribute_offset < primitive.attributes_count; ++attribute_offset) {
        const cgltf_attribute& attribute = primitive.attributes[attribute_offset];
        if (attribute.type == attribute_type && attribute.index == attribute_set_index &&
            attribute.data != nullptr) {
            return attribute.data;
        }
    }
    return nullptr;
}

void validate_gltf_node(const cgltf_node& node) {
    if (node.skin != nullptr) {
        throw BuilderError("glTF skins are not yet supported by the builder");
    }
    if (node.has_mesh_gpu_instancing) {
        throw BuilderError("glTF EXT_mesh_gpu_instancing is not yet supported by the builder");
    }
}

void validate_gltf_primitive(const cgltf_primitive& primitive) {
    if (primitive.type != cgltf_primitive_type_triangles) {
        throw BuilderError("glTF import currently supports triangle primitives only");
    }
    if (primitive.has_draco_mesh_compression) {
        throw BuilderError("glTF Draco mesh compression is not yet supported by the builder");
    }
    if (primitive.targets_count != 0) {
        throw BuilderError("glTF morph targets are not yet supported by the builder");
    }

    const cgltf_accessor* positions = find_attribute_accessor(primitive, cgltf_attribute_type_position);
    if (positions == nullptr || positions->type != cgltf_type_vec3) {
        throw BuilderError("glTF primitive is missing a vec3 POSITION accessor");
    }

    const cgltf_accessor* normals = find_attribute_accessor(primitive, cgltf_attribute_type_normal);
    if (normals != nullptr && normals->count != positions->count) {
        throw BuilderError("glTF NORMAL accessor count must match POSITION count");
    }

    const cgltf_accessor* texcoords = find_attribute_accessor(primitive, cgltf_attribute_type_texcoord, 0);
    if (texcoords != nullptr && texcoords->count != positions->count) {
        throw BuilderError("glTF TEXCOORD_0 accessor count must match POSITION count");
    }
}

void append_gltf_primitive(MeshData& mesh, const cgltf_primitive& primitive, const float world_matrix[16],
                           uint32_t material_section_index) {
    validate_gltf_primitive(primitive);
    const cgltf_accessor* positions = find_attribute_accessor(primitive, cgltf_attribute_type_position);
    const cgltf_accessor* normals = find_attribute_accessor(primitive, cgltf_attribute_type_normal);
    const cgltf_accessor* texcoords = find_attribute_accessor(primitive, cgltf_attribute_type_texcoord, 0);

    const uint32_t base_vertex_index = static_cast<uint32_t>(mesh.positions.size());
    std::vector<float> unpacked_positions(positions->count * 3, 0.0f);
    if (cgltf_accessor_unpack_floats(positions, unpacked_positions.data(), unpacked_positions.size()) !=
        unpacked_positions.size()) {
        throw BuilderError("failed to unpack glTF POSITION accessor");
    }

    std::vector<float> unpacked_normals;
    if (normals != nullptr) {
        unpacked_normals.resize(normals->count * 3, 0.0f);
        if (cgltf_accessor_unpack_floats(normals, unpacked_normals.data(), unpacked_normals.size()) !=
            unpacked_normals.size()) {
            throw BuilderError("failed to unpack glTF NORMAL accessor");
        }
    }

    std::vector<float> unpacked_texcoords;
    if (texcoords != nullptr) {
        unpacked_texcoords.resize(texcoords->count * 2, 0.0f);
        if (cgltf_accessor_unpack_floats(texcoords, unpacked_texcoords.data(), unpacked_texcoords.size()) !=
            unpacked_texcoords.size()) {
            throw BuilderError("failed to unpack glTF TEXCOORD_0 accessor");
        }
    }

    mesh.positions.reserve(mesh.positions.size() + positions->count);
    mesh.seam_attributes.reserve(mesh.seam_attributes.size() + positions->count);
    for (cgltf_size vertex_index = 0; vertex_index < positions->count; ++vertex_index) {
        const Vec3f transformed = transform_point(
            world_matrix,
            Vec3f{unpacked_positions[vertex_index * 3 + 0], unpacked_positions[vertex_index * 3 + 1],
                  unpacked_positions[vertex_index * 3 + 2]});
        mesh.positions.push_back(transformed);
        update_bounds(mesh.bounds, transformed);

        MeshData::VertexSeamAttributes attributes;
        if (!unpacked_normals.empty()) {
            attributes.has_normal = true;
            attributes.normal = Vec3f{unpacked_normals[vertex_index * 3 + 0],
                                      unpacked_normals[vertex_index * 3 + 1],
                                      unpacked_normals[vertex_index * 3 + 2]};
        }
        if (!unpacked_texcoords.empty()) {
            attributes.has_texcoord0 = true;
            attributes.texcoord0[0] = unpacked_texcoords[vertex_index * 2 + 0];
            attributes.texcoord0[1] = unpacked_texcoords[vertex_index * 2 + 1];
        }
        mesh.seam_attributes.push_back(attributes);
    }

    MeshSection& section = mesh.sections[material_section_index];
    if (primitive.indices != nullptr) {
        if (primitive.indices->count % 3 != 0) {
            throw BuilderError("glTF triangle index accessor count must be divisible by 3");
        }
        const size_t previous_index_count = section.indices.size();
        section.indices.resize(previous_index_count + primitive.indices->count);
        if (cgltf_accessor_unpack_indices(primitive.indices, section.indices.data() + previous_index_count,
                                          sizeof(uint32_t), primitive.indices->count) !=
            primitive.indices->count) {
            throw BuilderError("failed to unpack glTF index accessor");
        }
        for (size_t index = previous_index_count; index < section.indices.size(); ++index) {
            section.indices[index] += base_vertex_index;
        }
    } else {
        if (positions->count % 3 != 0) {
            throw BuilderError("glTF non-indexed triangle primitive vertex count must be divisible by 3");
        }
        for (cgltf_size index = 0; index < positions->count; ++index) {
            section.indices.push_back(base_vertex_index + static_cast<uint32_t>(index));
        }
    }
}

std::vector<unsigned char> build_vertex_locks(const MeshData& mesh) {
    std::vector<unsigned char> vertex_locks(mesh.positions.size(), 0);
    if (mesh.positions.empty()) {
        return vertex_locks;
    }

    std::vector<unsigned int> position_remap(mesh.positions.size());
    meshopt_generatePositionRemap(position_remap.data(), reinterpret_cast<const float*>(mesh.positions.data()),
                                 mesh.positions.size(), sizeof(Vec3f));

    std::vector<uint32_t> canonical_first_material(mesh.positions.size(), 0xffffffffu);
    std::vector<uint8_t> canonical_is_seam(mesh.positions.size(), 0);
    std::vector<MeshData::VertexSeamAttributes> canonical_first_attributes(mesh.positions.size());
    std::vector<uint8_t> canonical_has_attributes(mesh.positions.size(), 0);

    for (const MeshSection& section : mesh.sections) {
        for (const uint32_t vertex_index : section.indices) {
            const uint32_t canonical_index = position_remap[vertex_index];
            uint32_t& first_material = canonical_first_material[canonical_index];
            if (first_material == 0xffffffffu) {
                first_material = section.material_section_index;
            } else if (first_material != section.material_section_index) {
                canonical_is_seam[canonical_index] = 1;
            }

            if (vertex_index < mesh.seam_attributes.size()) {
                if (!canonical_has_attributes[canonical_index]) {
                    canonical_first_attributes[canonical_index] = mesh.seam_attributes[vertex_index];
                    canonical_has_attributes[canonical_index] = 1;
                } else if (!same_seam_attributes(canonical_first_attributes[canonical_index],
                                                 mesh.seam_attributes[vertex_index])) {
                    canonical_is_seam[canonical_index] = 1;
                }
            }
        }
    }

    for (uint32_t vertex_index = 0; vertex_index < vertex_locks.size(); ++vertex_index) {
        if (canonical_is_seam[position_remap[vertex_index]]) {
            vertex_locks[vertex_index] = meshopt_SimplifyVertex_Lock;
        }
    }

    return vertex_locks;
}

MeshData load_obj_mesh(const BuildManifest& manifest) {
    const std::filesystem::path& source_asset = manifest.source_asset;
    std::ifstream input(source_asset);
    if (!input) {
        throw BuilderError("failed to open source asset: " + source_asset.string());
    }

    MeshData mesh = make_empty_mesh(manifest);
    std::unordered_map<std::string, uint32_t> material_lookup = build_material_lookup(manifest);

    uint32_t active_material_index = 0;
    bool has_active_material = manifest.material_slots.size() == 1;

    std::string line;
    size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed.starts_with('#')) {
            continue;
        }

        std::istringstream stream(trimmed);
        std::string keyword;
        stream >> keyword;

        if (keyword == "v") {
            Vec3f position;
            if (!(stream >> position.x >> position.y >> position.z)) {
                throw BuilderError("invalid vertex at line " + std::to_string(line_number));
            }
            mesh.positions.push_back(position);
            update_bounds(mesh.bounds, position);
            continue;
        }

        if (keyword == "usemtl") {
            std::string material_name;
            if (!(stream >> material_name)) {
                throw BuilderError("missing material name at line " + std::to_string(line_number));
            }
            active_material_index = resolve_material_slot(material_lookup, material_name, line_number);
            has_active_material = true;
            continue;
        }

        if (keyword != "f") {
            continue;
        }

        if (!has_active_material) {
            throw BuilderError(
                "face uses no active material; add usemtl entries or reduce manifest material_slots");
        }

        std::vector<uint32_t> face_indices;
        std::string token;
        while (stream >> token) {
            const auto parsed = parse_obj_index_token(token, mesh.positions.size());
            if (!parsed.has_value() || *parsed >= mesh.positions.size()) {
                throw BuilderError("invalid face index at line " + std::to_string(line_number));
            }
            face_indices.push_back(*parsed);
        }

        if (face_indices.size() < 3) {
            throw BuilderError("face with fewer than 3 vertices at line " +
                               std::to_string(line_number));
        }

        for (size_t i = 1; i + 1 < face_indices.size(); ++i) {
            MeshSection& section = mesh.sections[active_material_index];
            section.indices.push_back(face_indices[0]);
            section.indices.push_back(face_indices[i]);
            section.indices.push_back(face_indices[i + 1]);
        }
    }

    size_t total_index_count = 0;
    for (const MeshSection& section : mesh.sections) {
        total_index_count += section.indices.size();
    }

    if (mesh.positions.empty() || total_index_count == 0) {
        throw BuilderError("source asset did not produce any triangles: " + source_asset.string());
    }

    mesh.vertex_locks = build_vertex_locks(mesh);
    return mesh;
}

MeshData load_gltf_mesh(const BuildManifest& manifest) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    const std::filesystem::path& source_asset = manifest.source_asset;

    const cgltf_result parse_result = cgltf_parse_file(&options, source_asset.string().c_str(), &data);
    if (parse_result != cgltf_result_success || data == nullptr) {
        throw BuilderError("failed to parse glTF asset: " + source_asset.string());
    }

    const auto free_data = [&]() {
        if (data != nullptr) {
            cgltf_free(data);
            data = nullptr;
        }
    };

    if (cgltf_load_buffers(&options, data, source_asset.string().c_str()) != cgltf_result_success) {
        free_data();
        throw BuilderError("failed to load glTF buffers: " + source_asset.string());
    }
    if (cgltf_validate(data) != cgltf_result_success) {
        free_data();
        throw BuilderError("glTF validation failed: " + source_asset.string());
    }
    if (decompress_meshopt_buffer_views(data) != cgltf_result_success) {
        free_data();
        throw BuilderError("failed to decode EXT_meshopt_compression buffer views: " +
                           source_asset.string());
    }

    MeshData mesh = make_empty_mesh(manifest);
    const std::unordered_map<std::string, uint32_t> material_lookup = build_material_lookup(manifest);

    const auto visit_node = [&](const auto& self, const cgltf_node* node) -> void {
        validate_gltf_node(*node);
        if (node->mesh != nullptr) {
            float world_matrix[16] = {};
            cgltf_node_transform_world(node, world_matrix);
            for (cgltf_size primitive_index = 0; primitive_index < node->mesh->primitives_count; ++primitive_index) {
                const cgltf_primitive& primitive = node->mesh->primitives[primitive_index];
                const uint32_t material_section_index =
                    resolve_gltf_material_slot(primitive, *data, material_lookup, manifest);
                append_gltf_primitive(mesh, primitive, world_matrix, material_section_index);
            }
        }

        for (cgltf_size child_index = 0; child_index < node->children_count; ++child_index) {
            self(self, node->children[child_index]);
        }
    };

    if (data->scene != nullptr) {
        for (cgltf_size root_index = 0; root_index < data->scene->nodes_count; ++root_index) {
            visit_node(visit_node, data->scene->nodes[root_index]);
        }
    } else {
        for (cgltf_size node_index = 0; node_index < data->nodes_count; ++node_index) {
            if (data->nodes[node_index].parent == nullptr) {
                visit_node(visit_node, &data->nodes[node_index]);
            }
        }
    }

    free_data();

    size_t total_index_count = 0;
    for (const MeshSection& section : mesh.sections) {
        total_index_count += section.indices.size();
    }

    if (mesh.positions.empty() || total_index_count == 0) {
        throw BuilderError("glTF asset did not produce any triangles: " + source_asset.string());
    }

    mesh.vertex_locks = build_vertex_locks(mesh);
    return mesh;
}

MeshData load_mesh(const BuildManifest& manifest) {
    const std::string extension = manifest.source_asset.extension().string();
    if (extension == ".obj" || extension == ".OBJ") {
        return load_obj_mesh(manifest);
    }
    if (extension == ".gltf" || extension == ".GLTF" || extension == ".glb" || extension == ".GLB") {
        return load_gltf_mesh(manifest);
    }

    throw BuilderError("unsupported source asset format: " + extension +
                       " (first-pass builder supports .obj, .gltf, and .glb)");
}

}  // namespace meridian::detail
