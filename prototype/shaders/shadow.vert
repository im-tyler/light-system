#version 450
// Enables writing gl_Layer from the vertex stage (SPIR-V capability
// ShaderViewportIndexLayerEXT); required by the merged layered shadow pass.
#extension GL_ARB_shader_viewport_layer_array : require

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

// Merged multi-cascade draw list: one draw per caster cluster, one instance
// per overlapping cascade. draw_first_instance = entry_index * 4 (see
// kShadowInstanceStride), so the entry index is gl_InstanceIndex >> 2 and the
// instance's slot within the draw is gl_InstanceIndex & 3. The cascade
// overlap mask rides in geometry_kind bits 17..19; instance slot i renders
// into the i-th set bit of that mask.
uint read_u32(uint byte_offset, uint geometry_kind) {
    uint w = byte_offset >> 2u;
    return geometry_kind == 0u ? base_data[w] : lod_data[w];
}

void main() {
    DrawEntry entry = draws[gl_InstanceIndex >> 2u];
    uint domain = entry.geometry_kind & 0xffffu;
    bool has_uv = (entry.geometry_kind & 0x10000u) != 0u;
    uint cascade_mask = (entry.geometry_kind >> 17u) & 7u;

    // Instance-folded draws: every kShadowInstanceStride slot runs, including
    // slots past this entry's cascade-overlap popcount, and vertexCount is the
    // bucket maximum -- both cases collapse to a shared point (zero-area
    // triangle, discarded by the rasterizer).
    uint corner = gl_VertexIndex;
    uint slot = gl_InstanceIndex & 3u;
    uint cascade = 0u;
    bool slot_live = false;
    {
        for (uint b = 0u; b < 3u; ++b) {
            if ((cascade_mask & (1u << b)) == 0u) continue;
            if (slot == 0u) {
                cascade = b;
                slot_live = true;
                break;
            }
            slot -= 1u;
        }
    }
    if (!slot_live || corner >= entry.draw_vertex_count) {
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        gl_Layer = 0;
        return;
    }

    uint pos_base = entry.payload_offset + 8u;
    uint idx_base = pos_base + entry.local_vertex_count * (has_uv ? 32u : 24u);
    uint local_idx = read_u32(idx_base + gl_VertexIndex * 4u, domain);
    uint addr = pos_base + local_idx * 12u;
    vec3 pos = vec3(uintBitsToFloat(read_u32(addr, domain)),
                    uintBitsToFloat(read_u32(addr+4u, domain)),
                    uintBitsToFloat(read_u32(addr+8u, domain)));
    gl_Position = frame.light_vp[cascade] * vec4(pos, 1.0);
    gl_Layer = int(cascade);
}
