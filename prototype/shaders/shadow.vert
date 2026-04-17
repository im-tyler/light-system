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
layout(set = 0, binding = 3) readonly buffer DrawList { DrawEntry draws[]; };

layout(push_constant) uniform ShadowPush {
    uint cascade_index;
} push;

uint read_u32(uint byte_offset, uint geometry_kind) {
    uint w = byte_offset >> 2u;
    return geometry_kind == 0u ? base_data[w] : lod_data[w];
}

void main() {
    DrawEntry entry = draws[gl_InstanceIndex];
    uint pos_base = entry.payload_offset + 8u;
    uint idx_base = pos_base + entry.local_vertex_count * 24u;
    uint local_idx = read_u32(idx_base + gl_VertexIndex * 4u, entry.geometry_kind);
    uint addr = pos_base + local_idx * 12u;
    vec3 pos = vec3(uintBitsToFloat(read_u32(addr, entry.geometry_kind)),
                    uintBitsToFloat(read_u32(addr+4u, entry.geometry_kind)),
                    uintBitsToFloat(read_u32(addr+8u, entry.geometry_kind)));
    gl_Position = frame.light_vp[push.cascade_index] * vec4(pos, 1.0);
}
