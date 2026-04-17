#version 450

layout(set = 0, binding = 2) uniform FrameData {
    mat4 view_projection;
    mat4 light_vp;
    vec4 light_dir;
} frame;

layout(set = 0, binding = 3) uniform sampler2DShadow shadow_map;

layout(location = 0) in vec3 frag_normal;
layout(location = 1) flat in uint frag_geometry_index;
layout(location = 2) flat in uint frag_geometry_kind;
layout(location = 3) in vec3 frag_world_pos;
layout(location = 4) flat in uint frag_local_triangle;

layout(location = 0) out vec4 out_color;
layout(location = 1) out uvec2 out_visibility;

void main() {
    // Per-cluster color variation using geometry index as seed
    uint hash = frag_geometry_index * 2654435761u;
    float hue = float(hash & 0xFFu) / 255.0;
    // Subtle warm/cool variation around a base grey
    vec3 base_color = vec3(0.62 + hue * 0.12, 0.64 + hue * 0.08, 0.68 - hue * 0.06);

    vec3 N = gl_FrontFacing ? frag_normal : -frag_normal;
    vec3 L = normalize(frame.light_dir.xyz);
    float ndotl = max(dot(N, L), 0.0);

    // Shadow map lookup
    vec4 light_clip = frame.light_vp * vec4(frag_world_pos, 1.0);
    vec3 light_ndc = light_clip.xyz / light_clip.w;
    vec2 shadow_uv = light_ndc.xy * 0.5 + 0.5;
    float shadow_ref = light_ndc.z;
    float shadow = 1.0;
    if (shadow_uv.x >= 0.0 && shadow_uv.x <= 1.0 &&
        shadow_uv.y >= 0.0 && shadow_uv.y <= 1.0 &&
        shadow_ref >= 0.0 && shadow_ref <= 1.0) {
        // PCF 3x3 shadow filtering
        shadow = 0.0;
        vec2 texel_size = vec2(1.0) / vec2(textureSize(shadow_map, 0));
        for (int x = -1; x <= 1; x++) {
            for (int y = -1; y <= 1; y++) {
                vec2 offset = vec2(float(x), float(y)) * texel_size;
                shadow += texture(shadow_map, vec3(shadow_uv + offset, shadow_ref));
            }
        }
        shadow /= 9.0;
    }

    // Hemisphere ambient (sky blue from above, ground bounce from below)
    float up = N.y * 0.5 + 0.5;
    vec3 sky_color = vec3(0.55, 0.6, 0.7);
    vec3 ground_color = vec3(0.35, 0.3, 0.28);
    vec3 ambient = base_color * mix(ground_color, sky_color, up) * 0.75;
    vec3 diffuse = base_color * ndotl * shadow * 0.8;
    out_color = vec4(ambient + diffuse, 1.0);

    // Two-word visibility encoding matching visibility_format.h:
    // word0 = instance_index (always 0 for single-instance scenes)
    // word1 = valid_bit(31) | geometry_kind(30) | geometry_index(8..29) | local_triangle(0..7)
    uint word0 = 0u;
    uint word1 = (1u << 31u) |
                 (frag_geometry_kind << 30u) |
                 ((frag_geometry_index & 0x3fffffu) << 8u) |
                 (frag_local_triangle & 0xffu);
    out_visibility = uvec2(word0, word1);
}
