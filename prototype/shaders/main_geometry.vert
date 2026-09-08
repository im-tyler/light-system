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
layout(location = 5) out vec2 frag_uv;
layout(location = 6) flat out uint frag_has_uv;

uint read_u32(uint byte_offset, uint domain) {
    uint word_index = byte_offset >> 2u;
    if (domain == 0u) {
        return base_data[word_index];
    } else {
        return lod_data[word_index];
    }
}

vec3 read_vec3(uint base, uint index, uint domain) {
    uint addr = base + index * 12u;
    return vec3(uintBitsToFloat(read_u32(addr, domain)),
                uintBitsToFloat(read_u32(addr + 4u, domain)),
                uintBitsToFloat(read_u32(addr + 8u, domain)));
}

vec2 read_vec2(uint base, uint index, uint domain) {
    uint addr = base + index * 8u;
    return vec2(uintBitsToFloat(read_u32(addr, domain)),
                uintBitsToFloat(read_u32(addr + 4u, domain)));
}

void main() {
    DrawEntry entry = draws[gl_InstanceIndex];
    // Instance-folded draws: vertexCount is the bucket maximum, so corners
    // past this cluster's own count collapse to a shared point (counts are
    // triangle_count * 3, so no triangle straddles the boundary) and the
    // zero-area triangle is discarded by the rasterizer.
    if (gl_VertexIndex >= entry.draw_vertex_count) {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        frag_normal = vec3(0.0);
        frag_geometry_index = entry.cluster_index;
        frag_geometry_kind = entry.geometry_kind & 0xffffu;
        frag_world_pos = vec3(0.0);
        frag_local_triangle = 0u;
        frag_uv = vec2(0.0);
        frag_has_uv = 0u;
        return;
    }
    uint domain = entry.geometry_kind & 0xffffu;
    bool has_uv = (entry.geometry_kind & 0x10000u) != 0u;
    uint pos_base = entry.payload_offset + 8u;
    uint normal_base = pos_base + entry.local_vertex_count * 12u;
    uint uv_base = normal_base + entry.local_vertex_count * 12u;
    uint idx_base = uv_base + (has_uv ? entry.local_vertex_count * 8u : 0u);

    uint local_index = read_u32(idx_base + gl_VertexIndex * 4u, domain);
    vec3 position = read_vec3(pos_base, local_index, domain);
    vec3 smooth_normal = read_vec3(normal_base, local_index, domain);
    vec2 uv = has_uv ? read_vec2(uv_base, local_index, domain) : vec2(0.0);

    gl_Position = frame.view_projection * vec4(position, 1.0);
    frag_normal = normalize(smooth_normal);
    frag_world_pos = position;
    frag_geometry_index = entry.cluster_index;
    frag_geometry_kind = domain;
    frag_local_triangle = gl_VertexIndex / 3u;
    frag_uv = uv;
    frag_has_uv = has_uv ? 1u : 0u;
}
