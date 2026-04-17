#version 450

layout(set = 0, binding = 2) uniform FrameData {
    mat4 view_projection;
    mat4 light_vp[3];
    vec4 light_dir;
    vec4 cascade_splits;
} frame;

layout(set = 0, binding = 0) readonly buffer BasePayload { uint base_data[]; };
layout(set = 0, binding = 1) readonly buffer LodPayload { uint lod_data[]; };

struct DrawEntry {
    uint draw_vertex_count;
    uint draw_instance_count;
    uint draw_first_vertex;
    uint draw_first_instance;
    uint cluster_index;
    uint geometry_kind;
    uint payload_offset;
    uint local_vertex_count;
};
layout(set = 0, binding = 4) readonly buffer DrawList { DrawEntry draws[]; };

layout(location = 0) out vec3 frag_normal;
layout(location = 1) flat out uint frag_geometry_index;
layout(location = 2) flat out uint frag_geometry_kind;
layout(location = 3) out vec3 frag_world_pos;
layout(location = 4) flat out uint frag_local_triangle;

uint read_u32(uint byte_offset, uint geometry_kind) {
    uint word_index = byte_offset >> 2u;
    if (geometry_kind == 0u) {
        return base_data[word_index];
    } else {
        return lod_data[word_index];
    }
}

vec3 read_vec3(uint base, uint index, uint gk) {
    uint addr = base + index * 12u;
    return vec3(uintBitsToFloat(read_u32(addr, gk)),
                uintBitsToFloat(read_u32(addr + 4u, gk)),
                uintBitsToFloat(read_u32(addr + 8u, gk)));
}

void main() {
    DrawEntry entry = draws[gl_InstanceIndex];
    uint pos_base = entry.payload_offset + 8u;
    uint normal_base = pos_base + entry.local_vertex_count * 12u;
    uint idx_base = pos_base + entry.local_vertex_count * 24u;

    uint local_index = read_u32(idx_base + gl_VertexIndex * 4u, entry.geometry_kind);
    vec3 position = read_vec3(pos_base, local_index, entry.geometry_kind);
    vec3 smooth_normal = read_vec3(normal_base, local_index, entry.geometry_kind);

    gl_Position = frame.view_projection * vec4(position, 1.0);
    frag_normal = normalize(smooth_normal);
    frag_world_pos = position;
    frag_geometry_index = entry.cluster_index;
    frag_geometry_kind = entry.geometry_kind;
    frag_local_triangle = gl_VertexIndex / 3u;
}
